
/*
 * Copyright (C) 2014 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser General
 * Public License v2.1. See the file LICENSE in the top level directory for more
 * details.
 */

/**
 * @defgroup    boards_stm32l486vg-pinboard STM32L4 PinBoard
 * @ingroup     boards
 * @brief       Support for the stm32l486vg-pinboard board
 * @{
 *
 * @file
 * @brief       Board specific definitions for the stm32l486vg-pinboard
 *
 * @author      John Kurbanov <kurbanovd.s@gmail.com>
 */

#ifndef BOARD_H
#define BOARD_H

#include "cpu.h"
#include "periph_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name xtimer configuration
 * @{
 */
#define XTIMER_OVERHEAD     (6)
#define XTIMER_BACKOFF      (10)
/** @} */

#define BTN0_PIN                    GPIO_PIN(0, 17)
#define BTN0_MODE                   GPIO_IN
#define BTN1_PIN                    GPIO_PIN(0, 26)
#define BTN1_MODE GPIO_IN

void board_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
/** @} */