/*
 * si5351mcu - Si5351 library for Arduino MCU tuned for size and click-less
 *
 * Copyright (C) 2017 Pavel Milanes <pavelmc@gmail.com>
 *
 * Many chunk of codes are derived-from/copied from other libs
 * all GNU GPL licenced:
 *  - Linux Kernel (www.kernel.org)
 *  - Hans Summers libs and demo code (qrp-labs.com)
 *  - Etherkit (NT7S) Si5351 libs on github
 *  - DK7IH example.
 *  - Jerry Gaffke integer routines for the bitx20 group
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <SoftI2C.h>
#include "si5351mcu.h"
#include "si5351reg.h"


// base xtal freq, over this we apply the correction factor
// by default 27 MHz
uint32_t base_xtal = SI_XTAL;

// this is the work value, with the correction applied
// via the correction() procedure
uint32_t int_xtal = SI_XTAL;

// clk# power holders (2ma by default)
uint8_t clkpower[SICHANNELS] = { 0 };

// local var to keep track of when to reset the "pll"
/*********************************************************
 * BAD CONCEPT on the datasheet and AN:
 *
 * The chip has a soft-reset for PLL A & B but in
 * practice the PLL does not need to be resetted.
 *
 * Test shows that if you fix the Msynth output
 * dividers and move any of the VCO from bottom to top
 * the frequency moves smooth and clean, no reset needed
 *
 * The reset is needed when you changes the value of the
 * Msynth output divider, even so it's not always needed
 * so we use this var to keep track of all three and only
 * reset the "PLL" when this value changes to be sure
 *
 * It's a word (16 bit) because the final max value is 900
 *********************************************************/
uint16_t omsynth[SICHANNELS] = { 0 };
uint8_t o_Rdiv[SICHANNELS] = { 0 };

// var to check the clock state
bool clkOn[SICHANNELS] = { 0 };  // This should not really be public - use isEnabled()

/*****************************************************************************
 * This is the default init procedure, it set the Si5351 with this params:
 * XTAL 27.000 Mhz
 *****************************************************************************/
void si5351mcu_default_init(void) {
  // init with the default freq
  si5351mcu_init(SI5351_CRYSTAL_LOAD_8PF, SI_XTAL);
}

/*****************************************************************************
 * This is the custom init procedure, it's used to pass a custom xtal
 * and has the duty of init the I2C protocol handshake
 *****************************************************************************/
void si5351mcu_init(uint8_t xtal_load_cap, uint32_t nxtal) {

  // set the new base xtal freq
  base_xtal = int_xtal = nxtal;

  // start I2C (wire) procedures
  I2CInit();

  // wait until initialized
  while (i2cRead(SI5351_DEVICE_STATUS) & SI5351_STATUS_SYS_INIT)
    ;

  // shut off the spread spectrum by default, DWaite contibuted code
  uint8_t regval;
  regval = i2cRead(149);
  regval &= ~0x80;  // set bit 7 LOW to turn OFF spread spectrum mode
  i2cWrite(149, regval);

  // Set crystal load capacitance (between 6pF and 10pF, 0pf for external oscillator)
  regval = (xtal_load_cap & SI5351_CRYSTAL_LOAD_MASK) | 0b00010010;
  i2cWrite(SI5351_CRYSTAL_LOAD, regval);

  // power off all the outputs
  si5351mcu_off();
}


/*****************************************************************************
 * This function set the freq of the corresponding clock.
 *
 * In my tests my Si5351 can work between 7,8 Khz and ~225 Mhz [~250 MHz with
 * overclocking] as usual YMMV
 *
 * Click noise:
 * - The lib has a reset programmed [aka: click noise] every time it needs to
 *   change the output divider of a particular MSynth, if you move in big steps
 *   this can lead to an increased rate of click noise per tuning step.
 * - If you move at a pace of a few Hz each time the output divider will
 *   change at a low rate, hence less click noise per tuning step.
 * - The output divider moves [change] faster at high frequencies, so at HF the
 *   clikc noise is at the real minimum possible.
 *
 * [See the README.md file for other details]
 ****************************************************************************/
void si5351mcu_setFreq(uint8_t clk, uint32_t freq) {
  uint8_t a, R = 1, pll_stride = 0, msyn_stride = 0;
  uint32_t b, c, f, fvco, outdivider;
  uint32_t MSx_P1, MSNx_P1, MSNx_P2, MSNx_P3;

// Overclock option
#ifdef SI_OVERCLOCK
  // user a overclock setting for the VCO, max value in my hardware
  // was 1.05 to 1.1 GHz, as usual YMMV [See README.md for details]
  outdivider = SI_OVERCLOCK / freq;
#else
  // normal VCO from the datasheet and AN
  // With 900 MHz beeing the maximum internal PLL-Frequency
  outdivider = 900000000 / freq;
#endif

  // use additional Output divider ("R")
  while (outdivider > 900) {
    R = R * 2;
    outdivider = outdivider / 2;
  }

  // finds the even divider which delivers the intended Frequency
  if (outdivider % 2) outdivider--;

  // Calculate the PLL-Frequency (given the even divider)
  fvco = outdivider * R * freq;

  // Convert the Output Divider to the bit-setting required in register 44
  switch (R) {
    case 1: R = 0; break;
    case 2: R = 16; break;
    case 4: R = 32; break;
    case 8: R = 48; break;
    case 16: R = 64; break;
    case 32: R = 80; break;
    case 64: R = 96; break;
    case 128: R = 112; break;
  }

  // we have now the integer part of the output msynth
  // the b & c is fixed below

  MSx_P1 = 128 * outdivider - 512;

  // calc the a/b/c for the PLL Msynth
  /***************************************************************************
    * We will use integer only on the b/c relation, and will >> 5 (/32) both
    * to fit it on the 1048 k limit of C and keep the relation
    * the most accurate possible, this works fine with xtals from
    * 24 to 28 Mhz.
    *
    * This will give errors of about +/- 2 Hz maximum
    * as per my test and simulations in the worst case, well below the
    * XTAl ppm error...
    *
    * This will free more than 1K of the final eeprom
    *
    ****************************************************************************/
  a = fvco / int_xtal;
  b = (fvco % int_xtal) >> 5;  // Integer part of the fraction
                               // scaled to match "c" limits
  c = int_xtal >> 5;           // "c" scaled to match it's limits
                               // in the register

  // f is (128*b)/c to mimic the Floor(128*(b/c)) from the datasheet
  f = (128 * b) / c;

  // build the registers to write
  MSNx_P1 = 128 * a + f - 512;
  MSNx_P2 = 128 * b - f * c;
  MSNx_P3 = c;

  // PLLs and CLK# registers are allocated with a stride, we handle that with
  // the stride var to make code smaller
  if (clk > 0) pll_stride = 8;

  // HEX makes it easier to human read on bit shifts
  uint8_t reg_bank_26[] = {
    (MSNx_P3 & 0xFF00) >> 8,  // Bits [15:8] of MSNx_P3 in register 26
    MSNx_P3 & 0xFF,
    (MSNx_P1 & 0x030000L) >> 16,
    (MSNx_P1 & 0xFF00) >> 8,                                       // Bits [15:8]  of MSNx_P1 in register 29
    MSNx_P1 & 0xFF,                                                // Bits [7:0]  of MSNx_P1 in register 30
    ((MSNx_P3 & 0x0F0000L) >> 12) | ((MSNx_P2 & 0x0F0000) >> 16),  // Parts of MSNx_P3 and MSNx_P1
    (MSNx_P2 & 0xFF00) >> 8,                                       // Bits [15:8]  of MSNx_P2 in register 32
    MSNx_P2 & 0xFF                                                 // Bits [7:0]  of MSNx_P2 in register 33
  };

  // We could do this here - but move it next to the reg_bank_42 write
  // i2cWriteBurst(26 + pll_stride, reg_bank_26, sizeof(reg_bank_26));

  // Write the output divider msynth only if we need to, in this way we can
  // speed up the frequency changes almost by half the time most of the time
  // and the main goal is to avoid the nasty click noise on freq change
  if (omsynth[clk] != outdivider || o_Rdiv[clk] != R) {

    // CLK# registers are exactly 8 * clk# bytes stride from a base register.
    msyn_stride = clk * 8;

    // keep track of the change
    omsynth[clk] = (uint16_t)outdivider;
    o_Rdiv[clk] = R;  // cache it now, before we OR mask up R for special divide by 4

    // See datasheet, special trick when MSx == 4
    //    MSx_P1 is always 0 if outdivider == 4, from the above equations, so there is
    //    no need to set it to 0. ... MSx_P1 = 128 * outdivider - 512;
    //
    //        See para 4.1.3 on the datasheet.
    //

    if (outdivider == 4) {
      R |= 0x0C;  // bit set OR mask for MSYNTH divide by 4, for reg 44 {3:2]
    }

    // HEX makes it easier to human read on bit shifts
    uint8_t reg_bank_42[] = {
      0,                                 // Bits [15:8] of MS0_P3 (always 0) in register 42
      1,                                 // Bits [7:0]  of MS0_P3 (always 1) in register 43
      ((MSx_P1 & 0x030000L) >> 16) | R,  // Bits [17:16] of MSx_P1 in bits [1:0] and R in [7:4] | [3:2]
      (MSx_P1 & 0xFF00) >> 8,            // Bits [15:8]  of MSx_P1 in register 45
      MSx_P1 & 0xFF,                     // Bits [7:0]  of MSx_P1 in register 46
      0,                                 // Bits [19:16] of MS0_P2 and MS0_P3 are always 0
      0,                                 // Bits [15:8]  of MS0_P2 are always 0
      0                                  // Bits [7:0]   of MS0_P2 are always 0
    };

    // Get the two write bursts as close together as possible,
    // to attempt to reduce any more click glitches.  This is
    // at the expense of only 24 increased bytes compilation size in AVR 328.
    // Everything is already precalculated above, reducing any delay,
    // by not doing calculations between the burst writes.

    i2cWriteBurst(26 + pll_stride, reg_bank_26, sizeof(reg_bank_26));
    i2cWriteBurst(42 + msyn_stride, reg_bank_42, sizeof(reg_bank_42));

    //
    // https://www.silabs.com/documents/public/application-notes/Si5350-Si5351%20FAQ.pdf
    //
    // 11.1 "The Int, R and N register values inside the Interpolative Dividers are updated
    //      when the LSB of R is written via I2C." - Q. does this mean reg 44 or 49 (misprint ?) ???
    //
    // 10.1 "All outputs are within +/- 500ps of one another at power up (if pre-programmed)
    //      or if PLLA and PLLB are reset simultaneously via register 177."
    //
    // 17.1 "The PLL can track any abrupt input frequency changes of 3–4% without losing
    //      lock to it. Any input frequency changes greater than this amount will not
    //      necessarily track from the input to the output

    // must reset the so called "PLL", in fact the output msynth
    si5351mcu_reset();

  } else {
    i2cWriteBurst(26 + pll_stride, reg_bank_26, sizeof(reg_bank_26));
  }
}


/*****************************************************************************
 * Reset of the PLLs and multisynths output enable
 *
 * This must be called to soft reset the PLLs and cycle the output of the
 * multisynths: this is the "click" noise source in the RF spectrum.
 *
 * So it must be avoided at all costs, so this lib just call it at the
 * initialization of the PLLs and when a correction is applied
 *
 * If you are concerned with accuracy you can implement a reset every
 * other Mhz to be sure it get exactly on spot.
 ****************************************************************************/
void si5351mcu_reset(void) {
  // This soft-resets PLL A & B (32 + 128) in just one step
  i2cWrite(177, 0xA0);
}


/*****************************************************************************
 * Function to disable all outputs
 *
 * The PLL are kept running, just the m-synths are powered off.
 *
 * This allows to keep the chip warm and exactly on freq the next time you
 * enable an output.
 ****************************************************************************/
void si5351mcu_off(void) {
  // This disable all the CLK outputs
  for (byte i = 0; i < SICHANNELS; i++) {
    si5351mcu_disable(i);
  }
}


/*****************************************************************************
 * Function to set the correction in Hz over the Si5351 XTAL.
 *
 * This will call a reset of the PLLs and multi-synths so it will produce a
 * click every time it's called
 ****************************************************************************/
void si5351mcu_correction(int32_t diff) {
  // apply some corrections to the xtal
  int_xtal = base_xtal + diff;

  // reset the PLLs to apply the correction
  si5351mcu_reset();
}


/*****************************************************************************
 * This function enables the selected output
 *
 * Beware: ZERO is clock output enabled, in register 16+CLK
 *****************************************************************************/
void si5351mcu_enable(uint8_t clk) {
  // var to handle the mask of the registers value
  uint8_t m = SICLK0_R;

  if (clk > 0) {
    m = SICLK12_R;
  }

  // write the register value
  i2cWrite(16 + clk, m + clkpower[clk]);

  // 1 & 2 are mutually exclusive
  if (clk == 1) si5351mcu_disable(2);
  if (clk == 2) si5351mcu_disable(1);

  // update the status of the clk
  clkOn[clk] = 1;
}


/*****************************************************************************
 * This function disables the selected output
 *
 * Beware: ONE is clock output disabled, in register 16+CLK
 * *****************************************************************************/
void si5351mcu_disable(uint8_t clk) {
  // send
  i2cWrite(16 + clk, 0x80);

  // update the status of the clk
  clkOn[clk] = 0;
}


/****************************************************************************
 * Set the power output for each output independently
 ***************************************************************************/
void si5351mcu_setPower(uint8_t clk, uint8_t power) {
  // set the power to the correct var
  clkpower[clk] = power;

  // now enable the output to get it applied
  si5351mcu_enable(clk);
}

/****************************************************************************
 * Check si5351 ready
 ***************************************************************************/
bool si5351mcu_not_ready() {  // return true if si5351 pll's have lost lock
  bool not_ready;
  int16_t status = i2cRead(SI5351_DEVICE_STATUS);
#if 0
  if (status == -1) {
    USBSerial_println("I2C"); // i2c error
  }
  if (status & SI5351_STATUS_SYS_INIT) {
    USBSerial_println("INIT"); // still initializing
  }
  if ((status & SI5351_STATUS_LOL_A) && clkOn[0]) {
    USBSerial_println("PLLA"); // loss of lock pll a
  }
  if ((status & SI5351_STATUS_LOL_B) && (clkOn[1] || clkOn[2])) {
    USBSerial_println("PLLB"); // loss of lock pll a
  }
  if (status & SI5351_STATUS_LOS_XTAL) {
    USBSerial_println("XTAL"); // crystal does not oscillate
  }
#endif
  not_ready = (status == -1)                                                 // no i2c response
              || (status & SI5351_STATUS_SYS_INIT)                           // still initializing
              || ((status & SI5351_STATUS_LOL_A) && clkOn[0])                // CLK0 uses PLLA
              || ((status & SI5351_STATUS_LOL_B) && (clkOn[1] || clkOn[2]))  // CLK1 and CLK2 use PLLB
              || (status & SI5351_STATUS_LOS_XTAL);                          // crystal not oscillating
  return not_ready;                                                          // false if si5351 ok
}


/****************************************************************************
 * method to send multi-byte burst register data.
 ***************************************************************************/
uint8_t i2cWriteBurst(const uint8_t start_register,
                      const uint8_t *data,
                      const uint8_t numbytes) {

  // This method saves the massive overhead of having to keep opening
  // and closing the I2C bus for consecutive register writes.  It
  // also saves numbytes - 1 writes for register address selection.
  // All of the bytes queued up in the above write() calls are buffered
  // up and will be sent to the slave in one "burst", on the call to
  // endTransmission().  This also sends the I2C STOP to the Slave.

  uint8_t ack_bit;
  uint8_t i;

  I2CStart();
  ack_bit = I2CSend(SIADDR << 1 | 0);
  if (ack_bit == 0) ack_bit = I2CSend(start_register);
  for (i = 0; i < numbytes; i++)
    if (ack_bit == 0) ack_bit = I2CSend(data[i]);
  I2CStop();
  // returns non zero on error
  return (ack_bit != 0);
}

/****************************************************************************
 * function to send the register data to the Si5351, arduino way.
 ***************************************************************************/
void i2cWrite(const uint8_t regist, const uint8_t value) {
  // Using the "burst" method instead of
  // doing it longhand saves a few bytes
  i2cWriteBurst(regist, &value, 1);
}

/****************************************************************************
 * Read i2C register, returns -1 on error or timeout
 ***************************************************************************/
int16_t i2cRead(const uint8_t regist) {
  int value = -1;
  uint8_t ack_bit;

  I2CStart();
  ack_bit = I2CSend(SIADDR << 1 | 0);
  if (ack_bit == 0) ack_bit = I2CSend(regist);
  I2CStop();
  if (ack_bit == 0) {
    I2CStart();
    ack_bit = I2CSend(SIADDR << 1 | 1);
    if (ack_bit == 0) value = I2CRead();
    I2CNak();
    I2CStop();
  }
  if (ack_bit != 0) value = -1;  // "EOF" in C
  return value;
}
