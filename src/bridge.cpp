#include <bridge.hpp>
#include <midi.hpp>
#include <string.hpp>
#include <dsp/ringbuffer.hpp>

#include <unistd.h>
#if defined ARCH_WIN
	#include <winsock2.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netinet/tcp.h>
	#include <fcntl.h>
#endif

#include <thread>


namespace rack {


struct BridgeMidiDriver;


struct BridgeClientConnection;
static BridgeClientConnection* connections[BRIDGE_NUM_PORTS] = {};
static audio::Port* audioListeners[BRIDGE_NUM_PORTS] = {};
static std::thread serverThread;
static bool serverRunning = false;
static BridgeMidiDriver* driver = NULL;


struct BridgeMidiInputDevice : midi::InputDevice {
};


struct BridgeMidiDriver : midi::Driver {
	BridgeMidiInputDevice devices[16];

	std::string getName() override {
		return "Bridge";
	}

	std::vector<int> getInputDeviceIds() override {
		std::vector<int> deviceIds;
		for (int i = 0; i < 16; i++) {
			deviceIds.push_back(i);
		}
		return deviceIds;
	}

	std::string getInputDeviceName(int deviceId) override {
		if (deviceId < 0)
			return "";
		return string::f("Port %d", deviceId + 1);
	}

	midi::InputDevice* subscribeInput(int deviceId, midi::Input* input) override {
		if (!(0 <= deviceId && deviceId < 16))
			return NULL;

		devices[deviceId].subscribe(input);
		return &devices[deviceId];
	}

	void unsubscribeInput(int deviceId, midi::Input* input) override {
		if (!(0 <= deviceId && deviceId < 16))
			return;

		devices[deviceId].unsubscribe(input);
	}
};


struct BridgeClientConnection {
	int client;
	bool ready = false;

	int port = -1;
	int sampleRate = 0;

	~BridgeClientConnection() {
		setPort(-1);
	}

	/** Returns true if successful */
	bool send(const void* buffer, int length) {
		if (length <= 0)
			return false;

#if defined ARCH_LIN
		int flags = MSG_NOSIGNAL;
#else
		int flags = 0;
#endif
		ssize_t remaining = 0;
		while (remaining < length) {
			ssize_t actual = ::send(client, (const char*) buffer, length, flags);
			if (actual <= 0) {
				ready = false;
				return false;
			}
			remaining += actual;
		}
		return true;
	}

	template <typename T>
	bool send(T x) {
		return send(&x, sizeof(x));
	}

	/** Returns true if successful */
	bool recv(void* buffer, int length) {
		if (length <= 0)
			return false;

#if defined ARCH_LIN
		int flags = MSG_NOSIGNAL;
#else
		int flags = 0;
#endif
		ssize_t remaining = 0;
		while (remaining < length) {
			ssize_t actual = ::recv(client, (char*) buffer + remaining, length - remaining, flags);
			if (actual <= 0) {
				ready = false;
				return false;
			}
			remaining += actual;
		}
		return true;
	}

	template <typename T>
	bool recv(T* x) {
		return recv(x, sizeof(*x));
	}

	void flush() {
		// Turn off Nagle
		int flag = 1;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(int));
		// Turn on Nagle
		flag = 0;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(int));
	}

	void run() {
		INFO("Bridge client connected");

		// Check hello key
		uint32_t hello = -1;
		recv<uint32_t>(&hello);
		if (hello != BRIDGE_HELLO) {
			INFO("Bridge client protocol mismatch %x %x", hello, BRIDGE_HELLO);
			return;
		}

		// Process commands until no longer ready
		ready = true;
		while (ready) {
			step();
		}

		INFO("Bridge client closed");
	}

	/** Accepts a command from the client */
	void step() {
		uint8_t command;
		if (!recv<uint8_t>(&command)) {
			return;
		}

		switch (command) {
			default:
			case NO_COMMAND: {
				WARN("Bridge client: bad command %d detected, closing", command);
				ready = false;
			} break;

			case QUIT_COMMAND: {
				ready = false;
			} break;

			case PORT_SET_COMMAND: {
				uint8_t port = -1;
				recv<uint8_t>(&port);
				setPort(port);
			} break;

			case MIDI_MESSAGE_COMMAND: {
				midi::Message message;
				if (!recv(&message.bytes, 3)) {
					return;
				}
				processMidi(message);
			} break;

			case AUDIO_SAMPLE_RATE_SET_COMMAND: {
				uint32_t sampleRate = 0;
				recv<uint32_t>(&sampleRate);
				setSampleRate(sampleRate);
			} break;

			case AUDIO_PROCESS_COMMAND: {
				uint32_t frames = 0;
				recv<uint32_t>(&frames);
				if (frames == 0 || frames > (1 << 16)) {
					ready = false;
					return;
				}

				float input[BRIDGE_INPUTS * frames];
				if (!recv(&input, BRIDGE_INPUTS * frames * sizeof(float))) {
					DEBUG("Failed to receive");
					return;
				}

				float output[BRIDGE_OUTPUTS * frames];
				std::memset(&output, 0, sizeof(output));
				processStream(input, output, frames);
				if (!send(&output, BRIDGE_OUTPUTS * frames * sizeof(float))) {
					DEBUG("Failed to send");
					return;
				}
				// flush();
			} break;
		}
	}

	void setPort(int port) {
		// Unbind from existing port
		if (0 <= this->port && connections[this->port] == this) {
			connections[this->port] = NULL;
		}

		// Bind to new port
		if ((0 <= port && port < BRIDGE_NUM_PORTS) && !connections[port]) {
			this->port = port;
			connections[this->port] = this;
			refreshAudio();
		}
		else {
			this->port = -1;
		}
	}

	void processMidi(midi::Message message) {
		if (!(0 <= port && port < BRIDGE_NUM_PORTS))
			return;
		if (!driver)
			return;
		driver->devices[port].onMessage(message);
	}

	void setSampleRate(int sampleRate) {
		this->sampleRate = sampleRate;
		refreshAudio();
	}

	void processStream(const float* input, float* output, int frames) {
		if (!(0 <= port && port < BRIDGE_NUM_PORTS))
			return;
		if (!audioListeners[port])
			return;
		audioListeners[port]->setBlockSize(frames);
		audioListeners[port]->processStream(input, output, frames);
	}

	void refreshAudio() {
		if (!(0 <= port && port < BRIDGE_NUM_PORTS))
			return;
		if (connections[port] != this)
			return;
		if (!audioListeners[port])
			return;
		audioListeners[port]->setSampleRate(sampleRate);
	}
};


static void clientRun(int client) {
	DEFER({
#if defined ARCH_WIN
		if (shutdown(client, SD_SEND)) {
			WARN("Bridge client shutdown() failed");
		}
		if (closesocket(client)) {
			WARN("Bridge client closesocket() failed");
		}
#else
		if (close(client)) {
			WARN("Bridge client close() failed");
		}
#endif
	});

#if defined ARCH_MAC
	// Avoid SIGPIPE
	int flag = 1;
	if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(int))) {
		WARN("Bridge client setsockopt() failed");
		return;
	}
#endif

	// Disable non-blocking
#if defined ARCH_WIN
	unsigned long blockingMode = 0;
	if (ioctlsocket(client, FIONBIO, &blockingMode)) {
		WARN("Bridge client ioctlsocket() failed");
		return;
	}
#else
	if (fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) & ~O_NONBLOCK)) {
		WARN("Bridge client fcntl() failed");
		return;
	}
#endif

	BridgeClientConnection connection;
	connection.client = client;
	connection.run();
}


static void serverConnect() {
	// Initialize sockets
#if defined ARCH_WIN
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		WARN("Bridge server WSAStartup() failed");
		return;
	}
	DEFER({
		WSACleanup();
	});
#endif

	// Get address
	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(BRIDGE_PORT);
#if defined ARCH_WIN
	addr.sin_addr.s_addr = inet_addr(BRIDGE_HOST);
#else
	inet_pton(AF_INET, BRIDGE_HOST, &addr.sin_addr);
#endif

	// Open socket
	int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		WARN("Bridge server socket() failed");
		return;
	}
	DEFER({
		if (close(server)) {
			WARN("Bridge server close() failed");
			return;
		}
		INFO("Bridge server closed");
	});

#if defined ARCH_MAC || defined ARCH_LIN
	int reuseAddrFlag = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuseAddrFlag, sizeof(reuseAddrFlag));
#endif

	// Bind socket to address
	if (bind(server, (struct sockaddr*) &addr, sizeof(addr))) {
		WARN("Bridge server bind() failed");
		return;
	}

	// Listen for clients
	if (listen(server, 20)) {
		WARN("Bridge server listen() failed");
		return;
	}
	INFO("Bridge server started");

	// Enable non-blocking
#if defined ARCH_WIN
	unsigned long blockingMode = 1;
	if (ioctlsocket(server, FIONBIO, &blockingMode)) {
		WARN("Bridge server ioctlsocket() failed");
		return;
	}
#else
	int flags = fcntl(server, F_GETFL, 0);
	fcntl(server, F_SETFL, flags | O_NONBLOCK);
#endif

	// Accept clients
	while (serverRunning) {
		int client = accept(server, NULL, NULL);
		if (client < 0) {
			// Wait a bit before attempting to accept another client
			std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
			continue;
		}

		// Launch client thread
		std::thread clientThread(clientRun, client);
		clientThread.detach();
	}
}

static void serverRun() {
	while (serverRunning) {
		std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
		serverConnect();
	}
}


void bridgeInit() {
	serverRunning = true;
	serverThread = std::thread(serverRun);

	driver = new BridgeMidiDriver;
	midi::addDriver(BRIDGE_DRIVER, driver);
}

void bridgeDestroy() {
	serverRunning = false;
	serverThread.join();
}

void bridgeAudioSubscribe(int port, audio::Port* audio) {
	if (!(0 <= port && port < BRIDGE_NUM_PORTS))
		return;
	// Check if an Audio is already subscribed on the port
	if (audioListeners[port])
		return;
	audioListeners[port] = audio;
	if (connections[port])
		connections[port]->refreshAudio();
}

void bridgeAudioUnsubscribe(int port, audio::Port* audio) {
	if (!(0 <= port && port < BRIDGE_NUM_PORTS))
		return;
	if (audioListeners[port] != audio)
		return;
	audioListeners[port] = NULL;
}


} // namespace rack
