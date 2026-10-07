#include "amdva/publishing.hpp"
#include <Python.h>
#include <gst/base/gstbasetransform.h>
#include <mutex>
namespace {
std::string python_error() {
    if (!PyErr_Occurred())
        return "Python callback failed";
    PyObject *type{}, *value{}, *trace{};
    PyErr_Fetch(&type, &value, &trace);
    PyErr_NormalizeException(&type, &value, &trace);
    auto *text = PyObject_Str(value ? value : Py_None);
    const char *message = text ? PyUnicode_AsUTF8(text) : nullptr;
    std::string result = message ? message : "Python callback failed";
    Py_XDECREF(text);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(trace);
    return result;
}
struct PythonState {
    PyObject *module{}, *callback{}, *loads{}, *dumps{};
    std::string module_name;
    void unregister() {
        if (!module_name.empty())
            if (PyDict_DelItemString(PyImport_GetModuleDict(), module_name.c_str()))
                PyErr_Clear();
    }
    static void initialize() {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        if (!Py_IsInitialized()) {
            Py_Initialize();
            PyEval_SaveThread();
        }
    }
    PythonState(const char *path, const char *name) {
        initialize();
        auto gil = PyGILState_Ensure();
        try {
            auto *util = PyImport_ImportModule("importlib.util");
            if (!util)
                throw std::runtime_error(python_error());
            auto *id = g_uuid_string_random();
            module_name = "amdva_callback_" + std::string(id);
            g_free(id);
            auto *spec = PyObject_CallMethod(util, "spec_from_file_location", "ss",
                                             module_name.c_str(), path);
            if (!spec || spec == Py_None) {
                Py_XDECREF(spec);
                Py_DECREF(util);
                throw std::runtime_error("Cannot load Python source file: " + std::string(path));
            }
            module = PyObject_CallMethod(util, "module_from_spec", "O", spec);
            Py_DECREF(util);
            auto *loader = PyObject_GetAttrString(spec, "loader");
            Py_DECREF(spec);
            if (module &&
                PyDict_SetItemString(PyImport_GetModuleDict(), module_name.c_str(), module) < 0) {
                Py_XDECREF(loader);
                throw std::runtime_error(python_error());
            }
            auto *executed = module && loader
                                 ? PyObject_CallMethod(loader, "exec_module", "O", module)
                                 : nullptr;
            Py_XDECREF(loader);
            if (!executed)
                throw std::runtime_error(python_error());
            Py_DECREF(executed);
            callback = PyObject_GetAttrString(module, name);
            if (!callback || !PyCallable_Check(callback))
                throw std::runtime_error("Python function is missing or not callable");
            auto *json = PyImport_ImportModule("json");
            if (!json)
                throw std::runtime_error(python_error());
            loads = PyObject_GetAttrString(json, "loads");
            dumps = PyObject_GetAttrString(json, "dumps");
            Py_DECREF(json);
        } catch (...) {
            unregister();
            Py_XDECREF(module);
            Py_XDECREF(callback);
            Py_XDECREF(loads);
            Py_XDECREF(dumps);
            PyGILState_Release(gil);
            throw;
        }
        PyGILState_Release(gil);
    }
    ~PythonState() {
        auto gil = PyGILState_Ensure();
        unregister();
        Py_XDECREF(module);
        Py_XDECREF(callback);
        Py_XDECREF(loads);
        Py_XDECREF(dumps);
        PyGILState_Release(gil);
    }
    bool process(GstBuffer *buffer) {
        auto gil = PyGILState_Ensure();
        try {
            auto input = amdva::analysis_json(buffer);
            auto *json = PyUnicode_FromStringAndSize(input.data(), input.size());
            auto *frame = json ? PyObject_CallFunctionObjArgs(loads, json, nullptr) : nullptr;
            Py_XDECREF(json);
            if (!frame)
                throw std::runtime_error(python_error());
            auto *result = PyObject_CallFunctionObjArgs(callback, frame, nullptr);
            Py_DECREF(frame);
            if (!result)
                throw std::runtime_error(python_error());
            if (result != Py_None && result != Py_True && result != Py_False &&
                !PyDict_Check(result)) {
                Py_DECREF(result);
                throw std::runtime_error("Python callback must return None, bool or dict");
            }
            const bool keep = result != Py_False;
            if (PyDict_Check(result)) {
                auto *args = PyTuple_Pack(1, result);
                auto *keywords = Py_BuildValue("{s:O}", "allow_nan", Py_False);
                auto *serialized =
                    args && keywords ? PyObject_Call(dumps, args, keywords) : nullptr;
                Py_XDECREF(args);
                Py_XDECREF(keywords);
                if (!serialized) {
                    Py_DECREF(result);
                    throw std::runtime_error(python_error());
                }
                Py_ssize_t bytes{};
                auto *text = PyUnicode_AsUTF8AndSize(serialized, &bytes);
                if (!text || bytes > 65536) {
                    Py_DECREF(serialized);
                    Py_DECREF(result);
                    throw std::runtime_error("Python custom metadata exceeds 64 KiB");
                }
                auto *meta = amdva::get_analysis_meta(buffer);
                if (!meta)
                    meta =
                        amdva::add_analysis_meta(buffer, std::make_shared<amdva::AnalysisData>());
                if (!meta) {
                    Py_DECREF(serialized);
                    Py_DECREF(result);
                    throw std::runtime_error("Cannot attach Python metadata");
                }
                (**meta->data).custom_json = text;
                Py_DECREF(serialized);
            }
            Py_DECREF(result);
            PyGILState_Release(gil);
            return keep;
        } catch (...) {
            PyGILState_Release(gil);
            throw;
        }
    }
};
} // namespace
typedef struct {
    GstBaseTransform parent;
    gchar *module, *function;
    PythonState *state;
} AmdPython;
typedef struct {
    GstBaseTransformClass parent;
} AmdPythonClass;
G_DEFINE_TYPE(AmdPython, amd_python, GST_TYPE_BASE_TRANSFORM)
enum { P_NONE, P_MODULE, P_FUNCTION };
static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdPython *>(o);
    if (GST_STATE(s) > GST_STATE_READY)
        return;
    if (p == P_MODULE) {
        g_free(s->module);
        s->module = g_value_dup_string(v);
    } else if (p == P_FUNCTION) {
        g_free(s->function);
        s->function = g_value_dup_string(v);
    } else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdPython *>(o);
    if (p == P_MODULE)
        g_value_set_string(v, s->module);
    else if (p == P_FUNCTION)
        g_value_set_string(v, s->function);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
}
static gboolean start(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdPython *>(t);
    try {
        if (!s->module || !s->function)
            throw std::runtime_error("Python module and function are required");
        auto state = std::make_unique<PythonState>(s->module, s->function);
        delete s->state;
        s->state = state.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, LIBRARY, INIT, ("Python initialization failed"), ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdPython *>(t);
    delete s->state;
    s->state = nullptr;
    return TRUE;
}
static GstFlowReturn transform(GstBaseTransform *t, GstBuffer *buffer) {
    auto *s = reinterpret_cast<AmdPython *>(t);
    try {
        return s->state->process(buffer) ? GST_FLOW_OK : GST_BASE_TRANSFORM_FLOW_DROPPED;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, STREAM, FAILED, ("Python callback failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject *o) {
    auto *s = reinterpret_cast<AmdPython *>(o);
    delete s->state;
    g_free(s->module);
    g_free(s->function);
    G_OBJECT_CLASS(amd_python_parent_class)->finalize(o);
}
static void amd_python_class_init(AmdPythonClass *k) {
    auto *object = G_OBJECT_CLASS(k);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                          GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_MODULE,
        g_param_spec_string("module", "Module", "Python source file", nullptr, flags));
    g_object_class_install_property(
        object, P_FUNCTION,
        g_param_spec_string("function", "Function",
                            "callback(frame_dict): None/True, False to drop, or custom dict",
                            "process", flags));
    auto *element = GST_ELEMENT_CLASS(k);
    gst_element_class_set_static_metadata(
        element, "AMD metadata Python hook", "Filter/Metadata",
        "Runs Python on compact JSON metadata without exposing pixels or device pointers", "AMD");
    gst_element_class_add_pad_template(
        element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_new_any()));
    gst_element_class_add_pad_template(
        element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_new_any()));
    auto *base = GST_BASE_TRANSFORM_CLASS(k);
    base->start = start;
    base->stop = stop;
    base->transform_ip = transform;
}
static void amd_python_init(AmdPython *s) {
    s->function = g_strdup("process");
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(s), TRUE);
}
