# STM32N6 LL-ATON fixtures

These files are test inputs only. The `*_corrupt.network_rel.bin` image is a
copy of the generated identity fixture with the first ST relocation site set to
`0xffffffff`; the adapter must reject it before calling the vendor installer.

Runtime containers belong under `/data/npu/`. Build-embedded compiler output
belongs under `drivers/stm32n6/npu/models/`. Nothing in this directory is a
runtime deployment artifact.
