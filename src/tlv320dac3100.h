/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TLV320DAC3100_H
#define TLV320DAC3100_H

#include <stdbool.h>

/*
 * TLV320DAC3100 audio codec init (Adafruit Fruit Jam).
 *
 * Releases PERIPH_RESET and programs the codec over bit-banged I2C so it
 * decodes the 44.1 kHz 16-bit I2S stream from PIO1, deriving all internal
 * clocks from BCLK (no MCLK needed). Routes audio to both the headphone
 * jack and the speaker output.
 *
 * On boards without this codec (FRANK M2) it compiles to a no-op.
 *
 * Returns true if the codec acknowledged and was configured.
 */
bool tlv320dac3100_init(void);

/* True once init completed with the codec ACKing (always true on M2). */
bool tlv320dac3100_ok(void);

#endif /* TLV320DAC3100_H */
