/*
 * Tiku Operating System — STM32F411RE (Nucleo-F411RE) linker script
 *
 * Memory map (STM32F411RE Datasheet §5):
 *   FLASH : 512 KB internal at 0x08000000
 *   SRAM  : 128 KB at 0x20000000
 *
 * Layout in flash:
 *   0x08000000  .vectors   (M4 vector table; SP and reset handler)
 *   0x08000xxx  .text      (kernel + app code)
 *   0x08xxxxx   .rodata    (read-only data)
 *   [LMA]       .data      (load address; copied to SRAM at boot)
 *   0x08060000  Sector 7 reserved for persistence write-back slots
 *
 * Layout in SRAM:
 *   .data       (initialised variables, copied from flash at boot)
 *   .bss        (zeroed at boot)
 *   .mpu_diag   (NOLOAD, 32-byte aligned — fault state across warm reset)
 *   .uninit     (NOLOAD, 32-byte aligned — persistent warm-reset state)
 *   __stack     (top of SRAM, grows downward)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
    FLASH         (rx)  : ORIGIN = 0x08000000, LENGTH = 384K
    PERSIST_FLASH (rx)  : ORIGIN = 0x08060000, LENGTH = 128K
    SRAM          (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

__sram_start  = ORIGIN(SRAM);
__sram_end    = ORIGIN(SRAM) + LENGTH(SRAM);
__flash_start = ORIGIN(FLASH);
__flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);
__flash_device_start = ORIGIN(FLASH);
__flash_device_end   = ORIGIN(PERSIST_FLASH) + LENGTH(PERSIST_FLASH);

/* The Cortex-M loads the initial SP from the first vector entry.
 * Place it at the very top of SRAM and let the stack grow downward. */
__stack = __sram_end;
_estack = __stack;

/*
 * Sector 7 is reserved for the STM32 flash-backed persistence store.
 * Export it through the generic __tiku_nvm_flash_* linker symbol
 * convention used by the shared memory code.
 */
__tiku_nvm_flash_sector = 7;
__tiku_nvm_flash_start  = ORIGIN(PERSIST_FLASH);
__tiku_nvm_flash_size   = LENGTH(PERSIST_FLASH);
__tiku_nvm_flash_end    = ORIGIN(PERSIST_FLASH) + LENGTH(PERSIST_FLASH);
__tiku_nvm_flash_offset = ORIGIN(PERSIST_FLASH) - ORIGIN(FLASH);

SECTIONS
{
    /* -------- 1. Cortex-M4 vector table ------------------------------ */
    .vectors ORIGIN(FLASH) :
    {
        __vectors_start = .;
        KEEP(*(.isr_vector))
        KEEP(*(.vectors))
        __vectors_end = .;
    } > FLASH

    /* -------- 2. Code & read-only data ------------------------------- */
    .text : ALIGN(4)
    {
        *(.text*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > FLASH

    .rodata : ALIGN(4)
    {
        *(.rodata*)
        *(.gnu.linkonce.r.*)
    } > FLASH

    .preinit_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        KEEP(*(.preinit_array.*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array : ALIGN(4)
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

    /* -------- 3. Initialised data ------------------------------------ */
    __data_load = LOADADDR(.data);
    _sidata = __data_load;
    .data : ALIGN(4)
    {
        __data_start = .;
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
        _edata = .;
    } > SRAM AT > FLASH

    /* -------- 4. Zero-initialised BSS -------------------------------- */
    .bss : ALIGN(4)
    {
        __bss_start = .;
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
        _ebss = .;
    } > SRAM

    /* -------- 5. MPU diagnostic state (NOLOAD) ----------------------- */
    .mpu_diag (NOLOAD) : ALIGN(32)
    {
        __mpu_diag_start = .;
        *(.mpu_diag*)
        . = ALIGN(32);
        __mpu_diag_end = .;
    } > SRAM

    /* -------- 6. Persistent warm-reset state (NOLOAD) ---------------- */
    .uninit (NOLOAD) : ALIGN(32)
    {
        __uninit_start = .;
        *(.uninit*)
        *(.persistent*)
        . = ALIGN(32);
        __uninit_end = .;
    } > SRAM

    /* Heap start aliases */
    _end       = .;
    end        = .;
    __end__    = .;
    __datastart = __data_start;

    /* Sanity checks */
    ASSERT((__stack - _end) >= 0x1000,
           "ERROR: SRAM exhausted — less than 4 KB free for stack")
    ASSERT(__data_load < ORIGIN(PERSIST_FLASH),
           "ERROR: flash image spills into reserved persistence sector")

    /DISCARD/ :
    {
        *(.note*)
        *(.comment)
        *(.ARM.attributes)
    }
}
