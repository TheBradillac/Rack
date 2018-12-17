#include "plugin/Model.hpp"
#include "event.hpp"
#include "ui/MenuLabel.hpp"
#include "ui/MenuItem.hpp"
#include "ui/Menu.hpp"
#include "app/Port.hpp"
#include "engine/Module.hpp"


namespace rack {


template <class TModule, class TModuleWidget, typename... Tags>
Model *createModel(std::string author, std::string slug, std::string name, Tags... tags) {
	struct TModel : Model {
		Module *createModule() override {
			TModule *o = new TModule;
			return o;
		}
		ModuleWidget *createModuleWidget() override {
			TModule *module = new TModule;
			TModuleWidget *o = new TModuleWidget(module);
			o->model = this;
			return o;
		}
		ModuleWidget *createModuleWidgetNull() override {
			TModuleWidget *o = new TModuleWidget(NULL);
			o->model = this;
			return o;
		}
	};

	Model *o = new TModel;
	o->author = author;
	o->slug = slug;
	o->name = name;
	o->tags = {tags...};
	return o;
}

template <class TWidget>
TWidget *createWidget(Vec pos) {
	TWidget *o = new TWidget;
	o->box.pos = pos;
	return o;
}

template <class TParamWidget>
TParamWidget *createParam(Vec pos, Module *module, int paramId, float minValue, float maxValue, float defaultValue) {
	TParamWidget *o = new TParamWidget;
	o->box.pos = pos;
	o->quantity->module = module;
	o->quantity->paramId = paramId;
	o->setLimits(minValue, maxValue);
	o->setDefaultValue(defaultValue);
	return o;
}

template <class TParamWidget>
TParamWidget *createParamCentered(Vec pos, Module *module, int paramId, float minValue, float maxValue, float defaultValue) {
	TParamWidget *o = new TParamWidget;
	o->box.pos = pos.minus(o->box.size.div(2));
	o->quantity->module = module;
	o->quantity->paramId = paramId;
	o->setLimits(minValue, maxValue);
	o->setDefaultValue(defaultValue);
	return o;
}

template <class TPort>
TPort *createInput(Vec pos, Module *module, int inputId) {
	TPort *o = new TPort;
	o->box.pos = pos;
	o->module = module;
	o->type = Port::INPUT;
	o->portId = inputId;
	return o;
}

template <class TPort>
TPort *createInputCentered(Vec pos, Module *module, int inputId) {
	TPort *o = new TPort;
	o->box.pos = pos.minus(o->box.size.div(2));
	o->module = module;
	o->type = Port::INPUT;
	o->portId = inputId;
	return o;
}

template <class TPort>
TPort *createOutput(Vec pos, Module *module, int outputId) {
	TPort *o = new TPort;
	o->box.pos = pos;
	o->module = module;
	o->type = Port::OUTPUT;
	o->portId = outputId;
	return o;
}

template <class TPort>
TPort *createOutputCentered(Vec pos, Module *module, int outputId) {
	TPort *o = new TPort;
	o->box.pos = pos.minus(o->box.size.div(2));
	o->module = module;
	o->type = Port::OUTPUT;
	o->portId = outputId;
	return o;
}

template <class TModuleLightWidget>
TModuleLightWidget *createLight(Vec pos, Module *module, int firstLightId) {
	TModuleLightWidget *o = new TModuleLightWidget;
	o->box.pos = pos;
	o->module = module;
	o->firstLightId = firstLightId;
	return o;
}

template <class TModuleLightWidget>
TModuleLightWidget *createLightCentered(Vec pos, Module *module, int firstLightId) {
	TModuleLightWidget *o = new TModuleLightWidget;
	o->box.pos = pos.minus(o->box.size.div(2));
	o->module = module;
	o->firstLightId = firstLightId;
	return o;
}

template <class TMenuLabel = MenuLabel>
TMenuLabel *createMenuLabel(std::string text) {
	TMenuLabel *o = new TMenuLabel;
	o->text = text;
	return o;
}

template <class TMenuItem = MenuItem>
TMenuItem *createMenuItem(std::string text, std::string rightText = "") {
	TMenuItem *o = new TMenuItem;
	o->text = text;
	o->rightText = rightText;
	return o;
}

// TODO Reevaluate this. Does it belong here?
inline Menu *createMenu() {
	Menu *o = new Menu;
	o->box.pos = gMousePos;

	MenuOverlay *menuOverlay = new MenuOverlay;
	menuOverlay->addChild(o);

	event::gContext->rootWidget->addChild(menuOverlay);
	return o;
}


} // namespace rack
