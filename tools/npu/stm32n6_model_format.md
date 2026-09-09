# STM32N6 runtime-loadable models

TikuOS accepts only ST EdgeAI relocatable `network_rel.bin` files on STM32N6.
The old TN6P envelope, manual EC relocation, and direct epoch-blob submission
are removed. The runtime ABI is the pinned ST EdgeAI 4.0.0 LL-ATON build in
`third_party/st/edgeai/4.0.0/`.

Storage is intentionally separated:

- `drivers/stm32n6/npu/models/` - optional compiler-generated embedded fixtures.
- the reserved XSPI bigblob slot - one combined model loaded dynamically at runtime.
- `tests/npu/fixtures/` - malformed and fault-injection inputs only.

Generate a combined model with retained debug metadata, for example:

```text
stedgeai generate -m yolov8_256_qdq_int8_od_coco-person.onnx \
  --target stm32n6 -st-neural-art --relocatable ecblob-in-params \
  --memory-pool tools/npu/stm32n6_bigblob.mpool
```

Package the resulting `network_rel.bin` without modifying it:

```text
python tools/npu/stm32n6_relpack.py build/network_rel.bin \
  --output-root data/npu --name network_rel.bin
```

The command emits `network_rel.bin.bigblob`, containing the 64 KiB
bigblob header area followed by the unchanged model payload. Provision that
image at XSPI offset `0x00880000`, before the scratch region at `0x03ffb000`.
The provisioning path must erase the slot, write the payload, write header
bytes `4..39`, verify the payload CRC, and program header bytes `0..3` last.
Do not raw-write the complete image in one operation: its published magic is
intentionally present at offset zero for the final write only. On-device
callers use `tiku_n6_model_store_begin()` followed by bounded
`tiku_n6_model_store_step()` calls; host programmers should perform the same
five ordered operations using the manifest offsets. Do not use `--c-array`: the
combined image is 3.2 MiB and cannot fit in the 255 KiB boot SRAM image.
The compatibility bind spelling remains `/data/npu/network_rel.bin`, but it
now resolves to the bigblob rather than a VFS file.

The packager rejects split binaries, requires EdgeAI reloc runtime 8.0 flags,
asynchronous mode, and `LL_ATON_EB_DBG_INFO`. It records the runtime version,
CRC, slot offset, and publication policy in the adjacent manifest. The
on-device adapter maps the complete payload through the bigblob slot and calls
`ll_aton_reloc_get_info()`, `ll_aton_reloc_install()` in COPY mode, and the
LL-ATON runtime lifecycle. No TikuOS layer patches the ST EC/GOT relocations
itself.

`rt_ram_copy` must fit the linker-owned NPU tier. RESET activation pools must
remain inside that tier; non-relocatable COPY pools must target writable
runtime RAM, not the mapped NOR model source. The packager rejects absolute
COPY pools targeting the XSPI window; regenerate those models with the
relocatable parameter pool. IO metadata is obtained from
LL-ATON debug descriptors after installation; models must not be generated
with `--no-dbg-info`.
