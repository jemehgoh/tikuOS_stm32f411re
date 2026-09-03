# STM32N6 fixed embedded model

ST Edge AI Core's STM32N6 Neural-ART output is not the older generic
`network_data.c` format. A generated network consists of `network.c`, its
`network.h` interface, `network_ecblobs.h`, and usually a separate
`network_atonbuf.xSPI2.raw` weight image. `network_ecblobs.h` contains aligned
static micro-instruction tables; `network.c` exposes epoch metadata and the
generated buffer map. The N6 memory-pool map is absolute, so the table contains
addresses such as NPU RAM5 (`0x342e0000`) and XSPI2 (`0x71000000`).

The generated user-IO variant is intentionally not the contract used here. It
allocates a runtime blob and calls `ec_copy_blob`/`ec_reloc` before an
inference. This step uses the compiler's allocated-IO fixed-address output:
the blob, IO buffers, and weight image are placed at build/program time and
the API validates that callers pass those exact addresses. There is no VFS
lookup, staging copy, runtime blob allocation, or relocation in the submit
path.

The checked-in [`stm32n6_identity_model.h`](fixtures/stm32n6_identity_model.h)
is the 1x1x4x4 int8 identity-convolution epoch blob generated with
`--enable-epoch-controller`. Its one-byte weight is 1, so the known-good
reference output is exactly the input. The board image process must program
the generated `network_atonbuf.xSPI2.raw` at `0x71000000`; TikuOS does not load
it.

For the process/event and timing acceptance harness, build the STM32N6 target
with `TIKU_NPU_EMBEDDED_TEST_ENABLE=1`. It runs an async submission and a sync
submission, checks that submission time is below the compiler's 16-cycle
estimate, compares all 16 output bytes, and keeps a second process active to
ensure the completion is not broadcast.

## VFS container header

The VFS form is a separate, backend-specific container. It is intended to be
stored as a file such as `/data/models/person.tn6`; `tiku_npu_model_bind()`
maps the object through the mounted data store, following the RA8P1 loader
pattern, and validates only the STM32N6 header and I/O geometry. It does not
use the shell-facing VFS path reader. `tiku_npu_model_load()` later maps the
bound object again and copies the declared container image into one NPU-tier
slice in bounded chunks. The copy is byte-for-byte: weights, parameters,
activations, ecblob, and the gaps/header bytes in the declared image are all
preserved. The loaded model's epoch pointer is then redirected to the copied
ecblob; no relocation table is walked and no payload address is patched.

Quantisation scale/multiplier data is not duplicated in this container. It
remains opaque inside the compiler-produced command stream and weight payload,
as it does for the RA8P1 port.

All integers are little-endian. The header is 56 bytes followed by one 40-byte
tensor record for every input and output, in that order:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `TIKU_NPU_MODEL_CONTAINER_MAGIC` (`TN6P`) |
| 4 | 2 | container version |
| 6 | 2 | total header bytes |
| 8 | 4 | flags, currently zero |
| 12 | 4 | weights bytes |
| 16 | 4 | parameter bytes |
| 20 | 4 | activation/workspace bytes |
| 24 | 4 | embedded ecblob bytes |
| 28 | 2 | input tensor count |
| 30 | 2 | output tensor count |
| 32 | 2 | tensor descriptor bytes |
| 34 | 2 | reserved, zero |
| 36 | 4 | total container bytes |
| 40 | 4 | weights payload offset |
| 44 | 4 | parameter payload offset |
| 48 | 4 | activation/workspace payload offset |
| 52 | 4 | ecblob payload offset |

Each tensor record contains `type:u8`, `rank:u8`, `flags:u16` (currently zero),
eight `shape:u32` entries, and `zero_point:i32`. The public
`tiku_npu_tensor_t` and `tiku_npu_model_io_t` types expose those fields without
allocation. There is no standalone floating-point scale field.

Model slots are declared with `TIKU_NPU_MODEL(name)`. The build-time
`TIKU_NPU_MODEL_MAX_*_BYTES` limits and `TIKU_NPU_MODEL_SLOT_BYTES` are checked
at bind time; the four declared regions must also fit in the aggregate slot.
Malformed/truncated headers, missing VFS paths, and over-capacity containers
return distinct `TIKU_NPU_ERR_*` values. A successful bind can therefore be
used for metadata inspection immediately, while the payload remains in `/data`
until the later loading step. `tiku_npu_model_load()` returns
`TIKU_NPU_ERR_BUSY` for an occupied NPU slot; call
`tiku_npu_model_unload()` before loading again. The current fixed-address
backend permits one loaded model at a time, matching the single epoch
controller and preventing an unload from orphaning another model's slice.
