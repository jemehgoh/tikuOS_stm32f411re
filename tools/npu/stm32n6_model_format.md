# STM32N6 runtime-loadable models

TikuOS accepts only ST EdgeAI relocatable `network_rel.bin` files on STM32N6.
The old TN6P envelope, manual EC relocation, and direct epoch-blob submission
are removed. The runtime ABI is the pinned ST EdgeAI 4.0.0 LL-ATON build in
`third_party/st/edgeai/4.0.0/`.

Storage is intentionally separated:

- `drivers/stm32n6/npu/models/` - optional compiler-generated embedded fixtures.
- the reserved OSPI raw slot - one combined model loaded dynamically at runtime.
- `tests/npu/fixtures/` - malformed and fault-injection inputs only.

Generate a combined model with retained debug metadata, for example:

```text
stedgeai generate -m yolov8_256_qdq_int8_od_coco-person.onnx \
  --target stm32n6 -st-neural-art --relocatable ecblob-in-params \
  --memory-pool tools/npu/stm32n6_model.mpool
```

Package the resulting `network_rel.bin` without modifying it:

```text
python tools/npu/stm32n6_relpack.py build/network_rel.bin \
  --output-root data/npu --name network_rel.bin
```

The command emits the unchanged raw image and a JSON/C manifest containing its
exact length and CRC. Provision `network_rel.bin` at OSPI offset `0x00880000`,
or mapped address `0x70880000`, before the scratch region at `0x03ffb000`.
The ST relocatable header is at byte offset zero, and the combined image uses
`file_params_ptr = NULL`. STM32CubeProgrammer can program the raw image
directly when the mapped binary start address and external loader are supplied:

```text
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG \
  -w data/npu/network_rel.bin 0x70880000 -v \
  -el /absolute/path/to/external_loader.stldr -hardRst
```

Direct programming is not power-loss atomic. A partial image is rejected by
the manifest CRC and bounded LL-ATON preflight on the next boot. On-device
callers may use `tiku_n6_model_store_begin()` followed by bounded
`tiku_n6_model_store_step()` calls, but the existing model interlock must be
clear first. Do not use `--c-array`: the combined image is too large for the
255 KiB boot SRAM image.
The compatibility bind spelling remains `/data/npu/network_rel.bin`, but it
resolves directly to the raw OSPI slot rather than a VFS file.

The packager rejects split binaries, requires EdgeAI reloc runtime 8.0 flags,
asynchronous mode, and `LL_ATON_EB_DBG_INFO`. It records the runtime version,
CRC, raw image length, slot offset, mapped address, and combined-image
parameter-pointer policy in the adjacent manifests. The on-device adapter maps
the complete raw image and calls
`ll_aton_reloc_get_info()`, `ll_aton_reloc_install()` in COPY mode, and the
LL-ATON runtime lifecycle. No TikuOS layer patches the ST EC/GOT relocations
itself.

`rt_ram_copy` must fit the linker-owned NPU tier. RESET activation pools must
remain inside that tier; non-relocatable COPY pools must target writable
runtime RAM, not the mapped NOR model source. The packager rejects absolute
COPY pools targeting the OSPI window; regenerate those models with the
relocatable parameter pool. IO metadata is obtained from
LL-ATON debug descriptors after installation; models must not be generated
with `--no-dbg-info`.
