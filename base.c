/*

 Crazy Clock for Arduino
 Copyright 2014 Nicholas W. Sayer
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * This is intended to run on an ATTiny45. Connect a 4.00 MHz or a 32.768 kHz
 * crystal and fuse it for the appropriate oscillator (set the divide-by-8 fuse for 4.x MHz),
 * no watchdog or brown-out detector.
 *
 * Connect PB0 and PB1 to the coil pins of a Lavet stepper coil of a clock movement
 * (with a series resistor and flyback diode to ground on each pin) and power it 
 * from a 3.3 volt boost converter.
 *
 * This file is the common infrastructure for all of the different clock types.
 * It sets up a 10 Hz interrupt. The clock code(s) keep accurate time by calling
 * either doTick() or doSleep() repeatedly. Each method will put the CPU to sleep
 * until the next tenth-of-a-second interrupt (doTick() will tick the clock once first).
 * In addition, doTick() and doSleep(), will occasionally (SEED_UPDATE_INTERVAL)
 * write out the PRNG seed (if it's changed) to EEPROM. This will insure that the clock
 * doesn't repeat its previous behavior every time you change the battery.
 *
 * The clock code should insure that it doesn't do so much work that works through
 * a 10 Hz interrupt interval. Every time that happens, the clock loses a tenth of
 * a second. In particular, generating random numbers is a costly operation.
 *
 */

#include <avr/io.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/cpufunc.h>
#include <util/delay.h>
#include <util/atomic.h>

#include "base.h"

#if !(defined(FOUR_MHZ_CLOCK) ^ defined(THIRTYTWO_KHZ_CLOCK))
#error Must pick either 4 MHz or 32 kHz option.
#endif

#if defined(FOUR_MHZ_CLOCK)
// 4,000,000 divided by 128 is 31,250.
// 31,250 divided by (64 * 10) is a divisor of 48 53/64, which is 49*53 + 48*11
#define CLOCK_CYCLES (64)
// Don't forget to decrement the OCR0A value - it's 0 based and inclusive
#define CLOCK_BASIC_CYCLE (48 - 1)
// a "long" cycle is CLOCK_BASIC_CYCLE + 1
#define CLOCK_NUM_LONG_CYCLES (53)
#elif defined(THIRTYTWO_KHZ_CLOCK)
// 32,768 divided by (64 * 10) yields a divisor of 51 1/5, which is 52 + 51*4
#define CLOCK_CYCLES (5)
// Don't forget to decrement the OCR0A value - it's 0 based and inclusive
#define CLOCK_BASIC_CYCLE (51 - 1)
// a "long" cycle is CLOCK_BASIC_CYCLE + 1
#define CLOCK_NUM_LONG_CYCLES (1)
#endif

// One day in tenths-of-a-second
#define SEED_UPDATE_INTERVAL 864000L
#define EE_PRNG_SEED_LOC ((void*)0)

#ifdef SW_TRIM
#define EE_TRIM_LOC ((void*)4)
#endif

// clock solenoid pins
#define P0 0
#define P1 1
#define P_UNUSED 2

// For a 32 kHz system clock speed, random() is too slow.
// Found this at http://uzebox.org/forums/viewtopic.php?f=3&t=250
static long seed;
#define M (0x7fffffffL)

unsigned long q_random() {
  seed = (seed >> 16) + ((seed << 15) & M) - (seed >> 21) - ((seed << 10) & M);
  if (seed < 0) seed += M;
  return (unsigned long) seed;
}

static void updateSeed() {
  // Don't bother exercising the eeprom if the seed hasn't changed
  // since last time.
  if (((long)eeprom_read_dword(EE_PRNG_SEED_LOC)) == seed) return;
  eeprom_write_dword(EE_PRNG_SEED_LOC, seed);
}

volatile static unsigned char sleep_miss_counter = 0;

static unsigned long seed_update_timer;
void doSleep() {

  if (--seed_update_timer == 0) {
    updateSeed();
    seed_update_timer = SEED_UPDATE_INTERVAL;
  }

  // If we missed a sleep, then try and catch up by *not* sleeping.
  // Note that the test-and-decrememnt must be atomic, so save a
  // copy of the present value before decrementing and use that
  // copy for the decision.
  unsigned char local_smc;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    local_smc = sleep_miss_counter--;
  }
  if (local_smc == 0)
    sleep_mode(); // this results in sleep_miss_counter being incremented.
#ifdef DEBUG
  else {
    // indicate an overflow
    PORTB |= _BV(P_UNUSED);
    while(1); // lock up
  }
#endif
}

// How long is each tick pulse?
#define TICK_LENGTH (35)

// This will alternate the ticks
#define TICK_PIN (lastTick == P0?P1:P0)

// Each call to doTick() will "eat" a single one of our interrupt "ticks"
void doTick() {
  static unsigned char lastTick = P0;

  PORTB |= _BV(TICK_PIN);
  _delay_ms(TICK_LENGTH);
  PORTB &= ~ _BV(TICK_PIN);
  lastTick = TICK_PIN;
  doSleep(); // eat the rest of this tick
}

ISR(TIMER0_COMPA_vect) {
  static unsigned char cycle_pos = 0;
#ifdef SW_TRIM
  static unsigned long trim_pos = 0;
#endif

  int offset = 0;
#ifdef SW_TRIM
  // This is how many crystal cycles we just went through.
  unsigned long crystal_cycles = OCR0A;
  if (trim_pos < crystal_cycles) {
  	trim_pos += 10000000; // ten million - the correction factor is tenths-of-a-ppm
	offset = (int)eeprom_read_word(EE_TRIM_LOC);
  }
  trim_pos -= crystal_cycles;
#endif

  // This is the magic for fractional counting.
  // Alternate between adding an extra count and
  // not adding one. This means that the intervals
  // are not uniform, but it's only by 2 ms or so,
  // which won't be noticable for this application.
  if (++cycle_pos == CLOCK_NUM_LONG_CYCLES)
    OCR0A = CLOCK_BASIC_CYCLE + offset;
  if (cycle_pos >= CLOCK_CYCLES) {
    OCR0A = CLOCK_BASIC_CYCLE + 1 + offset;
    cycle_pos = 0;
  }

  // Keep track of any interrupts we blew through.
  // Every increment here *should* be matched by
  // a decrement in doSleep();
  sleep_miss_counter++;
}

extern void loop();

void main() {
#ifndef THIRTYTWO_KHZ_CLOCK
  // change this so that we wind up with as near a 32 kHz CPU clock as possible.
  clock_prescale_set(clock_div_128);
#endif
  ADCSRA = 0; // DIE, ADC!!! DIE!!!
  ACSR = _BV(ACD); // Turn off analog comparator - but was it ever on anyway?
  power_adc_disable();
  power_usi_disable();
  power_timer1_disable();
  TCCR0A = _BV(WGM01); // mode 2 - CTC
  TCCR0B = _BV(CS01) | _BV(CS00); // prescale = 64
  TIMSK = _BV(OCIE0A); // OCR0A interrupt only.
  
  set_sleep_mode(SLEEP_MODE_IDLE);

  DDRB = _BV(P0) | _BV(P1) | _BV(P_UNUSED); // all our pins are output.
  PORTB = 0; // Initialize all pins low.

  // Try and perturb the PRNG as best as we can
  seed = (long)eeprom_read_dword(EE_PRNG_SEED_LOC);
  // it can't be all 0 or all 1
  if (seed == 0 || ((seed & M) == M)) seed=0x12345678L;
  q_random(); // perturb it once...
  updateSeed(); // and write it back out - a new seed every battery change.

  // initialize this so it doesn't have to be in the data segment.
  seed_update_timer = SEED_UPDATE_INTERVAL;

  // Set up the initial state of the timer.
  OCR0A = CLOCK_BASIC_CYCLE + 1;
  TCNT0 = 0;

  // Don't forget to turn the interrupts on.
  sei();

  // Now hand off to the specific clock code
  while(1) loop();

}

