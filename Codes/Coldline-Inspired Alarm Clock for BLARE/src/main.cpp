#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "time.h"
#include "esp_timer.h"

// Fonts
#include "Fonts/FreeMono12pt7b.h"
#include "Fonts/FreeMonoBold18pt7b.h"

// Dev's QOL Configs
#define TIME_ACCURACY 1000000

// TFT pins
#define TFT_SCLK 9
#define TFT_MOSI 10
#define TFT_DC 8
#define TFT_CS 6

// Peripherals pins
#define SW1 0
#define SW2 1
#define SW3 2
#define RE_B 3
#define RE_A 4
#define RE_CLICK 5
#define BUZZER 7

// Operation mode
#define STANDARD 10
#define ALARM_RINGING 11
#define ALARM_DM1 12
#define ALARM_DM2 13
#define ALARM_DM3 14
#define CONFIG_CLOCK 20
#define CONFIG_ALARM 21

// Create display entity
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK);

// Datacontainers
struct SysFlags {
  uint8_t mode;
  bool use_24h;
  bool alarm_active;
};


struct SysConfig {
  uint8_t s_offset;
  uint8_t m_offset;
  uint8_t h_offset;
  uint8_t alarm_h24;
  uint8_t alarm_m;
};

struct TimestepReturn {
  uint8_t sec;
  uint8_t min;
  uint8_t hour;
  uint8_t h24hour;
  bool is_am;
};

struct RingmodeData {
  uint8_t screen_state;
  uint8_t buzzer_state;
  unsigned int buzzing_duration;
};

struct InputData {
  bool re_a_active;
  bool re_b_active;
  bool re_click_active;
  bool sw1_active;
  bool sw2_active;
  bool sw3_active;
};

struct MonoDMData {
  uint8_t dep_to;
  uint8_t mid; // Minigame id
  uint16_t code;
};

struct DMData {
  MonoDMData dm1;
  MonoDMData dm2;
  MonoDMData dm3;
  uint8_t timer; // max 60 seconds to shut down?
  bool resolved;
};

DMData construct_dmdata(uint8_t timer = 60, uint8_t dm_counts = 3, uint8_t available_minigames = 4) {
  uint8_t dep_dm_limit_count = random(1, dm_counts); // Arduino has exclusive upper-bound
  uint8_t dep_dm_count = 0;
  MonoDMData dm[dm_counts] = {};
  
  for (uint8_t i = 0; i < dm_counts; i++) {
    dm[i].code = random(1000, 9999);
    dm[i].mid = random(available_minigames + 1);

    if (random(0, 2) && dep_dm_count < dep_dm_limit_count) {
      uint8_t r = random(dm_counts - 1);
      if (r >= i) r++;
      dm[i].dep_to = r + 1;
      dep_dm_count++;

    } else {
      dm[i].dep_to = 0;
    }
  }

  DMData packet = {
    dm[0],
    dm[1],
    dm[2],
    timer,
    false
  };
  return packet;
}

uint8_t buzzhandler(uint8_t buzzer_state, unsigned int buzzing_duration) {
}

TimestepReturn tickhandler(
  uint8_t s_offset,
  uint8_t m_offset,
  uint8_t h_offset
) {
  int64_t now = esp_timer_get_time();
  int64_t total_sec = now / TIME_ACCURACY;
  total_sec += s_offset + m_offset*60 + h_offset*3600;
  total_sec %= 86400; // 3600*24

  TimestepReturn ret;
  ret.sec = total_sec % 60;
  ret.min = (total_sec / 60) % 60;
  ret.h24hour = (total_sec / 3600) % 24;
  ret.is_am = ret.h24hour < 12;

  ret.hour = ret.h24hour % 12;
  ret.hour = (ret.hour == 0) ? 12 : ret.hour; // Pesky americans couldn't understand 00:00
  
  return ret; 
}

InputData read_inputs() {}

void standard_screen_update(
  unsigned int min,
  unsigned int hour,
  unsigned int h24hour,
  bool use_24h,
  bool alarm_active
) {

}

uint8_t ringing_screen_update(
  uint8_t state
) {}

void setup() {
  // Setup serial
  Serial.begin(115200);

  // initiate display
  tft.init(76, 284);
  // tft.setOffsets(82, 16);
  tft.invertDisplay(false);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  Serial.println("TFT DISPLAY INITIATED");
  tft.setCursor(0, 0);

  Serial.println("HELLO WORLD :3");
}

void loop() {
  // Static var def
  static SysFlags flags {STANDARD, false, false};
  static SysConfig config {0, 0, 0, 0, 0};
  static RingmodeData mdata_ring {0, 0, 0}; // mdata = mode data
  static uint8_t last_sec = 0;

  // UPDATES
  TimestepReturn ret = tickhandler(
    config.s_offset,
    config.m_offset,
    config.h_offset
  );
  
  InputData idata = read_inputs();

  if (ret.min == config.alarm_m && ret.h24hour == config.alarm_h24 && flags.alarm_active) {
    flags.mode = ALARM_RINGING;
  }

  // RENDERINGS
  if (ret.sec != last_sec) {
    last_sec = ret.sec;

    if (flags.mode == STANDARD) {
      standard_screen_update(ret.min, ret.hour, ret.h24hour, flags.use_24h, flags.alarm_active);
    }
  }

  if (flags.mode == ALARM_RINGING) {
    mdata_ring.buzzer_state = buzzhandler(mdata_ring.buzzer_state, mdata_ring.buzzing_duration);

    if (ret.sec != last_sec) {
      mdata_ring.buzzing_duration += 1; // buzzing_duration variable is non-critical, so slight deviation is ok
      mdata_ring.screen_state = ringing_screen_update(mdata_ring.screen_state);
    }
  }

  if (flags.mode == ALARM_DM1) {}
  if (flags.mode == ALARM_DM2) {}
  if (flags.mode == ALARM_DM3) {}
}