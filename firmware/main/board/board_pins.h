#pragma once

/*
 * EasyInput V2.0 pin constants for easyinput-beatbox.
 *
 * Source of truth: easyinput-board-cy / references/board-contract.json
 * Do not invent alternate GPIO numbers here. If these diverge from the board
 * skill contract, fix the project or update the board evidence — never both.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main keys S1–S8, active low */
#define BOARD_GPIO_S1                 2
#define BOARD_GPIO_S2                47
#define BOARD_GPIO_S3                38
#define BOARD_GPIO_S4                41
#define BOARD_GPIO_S5                 1
#define BOARD_GPIO_S6                 6
#define BOARD_GPIO_S7                 7
#define BOARD_GPIO_S8                48

/* Encoder: A/B quadrature + press (S9), press active low */
#define BOARD_GPIO_ENC_A             17
#define BOARD_GPIO_ENC_B             16
#define BOARD_GPIO_ENC_PRESS         18

#define BOARD_GPIO_KEY_WAKE          21

/* Shared peripheral rail for WS2812 / MIC / SPK, active high */
#define BOARD_GPIO_PWR_EN             8

/* 5x WS2812 on one data line */
#define BOARD_GPIO_LED_DIN           12
#define BOARD_WS2812_COUNT            5

#define BOARD_GPIO_STATUS_LED        42

/* Speaker path: MAX98357A */
#define BOARD_GPIO_SPK_BCLK          14
#define BOARD_GPIO_SPK_WS            13
#define BOARD_GPIO_SPK_DOUT          15

/* Microphone (not used in P0/P1 real-time path) */
#define BOARD_GPIO_MIC_BCLK           9
#define BOARD_GPIO_MIC_WS            10
#define BOARD_GPIO_MIC_DIN           11

/* Project power policy: settle wait after enabling GPIO8.
 * Board skill marks the hardware minimum as UNKNOWN; qualify on hardware. */
#define BOARD_PWR_SETTLE_MS          50

#ifdef __cplusplus
}
#endif
