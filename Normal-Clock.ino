/*

 Normal Clock for Arduino
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
 * This is intended to run on an ATTiny85. Connect a 16.384 MHz crystal and fuse it
 * for divide-by-8 clocking, no watchdog or brown-out detector.
 *
 * Connect PB0 and PB1 to the coil pins of a Lavet stepper coil of a clock movement
 * (with a series resistor and flyback diode to ground on each pin) and power it 
 * from a 3.3 volt boost converter.
 *
 * This clock just keeps normal time by ticking once per second. No tricks.
 *
 */
 
#include <Arduino.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/power.h>

// Turn this on to use the unused output as a debug output
// #define DEBUG

// Update the PRNG seed daily
#define SEED_UPDATE_INTERVAL (86400)

// We're going to set up a timer interrupt for every 100 msec, so what's 1/.1?
// Note that while we're actually doing stuff, we *must* insure that we never
// work through an interrupt. This is because we're not *counting* these
// interrupts, we're just waiting for each one in turn.
#define IRQS_PER_SECOND (10)

// Our design choices may wind up with a system clock of either 500 kHz
// or 512 kHz. If it's 500 kHz, then we have to do some juggling to wind up
// with the proper IRQS_PER_SECOND value of 10. To set that up, uncomment
// this:
#define TEN_BASED_CLOCK

#ifdef TEN_BASED_CLOCK
// 50,000 divided by 1024 is 48 53/64, which is 49*53 + 48*(64-53)
#define CLOCK_CYCLES (64)
// Don't forget to decrement the OCR0A value - it's 0 based and inclusive
#define CLOCK_BASIC_CYCLE (48 - 1)
// a "long" cycle is CLOCK_BASIC_CYCLE + 1
#define CLOCK_NUM_LONG_CYCLES (53)
#endif

// clock solenoid pins
#define P0 0
#define P1 1
#define P_UNUSED 2

// How long is each tick? In this case, we're going to busy-wait on the timer.
#define TICK_LENGTH (35)

// This delay loop is magical because we know the timer is ticking at 500 Hz.
// So we just wait until it counts N/2 times and that will be an N msec delay.
static void delay_ms(unsigned char msec) {
   unsigned char start_time = TCNT0;
   while(TCNT0 - start_time < msec / 2) ; // sit-n-spin
}

static void doSleep() {
#ifdef TEN_BASED_CLOCK
  static unsigned char cycle_pos = 0xff; // force a reset

  if (cycle_pos == CLOCK_NUM_LONG_CYCLES)
    OCR0A = CLOCK_BASIC_CYCLE;
  if (cycle_pos++ >= CLOCK_CYCLES) {
    OCR0A = CLOCK_BASIC_CYCLE + 1;
    cycle_pos = 0;
  }
#endif
  sleep_mode();
}

// This will alternate the ticks
#define TICK_PIN (lastTick == P0?P1:P0)

// Each call to doTick() will "eat" a single one of our interrupt "ticks"
static void doTick() {
  static unsigned char lastTick = P0;

  digitalWrite(TICK_PIN, HIGH);
  delay_ms(TICK_LENGTH);
  digitalWrite(TICK_PIN, LOW);
  lastTick = TICK_PIN;
  doSleep(); // eat the rest of this tick
}

ISR(TIMER0_COMPA_vect) {
  // do nothing - just wake up
}

void setup() {
  clock_prescale_set(clock_div_8);
  ADCSRA = 0; // DIE, ADC!!! DIE!!!
  ACSR = _BV(ACD); // Turn off analog comparator - but was it ever on anyway?
  power_adc_disable();
  power_usi_disable();
  power_timer1_disable();
  TCCR0A = _BV(WGM01); // mode 2 - CTC
  TCCR0B = _BV(CS02) | _BV(CS00); // prescale = 1024
  // xtal freq = 4.096 MHz.
  // CPU freq = 4.096 MHz / 8 = 512 kHz
  // count freq = 512 kHz / 1024 = 500 Hz
#ifndef TEN_BASED_CLOCK
  OCR0A = 49; // 10 Hz - don't forget to subtract 1 - the counter is 0-49.
#endif
  TIMSK = _BV(OCIE0A); // OCR0A interrupt only.
  
  set_sleep_mode(SLEEP_MODE_IDLE);

  pinMode(P_UNUSED, OUTPUT);
  digitalWrite(P_UNUSED, LOW);
  pinMode(P0, OUTPUT);
  pinMode(P1, OUTPUT);
  digitalWrite(P0, LOW);
  digitalWrite(P1, LOW);

}

void loop() {
  while(1){
    doTick();
    for(int i = 0; i < IRQS_PER_SECOND - 1; i++)
      doSleep();
  }
}