import argparse
import pathlib
import random
import struct

parser = argparse.ArgumentParser(
    description="Generate deterministic DDS decode comparison fixtures."
)
parser.add_argument("output", type=pathlib.Path)
root = parser.parse_args().output
root.mkdir(parents=True, exist_ok=True)
rng = random.Random(312)
formats = [
    ("bc1", b"DXT1", 71, 8),
    ("bc2", b"DXT3", 74, 16),
    ("bc3", b"DXT5", 77, 16),
    ("bc4", b"ATI1", 80, 8),
    ("bc5", b"ATI2", 83, 16),
    ("bc4s", b"BC4S", 81, 8),
    ("bc5s", b"BC5S", 84, 16),
]


def fixture(
    name,
    code,
    dxgi,
    block,
    w,
    h,
    *,
    mips=1,
    depth=1,
    layers=1,
    cube=False,
    volume=False,
):
    use_dx = cube or volume
    head = bytearray(148 if use_dx else 128)
    head[:4] = b"DDS "

    def put(o, v):
        struct.pack_into("<I", head, o, v)

    for o, v in [
        (4, 124),
        (12, h),
        (16, w),
        (24, depth if volume else 0),
        (28, mips),
        (76, 32),
        (80, 4),
        (108, 0x1000),
    ]:
        put(o, v)
    head[84:88] = b"DX10" if use_dx else code
    if use_dx:
        for o, v in [
            (128, dxgi),
            (132, 4 if volume else 3),
            (136, 4 if cube else 0),
            (140, layers),
        ]:
            put(o, v)
    payload = bytearray()
    for layer in range(layers * (6 if cube else 1)):
        for mip in range(mips):
            mw, mh, md = (
                max(1, w >> mip),
                max(1, h >> mip),
                max(1, depth >> mip) if volume else 1,
            )
            payload += rng.randbytes(((mw + 3) // 4) * ((mh + 3) // 4) * block * md)
    (root / (name + ".dds")).write_bytes(head + payload)


for name, code, dxgi, block in formats:
    fixture(name + "-1024", code, dxgi, block, 1024, 1024)
    for w, h in [(1, 1), (3, 5), (4, 4), (5, 4), (8, 7), (9, 9), (17, 31)]:
        fixture(f"{name}-{w}x{h}", code, dxgi, block, w, h)
    fixture(name + "-cube-mips", code, dxgi, block, 9, 7, mips=4, layers=2, cube=True)
    fixture(
        name + "-volume-mips", code, dxgi, block, 7, 9, mips=3, depth=3, volume=True
    )
print("fixtures", len(list(root.glob("*.dds"))))
