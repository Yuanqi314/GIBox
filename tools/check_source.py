"""Ensure the deliverable contains source only, no binary payloads."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
for path in root.rglob('*'):
    if not path.is_file() or any(p in {'build','.gradle','.cxx','__pycache__','.git'} for p in path.relative_to(root).parts):
        continue
    assert path.suffix.lower() not in {'.apk','.dex','.so','.aar','.jar'}, f'Binary payload: {path}'
assert (root/'app/src/main/java/com/gibox/tool/ModelConverter.java').is_file()
assert (root/'app/src/main/cpp/runtime.cpp').is_file()
print('Source audit: PASS (source only: no APK/DEX/SO/AAR/JAR payloads)')
