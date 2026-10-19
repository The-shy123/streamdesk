"""Fetch a fixed official Boost archive into build/, never install system files."""
from pathlib import Path
import hashlib
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
URL = "https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.bz2"
SHA256 = "46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b"
archive = ROOT / "build/downloads/boost_1_88_0.tar.bz2"
archive.parent.mkdir(parents=True, exist_ok=True)
if not archive.exists():
    partial = archive.with_suffix(".partial")
    with urllib.request.urlopen(URL, timeout=30) as response, partial.open("wb") as target:
        total = 0
        while block := response.read(1024 * 1024):
            total += len(block)
            if total > 200 * 1024 * 1024:
                raise RuntimeError("archive exceeds download limit")
            target.write(block)
    partial.rename(archive)
with archive.open("rb") as file:
    digest = hashlib.sha256()
    for block in iter(lambda: file.read(1024 * 1024), b""):
        digest.update(block)
if digest.hexdigest() != SHA256:
    raise RuntimeError("Boost archive checksum mismatch; preserve for inspection, do not extract")
destination = ROOT / "build/deps"
with tarfile.open(archive, "r:bz2") as tar:
    for member in tar:
        if not (member.name.startswith("boost_1_88_0/boost/") or member.name == "boost_1_88_0/LICENSE_1_0.txt"):
            continue
        path = destination / member.name
        if not path.resolve().is_relative_to(destination.resolve()):
            raise RuntimeError("unsafe archive path")
        if member.isdir():
            path.mkdir(parents=True, exist_ok=True)
        elif member.isfile():
            path.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source, path.open("wb") as target:
                shutil.copyfileobj(source, target)
        else:
            raise RuntimeError("unexpected archive link")
print("Verified Boost 1.88.0 headers:", destination / "boost_1_88_0")
