/*
 * Tiny ST Neural-ART fixture for the STM32N6 embedded-model API.
 *
 * Generated with ST Edge AI Core 4.0.1 from a 1x1x4x4 int8 QLinearConv whose
 * sole weight is 1, with --target stm32n6 --st-neural-art and
 * --enable-epoch-controller. This is the fixed-address output: it is the
 * network_ecblobs.h static const table, not the user-IO form that calls
 * ec_copy_blob()/ec_reloc().
 *
 * The compiler's fixed memory map is:
 *   input  0x342e0000 (16 bytes)
 *   output 0x342e0010 (16 bytes)
 *   weight 0x71000000 (1 byte, network_atonbuf.xSPI2.raw)
 *
 * The external weight byte is programmed at the fixed XSPI2 address by the
 * board image process. It is not loaded by TikuOS and has no VFS binding.
 * The reference output is byte-for-byte equal to the input.
 */

#ifndef TIKU_STM32N6_IDENTITY_MODEL_H_
#define TIKU_STM32N6_IDENTITY_MODEL_H_

#include <stdint.h>

#include <interfaces/npu/tiku_npu.h>

#define TIKU_NPU_FIXTURE_INPUT_ADDRESS  ((uintptr_t)0x342e0000UL)
#define TIKU_NPU_FIXTURE_OUTPUT_ADDRESS ((uintptr_t)0x342e0010UL)
#define TIKU_NPU_FIXTURE_INPUT_BYTES    16U
#define TIKU_NPU_FIXTURE_OUTPUT_BYTES   16U
#define TIKU_NPU_FIXTURE_WEIGHT_ADDRESS ((uintptr_t)0x71000000UL)
#define TIKU_NPU_FIXTURE_ESTIMATED_CYCLES 16U

/* network_ecblobs.h, _ec_blob_network_1, generated fixed-address form. */
static const uint64_t tiku_npu_fixture_epoch_blob[96]
    __attribute__((aligned(64), used)) = {
    0x000000baca057a7aULL, 0x008021410000003dULL,
    0x000000343c007c00ULL, 0x010101013c0200e3ULL,
    0x1063400000001100ULL, 0x0003000000040004ULL,
    0x0000000100030000ULL, 0x000000003c0a7c00ULL,
    0x000000003c0d7c00ULL, 0x5c00004200802241ULL,
    0x0500006008000000ULL, 0x000000005c027c00ULL,
    0x000000015c070043ULL, 0x5c0c004300000000ULL,
    0x2042204200000000ULL, 0x18007c0000802021ULL,
    0x1802004300080104ULL, 0x00000010342e0000ULL,
    0x00000000180500c3ULL, 0x0000000100000010ULL,
    0x0000002400000000ULL, 0x180c004200100000ULL,
    0x0000000700000006ULL, 0x00000001180d0043ULL,
    0x18110043342e004fULL, 0x0000000000000000ULL,
    0x2c007c00008020c1ULL, 0x2c02004300080184ULL,
    0x0000000171000000ULL, 0x000000002c0500c3ULL,
    0x0000000000000000ULL, 0x0000002400000000ULL,
    0x2c0c004200100000ULL, 0x0000000700000006ULL,
    0x000000012c0d0043ULL, 0x2c11004371000047ULL,
    0x0000000800000000ULL, 0x34007c0000802101ULL,
    0x340200430008010cULL, 0x00000010342e0010ULL,
    0x00000000340500c3ULL, 0x0000000000000010ULL,
    0x0000002400000000ULL, 0x340c004200100000ULL,
    0x0000000700000006ULL, 0x00000001340d0043ULL,
    0x34110043342e005fULL, 0x0000000000000000ULL,
    0x00000025100a7c00ULL, 0x00000003100c0043ULL,
    0x101e7c000000000dULL, 0x10007c0000000015ULL,
    0x3400200100000001ULL, 0x000000353c007c00ULL,
    0x180020015c002001ULL, 0x34007fe42c002001ULL,
    0x0000000000000064ULL, 0x0000000210007c00ULL,
    0x0000006410000424ULL, 0x10007c0000000000ULL,
    0x10007bc440000000ULL, 0x0000000000000064ULL,
    0x00000000100a7c00ULL, 0x00000000100c0043ULL,
    0x101e7c0000000000ULL, 0x34007c0000000000ULL,
    0x3400042400000002ULL, 0x0000000000000064ULL,
    0x4000000034007c00ULL, 0x0000006434007bc4ULL,
    0x0080010100000000ULL, 0x000000023c007c00ULL,
    0x000000643c000424ULL, 0x3c007c0000000000ULL,
    0x3c007bc440000000ULL, 0x0000000000000064ULL,
    0x5c007c0000800141ULL, 0x5c00042408000002ULL,
    0x0000000000000064ULL, 0x480000005c007c00ULL,
    0x000000645c007bc4ULL, 0x0080024100000000ULL,
    0x0000000218007c00ULL, 0x0000006418000424ULL,
    0x18007c0000000000ULL, 0x18007bc440000000ULL,
    0x0000000000000064ULL, 0x2c007c0000800021ULL,
    0x2c00042400000002ULL, 0x0000000000000064ULL,
    0x400000002c007c00ULL, 0x000000642c007bc4ULL,
    0x008000c100000000ULL, 0x0000000c0000005dULL
};

static const uintptr_t tiku_npu_fixture_input_addresses[] = {
    TIKU_NPU_FIXTURE_INPUT_ADDRESS
};
static const uintptr_t tiku_npu_fixture_output_addresses[] = {
    TIKU_NPU_FIXTURE_OUTPUT_ADDRESS
};
static const uint32_t tiku_npu_fixture_input_sizes[] = {
    TIKU_NPU_FIXTURE_INPUT_BYTES
};
static const uint32_t tiku_npu_fixture_output_sizes[] = {
    TIKU_NPU_FIXTURE_OUTPUT_BYTES
};

static const tiku_npu_model_t tiku_npu_fixture_model = {
    .magic = TIKU_NPU_MODEL_MAGIC,
    .abi_version = TIKU_NPU_MODEL_ABI_VERSION,
    .flags = TIKU_NPU_MODEL_F_FIXED,
    .epoch_blob = tiku_npu_fixture_epoch_blob,
    .epoch_blob_bytes = sizeof(tiku_npu_fixture_epoch_blob),
    .input_count = 1U,
    .output_count = 1U,
    .input_addresses = tiku_npu_fixture_input_addresses,
    .output_addresses = tiku_npu_fixture_output_addresses,
    .input_sizes = tiku_npu_fixture_input_sizes,
    .output_sizes = tiku_npu_fixture_output_sizes,
    .estimated_latency_cycles = TIKU_NPU_FIXTURE_ESTIMATED_CYCLES
};

#endif /* TIKU_STM32N6_IDENTITY_MODEL_H_ */
