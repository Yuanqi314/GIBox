"""Host validation. This does not claim an Android build or game-device test."""
from pathlib import Path
import os,shutil,subprocess
import host_stubs
root=Path(__file__).resolve().parents[1]
out=root/'build'/'host'
out.mkdir(parents=True,exist_ok=True)
def run(cmd):
    subprocess.run([str(x) for x in cmd],cwd=root,check=True)
flags=['-std=c++17','-Wall','-Wextra','-Werror','-Iapp/src/main/cpp','-Ithird_party/bcdec']
run(['g++',*flags,'tests/core_test.cpp','app/src/main/cpp/runtime.cpp','app/src/main/cpp/dds.cpp','-o',out/'core_test'])
run([out/'core_test'])
if (root/'tests/runtime_test.cpp').exists():
    run(['g++',*flags,'-pthread','tests/runtime_test.cpp','app/src/main/cpp/runtime.cpp','-o',out/'runtime_test'])
    run([out/'runtime_test'])
java=shutil.which('java')
if not java:raise RuntimeError('JDK 17 is required')
stubsrc=out/'stub-src';stubclasses=out/'stub-classes';classes=out/'classes'
for path in [stubsrc,stubclasses,classes]:
    if path.exists():shutil.rmtree(path)
    path.mkdir(parents=True)
stubs=host_stubs.write(stubsrc)
run([java,'com.sun.tools.javac.Main','--release','8','-d',stubclasses,*stubs])
sources=sorted((root/'app/src/main/java').rglob('*.java'))
run([java,'com.sun.tools.javac.Main','--release','8','-cp',stubclasses,'-d',classes,*sources,*sorted((root/'tests').glob('*.java'))])
run([java,'-cp',classes,'com.gibox.tool.ConverterTest',out/'core_test'])
run([java,'-cp',classes,'com.gibox.tool.DocumentPathsTest'])
patterns=out/'gimi-patterns'
if patterns.exists():shutil.rmtree(patterns)
patterns.mkdir(parents=True)
run([java,'-cp',classes,'com.gibox.tool.GimiPatternsTest',patterns])
run([java,'-cp',classes,'com.gibox.tool.CompatibilityTest'])
importer=out/'importer'
if importer.exists():shutil.rmtree(importer)
importer.mkdir(parents=True)
run([java,'-cp',os.pathsep.join([str(classes),str(stubclasses)]),'com.gibox.tool.ImporterTest',importer,root/'tests/avatar_ids.txt'])
print('Java app source: PASS (host API-signature compilation; Android SDK build still required)')
run(['python3',root/'tools/test_handoff.py'])
