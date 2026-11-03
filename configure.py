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
# Native SDK/runtime descriptors retain local host paths outside the package.
provider_root = ROOT / 'build/native-providers'
subprocess.run([sys.executable, str(ROOT / 'release/providers.py'), '--interpreter', a.python, '--out', str(provider_root)], check=True)
provider_facts = json.loads((provider_root / 'facts.json').read_text())
requirements = provider_facts['requirements']
extension.update(requirements=[item['id'] for item in requirements], include_dirs=[], library_dirs=[], runtime_library_dirs=[], link_libraries=[])
manifest.pop('modules', None)
manifest.update(version='0.1.0', source_root='src', files=['src/' + NAME + '.s', 'native/' + NAME + '.cpp', 'README.md'], native_requirements=requirements)
(ROOT / 'sputnik.build.yaml').write_text(''.join(key + ': ' + json.dumps(value) + '\n' for key, value in manifest.items()))
print('Prepared ' + NAME + '; bind native providers with SPUTNIK_NATIVE_PROVIDERS=' + str(provider_root / 'runtime') + ':' + str(provider_root / 'sdk'))
