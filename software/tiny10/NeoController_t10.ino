// ===================================================================================
// Project:   TinyNeoController - NeoPixel Controller based on ATtiny10
// Version:   v1.0
// Year:      2021
// Author:    Stefan Wagner
// Github:    https://github.com/wagiminator
// EasyEDA:   https://easyeda.com/wagiminator
// License:   http://creativecommons.org/licenses/by-sa/3.0/
// ===================================================================================
//
// Description:
// ------------
// The NeoController was developed to test 800kHz NeoPixel strings with
// up to 255 pixels. Since there was still so much flash left in the
// ATtiny10, an IR receiver was integrated so that some parameters can
// be controlled with an IR remote control.
//
// References:
// -----------
// The Neopixel implementation is based on NeoCandle.
// https://github.com/wagiminator/ATtiny85-TinyCandle
//
// The IR receiver implementation (NEC protocol) is based on TinyDecoder
// https://github.com/wagiminator/ATtiny13-TinyDecoder
//
// Wiring:
// -------
//                       +-\/-+
// NEOPIXELS ----- PB0  1|°   |6  PB3 --- 
//                 GND  2|    |5  Vcc
// IR RECEIVER --- PB1  3|    |4  PB2 --- 
//                       +----+  
//
// Compilation Settings:
// ---------------------
// Core:        ATtiny10Core (https://github.com/technoblogy/attiny10core)
// Chip:        ATtiny10
// Clock:       8 MHz

// Leave the rest on default settings. Don't forget to "Burn bootloader"!
// No Arduino core functions or libraries are used. Use the makefile if 
// you want to compile without Arduino IDE.
//
// Fuse settings: -U BYTE0:w:0xff:m


// ===================================================================================
// Libraries and Definitions
// ===================================================================================

// Libraries
#include <avr/io.h>           // for GPIO
#include <avr/sleep.h>        // for sleep functions
#include <avr/interrupt.h>    // for interrupts
#include <util/delay.h>       // for delays

// Pin definitions
#define NEO_PIN       0       // Pin for neopixels
#define IR_PIN        1       // Pin for IR receiver

// NeoPixel parameter
#define NEO_GRB               // type of pixel: NEO_GRB, NEO_RGB or NEO_RGBW
#define NEO_PIXELS    255     // number of pixels in the string (max 255)

// IR codes
#define IR_ADDR       0x1A    // IR device address
#define IR_POWER      0x01    // IR code for power on/off
#define IR_BRIGHT     0x02    // IR code for brightness
#define IR_SPEED      0x03    // IR code for animation speed
#define IR_FAIL       0xFF    // IR fail code

// Global variables
uint8_t NEO_brightness = 0;   // 0..2

// ===================================================================================
// Neopixel Implementation for 8 MHz MCU Clock and 800 kHz Pixels
// ===================================================================================

// NeoPixel parameter and macros
#define NEO_init()    DDRB |= (1<<NEO_PIN)    // set pixel DATA pin as output
#define NEO_latch()   _delay_us(281)          // delay to show shifted colors

// Send a byte to the pixels string
void NEO_sendByte(uint8_t byte) {
  uint8_t bit = 8;                            // 8 bits, MSB first
  asm volatile (
    "sbi  %[port], %[pin]   \n\t"             // DATA HIGH
    "sbrs %[byte], 7        \n\t"             // if "1"-bit skip next instruction
    "cbi  %[port], %[pin]   \n\t"             // "0"-bit: DATA LOW after 3 cycles
    "add  %[byte], %[byte]  \n\t"             // byte <<= 1
    "subi %[bit],  0x01     \n\t"             // count--
    "cbi  %[port], %[pin]   \n\t"             // "1"-bit: DATA LOW after 6 cycles
    "brne .-14              \n\t"             // while(count)
    ::
    [port]  "I"   (_SFR_IO_ADDR(PORTB)),
    [pin]   "I"   (NEO_PIN),
    [byte]  "r"   (byte),
    [bit]   "r"   (bit)
  );
}

// Write color to a single pixel
void NEO_writeColor(uint8_t r, uint8_t g, uint8_t b) {
  #if defined (NEO_GRB)
    NEO_sendByte(g); NEO_sendByte(r); NEO_sendByte(b);
  #elif defined (NEO_RGB)
    NEO_sendByte(r); NEO_sendByte(g); NEO_sendByte(b);
  #elif defined (NEO_RGBW)
    NEO_sendByte(r); NEO_sendByte(g); NEO_sendByte(b); NEO_sendByte(0);
  #else
    #error Wrong or missing NeoPixel type definition!
  #endif
}

// Switch off all pixels
void NEO_clear(void) {
  cli();
  for(uint8_t i = NEO_PIXELS; i; i--) NEO_writeColor(0, 0, 0);
  sei();
}

// Write hue value (0..191) to a single pixel
void NEO_writeHue(uint8_t hue) {
  uint8_t phase = hue >> 6;
  uint8_t step  = (hue & 63) << NEO_brightness;
  uint8_t nstep = (63 << NEO_brightness) - step;
  switch(phase) {
    case 0:   NEO_writeColor(nstep,  step,     0); break;
    case 1:   NEO_writeColor(    0, nstep,  step); break;
    case 2:   NEO_writeColor( step,     0, nstep); break;
    default:  break;
  }
}

// ===================================================================================
// IR Receiver Implementation (NEC Protocol) using Timer0
// ===================================================================================

// IR receiver definitions and macros
#define IR_available()  (~PINB & (1<<IR_PIN)) // return true if IR line is low
#define IR_PRESCALER    1024                  // prescaler of the timer
#define IR_time(t)      ((F_CPU / 1000) * t) / IR_PRESCALER / 1000  // convert us to counts
#define IR_TOP          IR_time(12000UL)      // TOP value of timer (timeout)

// IR global variables
volatile uint8_t IR_dur;                      // for storing duration of last burst/pause
volatile uint8_t IR_flag;                     // gets zero in pin change or time over

// IR initialize the receiver
void IR_init(void) {
  PORTB  |=  (1<<IR_PIN);                     // pullup on IR pin
  PCMSK  |=  (1<<IR_PIN);                     // enable interrupt on IR pin
  PCICR  |=  (1<<PCIE0);                      // enable pin change interrupts
  OCR0A   =   IR_TOP;                         // timeout causes OCA interrupt
  TIMSK0 |=  (1<<OCIE0A);                     // enable output compare match interrupt
}

// IR wait for signal change
void IR_wait(void) {
  IR_flag = 1;                                // reset flag
  while(IR_flag);                             // wait for pin change or timeout
}

// IR read data according to NEC protocol
uint8_t IR_read(void) {
  uint32_t data;                              // variable for received data
  uint16_t addr;                              // variable for received address
  if(!IR_available()) return IR_FAIL;         // exit if no signal
  IR_wait();                                  // wait for end of start burst
  if(IR_dur < IR_time(8000)) return IR_FAIL;  // exit if no start condition
  IR_wait();                                  // wait for end of start pause
  if(IR_dur < IR_time(4000)) return IR_FAIL;  // exit if no start condition
  for(uint8_t i=32; i; i--) {                 // receive 32 bits
    data >>= 1;                               // LSB first
    IR_wait();                                // wait for end of burst
    if(!IR_dur) return IR_FAIL;               // exit if overflow
    IR_wait();                                // wait for end of pause
    if(!IR_dur) return IR_FAIL;               // exit if overflow
    if(IR_dur > IR_time(1124)) data |= 0x80000000; // bit "0" or "1" depends on pause duration
  }
  IR_wait();                                  // wait for end of final burst
  uint8_t addr1 = data;                       // get first  address byte
  uint8_t addr2 = data >> 8;                  // get second address byte
  uint8_t cmd1  = data >> 16;                 // get first  command byte
  uint8_t cmd2  = data >> 24;                 // get second command byte
  if((cmd1 + cmd2) < 255) return IR_FAIL;     // if second command byte is not the inverse of the first
  if((addr1 + addr2) == 255) addr = addr1;    // check if it's extended NEC-protocol ...
  else addr = data;                           // ... and get the correct address
  if(addr != IR_ADDR) return IR_FAIL;         // wrong address
  return cmd1;                                // return command code
}

// Pin change interrupt service routine
ISR (PCINT0_vect) {
  IR_dur  = TCNT0L;                           // save timer value
  TCNT0   = 0;                                // reset timer0
  TCCR0B  = (1<<WGM02)|(1<<CS02)|(1<<CS00);   // start timer0 CTC with prescaler 1024
  IR_flag = 0;                                // raise flag
}

// Timer0 overflow interrupt service routine (timeout)
ISR(TIM0_COMPA_vect) {
  TCCR0B  = 0;                                // stop timer0
  TCNT0   = 0;                                // reset timer0
  IR_flag = 0;                                // raise flag
  IR_dur  = 0;                                // set duration value to zero
}

// ===================================================================================
// Standby Implementation
// ===================================================================================

// Go to standby mode
void standby(void) {
  NEO_clear();                                // turn off NeoPixels
  while(1) {
    PCICR |= (1<<PCIF0);                      // clear any outstanding interrupts
    sleep_mode();                             // sleep until IR interrupt
    if( (IR_available()) && (IR_read() == IR_POWER) ) break;  // exit on power button
  }
}

// ===================================================================================
// Main Function
// ===================================================================================

int main(void) {
  // Set clock speed 8 MHz
  CCP = 0xD8;                                 // unlock register protection
  CLKPSR = 0;                                 // set clock prescaler to 0 -> 8 Mhz

  // Local variables
  uint8_t start = 0;
  uint8_t speed = 3;
  uint8_t dense = 2;

  // Setup
  NEO_init();                                 // init Neopixels
  IR_init();                                  // init IR receiver
  sei();                                      // enable global interrupts

  // Loop
  while(1) {
    // Set new start value
    uint8_t current = start;
    start += speed;
    if(start >= 192) start -= 192;
    
    // Set the NeoPixels
    for(uint8_t i = NEO_PIXELS; i; i--) {
      cli();
      NEO_writeHue(current);
      sei();
      current += dense;
      if(current >= 192) current -= 192;
    }
    
    // Check IR receiver and change parameters; delay 80ms
    for(uint8_t i=80; i; i--) {
      if(IR_available()) {
        uint8_t command = IR_read();
        switch(command) {
          case IR_POWER:    standby(); break;
          case IR_BRIGHT:   if(++NEO_brightness > 2) NEO_brightness = 0; break;
          case IR_SPEED:    speed <<= 1; if(++speed > 7) speed = 0; break;
          default:          break;
        }       
      }
      _delay_ms(1);
    }
  }
}
