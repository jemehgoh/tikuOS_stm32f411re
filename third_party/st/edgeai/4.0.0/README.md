# Pinned ST LL-ATON runtime

This directory is the TikuOS vendor snapshot used by the STM32N6 NPU wrapper.
It was copied from the installed ST EdgeAI 4.0.0 distribution (`4.0.1-20581`,
ST EdgeAI commit `7ed50de05`) and is intentionally kept separate from generated
model output.

The runtime is built for Cortex-M55 with relocatable, asynchronous LL-ATON
support and links `NetworkRuntime1201_CM55_GCC_PIC.a`. The corresponding ST
license is retained in `LICENSE.txt`; the vendor files must be replaced as one
versioned unit if the runtime is upgraded.

TikuOS-specific platform and OSAL glue lives in `arch/stm32n6/`, not in the
vendor source tree. Generated `network_rel.bin` files are packaged into
`/data/npu/`; test-only malformed files live under `tests/npu/fixtures/`.
