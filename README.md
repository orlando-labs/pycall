# pycall

Embed CPython in Sputnik and use installed Python libraries directly:

```sputnik
import pycall.numpy as np
from pycall.json import dumps

array = np.array([1, 2, 3], dtype: "int64")
print(array.tolist().to_a)
print(dumps({name: "Ada"}))
```

`import pycall.a.b as name` imports Python module `a.b`. `from pycall.a import X`
imports attribute `X` from module `a`. Imports initialize at Sputnik module load;
Python's import cache preserves the identity of the underlying module. Imported
bindings are read-only. The bridge does not install pip dependencies.

## Prepare and install

Use the updated Sputnik compiler from the sibling `sputnik-lang` checkout and
CPython 3.10+ with development headers and a shared library. Select the actual
interpreter (including a virtual environment) explicitly:

```sh
python3 configure.py --python /path/to/python3
../sputnik-lang/build/sputnik pkg install "$PWD"
```

Configuration writes `sputnik.build.json` and `sputnik.toml` with the selected
SDK/library paths. These are local build facts; regenerate them on another host.
For example on this development Mac the interpreter is `/opt/homebrew/bin/python3.14`.
The compiler itself has no Python dependency.

Run an application with `sputnik run app.s --grant ffi`. Native applications use
`sputnik build app.s --require-full-native -o app` and retain the selected
interpreter/library dependency. This implementation has been exercised on macOS
arm64; the source/configuration also handle Linux shared-library layouts. Windows
is not supported.

## Objects and conversions

Null, booleans, Int64, floats, UTF-8 strings and bytes cross automatically. Sputnik
arrays/tuples and maps are recursively copied into Python lists/dicts. Symbols
become strings. Cycles/depth beyond 64 in input containers are rejected.

Other Python values, including containers, NumPy arrays/scalars and large Python
integers, remain `pycall.Object` proxies. No implicit array copying occurs on
return. Use indexing, explicit `to_a`/`to_map`, or the library's own conversions.
`to_a`/`to_map` convert one container level; nested objects stay proxies.

```sputnik
import pycall

items = pycall.wrap([1, 2, 3])
items[1] = 9
print(items.to_a)
obj = pycall.eval("type('Example', (), {'value': 7})()")
print(obj.value)
obj.setattr!("value", 8)
```

A callable attribute invoked through `.` is called; a non-callable attribute is
read when no arguments are supplied. `getattr(name)` retrieves an attribute
without invoking it. Proxy objects implement `call`, so Python functions and
classes support ordinary Sputnik call syntax. `invoke(name, ...)` bypasses
collisions with bridge helper methods; `repr()` returns Python `repr`.
Attribute assignment uses `setattr!`; item assignment uses `obj[key] = value`.
Foreign exceptions become rescuable `pycall.Error` with Python traceback text.

## Callbacks and lifetime

Python takes callable arguments rather than a Ruby-style block channel:

```sputnik
import pycall.builtins as builtins
import pycall

key = pycall.callback(|x|: 0 - x)
print(builtins.sorted([3, 1, 2], key: key).to_a)
```

Callbacks preserve Sputnik captures and can reenter Python. They may be retained
by Python and called during a later bridge invocation. The bridge roots Sputnik
closures until Python releases their callable; release notifications are drained
on later bridge entries. Calls from autonomous Python threads outside an active
Sputnik bridge invocation raise a Python `RuntimeError`. Callback keyword
arguments are not yet supported.

Python calls acquire its GIL and run through Sputnik's blocking FFI boundary.
The GIL is released while a Sputnik callback runs. Foreign object reclamation is
queued; Sputnik collector threads never run Python destructors. Explicit
`destroy!` invalidates the proxy handle immediately and queues release. Runtime
instances and their trampoline library stay alive until process exit; interpreter
restart/unload, subinterpreters, buffer zero-copy and `with` protocol sugar are
not implemented. A foreign call must finish before cancellation can retire it.

## Tests

```sh
python3 test/run.py
python3 test/run.py --numpy   # additionally requires NumPy in the selected Python
```

The runner checks VM execution and `--require-full-native`, runs the produced
executables, verifies zero VM fallback, and writes evidence under `build/tests/`.

## Notebook environments

Notebook workers can override the configured executable with a project `.venv`
(or `venv`) before Python initializes. The venv must match this extension's Python
major/minor SDK. Changing the selection requires a worker restart. Remote core
prepares persistent per-notebook environments from requirements.lock / requirements.txt;
it reuses them across reconnects and worker restarts. See the notebook repository's
[environment contract](../sputnik-notebook/docs/engineering/notebook-foreign-environments.md).

## Registry release

The registry reference is `slowpilot/pycall@0.1.0`. Source and prebuilt
adapters pin the exact interpreter version, ABI and native build digest. The
interpreter, its standard library and installed third-party libraries stay on
the consumer machine. No developer include/library path is in the archive.

Create local provider descriptors from the selected installed interpreter:

```sh
python3 release/providers.py --interpreter /path/to/python3 --out build/release-providers
export SPUTNIK_NATIVE_PROVIDERS="$PWD/build/release-providers/runtime:$PWD/build/release-providers/sdk"
export SPUTNIK_PYTHON_EXECUTABLE=/path/to/python3
sputnik pkg install slowpilot/pycall --version 0.1.0 --allow-native
```

Add `--build-from-source` for an adapter built locally. Descriptor identity must
match the selected published variant; merely sharing a major version does not
substitute for the declared runtime and SDK. Registry imports use the publisher
prefix, for example `import slowpilot.pycall.math as math`.

On macOS the provider/cache and selected interpreter must be on the same
filesystem. The runtime image is hard-linked so installed native modules use
the same interpreter instance; no interpreter library bytes are modified.
