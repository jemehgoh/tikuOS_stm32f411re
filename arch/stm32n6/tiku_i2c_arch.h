/*
 * Tiku Operating System v0.06
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tiku_i2c_arch.h - STM32N6 polled I2C master.
 *
 * The selected board provides an I2C1 or I2C2 master bus. Operations block
 * while polling the peripheral and support 7-bit addresses.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKU_STM32N6_I2C_ARCH_H_
#define TIKU_STM32N6_I2C_ARCH_H_

#include <interfaces/bus/tiku_i2c_bus.h>

/** @brief Initialize the board's I2C master at standard or fast speed. */
int  tiku_i2c_arch_init(const tiku_i2c_config_t *config);

/** @brief Disable the I2C peripheral and release its pins to input mode. */
void tiku_i2c_arch_close(void);

/** @brief Write a non-empty buffer to a 7-bit target address. */
int  tiku_i2c_arch_write(uint8_t addr, const uint8_t *buf, uint16_t len);

/** @brief Read a non-empty buffer from a 7-bit target address. */
int  tiku_i2c_arch_read (uint8_t addr, uint8_t *buf, uint16_t len);

/** @brief Probe a 7-bit address with an address-only transaction. */
int  tiku_i2c_arch_probe(uint8_t addr);

/**
 * @brief Write then read using a repeated START and one final STOP.
 */
int  tiku_i2c_arch_write_read(uint8_t addr,
                              const uint8_t *tx_buf, uint16_t tx_len,
                              uint8_t *rx_buf,       uint16_t rx_len);

#endif /* TIKU_STM32N6_I2C_ARCH_H_ */
