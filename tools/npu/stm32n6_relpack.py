#!/usr/bin/env python3
"""Validate and publish an ST EdgeAI 4.x raw network_rel.bin for TikuOS."""
from __future__ import annotations

import argparse
import json
import pathlib
import struct

MAGIC = 0x4E49424E
HEADER_BYTES = 104
RUNTIME_VERSION = "STEdgeAI 4.0.0 / NetworkRuntime1201"
OSPI_MODEL_OFFSET = 0x00880000
OSPI_SCRATCH_OFFSET = 0x03FFB000
OSPI_MODEL_BYTES = OSPI_SCRATCH_OFFSET - OSPI_MODEL_OFFSET
OSPI_MMAP_BASE = 0x70000000
OSPI_SIZE_BYTES = 0x04000000
RUNTIME_RAM_BASE = 0x34000000
RUNTIME_RAM_BYTES = 0x003C0000


def crc32(data: bytes) -> int:
    """Match tiku_nvm_crc32(): reflected CRC-32, init/final all ones."""
    nib = (
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    )
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        crc = (crc >> 4) ^ nib[crc & 0xF]
        crc = (crc >> 4) ^ nib[crc & 0xF]
    return crc ^ 0xFFFFFFFF


def validate_pool_targets(image: bytes) -> None:
    """Reject absolute COPY pools aimed at the read-only OSPI window."""
    mask = 0x0FFFFFFF
    words = struct.unpack_from("<26I", image, 0)
    data_data = words[4] & mask
    params_start = words[11] & mask
    at = data_data + params_start
    if at > len(image) - 20:
        raise ValueError("ST memory-pool descriptor table exceeds input length")
    for index in range(10):
        desc_at = at + index * 20
        if desc_at > len(image) - 20:
            raise ValueError("ST memory-pool descriptor table is unterminated")
        name, flags, foff, dst, size = struct.unpack_from("<5I", image, desc_at)
        if name == 0 and flags == 0:
            return
        if name == 0 or flags == 0:
            raise ValueError(f"memory-pool {index} has a partial terminator")
        name_at = name & mask
        if name_at >= len(image) or image.find(b"\0", name_at) < 0:
            raise ValueError(f"memory-pool {index} name exceeds input length")
        pool_type = (flags >> 24) & 0xFF
        pool_id = flags & 0xFF
        if pool_type not in (1, 2, 3):
            raise ValueError(f"memory-pool {index} has unsupported type {pool_type}")
        if pool_type == 1 and pool_id != 0:
            raise ValueError("external relocatable memory-pool IDs are unsupported")
        if pool_type == 2:
            span = (size + 31) & ~31
            in_runtime_ram = (RUNTIME_RAM_BASE <= dst <
                              RUNTIME_RAM_BASE + RUNTIME_RAM_BYTES and
                              span <= RUNTIME_RAM_BASE + RUNTIME_RAM_BYTES - dst)
            if not in_runtime_ram:
                location = ("read-only OSPI" if
                            OSPI_MMAP_BASE <= dst < OSPI_MMAP_BASE + OSPI_SIZE_BYTES
                            else "unsupported/non-writable memory")
                raise ValueError(
                    f"COPY pool {index} targets {location} address 0x{dst:08x}; "
                    "regenerate with a writable runtime-RAM destination")
        if foff > len(image) or size > len(image) - foff:
            raise ValueError(f"memory-pool {index} source span exceeds input length")
    raise ValueError("ST memory-pool descriptor table is unterminated")


def validate(image: bytes) -> dict[str, int]:
    if len(image) < HEADER_BYTES:
        raise ValueError("network_rel.bin is shorter than the ST reloc header")
    if len(image) > OSPI_MODEL_BYTES:
        raise ValueError("network_rel.bin does not fit the raw N6 model slot")
    words = struct.unpack_from("<26I", image, 0)
    if words[0] != MAGIC:
        raise ValueError("input is not an ST network_rel.bin (AI_RELOC_MAGIC)")
    flags = words[1]
    params_offset = words[12]
    if params_offset == 0:
        raise ValueError("split relocatable models are unsupported; use combined output")
    mask = 0x0FFFFFFF
    for name, value in (("text/data", words[4]), ("bss", words[6]),
                        ("rel", words[10])):
        if (value & mask) > len(image):
            raise ValueError(f"ST {name} section exceeds input length")
    rel_start = words[9] & mask
    rel_end = words[10] & mask
    if rel_start > rel_end or rel_end > len(image) or (rel_end - rel_start) % 4:
        raise ValueError("ST relocation section has invalid geometry")
    for at in range(rel_start, rel_end, 4):
        site = struct.unpack_from("<I", image, at)[0]
        if (site & 0xF0000000) not in (0x20000000, 0x40000000):
            raise ValueError(f"ST relocation site at 0x{at:x} has invalid address class")
        if (site & mask) > len(image) - 4:
            raise ValueError(f"ST relocation site at 0x{at:x} exceeds input length")
    if ((flags >> 28) & 0xF, (flags >> 24) & 0xF) != (8, 0):
        raise ValueError("network_rel.bin was built for an incompatible reloc runtime")
    if not flags & (1 << 21):
        raise ValueError("network_rel.bin lacks LL_ATON_EB_DBG_INFO metadata")
    if not flags & (1 << 22):
        raise ValueError("network_rel.bin is not an asynchronous LL-ATON model")
    validate_pool_targets(image)
    return {"bytes": len(image), "crc32": crc32(image), "flags": flags,
            "params_offset": params_offset}


def c_array(image: bytes, symbol: str) -> str:
    values = ", ".join(f"0x{b:02x}" for b in image)
    return ("/* Generated by stm32n6_relpack.py; source bytes are unchanged. */\n"
            "#include <stdint.h>\n"
            f"const uint8_t {symbol}[] __attribute__((aligned(8), used)) = {{\n"
            f"    {values}\n}};\n"
            f"const uint32_t {symbol}_len = {len(image)}u;\n")


def manifest_header(metadata: dict[str, int], name: str) -> str:
    """Render the small firmware manifest consumed by the model manager."""
    return ("/* Generated by stm32n6_relpack.py; do not edit manually. */\n"
            "/* The values describe the raw ST image at slot offset zero. */\n"
            "#ifndef TIKU_STM32N6_MODEL_MANIFEST_H_\n"
            "#define TIKU_STM32N6_MODEL_MANIFEST_H_\n\n"
            "#include <stdint.h>\n\n"
            f"#define TIKU_NPU_MODEL_NAME \"{name}\"\n"
            f"#define TIKU_NPU_MODEL_FILE_BYTES {metadata['bytes']}U\n"
            f"#define TIKU_NPU_MODEL_FILE_CRC32 UINT32_C({metadata['crc32']})\n"
            f"#define TIKU_NPU_MODEL_FLAGS UINT32_C({metadata['flags']})\n"
            f"#define TIKU_NPU_MODEL_PARAMS_OFFSET {metadata['params_offset']}U\n"
            "#define TIKU_NPU_MODEL_FILE_PARAMS_PTR 0U\n\n"
            "#endif /* TIKU_STM32N6_MODEL_MANIFEST_H_ */\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("--output-root", type=pathlib.Path, default=pathlib.Path("data/npu"))
    parser.add_argument("--name")
    parser.add_argument("--raw-image", type=pathlib.Path,
                        help="write the unchanged raw image to this path")
    parser.add_argument("--slot-offset", type=lambda value: int(value, 0),
                        default=OSPI_MODEL_OFFSET,
                        help="physical OSPI slot offset for the manifest")
    parser.add_argument("--c-array", type=pathlib.Path)
    parser.add_argument("--manifest-header", type=pathlib.Path,
                        default=pathlib.Path("arch/stm32n6/tiku_n6_model_manifest.h"),
                        help="write the generated firmware manifest header")
    parser.add_argument("--symbol", default="tiku_npu_network_rel")
    args = parser.parse_args()
    image = args.input.read_bytes()
    metadata = validate(image)
    name = args.name or args.input.name
    if (pathlib.PurePosixPath(name).name != name or "/" in name or
            "\\" in name or '"' in name or "\0" in name or
            not name.endswith(".bin")):
        raise SystemExit("--name must be a single .bin filename")
    if args.slot_offset != OSPI_MODEL_OFFSET:
        raise SystemExit(
            f"--slot-offset must be the production N6 model slot "
            f"0x{OSPI_MODEL_OFFSET:08x}")
    args.output_root.mkdir(parents=True, exist_ok=True)
    raw_destination = args.raw_image or (args.output_root / name)
    raw_destination.parent.mkdir(parents=True, exist_ok=True)
    raw_destination.write_bytes(image)
    manifest = dict(metadata, runtime=RUNTIME_VERSION, model=name,
                    namespace="/data/npu/", mode="COPY", split=False,
                    storage="raw-slot", file_params_ptr=None,
                    slot_offset=args.slot_offset,
                    mapped_address=OSPI_MMAP_BASE + args.slot_offset,
                    raw_offset=0, payload_offset=0,
                    slot_bytes=OSPI_MODEL_BYTES,
                    raw_image=str(raw_destination))
    (args.output_root / (name + ".manifest.json")).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    args.manifest_header.parent.mkdir(parents=True, exist_ok=True)
    args.manifest_header.write_text(manifest_header(metadata, name),
                                    encoding="utf-8")
    if args.c_array:
        args.c_array.parent.mkdir(parents=True, exist_ok=True)
        args.c_array.write_text(c_array(image, args.symbol), encoding="utf-8")
    print(f"{args.input} -> raw image {raw_destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
