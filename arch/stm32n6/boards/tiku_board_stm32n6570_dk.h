/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Jeremy Goh
 *
 * tiku_board_stm32n6570_dk.h - STM32N6570-DK board wiring.
 *
 * Pin assignments follow ST's STM32N6570-DK user manual (UM3300).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_BOARD_STM32N6570_DK_H_
#define TIKU_BOARD_STM32N6570_DK_H_

#include <arch/stm32n6/tiku_gpio_arch.h>
#include <arch/stm32n6/tiku_stm32n6_regs.h>

#define TIKU_BOARD_NAME             "STM32N6570-DK"

/* LD1 is active high on PO1; LD2 is active low on PG10. */
#define TIKU_BOARD_LED_COUNT        3

#define TIKU_BOARD_LED1_PORT        STM32N6_GPIO_PORT_O
#define TIKU_BOARD_LED1_PIN         1U
#define TIKU_BOARD_LED1_INIT()      do { \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_LED1_PORT, TIKU_BOARD_LED1_PIN); \
    TIKU_BOARD_LED1_OFF(); \
} while (0)
#define TIKU_BOARD_LED1_ON()        tiku_stm32n6_gpio_set(TIKU_BOARD_LED1_PORT, TIKU_BOARD_LED1_PIN, 1U)
#define TIKU_BOARD_LED1_OFF()       tiku_stm32n6_gpio_set(TIKU_BOARD_LED1_PORT, TIKU_BOARD_LED1_PIN, 0U)
#define TIKU_BOARD_LED1_TOGGLE()    tiku_stm32n6_gpio_toggle(TIKU_BOARD_LED1_PORT, TIKU_BOARD_LED1_PIN)

#define TIKU_BOARD_LED2_PORT        STM32N6_GPIO_PORT_G
#define TIKU_BOARD_LED2_PIN         10U
#define TIKU_BOARD_LED2_INIT()      do { \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_LED2_PORT, TIKU_BOARD_LED2_PIN); \
    TIKU_BOARD_LED2_OFF(); \
} while (0)
#define TIKU_BOARD_LED2_ON()        tiku_stm32n6_gpio_set(TIKU_BOARD_LED2_PORT, TIKU_BOARD_LED2_PIN, 0U)
#define TIKU_BOARD_LED2_OFF()       tiku_stm32n6_gpio_set(TIKU_BOARD_LED2_PORT, TIKU_BOARD_LED2_PIN, 1U)
#define TIKU_BOARD_LED2_TOGGLE()    tiku_stm32n6_gpio_toggle(TIKU_BOARD_LED2_PORT, TIKU_BOARD_LED2_PIN)

#define TIKU_BOARD_LED3_PORT        STM32N6_GPIO_PORT_E
#define TIKU_BOARD_LED3_PIN         15U
#define TIKU_BOARD_LED3_INIT()      do { \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_LED3_PORT, TIKU_BOARD_LED3_PIN); \
    TIKU_BOARD_LED3_OFF(); \
} while (0)
#define TIKU_BOARD_LED3_ON()        tiku_stm32n6_gpio_set(TIKU_BOARD_LED3_PORT, TIKU_BOARD_LED3_PIN, 1U)
#define TIKU_BOARD_LED3_OFF()       tiku_stm32n6_gpio_set(TIKU_BOARD_LED3_PORT, TIKU_BOARD_LED3_PIN, 0U)
#define TIKU_BOARD_LED3_TOGGLE()    tiku_stm32n6_gpio_toggle(TIKU_BOARD_LED3_PORT, TIKU_BOARD_LED3_PIN)

/* The ST-LINK VCP is USART1 on PE5/PE6, alternate function 7. */
#define TIKU_BOARD_UART_PORT        STM32N6_GPIO_PORT_E
#define TIKU_BOARD_UART_TX_PIN      5U
#define TIKU_BOARD_UART_RX_PIN      6U
#define TIKU_BOARD_UART_AF          7U
#ifndef TIKU_BOARD_UART_BAUD
#define TIKU_BOARD_UART_BAUD        115200U
#endif
#define TIKU_BOARD_UART_PINS_INIT() do { } while (0)

/* B2 USER1 is PC13 and B4 TAMP is PE0; both assert high when pressed. */
#define TIKU_BOARD_BTN1_PORT        STM32N6_GPIO_PORT_C
#define TIKU_BOARD_BTN1_PIN         13U
#define TIKU_BOARD_BTN1_INIT()      (void)tiku_gpio_arch_set_input(TIKU_BOARD_BTN1_PORT, TIKU_BOARD_BTN1_PIN)
#define TIKU_BOARD_BTN1_PRESSED()   (tiku_gpio_arch_read(TIKU_BOARD_BTN1_PORT, TIKU_BOARD_BTN1_PIN) == 1)

#define TIKU_BOARD_BTN2_PORT        STM32N6_GPIO_PORT_E
#define TIKU_BOARD_BTN2_PIN         0U
#define TIKU_BOARD_BTN2_INIT()      (void)tiku_gpio_arch_set_input(TIKU_BOARD_BTN2_PORT, TIKU_BOARD_BTN2_PIN)
#define TIKU_BOARD_BTN2_PRESSED()   (tiku_gpio_arch_read(TIKU_BOARD_BTN2_PORT, TIKU_BOARD_BTN2_PIN) == 1)

/* EXT_SMPS_MODE is driven from PF4; high selects the asserted SMPS mode. */
#define TIKU_BOARD_SMPS_PORT        STM32N6_GPIO_PORT_F
#define TIKU_BOARD_SMPS_PIN         4U
#define TIKU_BOARD_SMPS_ON_LEVEL    1U

/* LCD control signals on CN3: ON/OFF (PQ3), backlight (PQ6), and NRST (PE1). */
#define TIKU_BOARD_LCD_ENABLE_PORT  STM32N6_GPIO_PORT_Q
#define TIKU_BOARD_LCD_ENABLE_PIN   3U
#define TIKU_BOARD_LCD_ENABLE_LEVEL 1U
#define TIKU_BOARD_LCD_DISABLE_LEVEL 0U
#define TIKU_BOARD_LCD_ENABLE()     tiku_stm32n6_gpio_set( \
    TIKU_BOARD_LCD_ENABLE_PORT, TIKU_BOARD_LCD_ENABLE_PIN, \
    TIKU_BOARD_LCD_ENABLE_LEVEL)
#define TIKU_BOARD_LCD_DISABLE()    tiku_stm32n6_gpio_set( \
    TIKU_BOARD_LCD_ENABLE_PORT, TIKU_BOARD_LCD_ENABLE_PIN, \
    TIKU_BOARD_LCD_DISABLE_LEVEL)

#define TIKU_BOARD_LCD_BACKLIGHT_PORT STM32N6_GPIO_PORT_Q
#define TIKU_BOARD_LCD_BACKLIGHT_PIN  6U
#define TIKU_BOARD_LCD_BACKLIGHT_LEVEL 1U
#define TIKU_BOARD_LCD_BACKLIGHT_OFF_LEVEL 0U
#define TIKU_BOARD_LCD_BACKLIGHT_ON() tiku_stm32n6_gpio_set( \
    TIKU_BOARD_LCD_BACKLIGHT_PORT, TIKU_BOARD_LCD_BACKLIGHT_PIN, \
    TIKU_BOARD_LCD_BACKLIGHT_LEVEL)
#define TIKU_BOARD_LCD_BACKLIGHT_OFF() tiku_stm32n6_gpio_set( \
    TIKU_BOARD_LCD_BACKLIGHT_PORT, TIKU_BOARD_LCD_BACKLIGHT_PIN, \
    TIKU_BOARD_LCD_BACKLIGHT_OFF_LEVEL)

#define TIKU_BOARD_LCD_RESET_PORT   STM32N6_GPIO_PORT_E
#define TIKU_BOARD_LCD_RESET_PIN    1U
#define TIKU_BOARD_LCD_RESET_ASSERT_LEVEL 0U
#define TIKU_BOARD_LCD_RESET_RELEASE_LEVEL 1U
#define TIKU_BOARD_LCD_RESET_ASSERT() tiku_stm32n6_gpio_set( \
    TIKU_BOARD_LCD_RESET_PORT, TIKU_BOARD_LCD_RESET_PIN, \
    TIKU_BOARD_LCD_RESET_ASSERT_LEVEL)
#define TIKU_BOARD_LCD_RESET_RELEASE() tiku_stm32n6_gpio_set( \
    TIKU_BOARD_LCD_RESET_PORT, TIKU_BOARD_LCD_RESET_PIN, \
    TIKU_BOARD_LCD_RESET_RELEASE_LEVEL)
#define TIKU_BOARD_LCD_INIT()       do { \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_LCD_ENABLE_PORT, \
                                  TIKU_BOARD_LCD_ENABLE_PIN); \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_LCD_BACKLIGHT_PORT, \
                                  TIKU_BOARD_LCD_BACKLIGHT_PIN); \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_LCD_RESET_PORT, \
                                  TIKU_BOARD_LCD_RESET_PIN); \
    TIKU_BOARD_LCD_DISABLE(); \
    TIKU_BOARD_LCD_BACKLIGHT_OFF(); \
    TIKU_BOARD_LCD_RESET_ASSERT(); \
} while (0)

/* Camera module controls on CN14: NRST_CAM (PC8), EN_MODULE (PD2). */
#define TIKU_BOARD_CAMERA_RESET_PORT STM32N6_GPIO_PORT_C
#define TIKU_BOARD_CAMERA_RESET_PIN  8U
#define TIKU_BOARD_CAMERA_RESET_ASSERT_LEVEL 0U
#define TIKU_BOARD_CAMERA_RESET_RELEASE_LEVEL 1U
#define TIKU_BOARD_CAMERA_ENABLE_PORT STM32N6_GPIO_PORT_D
#define TIKU_BOARD_CAMERA_ENABLE_PIN 2U
#define TIKU_BOARD_CAMERA_ENABLE_LEVEL 1U
#define TIKU_BOARD_CAMERA_DISABLE_LEVEL 0U
#define TIKU_BOARD_CAMERA_RESET_ASSERT() tiku_stm32n6_gpio_set( \
    TIKU_BOARD_CAMERA_RESET_PORT, TIKU_BOARD_CAMERA_RESET_PIN, \
    TIKU_BOARD_CAMERA_RESET_ASSERT_LEVEL)
#define TIKU_BOARD_CAMERA_RESET_RELEASE() tiku_stm32n6_gpio_set( \
    TIKU_BOARD_CAMERA_RESET_PORT, TIKU_BOARD_CAMERA_RESET_PIN, \
    TIKU_BOARD_CAMERA_RESET_RELEASE_LEVEL)
#define TIKU_BOARD_CAMERA_ENABLE()   tiku_stm32n6_gpio_set( \
    TIKU_BOARD_CAMERA_ENABLE_PORT, TIKU_BOARD_CAMERA_ENABLE_PIN, \
    TIKU_BOARD_CAMERA_ENABLE_LEVEL)
#define TIKU_BOARD_CAMERA_DISABLE()  tiku_stm32n6_gpio_set( \
    TIKU_BOARD_CAMERA_ENABLE_PORT, TIKU_BOARD_CAMERA_ENABLE_PIN, \
    TIKU_BOARD_CAMERA_DISABLE_LEVEL)
#define TIKU_BOARD_CAMERA_INIT()     do { \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_CAMERA_RESET_PORT, \
                                  TIKU_BOARD_CAMERA_RESET_PIN); \
    tiku_stm32n6_gpio_init_output(TIKU_BOARD_CAMERA_ENABLE_PORT, \
                                  TIKU_BOARD_CAMERA_ENABLE_PIN); \
    TIKU_BOARD_CAMERA_DISABLE(); \
    TIKU_BOARD_CAMERA_RESET_ASSERT(); \
} while (0)

/* Backscatter, ADC, one-wire and SPI have no arch backend on this port yet. */
#define TIKU_BOARD_BSCAT_PORT       0U
#define TIKU_BOARD_BSCAT_PIN        0U
#define TIKU_BOARD_ADC_AVAILABLE    0
#define TIKU_BOARD_I2C_BRW_100K     1
#define TIKU_BOARD_OW_AVAILABLE     0
#define TIKU_BOARD_OW_PIN           0U
/* The user headers route I2C1 to D14 (PC1 SDA) and D15 (PH9 SCL), AF4. */
#define TIKU_BOARD_I2C_INSTANCE     1U
#define TIKU_BOARD_I2C_SDA_PORT     STM32N6_GPIO_PORT_C
#define TIKU_BOARD_I2C_SDA_PIN      1U
#define TIKU_BOARD_I2C_SCL_PORT     STM32N6_GPIO_PORT_H
#define TIKU_BOARD_I2C_SCL_PIN      9U
#define TIKU_BOARD_I2C_AF           4U
#define TIKU_BOARD_SPI0_MISO_PIN    0U
#define TIKU_BOARD_SPI0_SCK_PIN     0U
#define TIKU_BOARD_SPI0_MOSI_PIN    0U

#endif /* TIKU_BOARD_STM32N6570_DK_H_ */
