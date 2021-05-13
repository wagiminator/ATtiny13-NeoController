// TinyNeoController for ATtiny13A - with Gamma Correction
//
// An ATtiny13 is more than sufficient to control almost any number
// of NeoPixels via an IR remote. The NeoController was originally
// developed as a tester for 800kHz NeoPixel strings. Since there was
// still so much flash left in the ATtiny13, an IR receiver was
// integrated so that some parameters can be controlled with an IR
// remote control. In this way, it is also suitable as a simple and
// cheap remote-controlled control unit for NeoPixels. Due to its
// small size, it can be soldered directly to the LED strip without
// any problems. The power supply via a USB-C connection enables
// currents of up to 3A. There is still more than a third of the
// flash memory left for additional ideas.
//
//                               +-\/-+
//             --- A0 (D5) PB5  1|°   |8  Vcc
// NEOPIXELS ----- A3 (D3) PB3  2|    |7  PB2 (D2) A1 ---
// IR RECEIVER --- A2 (D4) PB4  3|    |6  PB1 (D1) ------
//                         GND  4|    |5  PB0 (D0) ------
//                               +----+
//
// Controller:  ATtiny13A
// Core:        MicroCore (https://github.com/MCUdude/MicroCore)
// Clockspeed:  9.6 MHz internal
// BOD:         BOD disabled
// Timing:      Micros disabled
// Leave the rest on default settings. Don't forget to "Burn bootloader"!
// No Arduino core functions or libraries are used. Use the makefile if 
// you want to compile without Arduino IDE.
//
// The Neopixel implementation is based on NeoCandle.
// https://github.com/wagiminator/ATtiny85-TinyCandle
//
// Gamma correction table is adapted from Adafruit: LED Tricks
// https://learn.adafruit.com/led-tricks-gamma-correction
//
// 2021 by Stefan Wagner 
// Project Files (EasyEDA): https://easyeda.com/wagiminator
// Project Files (Github):  https://github.com/wagiminator
// License: http://creativecommons.org/licenses/by-sa/3.0/


// Libraries
#include <avr/io.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <util/delay.h>

// Pins
#define NEO_PIN       3       // Pin for neopixels
#define IR_PIN        4       // Pin for IR receiver

// NeoPixel parameter
#define NEO_GRB               // type of pixel: NEO_GRB, NEO_RGB or NEO_RGBW
#define NEO_PIXELS    255     // number of pixels in the string (max 255)

// IR codes
#define IR_ADDR       0x1A    // IR device address
#define IR_POWER      0x01    // IR code for power on/off
#define IR_SPEED      0x03    // IR code for animation speed
#define IR_DENSE      0x04    // IR code for color density
#define IR_FAIL       0xFF    // IR fail code

// -----------------------------------------------------------------------------
// Neopixel Implementation for 9.6 MHz MCU Clock and 800 kHz Pixels
// -----------------------------------------------------------------------------

#define NEO_init()    DDRB |= (1<<NEO_PIN)      // set pixel DATA pin as output
#define NEO_latch()   _delay_us(281)            // delay to show shifted colors

// Gamma correction table
const uint8_t PROGMEM gamma[] = {
    0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   2,   2,   3,   3,   4,   5,
    6,   7,   8,  10,  11,  13,  14,  16,  18,  20,  22,  25,  27,  30,  33,  36,
   39,  43,  47,  50,  55,  59,  63,  68,  73,  78,  83,  89,  95, 101, 107, 114,
  120, 127, 135, 142, 150, 158, 167, 175, 184, 193, 203, 213, 223, 233, 244, 255
};

// Send a byte to the pixels string
void NEO_sendByte(uint8_t byte) {               // CLK  comment
  for(uint8_t bit=8; bit; bit--) asm volatile(  //  3   8 bits, MSB first
    "sbi  %[port], %[pin]   \n\t"               //  2   DATA HIGH
    "sbrs %[byte], 7        \n\t"               // 1-2  if "1"-bit skip next instruction
    "cbi  %[port], %[pin]   \n\t"               //  2   "0"-bit: DATA LOW after 3 cycles
    "rjmp .+0               \n\t"               //  2   delay 2 cycles
    "add  %[byte], %[byte]  \n\t"               //  1   byte <<= 1
    "cbi  %[port], %[pin]   \n\t"               //  2   "1"-bit: DATA LOW after 7 cycles
    ::
    [port]  "I"   (_SFR_IO_ADDR(PORTB)),
    [pin]   "I"   (NEO_PIN),
    [byte]  "r"   (byte)
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
  for(uint8_t i = NEO_PIXELS; i; i--) NEO_writeColor(0, 0, 0);
}

// Write hue value (0..191) with gamma correction to a single pixel
void NEO_writeHue(uint8_t hue) {
  uint8_t phase = hue >> 6;
  uint8_t step  = pgm_read_byte(&gamma[hue & 63]);
  uint8_t nstep = pgm_read_byte(&gamma[63 - (hue & 63)]);
  switch (phase) {
    case 0:   NEO_writeColor(nstep,  step,     0); break;
    case 1:   NEO_writeColor(    0, nstep,  step); break;
    case 2:   NEO_writeColor( step,     0, nstep); break;
    default:  break;
  }
}

// -----------------------------------------------------------------------------
// IR Receiver Implementation (NEC Protocol)
// -----------------------------------------------------------------------------

// IR receiver definitions and macros
#define IR_init()       PORTB |= (1<<IR_PIN)  // pullup on IR pin
#define IR_available()  (~PINB & (1<<IR_PIN)) // return true if IR line is low

// IR wait for signal change and measure duration
uint8_t IR_waitChange(uint8_t timeout) {
  uint8_t pinState = PINB & (1<<IR_PIN);      // get current signal state
  uint8_t dur = 0;                            // variable for measuring duration
  while ((PINB & (1<<IR_PIN)) == pinState) {  // measure length of signal
    if (dur++ > timeout) return 0;            // exit if timeout
    _delay_us(100);                           // count every 100us
  }
  return dur;                                 // return time in 100us
}

// IR read data byte
uint8_t IR_readByte(void) {
  uint8_t result;
  uint8_t dur;
  for (uint8_t i=8; i; i--) {                 // 8 bits
    result >>= 1;                             // LSB first
    if (IR_waitChange(11) < 3) return IR_FAIL;// exit if wrong burst length
    dur = IR_waitChange(21);                  // measure length of pause
    if (dur <  3) return IR_FAIL;             // exit if wrong pause length
    if (dur > 11) result |= 0x80;             // bit "0" or "1" depends on pause duration
  }
  return result;                              // return received byte
}

// IR read data according to NEC protocol
uint8_t IR_read(void) {
  uint16_t addr;                              // variable for received address
  if (!IR_available())        return IR_FAIL; // exit if no signal
  if (!IR_waitChange(100))    return IR_FAIL; // exit if wrong start burst length
  if (IR_waitChange(55) < 35) return IR_FAIL; // exit if wrong start pause length

  uint8_t addr1 = IR_readByte();              // get first  address byte
  uint8_t addr2 = IR_readByte();              // get second address byte
  uint8_t cmd1  = IR_readByte();              // get first  command byte
  uint8_t cmd2  = IR_readByte();              // get second command byte

  if (IR_waitChange(11) < 3)  return IR_FAIL; // exit if wrong final burst length
  if ((cmd1 + cmd2) < 255)    return IR_FAIL; // exit if command bytes are not inverse
  if ((addr1 + addr2) == 255) addr = addr1;   // check if it's extended NEC-protocol ...
  else addr = ((uint16_t)addr2 << 8) | addr1; // ... and get the correct address
  if (addr != IR_ADDR)        return IR_FAIL; // wrong address
  return cmd1;                                // return command code
}

// -----------------------------------------------------------------------------
// Standby Implementation
// -----------------------------------------------------------------------------

// Go to standby mode
void standby(void) {
  NEO_clear();                                // turn off NeoPixels
  while(1) {
    GIFR  |= (1<<PCIF);                       // clear any outstanding interrupts
    sei();                                    // enable interrupts
    sleep_mode();                             // sleep until IR interrupt
    cli();                                    // disable interrupts
    if( (IR_available()) && (IR_read() == IR_POWER) ) break;  // exit on power button
  }
}

// Pin change interrupt service routine
EMPTY_INTERRUPT (PCINT0_vect);                // just wake up from sleep

// -----------------------------------------------------------------------------
// Main Function
// -----------------------------------------------------------------------------

int main(void) {
  // Local variables
  uint8_t start = 0;
  uint8_t speed = 3;
  uint8_t dense = 2;

  // Disable unused peripherals and prepare sleep mode to save power
  ACSR   =  (1<<ACD);                         // disable analog comperator
  DIDR0  = ~(1<<IR_PIN) & 0x1F;               // disable digital intput buffer except IR pin
  PRR    =  (1<<PRADC);                       // shut down ADC
  GIMSK |=  (1<<PCIE);                        // enable pin change interrupts
  PCMSK |=  (1<<IR_PIN);                      // enable interrupt on IR pin
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);        // set sleep mode to power down

  // Setup
  NEO_init();                                 // init Neopixels
  IR_init();                                  // init IR receiver

  // Loop
  while(1) {
    uint8_t current = start;
    start += speed;
    if(start >= 192) start -= 192;
    
    for(uint8_t i = NEO_PIXELS; i; i--) {
      NEO_writeHue(current);
      current += dense;
      if(current >= 192) current -= 192;
    }
    
    for(uint8_t i=80; i; i--) {
      if(IR_available()) {
        uint8_t command = IR_read();
        switch (command) {
          case IR_POWER:    standby(); break;
          case IR_SPEED:    speed <<= 1; if(++speed > 7) speed = 0; break;
          case IR_DENSE:    dense += 2; if(dense > 7) dense = 0; break;
          default:          break;
        }       
      }
      _delay_ms(1);
    }
  }
}
