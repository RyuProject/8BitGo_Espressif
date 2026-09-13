/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**************************************************************************************************
 * BSP configuration
 **************************************************************************************************/

#if !defined(BSP_CONFIG_NO_GRAPHIC_LIB) // Check if the symbol is not coming from compiler definitions (-D...)
#define BSP_CONFIG_NO_GRAPHIC_LIB (0)
#endif
