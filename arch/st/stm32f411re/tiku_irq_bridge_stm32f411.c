/*
 * Tiku Operating System v0.04
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_irq_bridge_stm32f411.c - CMSIS/ST IRQ name bridge for STM32F411RE
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stm32f411xe.h>

static void stm32f411_bridge_default_handler(void)
{
    for (;;) {
        __WFE();
    }
}

#define TIKU_STM32_BRIDGE(st_name, tiku_name) \
    void tiku_name(void) __attribute__((weak)); \
    void st_name(void) \
    { \
        if (tiku_name != (void (*)(void))0) { \
            tiku_name(); \
        } else { \
            stm32f411_bridge_default_handler(); \
        } \
    }

TIKU_STM32_BRIDGE(NMI_Handler, tiku_stm32f411_nmi_handler)
TIKU_STM32_BRIDGE(HardFault_Handler, tiku_stm32f411_hard_fault_handler)
TIKU_STM32_BRIDGE(MemManage_Handler, tiku_stm32f411_mem_manage_handler)
TIKU_STM32_BRIDGE(BusFault_Handler, tiku_stm32f411_bus_fault_handler)
TIKU_STM32_BRIDGE(UsageFault_Handler, tiku_stm32f411_usage_fault_handler)
TIKU_STM32_BRIDGE(SVC_Handler, tiku_stm32f411_svcall_handler)
TIKU_STM32_BRIDGE(DebugMon_Handler, tiku_stm32f411_debug_mon_handler)

/* PendSV cannot use the normal C bridge: a thread switch returns with an
 * EXC_RETURN value, not to a C caller.  Tail-branch to the port hook so LR
 * remains the exception-return token and no MSP bridge frame is left behind. */
__attribute__((weak, naked))
void tiku_stm32f411_pendsv_handler(void)
{
    __asm__ volatile (
        "1:                                     \n"
        "wfe                                    \n"
        "b      1b                              \n");
}

__attribute__((naked))
void PendSV_Handler(void)
{
    __asm__ volatile (
        ".syntax unified                         \n"
        "ldr    r3, =tiku_stm32f411_pendsv_handler\n"
        "bx     r3                               \n"
        );
}

TIKU_STM32_BRIDGE(SysTick_Handler, tiku_stm32f411_systick_handler)

TIKU_STM32_BRIDGE(WWDG_IRQHandler, tiku_stm32f411_wwdg_irq_handler)
TIKU_STM32_BRIDGE(PVD_IRQHandler, tiku_stm32f411_exti16_pvd_irq_handler)
TIKU_STM32_BRIDGE(TAMP_STAMP_IRQHandler, tiku_stm32f411_exti21_tamp_stamp_irq_handler)
TIKU_STM32_BRIDGE(RTC_WKUP_IRQHandler, tiku_stm32f411_exti22_rtc_wkup_irq_handler)
TIKU_STM32_BRIDGE(FLASH_IRQHandler, tiku_stm32f411_flash_irq_handler)
TIKU_STM32_BRIDGE(RCC_IRQHandler, tiku_stm32f411_rcc_irq_handler)
TIKU_STM32_BRIDGE(EXTI0_IRQHandler, tiku_stm32f411_exti0_irq_handler)
TIKU_STM32_BRIDGE(EXTI1_IRQHandler, tiku_stm32f411_exti1_irq_handler)
TIKU_STM32_BRIDGE(EXTI2_IRQHandler, tiku_stm32f411_exti2_irq_handler)
TIKU_STM32_BRIDGE(EXTI3_IRQHandler, tiku_stm32f411_exti3_irq_handler)
TIKU_STM32_BRIDGE(EXTI4_IRQHandler, tiku_stm32f411_exti4_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream0_IRQHandler, tiku_stm32f411_dma1_stream0_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream1_IRQHandler, tiku_stm32f411_dma1_stream1_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream2_IRQHandler, tiku_stm32f411_dma1_stream2_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream3_IRQHandler, tiku_stm32f411_dma1_stream3_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream4_IRQHandler, tiku_stm32f411_dma1_stream4_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream5_IRQHandler, tiku_stm32f411_dma1_stream5_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream6_IRQHandler, tiku_stm32f411_dma1_stream6_irq_handler)
TIKU_STM32_BRIDGE(ADC_IRQHandler, tiku_stm32f411_adc_irq_handler)
TIKU_STM32_BRIDGE(EXTI9_5_IRQHandler, tiku_stm32f411_exti9_5_irq_handler)
TIKU_STM32_BRIDGE(TIM1_BRK_TIM9_IRQHandler, tiku_stm32f411_tim1_brk_tim9_irq_handler)
TIKU_STM32_BRIDGE(TIM1_UP_TIM10_IRQHandler, tiku_stm32f411_tim1_up_tim10_irq_handler)
TIKU_STM32_BRIDGE(TIM1_TRG_COM_TIM11_IRQHandler, tiku_stm32f411_tim1_trg_com_tim11_irq_handler)
TIKU_STM32_BRIDGE(TIM1_CC_IRQHandler, tiku_stm32f411_tim1_cc_irq_handler)
TIKU_STM32_BRIDGE(TIM2_IRQHandler, tiku_stm32f411_tim2_irq_handler)
TIKU_STM32_BRIDGE(TIM3_IRQHandler, tiku_stm32f411_tim3_irq_handler)
TIKU_STM32_BRIDGE(TIM4_IRQHandler, tiku_stm32f411_tim4_irq_handler)
TIKU_STM32_BRIDGE(I2C1_EV_IRQHandler, tiku_stm32f411_i2c1_ev_irq_handler)
TIKU_STM32_BRIDGE(I2C1_ER_IRQHandler, tiku_stm32f411_i2c1_er_irq_handler)
TIKU_STM32_BRIDGE(I2C2_EV_IRQHandler, tiku_stm32f411_i2c2_ev_irq_handler)
TIKU_STM32_BRIDGE(I2C2_ER_IRQHandler, tiku_stm32f411_i2c2_er_irq_handler)
TIKU_STM32_BRIDGE(SPI1_IRQHandler, tiku_stm32f411_spi1_irq_handler)
TIKU_STM32_BRIDGE(SPI2_IRQHandler, tiku_stm32f411_spi2_irq_handler)
TIKU_STM32_BRIDGE(USART1_IRQHandler, tiku_stm32f411_usart1_irq_handler)
TIKU_STM32_BRIDGE(USART2_IRQHandler, tiku_stm32f411_usart2_irq_handler)
TIKU_STM32_BRIDGE(EXTI15_10_IRQHandler, tiku_stm32f411_exti15_10_irq_handler)
TIKU_STM32_BRIDGE(RTC_Alarm_IRQHandler, tiku_stm32f411_exti17_rtc_alarm_irq_handler)
TIKU_STM32_BRIDGE(OTG_FS_WKUP_IRQHandler, tiku_stm32f411_exti18_otg_fs_wkup_irq_handler)
TIKU_STM32_BRIDGE(DMA1_Stream7_IRQHandler, tiku_stm32f411_dma1_stream7_irq_handler)
TIKU_STM32_BRIDGE(SDIO_IRQHandler, tiku_stm32f411_sdio_irq_handler)
TIKU_STM32_BRIDGE(TIM5_IRQHandler, tiku_stm32f411_tim5_irq_handler)
TIKU_STM32_BRIDGE(SPI3_IRQHandler, tiku_stm32f411_spi3_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream0_IRQHandler, tiku_stm32f411_dma2_stream0_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream1_IRQHandler, tiku_stm32f411_dma2_stream1_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream2_IRQHandler, tiku_stm32f411_dma2_stream2_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream3_IRQHandler, tiku_stm32f411_dma2_stream3_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream4_IRQHandler, tiku_stm32f411_dma2_stream4_irq_handler)
TIKU_STM32_BRIDGE(OTG_FS_IRQHandler, tiku_stm32f411_otg_fs_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream5_IRQHandler, tiku_stm32f411_dma2_stream5_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream6_IRQHandler, tiku_stm32f411_dma2_stream6_irq_handler)
TIKU_STM32_BRIDGE(DMA2_Stream7_IRQHandler, tiku_stm32f411_dma2_stream7_irq_handler)
TIKU_STM32_BRIDGE(USART6_IRQHandler, tiku_stm32f411_usart6_irq_handler)
TIKU_STM32_BRIDGE(I2C3_EV_IRQHandler, tiku_stm32f411_i2c3_ev_irq_handler)
TIKU_STM32_BRIDGE(I2C3_ER_IRQHandler, tiku_stm32f411_i2c3_er_irq_handler)
TIKU_STM32_BRIDGE(FPU_IRQHandler, tiku_stm32f411_fpu_irq_handler)
TIKU_STM32_BRIDGE(SPI4_IRQHandler, tiku_stm32f411_spi4_irq_handler)
TIKU_STM32_BRIDGE(SPI5_IRQHandler, tiku_stm32f411_spi5_irq_handler)
