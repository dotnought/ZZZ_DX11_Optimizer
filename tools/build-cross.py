#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the experimental PE x64 DLL using LLVM and an xwin SDK splat."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, required=True, help="xwin splat directory")
parser.add_argument("--llvm-bin", type=Path, required=True, help="Directory with clang, llvm-rc and lld-link")
parser.add_argument("--build-dir", type=Path, required=True, help="A new working directory for source and objects")
parser.add_argument("--jobs", type=int, default=6)
args = parser.parse_args()
sdk = args.sdk.resolve()
llvm = args.llvm_bin.resolve()
build_dir = args.build_dir.resolve()
build_dir.mkdir(parents=True, exist_ok=False)
source = build_dir / "xxmi"
out = build_dir / "out"
out.mkdir()
env = os.environ.copy()
repo = Path(__file__).resolve().parents[1]
pin = "8bd31f74727f033b3d9049509e19da3563c8b98c"
upstream = "https://github.com/SpectrumQT/XXMI-Libs-Package.git"
subprocess.run(["git", "clone", "--depth", "1", "--branch", "v1.3.3", upstream, source], check=True)
if subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"], text=True).strip() != pin:
    raise SystemExit("The XXMI source changed. Check the pinned release before rebuilding.")
for name in ("xxmi-v1.3.3.patch", "clang-cross.patch"):
    subprocess.run(["git", "-C", source, "apply", repo / "patches" / name], check=True)
for name in ("ZzzCapture.cpp", "ZzzCapture.h", "ZzzVertexChecks.h"):
    file = repo / "native" / name
    shutil.copyfile(file, source / "DirectX11" / file.name)
(source / "version.generated.h").write_text(
    '#define VERSION_MAJOR 1\n#define VERSION_MINOR 3\n#define VERSION_REVISION 3\n'
    '#define VERSION_BUILD 0\n#define VER_COMPANYNAME_STR "XXMI"\n'
    '#define VER_COMMENTS_STR "ZZZ DX11 capture build"\n',
    encoding="utf-8",
)
includes = [sdk/'crt/include', *(sdk/'sdk/include'/x for x in ('ucrt','shared','um','winrt'))]
libdirs = [sdk/'crt/lib/x86_64',sdk/'sdk/lib/ucrt/x86_64',sdk/'sdk/lib/um/x86_64']

# The Visual Studio project and SDK headers assume a case-insensitive filesystem.
# Apply aliases in the fresh source tree and the supplied, user-owned xwin splat.
headers = {}
for directory in [source, *includes]:
    for candidate in directory.rglob("*"):
        if candidate.suffix.lower() in (".h", ".hpp", ".inc", ".inl") and candidate.is_file():
            headers.setdefault(candidate.name.casefold(), []).append(candidate)
for file in source.rglob("*"):
    if file.suffix.lower() not in (".h", ".hpp", ".c", ".cpp", ".rc") or not file.is_file():
        continue
    for include in re.findall(r'#\s*include\s*[<"]([^">]+)', file.read_text(errors="ignore")):
        name = include.replace(chr(92), "/").rsplit("/", 1)[-1]
        for candidate in headers.get(name.casefold(), []):
            alias = candidate.with_name(name)
            if not alias.exists() and not alias.is_symlink():
                alias.symlink_to(candidate.name)
ns = {'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
flags = [str(llvm/'clang'), '--driver-mode=cl', '--target=x86_64-pc-windows-msvc',
         '/nologo', '/c', '/O2', '/MT', '/EHsc', '/Gy', '/Gw', '/utf-8', '/DUNICODE', '/D_UNICODE',
         '-Wno-ignored-attributes', '-Wno-microsoft-template', '-Wno-format', '-Wno-writable-strings', '-Wno-nonportable-include-path', '/clang:-maes'] + ['/imsvc'+str(i) for i in includes]

projects = [source / x for x in (
    'DirectX11/DirectX11.vcxproj', 'BinaryDecompiler/BinaryDecompiler.vcxproj',
    'D3D_Shaders/D3D_Shaders.vcxproj', 'DirectXTK/DirectXTK_Desktop_2017.vcxproj')]
tasks = []
deps = []
for p in projects:
    xml = ET.parse(p)
    project = next(group for group in xml.findall('m:ItemDefinitionGroup', ns)
                   if "'Release|x64'" in group.get('Condition',''))
    def field(name):
        return (project.findtext(name, '', ns) or '').strip()
    def expand(text):
        return text.replace('$(SolutionDir)', str(source) + '/').replace('$(ProjectDir)',str(p.parent) + '/').replace('\\','/')
    definitions = ['/D' + d for d in field('m:ClCompile/m:PreprocessorDefinitions').split(';') if d and not d.startswith('%(') and not d.startswith('_CRT_SECURE_CPP_OVERLOAD') ]
    inc = ['/I' + expand(d) for d in field('m:ClCompile/m:AdditionalIncludeDirectories').split(';') if d and not d.startswith('%(')]
    subdirs = ['/I'+str(p.parent), '/I'+str(source), '/I'+str(source/'BinaryDecompiler/include'),
               '/I'+str(source/'BinaryDecompiler'), '/I'+str(source/'crc32c-hw-1.0.5/include')]
    for item in xml.findall('.//m:ClCompile[@Include]', ns):
        f = (p.parent / expand(item.get('Include'))).resolve()
        if not f.exists():
            f = next((candidate for candidate in f.parent.iterdir()
                      if candidate.name.casefold() == f.name.casefold()), f)
        if f.suffix.lower() not in ('.cpp','.c'):
            continue
        # DirectX11 already compiles these shader sources. The cross build links the
        # DirectXTK units reached by its overlay and screenshot paths.
        if p.parent.name == 'D3D_Shaders' and f.name in ('Assembler.cpp', 'SignatureParser.cpp', 'Shaders.cpp'):
            continue
        if p.parent.name == 'DirectXTK' and f.stem not in (
            'CommonStates', 'SpriteBatch', 'SpriteFont', 'ScreenGrab', 'DDSTextureLoader', 'WICTextureLoader',
            'BasicEffect', 'EffectCommon', 'PrimitiveBatch', 'VertexTypes', 'BinaryReader'):
            continue
        obj = out / (p.parent.name + '-' + f.stem + '.obj')
        lang = '/std:c++17' if f.name == 'ZzzCapture.cpp' else '/std:c++14'
        cmd = flags + (['/clang:-mcrc32'] if f.name == 'crc32c.cpp' else []) + [lang] + definitions + inc + subdirs + ['/Fo' + str(obj), ('/Tc' if f.suffix.lower() == '.c' else '/Tp') + str(f)]
        tasks.append((f,obj,cmd))
    for d in field('m:Link/m:AdditionalDependencies').split(';'):
        if d and not d.startswith('%('):
            d = expand(d)
            deps.append(str((p.parent / d).resolve()) if '/' in d else d.lower())

def compile_task(t):
    f, obj, cmd = t
    run = subprocess.run(cmd, cwd=f.parent, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / (obj.stem + '.log')).write_bytes(run.stdout)
    if run.returncode:
        print(f'FAILED {f.name}', run.stdout.decode(errors='replace')[:2600], flush=True)
        return None
    return str(obj)

with ThreadPoolExecutor(max_workers=args.jobs) as pool:
    objects = list(pool.map(compile_task, tasks))
if None in objects:
    sys.exit(1)

resource = out/'DirectX11.res'
rc = [str(llvm/'llvm-rc'), '/nologo', '/fo', str(resource)] + [arg for i in includes for arg in ('/i',str(i))]
subprocess.run(rc+['DirectX11.rc'],cwd=source/'DirectX11',env=env,check=True)
print(f'Linking {len(objects)} objects', flush=True)
subprocess.run([str(llvm/'lld-link'), '/nologo', '/dll', '/machine:x64', '/subsystem:windows', '/opt:ref,icf',
                '/def:' + str(source/'DirectX11/d3d11Wrapper.def'), '/include:CreateDXGIFactory',
                '/out:' + str(out/'d3d11.dll')] +
                ['/libpath:' + str(d) for d in libdirs] + objects + [str(resource)] + sorted(set(deps)) +
                ['kernel32.lib','user32.lib','gdi32.lib','ole32.lib','uuid.lib','shell32.lib','winmm.lib'],
                cwd=source,env=env,check=True)
