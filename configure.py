#!/usr/bin/env python3
"""Prepare this package against an explicitly selected installed interpreter."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
NAME = 'pycall'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--python', default=os.environ.get('PYTHON', sys.executable))
a = p.parse_args()
facts = json.loads(subprocess.check_output([a.python, '-c', '''import json,sys,sysconfig
print(json.dumps(dict(executable=sys.executable, version=list(sys.version_info[:2]), include=sysconfig.get_path("include"), libdir=sysconfig.get_config_var("LIBDIR"), library=sysconfig.get_config_var("LDLIBRARY"), libpl=sysconfig.get_config_var("LIBPL"))))'''], text=True))
if facts['version'] < [3, 10]:
    p.error('select Python 3.10 or newer with --python (including its development headers and shared library)')
includes = [facts['include']]
library = facts['library']
if library and library.startswith('Python.framework/'):
    facts['libdir'] = facts['libpl']
    lib = 'python' + '.'.join(map(str, facts['version']))
elif library and library.startswith('lib') and ('.so' in library or '.dylib' in library):
    lib = library[3:].split('.so')[0].split('.dylib')[0]
else:
    p.error('selected Python must provide a shared libpython')
defines = ['SPUTNIK_PYTHON_EXECUTABLE=' + json.dumps(facts['executable'])]
for directory in includes:
    if not Path(directory).is_dir():
        p.error('missing interpreter headers: ' + directory)
extension = dict(name=NAME, language='c++', sources=['native/' + NAME + '.cpp'],
                 include_dirs=includes, library_dirs=[facts['libdir']],
                 runtime_library_dirs=[facts['libdir']], link_libraries=[lib] + (['dl'] if sys.platform.startswith('linux') else []), defines=defines)
manifest = dict(schema='sputnik.build.v1', name=NAME, root=NAME,
                profiles=dict(required=['core.v1', 'ffi.v1'], optional=[], forbidden=[]),
                modules=[dict(name=NAME, path='src/' + NAME + '.s')], native_extensions=[extension])
(ROOT / 'sputnik.build.json').write_text(json.dumps(manifest, indent=2) + '\n')
# Package installation uses the same native build facts as consumer manifests.
quote = lambda v: json.dumps(v, ensure_ascii=False)
lines = ['[package]', 'name = ' + quote(NAME), 'version = "0.1.0"', 'root = ' + quote(NAME),
         '', '[[modules]]', 'name = ' + quote(NAME), 'path = ' + quote('src/' + NAME + '.s'), '', '[[native]]']
for key, value in extension.items():
    lines.append(key + ' = ' + quote(value))
(ROOT / 'sputnik.toml').write_text('\n'.join(lines) + '\n')
print('Prepared ' + NAME + ' with ' + lib + ' in ' + facts['libdir'])
