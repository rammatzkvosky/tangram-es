#include "styleContext.h"

#include "data/propertyItem.h"
#include "data/tileData.h"
#include "log.h"
#include "platform.h"
#include "scene/filters.h"
#include "scene/scene.h"
#include "scene/styleParam.h"
#include "util/builders.h"
#include "util/fastmap.h"
#include "util/mapProjection.h"
#include "util/yamlUtil.h"
#include "yaml-cpp/yaml.h"

#ifdef TANGRAM_USE_DUKTAPE
#include "js/DuktapeContext.h"
#endif

#ifdef TANGRAM_USE_JAVASCRIPTCORE
#include "js/JSCoreContext.h"
#endif

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef RUNTIME_STYLE_CONTEXT
#define OVERRIDE
#else
#define OVERRIDE override
#endif

namespace Tangram {

template<class JSContext, class JSValue>
class JavaScriptScope {
public:
    explicit JavaScriptScope(JSContext& context) : _context(context) {
        _scopeMarker = _context.getScopeMarker();
    }

    ~JavaScriptScope() {
        _context.resetToScopeMarker(_scopeMarker);
    }

    JavaScriptScope& operator=(const JavaScriptScope& other) = delete;
    JavaScriptScope& operator=(JavaScriptScope&& other) = delete;

    JSValue newNull() { return _context.newNull(); }
    JSValue newBoolean(bool value) { return _context.newBoolean(value); }
    JSValue newNumber(double value) { return _context.newNumber(value); }
    JSValue newString(const std::string& value) { return _context.newString(value); }
    JSValue newArray() { return _context.newArray(); }
    JSValue newObject() { return _context.newObject(); }
    JSValue newFunction(const std::string& value) { return _context.newFunction(value); }
    JSValue getFunctionResult(JSFunctionIndex index) { return _context.getFunctionResult(index); }
private:
    JSContext& _context;
    JSScopeMarker _scopeMarker = 0;
};


template<class JSContext, class JSValue>
struct StyleContextBase : public StyleContext::DynamicStyleContext {

    using Scope = JavaScriptScope<JSContext, JSValue>;

    int32_t m_sceneId = -1;

    int m_functionCount = 0;

    JSContext m_jsContext;

    StyleContextBase() {}

    static JSValue pushYamlScalarAsJsPrimitive(Scope& jsScope, const YAML::Node& node) {
        bool booleanValue = false;
        double numberValue = 0.;
        if (YamlUtil::getBool(node, booleanValue)) {
            return jsScope.newBoolean(booleanValue);
        } else if (YamlUtil::getDouble(node, numberValue)) {
            return jsScope.newNumber(numberValue);
        } else {
            return jsScope.newString(node.Scalar());
        }
    }

    static JSValue pushYamlScalarAsJsFunctionOrString(Scope& jsScope, const YAML::Node& node) {
        auto value = jsScope.newFunction(node.Scalar());
        if (value) {
            return value;
        }
        return jsScope.newString(node.Scalar());
    }

    static JSValue parseSceneGlobals(Scope& jsScope, const YAML::Node& node) {
        switch(node.Type()) {
        case YAML::NodeType::Scalar: {
            auto& scalar = node.Scalar();
            if (scalar.compare(0, 8, "function") == 0) {
                return pushYamlScalarAsJsFunctionOrString(jsScope, node);
            }
            return pushYamlScalarAsJsPrimitive(jsScope, node);
        }
        case YAML::NodeType::Sequence: {
            auto jsArray = jsScope.newArray();
            for (size_t i = 0; i < node.size(); i++) {
                jsArray.setValueAtIndex(i, parseSceneGlobals(jsScope, node[i]));
            }
            return jsArray;
        }
        case YAML::NodeType::Map: {
            auto jsObject = jsScope.newObject();
            for (const auto& entry : node) {
                if (!entry.first.IsScalar()) {
                    continue; // Can't put non-scalar keys in JS objects.
                }
                jsObject.setValueForProperty(entry.first.Scalar(),
                                             parseSceneGlobals(jsScope, entry.second));
            }
            return jsObject;
        }
        default:
            return jsScope.newNull();
        }
    }

    void setSceneGlobals(const YAML::Node& sceneGlobals) OVERRIDE {
        if (!sceneGlobals) { return; }

        Scope jsScope(m_jsContext);
        auto jsValue = parseSceneGlobals(jsScope, sceneGlobals);

        m_jsContext.setGlobalValue("global", std::move(jsValue));
    }

    void initFunctions(const Scene& _scene) OVERRIDE {
        if (_scene.id == m_sceneId) { return; }
        m_sceneId = _scene.id;

        setSceneGlobals(_scene.config()["global"]);

        setFunctions(_scene.functions());
    }

    bool setFunctions(const std::vector<std::string>& _functions) OVERRIDE {
        uint32_t id = 0;
        bool success = true;
        for (auto& function : _functions) {
            success &= m_jsContext.setFunction(id++, function);
        }
        m_functionCount = id;

        return success;
    }

    bool addFunction(const std::string& _function) OVERRIDE {
        bool success = m_jsContext.setFunction(m_functionCount++, _function);
        return success;
    }

    void setFilterKey(Filter::Key _key, int _val) OVERRIDE {
        m_jsContext.setFilterKey(_key, _val);
    }

    void setFeature(const Feature& _feature) OVERRIDE {
        m_jsContext.setCurrentFeature(&_feature);
    }

    void clear() OVERRIDE {
        m_jsContext.setCurrentFeature(nullptr);
    }

    bool evalFilter(JSFunctionIndex _id) OVERRIDE {
        return m_jsContext.evaluateBooleanFunction(_id);
    }

    bool evalStyle(JSFunctionIndex _id, StyleParamKey _key, StyleParam::Value& _val) OVERRIDE {

        Scope jsScope(m_jsContext);

        auto jsValue = jsScope.getFunctionResult(_id);
        if (!jsValue) { return false; }

        _val = none_type{};

        // TODO check if duk/jscore provide functions to get a typeid and
        // switch on that instead of calling multiple times into them
        if (jsValue.isString()) {
            std::string value = jsValue.toString();

            switch (_key) {
            case StyleParamKey::outline_style:
            case StyleParamKey::repeat_group:
            case StyleParamKey::sprite:
            case StyleParamKey::sprite_default:
            case StyleParamKey::style:
            case StyleParamKey::text_align:
            case StyleParamKey::text_repeat_group:
            case StyleParamKey::text_source:
            case StyleParamKey::text_source_left:
            case StyleParamKey::text_source_right:
            case StyleParamKey::text_transform:
            case StyleParamKey::texture:
                _val = value;
                break;
            case StyleParamKey::color:
            case StyleParamKey::outline_color:
            case StyleParamKey::text_font_fill:
            case StyleParamKey::text_font_stroke_color: {
                Color result;
                if (StyleParam::parseColor(value, result)) {
                    _val = result.abgr;
                } else {
                    LOGW("Invalid color value: %s", value.c_str());
                }
                break;
            }
            default:
                _val = StyleParam::parseString(_key, value);
                break;
            }

        } else if (jsValue.isBoolean()) {
            bool value = jsValue.toBool();

            switch (_key) {
            case StyleParamKey::interactive:
            case StyleParamKey::text_interactive:
            case StyleParamKey::visible:
                _val = value;
                break;
            case StyleParamKey::extrude:
                _val = value ? glm::vec2(NAN, NAN) : glm::vec2(0.0f, 0.0f);
                break;
            default:
                break;
            }

        } else if (jsValue.isArray()) {
            auto len = jsValue.getLength();

            switch (_key) {
            case StyleParamKey::extrude: {
                if (len != 2) {
                    LOGW("Wrong array size for extrusion: '%d'.", len);
                    break;
                }

                double v1 = jsValue.getValueAtIndex(0).toDouble();
                double v2 = jsValue.getValueAtIndex(1).toDouble();

                _val = glm::vec2(v1, v2);
                break;
            }
            case StyleParamKey::color:
            case StyleParamKey::outline_color:
            case StyleParamKey::text_font_fill:
            case StyleParamKey::text_font_stroke_color: {
                if (len < 3 || len > 4) {
                    LOGW("Wrong array size for color: '%d'.", len);
                    break;
                }
                double r = jsValue.getValueAtIndex(0).toDouble();
                double g = jsValue.getValueAtIndex(1).toDouble();
                double b = jsValue.getValueAtIndex(2).toDouble();
                double a = 1.0;
                if (len == 4) {
                    a = jsValue.getValueAtIndex(3).toDouble();
                }
                _val = ColorF(r, g, b, a).toColor().abgr;
                break;
            }
            default:
                break;
            }
        } else if (jsValue.isNumber()) {
            double number = jsValue.toDouble();
            if (std::isnan(number)) {
                LOGD("duk evaluates JS method to NAN.\n");
            }
            switch (_key) {
            case StyleParamKey::text_source:
            case StyleParamKey::text_source_left:
            case StyleParamKey::text_source_right:
                _val = doubleToString(number);
                break;
            case StyleParamKey::extrude:
                _val = glm::vec2(0.f, number);
                break;
            case StyleParamKey::placement_spacing: {
                _val = StyleParam::Width{static_cast<float>(number), Unit::pixel};
                break;
            }
            case StyleParamKey::width:
            case StyleParamKey::outline_width: {
                // TODO more efficient way to return pixels.
                // atm this only works by return value as string
                _val = StyleParam::Width{static_cast<float>(number)};
                break;
            }
            case StyleParamKey::angle:
            case StyleParamKey::text_font_stroke_width:
            case StyleParamKey::placement_min_length_ratio: {
                _val = static_cast<float>(number);
                break;
            }
            case StyleParamKey::size: {
                StyleParam::SizeValue vec;
                vec.x.value = static_cast<float>(number);
                _val = vec;
                break;
            }
            case StyleParamKey::order:
            case StyleParamKey::outline_order:
            case StyleParamKey::priority:
            case StyleParamKey::color:
            case StyleParamKey::outline_color:
            case StyleParamKey::text_font_fill:
            case StyleParamKey::text_font_stroke_color: {
                _val = static_cast<uint32_t>(number);
                break;
            }
            default:
                break;
            }
        } else if (jsValue.isUndefined()) {
            // Explicitly set value as 'undefined'. This is important for some styling rules.
            _val = Undefined();
        } else {
            LOGW("Unhandled return type from Javascript style function for %d.", _key);
        }

        return !_val.is<none_type>();
    }
};

#ifdef TANGRAM_USE_DUKTAPE
using DuktapeStyleContext = StyleContextBase<Duktape::Context, Duktape::Value>;
#endif

#ifdef TANGRAM_USE_JAVASCRIPTCORE
using JSCoreStyleContext = StyleContextBase<JSCore::Context, JSCore::Value>;
#endif

#ifdef TANGRAM_USE_JAVASCRIPTCORE
StyleContext::StyleContext()
    : impl(std::make_unique<JSCoreStyleContext>()) {}
#elif TANGRAM_USE_DUKTAPE
StyleContext::StyleContext()
    : impl(std::make_unique<DuktapeStyleContext>()) {}
#endif

StyleContext::StyleContext(bool jscore) {
#ifdef TANGRAM_USE_JAVASCRIPTCORE
    if (jscore) {
        LOG("Hi I'm JavaScriptCore");
        impl.reset(new JSCoreStyleContext());
        return;
    }
#endif
    if (jscore) { LOGE("Sorry, built without JAvascpitcore.."); }
#if TANGRAM_USE_DUKTAPE
    LOG("Use more Duktape!");
    impl.reset(new DuktapeStyleContext());
#endif
}

void StyleContext::setFeature(const Feature& _feature) {
    impl->setFeature(_feature);
    setFilterKey(Filter::Key::geometry, _feature.geometryType);
}

void StyleContext::setFilterKey(Filter::Key _key, int _value) {
    if (_key == Filter::Key::other) { return; }

    if (m_filterKeys[uint8_t(_key)] == _value) {
        return;
    }
    m_filterKeys[uint8_t(_key)] = _value;

    if (_key == Filter::Key::zoom) {
        m_zoomLevel = _value;
        // scale the filter value with pixelsPerMeter
        // used with `px2` area filtering
        double metersPerPixel = (MapProjection::EARTH_CIRCUMFERENCE_METERS *
                                 exp2(-_value) / MapProjection::tileSize());
        m_pixelAreaScale = metersPerPixel * metersPerPixel;
    }
    impl->setFilterKey(_key, _value);
}

int StyleContext::getFilterKey(Filter::Key _key) const {
    return m_filterKeys[static_cast<uint8_t>(_key)];
}
bool StyleContext::evalFilter(JSFunctionIndex _id) {
    return impl->evalFilter(_id);
}
bool StyleContext::evalStyle(JSFunctionIndex _id, StyleParamKey _key, StyleParam::Value& _value) {
    return impl->evalStyle(_id, _key, _value);
}
void StyleContext::initFunctions(const Scene& _scene) {
    impl->initFunctions(_scene);
}
void StyleContext::clear() {
    impl->clear();
}
bool StyleContext::setFunctions(const std::vector<std::string>& _functions) {
    return impl->setFunctions(_functions);
}
bool StyleContext::addFunction(const std::string& _function) {
    return impl->addFunction(_function);
}
void StyleContext::setSceneGlobals(const YAML::Node& _sceneGlobals) {
    impl->setSceneGlobals(_sceneGlobals);
}

} // namespace Tangram
