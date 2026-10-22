#ifndef SPUTNIK_PYCALL_ERROR_NAMESPACE
#define SPUTNIK_PYCALL_ERROR_NAMESPACE "Pycall"
#endif
#define PY_SSIZE_T_CLEAN
#include "runtime/sputnik_ext.h"
#include <Python.h>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct HostFault {};
struct Ref {
  PyObject *p;
  explicit Ref(PyObject *value = nullptr) : p(value) {}
  ~Ref() { Py_XDECREF(p); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  PyObject *release() {
    auto *value = p;
    p = nullptr;
    return value;
  }
};
struct Handle {
  PyObject *object;
  Handle *next = nullptr;
};
struct CallbackData {
  uint64_t id;
  CallbackData *next = nullptr;
};
std::mutex callbacks_mutex;
CallbackData *released_callbacks = nullptr;
struct ActiveCall {
  SputnikCtx *cx;
  SputnikValue dispatcher;
  bool callback_failed = false;
};
thread_local ActiveCall *active_call = nullptr;
std::once_flag initialized;
std::mutex releases_mutex;
Handle *releases = nullptr;

void initialize() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(&initialize), &info) && info.dli_fname)
    (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
  if (Py_IsInitialized())
    return;
  PyConfig config;
  PyConfig_InitPythonConfig(&config);
  const char *executable = std::getenv("SPUTNIK_PYTHON_EXECUTABLE");
  const bool explicit_executable = executable && *executable;
#ifdef SPUTNIK_PYTHON_EXECUTABLE
  if (!explicit_executable)
    executable = SPUTNIK_PYTHON_EXECUTABLE;
#endif
  const auto cfg = executable && *executable
      ? std::filesystem::path(executable).parent_path().parent_path() / "pyvenv.cfg"
      : std::filesystem::path();
  if (explicit_executable && std::filesystem::is_regular_file(cfg)) {
    std::ifstream input(cfg);
    std::string line;
    bool compatible = false;
    while (std::getline(input, line)) {
      if (line.rfind("version = ", 0) == 0 ||
          line.rfind("version_info = ", 0) == 0) {
        const auto version = line.substr(line.find(" = ") + 3);
        const std::string expected = std::to_string(PY_MAJOR_VERSION) + "." +
                                     std::to_string(PY_MINOR_VERSION);
        compatible =
            version == expected || version.rfind(expected + ".", 0) == 0;
      }
    }
    if (!compatible) {
      PyConfig_Clear(&config);
      throw std::runtime_error(
          "worker venv must match pycall's Python " +
          std::to_string(PY_MAJOR_VERSION) + "." +
          std::to_string(PY_MINOR_VERSION) +
          "; rebuild pycall for the selected Python version");
    }
  }
  config.install_signal_handlers = 0;
  config.parse_argv = 0;
  if (executable && *executable) {
    PyStatus status =
        PyConfig_SetBytesString(&config, &config.program_name, executable);
    if (!PyStatus_Exception(status))
      status = PyConfig_SetBytesString(&config, &config.executable, executable);
    if (PyStatus_Exception(status)) {
      PyConfig_Clear(&config);
      throw std::runtime_error("cannot configure the Python interpreter");
    }
  }
  PyStatus status_init = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status_init))
    throw std::runtime_error("cannot initialize the Python interpreter");
  PyEval_SaveThread();
}
struct Python {
  PyGILState_STATE state;
  Python() {
    std::call_once(initialized, initialize);
    state = PyGILState_Ensure();
    Handle *pending;
    {
      std::lock_guard<std::mutex> lock(releases_mutex);
      pending = releases;
      releases = nullptr;
    }
    while (pending) {
      auto *next = pending->next;
      Py_DECREF(pending->object);
      delete pending;
      pending = next;
    }
  }
  ~Python() { PyGILState_Release(state); }
};
std::string text(SputnikCtx *cx, SputnikValue value) {
  const char *data;
  size_t size;
  if (!sputnik_str_view(cx, value, &data, &size))
    throw std::runtime_error("expected a string");
  return {data, size};
}
PyObject *checked(PyObject *value) {
  if (!value)
    throw std::runtime_error("Python operation failed");
  return value;
}
PyObject *object(SputnikCtx *cx, SputnikValue value) {
  void *raw = nullptr;
  if (!sputnik_handle_ptr(cx, value, "pycall.Handle", &raw))
    throw HostFault{};
  return static_cast<Handle *>(raw)->object;
}
PyObject *decode(SputnikCtx *cx, SputnikValue packet, unsigned depth = 0) {
  if (depth > 64 || !sputnik_is_list(cx, packet) ||
      sputnik_list_len(cx, packet) != 2)
    throw std::runtime_error("invalid Python conversion packet");
  int64_t tag;
  if (!sputnik_as_int(cx, sputnik_list_at(cx, packet, 0), &tag))
    throw std::runtime_error("invalid Python conversion tag");
  auto value = sputnik_list_at(cx, packet, 1);
  if (tag == 1)
    return Py_NewRef(object(cx, value));
  if (tag == 4) {
    auto name = text(cx, value);
    return checked(PyUnicode_DecodeUTF8(name.data(), name.size(), "strict"));
  }
  if (tag == 2 || tag == 3) {
    const size_t count = sputnik_list_len(cx, value);
    Ref result(tag == 2 ? PyList_New(count) : PyDict_New());
    checked(result.p);
    for (size_t i = 0; i < count; ++i) {
      auto entry = sputnik_list_at(cx, value, i);
      if (tag == 2) {
        PyList_SET_ITEM(result.p, i, decode(cx, entry, depth + 1));
      } else {
        Ref key(decode(cx, sputnik_list_at(cx, entry, 0), depth + 1));
        Ref item(decode(cx, sputnik_list_at(cx, entry, 1), depth + 1));
        if (PyDict_SetItem(result.p, key.p, item.p) < 0)
          throw std::runtime_error("cannot construct Python dict");
      }
    }
    return result.release();
  }
  if (tag != 0)
    throw std::runtime_error("unsupported Python argument type");
  int boolean;
  int64_t integer;
  double number;
  if (sputnik_is_null(cx, value))
    return Py_NewRef(Py_None);
  if (sputnik_as_bool(cx, value, &boolean))
    return PyBool_FromLong(boolean);
  if (sputnik_as_int(cx, value, &integer))
    return checked(PyLong_FromLongLong(integer));
  if (sputnik_as_float(cx, value, &number))
    return checked(PyFloat_FromDouble(number));
  if (sputnik_is_str(cx, value)) {
    auto string = text(cx, value);
    return checked(
        PyUnicode_DecodeUTF8(string.data(), string.size(), "strict"));
  }
  const uint8_t *bytes;
  size_t count;
  if (sputnik_bytes_view(cx, value, &bytes, &count))
    return checked(PyBytes_FromStringAndSize(
        reinterpret_cast<const char *>(bytes), count));
  throw std::runtime_error(
      "unsupported Python argument; use pycall.Object or simple values");
}
SputnikValue encode(SputnikCtx *cx, PyObject *value,
                    bool force_object = false) {
  checked(value);
  SputnikValue result = nullptr;
  int tag = 0;
  if (!force_object) {
    if (value == Py_None)
      result = sputnik_make_null(cx);
    else if (PyBool_Check(value))
      result = sputnik_make_bool(cx, value == Py_True);
    else if (PyLong_Check(value)) {
      int overflow = 0;
      auto integer = PyLong_AsLongLongAndOverflow(value, &overflow);
      if (!overflow && !PyErr_Occurred())
        result = sputnik_make_int(cx, integer);
      else
        PyErr_Clear();
    } else if (PyFloat_Check(value))
      result = sputnik_make_float(cx, PyFloat_AsDouble(value));
    else if (PyUnicode_Check(value)) {
      Py_ssize_t size;
      const char *data = PyUnicode_AsUTF8AndSize(value, &size);
      if (!data)
        throw std::runtime_error("cannot encode Python string");
      result = sputnik_make_str(cx, data, size);
    } else if (PyBytes_Check(value)) {
      result = sputnik_make_bytes(
          cx, reinterpret_cast<const uint8_t *>(PyBytes_AS_STRING(value)),
          PyBytes_GET_SIZE(value));
    }
  }
  if (!result) {
    auto *handle = new Handle{Py_NewRef(value)};
    result = sputnik_make_handle(cx, "pycall.Handle", handle);
    if (!result) {
      Py_DECREF(handle->object);
      delete handle;
      throw HostFault{};
    }
    tag = 1;
  }
  SputnikValue fields[] = {sputnik_make_int(cx, tag), result};
  return sputnik_make_list(cx, fields, 2);
}
std::string error_text(const char *fallback) {
  if (!PyErr_Occurred())
    return fallback;
  PyObject *type = nullptr, *value = nullptr, *trace = nullptr;
  PyErr_Fetch(&type, &value, &trace);
  PyErr_NormalizeException(&type, &value, &trace);
  Ref type_ref(type), value_ref(value), trace_ref(trace);
  Ref traceback(PyImport_ImportModule("traceback"));
  Ref lines(traceback.p ? PyObject_CallMethod(traceback.p, "format_exception",
                                              "OOO", type ? type : Py_None,
                                              value ? value : Py_None,
                                              trace ? trace : Py_None)
                        : nullptr);
  Ref separator(PyUnicode_FromString(""));
  Ref joined(lines.p ? PyUnicode_Join(separator.p, lines.p) : nullptr);
  Ref simple(!joined.p && value ? PyObject_Str(value) : nullptr);
  auto *rendered = joined.p ? joined.p : simple.p;
  const char *message = rendered ? PyUnicode_AsUTF8(rendered) : nullptr;
  std::string result = message ? message : fallback;
  PyErr_Clear();
  return result;
}
template <class Body>
SputnikStatus run(SputnikCtx *cx, SputnikValue *out, Body body,
                  SputnikValue dispatcher = nullptr) {
  ActiveCall call{cx, dispatcher};
  struct Scope {
    ActiveCall *previous;
    explicit Scope(ActiveCall *call) : previous(active_call) {
      active_call = call;
    }
    ~Scope() { active_call = previous; }
  } scope(&call);
  try {
    Python python;
    try {
      *out = body();
      return *out ? SPUTNIK_OK : SPUTNIK_ERR;
    } catch (const std::exception &error) {
      if (call.callback_failed) {
        PyErr_Clear();
        return SPUTNIK_ERR;
      }
      auto message = error_text(error.what());
      return sputnik_fault(cx, SPUTNIK_PYCALL_ERROR_NAMESPACE ".Error", message.c_str());
    }
  } catch (const HostFault &) {
    return SPUTNIK_ERR;
  } catch (const std::exception &error) {
    return sputnik_fault(cx, SPUTNIK_PYCALL_ERROR_NAMESPACE ".Error", error.what());
  }
}
void callback_destroy(PyObject *capsule) {
  auto *data = static_cast<CallbackData *>(
      PyCapsule_GetPointer(capsule, "sputnik.pycall.callback"));
  if (!data) {
    PyErr_Clear();
    return;
  }
  std::lock_guard<std::mutex> lock(callbacks_mutex);
  data->next = released_callbacks;
  released_callbacks = data;
}
PyObject *callback_invoke(PyObject *capsule, PyObject *args) {
  ActiveCall *call = active_call;
  if (!call || !call->dispatcher) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Sputnik callback requires an active pycall invocation on this thread");
    return nullptr;
  }
  try {
    auto *data = static_cast<CallbackData *>(
        PyCapsule_GetPointer(capsule, "sputnik.pycall.callback"));
    if (!data)
      return nullptr;
    std::vector<SputnikValue> packets;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); ++i)
      packets.push_back(encode(call->cx, PyTuple_GET_ITEM(args, i)));
    SputnikValue values[] = {
        sputnik_make_int(call->cx, data->id),
        sputnik_make_list(call->cx, packets.data(), packets.size())};
    SputnikValue output = nullptr;
    SputnikStatus status;
    {
      struct Unlocked {
        PyThreadState *state = PyEval_SaveThread();
        ~Unlocked() { PyEval_RestoreThread(state); }
      } unlocked;
      status =
          sputnik_call_block(call->cx, call->dispatcher, values, 2, &output);
    }
    if (status != SPUTNIK_OK) {
      call->callback_failed = true;
      PyErr_SetString(PyExc_RuntimeError, "Sputnik callback unwinding");
      return nullptr;
    }
    return decode(call->cx, output);
  } catch (const HostFault &) {
    call->callback_failed = true;
    PyErr_SetString(PyExc_RuntimeError, "Sputnik callback marshalling failed");
    return nullptr;
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
    return nullptr;
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "Sputnik callback failed");
    return nullptr;
  }
}
PyMethodDef callback_method = {"sputnik_callback", callback_invoke,
                               METH_VARARGS, nullptr};
void arity(size_t count, size_t expected) {
  if (count != expected)
    throw std::runtime_error("invalid pycall native arity");
}
} // namespace

extern "C" {
void pycall_release(void *raw) {
  auto *handle = static_cast<Handle *>(raw);
  // Collector threads never enter Python or run user __del__ methods.
  std::lock_guard<std::mutex> lock(releases_mutex);
  handle->next = releases;
  releases = handle;
}
SputnikStatus pycall_released_callbacks(SputnikCtx *cx, const SputnikValue *,
                                        size_t n, SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 0);
    CallbackData *pending;
    {
      std::lock_guard<std::mutex> lock(callbacks_mutex);
      pending = released_callbacks;
      released_callbacks = nullptr;
    }
    std::vector<SputnikValue> ids;
    while (pending) {
      auto *next = pending->next;
      ids.push_back(sputnik_make_int(cx, pending->id));
      delete pending;
      pending = next;
    }
    return sputnik_make_list(cx, ids.data(), ids.size());
  });
}
SputnikStatus pycall_callback(SputnikCtx *cx, const SputnikValue *a, size_t n,
                              SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    int64_t id;
    if (!sputnik_as_int(cx, a[0], &id))
      throw std::runtime_error("expected callback id");
    auto *data = new CallbackData{static_cast<uint64_t>(id)};
    Ref capsule(
        PyCapsule_New(data, "sputnik.pycall.callback", callback_destroy));
    if (!capsule.p) {
      delete data;
      throw std::runtime_error("cannot create Python callback");
    }
    Ref callable(
        checked(PyCFunction_NewEx(&callback_method, capsule.p, nullptr)));
    return encode(cx, callable.p, true);
  });
}
SputnikStatus pycall_import(SputnikCtx *cx, const SputnikValue *a, size_t n,
                            SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    auto name = text(cx, a[0]);
    Ref module(checked(PyImport_ImportModule(name.c_str())));
    return encode(cx, module.p);
  });
}
SputnikStatus pycall_eval(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(
      cx, out,
      [&] {
        arity(n, 2);
        auto source = text(cx, a[0]);
        PyObject *main = checked(PyImport_AddModule("__main__"));
        Ref value(checked(PyRun_StringFlags(source.c_str(), Py_eval_input,
                                            PyModule_GetDict(main),
                                            PyModule_GetDict(main), nullptr)));
        return encode(cx, value.p);
      },
      a[1]);
}
SputnikStatus pycall_value(SputnikCtx *cx, const SputnikValue *a, size_t n,
                           SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    Ref value(decode(cx, a[0]));
    return encode(cx, value.p, true);
  });
}
SputnikStatus pycall_get(SputnikCtx *cx, const SputnikValue *a, size_t n,
                         SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 2);
    auto name = text(cx, a[1]);
    Ref value(checked(PyObject_GetAttrString(object(cx, a[0]), name.c_str())));
    return encode(cx, value.p);
  });
}
SputnikStatus pycall_send(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(
      cx, out,
      [&] {
        arity(n, 6);
        if (!sputnik_is_null(cx, a[4]))
          throw std::runtime_error(
              "Python has no block channel; pass a callable argument instead");
        auto name = text(cx, a[1]);
        auto *receiver = object(cx, a[0]);
        Ref callable(name == "__call__" ? Py_NewRef(receiver)
                                        : checked(PyObject_GetAttrString(
                                              receiver, name.c_str())));
        const size_t count = sputnik_list_len(cx, a[2]);
        const size_t kw_count = sputnik_list_len(cx, a[3]);
        if (!PyCallable_Check(callable.p)) {
          if (count || kw_count)
            throw std::runtime_error("Python attribute is not callable");
          return encode(cx, callable.p);
        }
        Ref args(checked(PyTuple_New(count))), kwargs(checked(PyDict_New()));
        for (size_t i = 0; i < count; ++i)
          PyTuple_SET_ITEM(args.p, i, decode(cx, sputnik_list_at(cx, a[2], i)));
        for (size_t i = 0; i < kw_count; ++i) {
          auto entry = sputnik_list_at(cx, a[3], i);
          auto key = text(cx, sputnik_list_at(cx, entry, 0));
          Ref value(decode(cx, sputnik_list_at(cx, entry, 1)));
          if (PyDict_SetItemString(kwargs.p, key.c_str(), value.p) < 0)
            throw std::runtime_error("cannot construct Python kwargs");
        }
        Ref result(checked(PyObject_Call(callable.p, args.p, kwargs.p)));
        return encode(cx, result.p);
      },
      a[5]);
}
SputnikStatus pycall_set(SputnikCtx *cx, const SputnikValue *a, size_t n,
                         SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 3);
    auto name = text(cx, a[1]);
    Ref value(decode(cx, a[2]));
    if (PyObject_SetAttrString(object(cx, a[0]), name.c_str(), value.p) < 0)
      throw std::runtime_error("Python setattr failed");
    return encode(cx, value.p);
  });
}
SputnikStatus pycall_item(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 2);
    Ref key(decode(cx, a[1]));
    Ref value(checked(PyObject_GetItem(object(cx, a[0]), key.p)));
    return encode(cx, value.p);
  });
}
SputnikStatus pycall_set_item(SputnikCtx *cx, const SputnikValue *a, size_t n,
                              SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 3);
    Ref key(decode(cx, a[1])), value(decode(cx, a[2]));
    if (PyObject_SetItem(object(cx, a[0]), key.p, value.p) < 0)
      throw std::runtime_error("Python item assignment failed");
    return encode(cx, value.p);
  });
}
SputnikStatus pycall_sequence(SputnikCtx *cx, const SputnikValue *a, size_t n,
                              SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    Ref sequence(checked(
        PySequence_Fast(object(cx, a[0]), "expected a Python sequence")));
    std::vector<SputnikValue> items;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(sequence.p); ++i)
      items.push_back(encode(cx, PySequence_Fast_GET_ITEM(sequence.p, i)));
    return sputnik_make_list(cx, items.data(), items.size());
  });
}
SputnikStatus pycall_entries(SputnikCtx *cx, const SputnikValue *a, size_t n,
                             SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    Ref items(checked(PyMapping_Items(object(cx, a[0]))));
    std::vector<SputnikValue> entries;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items.p); ++i) {
      auto *pair = PyList_GET_ITEM(items.p, i);
      SputnikValue fields[] = {encode(cx, PyTuple_GET_ITEM(pair, 0)),
                               encode(cx, PyTuple_GET_ITEM(pair, 1))};
      entries.push_back(sputnik_make_list(cx, fields, 2));
    }
    return sputnik_make_list(cx, entries.data(), entries.size());
  });
}
SputnikStatus pycall_repr(SputnikCtx *cx, const SputnikValue *a, size_t n,
                          SputnikValue *out) {
  return run(cx, out, [&] {
    arity(n, 1);
    Ref value(checked(PyObject_Repr(object(cx, a[0]))));
    Py_ssize_t count;
    const char *data = PyUnicode_AsUTF8AndSize(value.p, &count);
    if (!data)
      throw std::runtime_error("Python repr failed");
    return sputnik_make_str(cx, data, count);
  });
}
}
