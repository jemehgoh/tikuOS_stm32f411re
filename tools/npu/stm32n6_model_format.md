# STM32N6 runtime-loadable models

TikuOS accepts only ST EdgeAI relocatable `network_rel.bin` files on STM32N6.
The old TN6P envelope, manual EC relocation, and direct epoch-blob submission
are removed. The runtime ABI is the pinned ST EdgeAI 4.0.0 LL-ATON build in
`third_party/st/edgeai/4.0.0/`.

Storage is intentionally separated:

- `drivers/stm32n6/npu/models/` — optional compiler-generated embedded fixtures.
- `/data/npu/` — combined VFS files loaded dynamically at runtime.
- `tests/npu/fixtures/` — malformed and fault-injection inputs only.

Generate a combined model with retained debug metadata, for example:

```text
stedgeai generate -m yolov8_256_qdq_int8_od_coco-person.onnx \
  --target stm32n6 -st-neural-art --relocatable ecblob-in-params
```

Package the resulting `network_rel.bin` without modifying it:

```text
python tools/npu/stm32n6_relpack.py build/network_rel.bin \
  --output-root data/npu --name network_rel.bin
```

For the regenerated YOLO model, provision `data/npu/network_rel.bin` into the
external-NOR TFS before running the STM32N6 acceptance tests. Do not use
`--c-array`: the combined image is 3.2 MiB and cannot fit in the 255 KiB boot
SRAM image. The tests bind the preloaded path `/data/npu/network_rel.bin`.

The packager rejects split binaries, requires EdgeAI reloc runtime 8.0 flags,
asynchronous mode, and `LL_ATON_EB_DBG_INFO`. It records the runtime version in
the adjacent manifest. The on-device adapter maps the complete file through
the VFS and calls `ll_aton_reloc_get_info()`, `ll_aton_reloc_install()` in COPY
mode, and the LL-ATON runtime lifecycle. No TikuOS layer patches the ST EC/GOT
relocations itself.

`rt_ram_copy` must fit the linker-owned NPU tier. RESET activation pools must
remain inside that tier; COPY parameter pools may target the STM32N6 mapped
NOR window. IO metadata is obtained from LL-ATON debug descriptors after
installation; models must not be generated with `--no-dbg-info`.
