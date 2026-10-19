"""Print reproducible source checksums; output paths are relative to the project."""
from pathlib import Path
import hashlib

root = Path(__file__).resolve().parents[1]
paths = [root / "CMakeLists.txt"]
for directory in ("libs", "examples", "apps", "tests", "tools", "third_party"):
    for path in (root / directory).rglob("*"):
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc":
            paths.append(path)
for path in sorted(paths):
    print(hashlib.sha256(path.read_bytes()).hexdigest(), " ", path.relative_to(root).as_posix(), sep="")
