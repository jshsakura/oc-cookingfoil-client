"""Compress the app font (scripts/subset-fonts.py output) for the app's RomFS."""
import ctypes
import ctypes.util
from pathlib import Path

root = Path(__file__).resolve().parents[1]
library = ctypes.CDLL(ctypes.util.find_library("zstd"))
library.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
library.ZSTD_compressBound.restype = ctypes.c_size_t
library.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
library.ZSTD_compress.restype = ctypes.c_size_t
library.ZSTD_isError.argtypes = [ctypes.c_size_t]
library.ZSTD_isError.restype = ctypes.c_uint
destination = root / "romfs/fonts"
destination.mkdir(exist_ok=True)
names = [f"CookingFoilSans{suffix}-{weight}.{ext}" for suffix, ext in (("", "otf"), ("JP", "otf"), ("SC", "ttf"))
         for weight in ("Regular", "Bold")]
for name in names:
    data = (root / "host/fonts" / name).read_bytes()
    output = ctypes.create_string_buffer(library.ZSTD_compressBound(len(data)))
    count = library.ZSTD_compress(output, len(output), data, len(data), 19)
    if library.ZSTD_isError(count):
        raise RuntimeError("Font compression failed")
    (destination / (name + ".zst")).write_bytes(output.raw[:count])
# The wordmark face is tiny (Latin subset) and ships as is.
(destination / "Baloo2-ExtraBold.ttf").write_bytes((root / "host/fonts/Baloo2-ExtraBold.ttf").read_bytes())
for notice in ("OFL.txt", "OFL-JP.txt", "OFL-SC.txt", "OFL-Baloo2.txt"):
    license_text = (root / "host/fonts" / notice).read_text()
    (destination / notice).write_text("\n".join(line.rstrip() for line in license_text.splitlines()) + "\n")
