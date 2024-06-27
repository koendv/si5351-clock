/*
 * Clock Generator 8 kHz - 200 MHz.
 * SI5351 or MS5351M.
 *
 * compile with ch55xduino for wch ch552e
 * https://github.com/DeqingSun/ch55xduino
 * in Arduino IDE Tools -> Settings:
 * Board: CH552 board 
 * Clock Source: 16 MHz Internal, 3.3V or 5V
 * Bootloader Pin: P3.6 (D+) pull-up
 * Upload method: USB
 * USB Settings: Default CDC
 */

#include <SoftI2C.h>
#include "si5351reg.h"
#include "si5351mcu.h"

#define FREQUENCY_CRYSTAL 26000000UL  // crystal frequency
#define FREQUENCY_MAX 200000000UL
#define FREQUENCY_MIN 7810UL
#define FREQUENCY0_DEFAULT 8000000UL
#define FREQUENCY1_DEFAULT 32768UL
#define CRYSTAL_LOAD_CAP SI5351_CRYSTAL_LOAD_0PF
#define POWER_MAX 3
#define POWER_DEFAULT 3
#define SETTINGS_MAGIC 0xC0FFEEUL
#define LED_BUILTIN 15

void PrintHelp();
void PrintClocks();
void PrintFrequency(uint32_t f_out);
void StoreSettings();
void RecallSettings();
void BlinkLed();
void SelfTest();

struct settings_struct {
  uint32_t magic;
  uint32_t frequency[2];
  uint8_t power[2];
};

struct settings_struct settings;

uint8_t clk = 0;  // clk0 or clk1
bool print_banner = true;
uint32_t led_millis = UINT32_MAX / 2;
char line_buf[10] = { 0 };  // character buffer to enter frequency
uint8_t line_len = 0;

void LedOn() {
  digitalWrite(LED_BUILTIN, HIGH);
}

void LedOff() {
  digitalWrite(LED_BUILTIN, LOW);
}

void SetFrequencyAndPower() {
  si5351mcu_setFreq(0, settings.frequency[0]);  // Set clock 0 frequency
  si5351mcu_setPower(0, settings.power[0]);     // Switch clock 0 on
  si5351mcu_setFreq(1, settings.frequency[1]);  // Set clock 1 frequency
  si5351mcu_setPower(1, settings.power[1]);     // Switch clock 1 on
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  LedOn();
  RecallSettings();
  sda_pin = 16;  // SDA on pin P1.6
  scl_pin = 17;  // SCL on pin P1.7
  si5351mcu_init(CRYSTAL_LOAD_CAP, FREQUENCY_CRYSTAL);
  SetFrequencyAndPower();
  delay(100);
  BlinkLed();
}

void loop() {
  if (USBSerial_available()) {
    if (print_banner) {
      USBSerial_print("\r\nsi5351 clock gen\r\n? for help\r\n");
      print_banner = false;
    }
    char ch = USBSerial_read();
    if ((ch >= 'A') && (ch <= 'Z')) ch += 'a' - 'A';  // to lower case
    switch (ch) {
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        if (line_len < sizeof(line_buf)) {
          USBSerial_write(ch);
          line_buf[line_len++] = ch;
        }
        break;
      case '\b':
        if (line_len > 0) {
          --line_len;
          USBSerial_print("\b \b");
        }
        break;
      case '\r':
      case '\n':
      case 'k':
      case 'm':  // set new frequency
        if (line_len > 0) {
          // convert to int
          uint32_t new_f = 0;
          for (uint8_t i = 0; i < line_len; i++)
            new_f = new_f * 10 + line_buf[i] - '0';
          // check bounds
          if (ch == 'k') {
            if (new_f <= FREQUENCY_MAX / 1000) new_f *= 1000;  // kHz
            else new_f = FREQUENCY_MAX;
          }
          if (ch == 'm') {
            if (new_f <= FREQUENCY_MAX / 1000000) new_f *= 1000000;  // MHz
            else new_f = FREQUENCY_MAX;
          }
          if (new_f < FREQUENCY_MIN) new_f = FREQUENCY_MIN;
          if (new_f > FREQUENCY_MAX) new_f = FREQUENCY_MAX;
          settings.frequency[clk] = new_f;
          si5351mcu_setFreq(clk, settings.frequency[clk]);
          line_len = 0;
        }
        break;
      case '+':  // more power
        if (settings.power[clk] < POWER_MAX) {
          settings.power[clk]++;
          si5351mcu_setPower(clk, settings.power[clk]);
          line_len = 0;
        }
        break;
      case '-':  // less power
        if (settings.power[clk] > 0) {
          --settings.power[clk];
          si5351mcu_setPower(clk, settings.power[clk]);
          line_len = 0;
        }
        break;
      case 'a':  // select CLK0
        clk = 0;
        line_len = 0;
        break;
      case 'b':  // select CLK1
        clk = 1;
        line_len = 0;
        break;
      case 's':  // store settings
        StoreSettings();
        USBSerial_print("store");
        line_len = 0;
        break;
      case 'r':  // recall settings
        RecallSettings();
        SetFrequencyAndPower();
        USBSerial_print("recall");
        line_len = 0;
        break;
      case 't':  // frequency sweep
        SelfTest();
        break;
      case '?':  // help
      case 'h':
        PrintHelp();
        line_len = 0;
        break;
      default:
        break;
    }

    if (line_len == 0) {  // not entering a frequency
      PrintClocks();
    }
    USBSerial_flush();
  }
  BlinkLed();
}

void PrintHelp() {
  USBSerial_print("\r\ncompiled " __DATE__ "\r\ncrystal ");
  PrintFrequency(FREQUENCY_CRYSTAL);
  USBSerial_print(
    "\r\n"
    "0..9,backspace: frequency\r\n"
    "enter: set Hz\r\n"
    "k: set kHz\r\n"
    "m: set MHz\r\n"
    "+,-: power up/down\r\n"
    "a: select clock 0\r\n"
    "b: select clock 1\r\n"
    "s: store\r\n"
    "r: recall\r\n"
    "t: self-test\r\n"
    "?,h: help\r\n");
}

void PrintClocks() {
  USBSerial_println();
  for (int i = 0; i < 2; i++) {
    USBSerial_print((char)('0' + i));
    USBSerial_print(" ");
    PrintFrequency(settings.frequency[i]);
    USBSerial_print(" ");
    USBSerial_print((char)('2' + 2 * settings.power[i]));  // '2', '4', '6' or '8'
    USBSerial_println("mA");
  }
  USBSerial_print("clock ");
  USBSerial_print((char)('0' + clk));
  USBSerial_print(" >");
}

void PrintFrequency(uint32_t f_out) {
  char prefix = 0;
  if (f_out % 1000000 == 0) {
    f_out /= 1000000;
    prefix = 'M';
  } else if (f_out % 1000 == 0) {
    f_out /= 1000;
    prefix = 'k';
  }
  USBSerial_print(f_out);
  if (prefix) USBSerial_print(prefix);
  USBSerial_print("Hz");
}

void StoreSettings() {  // store frequency and power in non-volatile memory
  uint8_t i;
  char *s = (char *)&settings;
  for (i = 0; i < sizeof(settings); i++)
    eeprom_write_byte(i, s[i]);
}

void RecallSettings() {  // recall frequency and power from non-volatile memory
  char *s = (char *)&settings;
  for (uint8_t i = 0; i < sizeof(settings); i++)
    s[i] = eeprom_read_byte(i);
  bool valid = (settings.magic == SETTINGS_MAGIC)
               && (settings.frequency[0] >= FREQUENCY_MIN) && (settings.frequency[0] <= FREQUENCY_MAX) && (settings.power[0] <= POWER_MAX)
               && (settings.frequency[1] >= FREQUENCY_MIN) && (settings.frequency[1] <= FREQUENCY_MAX) && (settings.power[1] <= POWER_MAX);
  if (!valid) {  // first run, set default values
    settings.magic = SETTINGS_MAGIC;
    settings.frequency[0] = FREQUENCY0_DEFAULT;
    settings.power[0] = POWER_DEFAULT;
    settings.frequency[1] = FREQUENCY1_DEFAULT;
    settings.power[1] = POWER_DEFAULT;
    StoreSettings();
  }
}

void SelfTest() {  // sweep through frequency range
  uint32_t f = 8000;

  USBSerial_println();
  while (f <= FREQUENCY_MAX) {
    PrintFrequency(f);
    USBSerial_println();
    USBSerial_flush();
    si5351mcu_setFreq(0, f);
    delay(3000);
    if (si5351mcu_not_ready()) {
      LedOn();
      USBSerial_println("fail");
    }
    if (f < 10000) f += 1000;
    else if (f < 100000) f += 10000;
    else if (f < 1000000) f += 100000;
    else if (f < 10000000) f += 1000000;
    else f += 10000000;
  }
  si5351mcu_setFreq(0, settings.frequency[0]);
}

void BlinkLed() {  // light led 5 seconds if error
  uint32_t current_millis = millis();
  if (current_millis - led_millis > 5000) {
    if (si5351mcu_not_ready()) {
      LedOn();
    } else
      LedOff();
    led_millis = current_millis;
  }
}

// not truncated
