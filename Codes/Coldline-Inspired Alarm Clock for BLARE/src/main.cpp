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
#define PIN_SW1 0
#define PIN_SW2 1
#define PIN_SW3 2
#define PIN_RE_B 3
#define PIN_RE_A 4
#define PIN_RE_CLICK 5
#define PIN_BUZZER 7

#define BUZZER_CHANNEL 2
#define BUZZER_MIN_FREQ 800
#define BUZZER_MAX_FREQ 2000
#define BUZZER_RESOLUTION 8

// Operation mode
// Status prefix + current space + space variation
// Status prefix 1 = idle; 2 = ringing/'war'
// Current space 0 = standard; 1 = config; 2 = peskyspace (minigame)
#define STANDARD 100
#define CONFIG_CLOCK 110
#define CONFIG_ALARM 111
#define ALARM_RINGING 200
#define ALARM_DMMENU 210
#define ALARM_DMID1 221
#define ALARM_DMID2 222
#define ALARM_DMID3 223
#define ALARM_DMID4 224

// MINIGAME PHASE
#define P_PREINTRO 0
#define P_INTRO 1
#define P_MINIGAME 2
#define P_CLOSING_PASS 3
#define P_CLOSING_FAIL 255

// STANDARD STATUS
#define S_HOLD 0
#define S_PASS 1
#define S_FAIL 2

// Create display entity
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK);

// Datacontainers
struct SysFlags {
  uint8_t mode = STANDARD;
  bool use_24h = false;
  bool alarm_active = false;
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
  uint8_t screen_state : 4 = 0;
  uint8_t direction : 3 = 1; // 1 = ascending, 0 = descending
  int8_t buzzer_state : 4 = -1; // 1 = active; -1 = deactivate
  int16_t current_freq : 13 = BUZZER_MIN_FREQ;

  uint64_t last_delta_us = 0;
};

struct InputData {
  bool re_clockwise = false;
  bool re_counterclockwise = false;
  bool re_bttn_clicked = false;
  bool re_bttn_holding = false;
  bool sws_clicked[3] = {false, false, false};
  bool sws_holding[3] = {false, false, false};
  bool any_sw_clicked = false;
  
  // bool input_locks[4] = {false, false, false}; // Input locks are replaced by sws_holding
  uint8_t re_a_last_dr = HIGH;
};

struct DMMenu_IIData {
  uint8_t current_selection = 0;
  bool made_selection = false;
  uint8_t effective_token;
};

struct MonoDMData {
  uint8_t dep_to;
  uint8_t mid; // Minigame id
  uint16_t code;
};

struct DMData {
  MonoDMData dms[3];
  uint8_t token;
  uint8_t timer_left = 60; // max 60 seconds to shut down?
  uint8_t current_operating_dmn = 0;

  void regenerate_token() {
    token = random(256);
  }
};

struct DMLockscreenData {
  int8_t status = 0; // 0 = hold, 1 = passed, -1 = failed
  uint8_t digit_idx = 0;
  uint8_t current_digit = 0;
  uint8_t locked_digits[4] = {0,0,0,0};

  uint16_t correct_digits;
};

struct DMID1Data {
  uint8_t effective_token;

  uint8_t phase = 0;
  uint8_t orders[6];

  int8_t blink_idx = 0;
  uint8_t so_idx = 0;

  int8_t selection_orders[6] = {-1, -1, -1, -1, -1, -1};
};


struct DMID2DialData {
  uint8_t position = 0;
  uint8_t wstart = 0;
  uint8_t wend = 0;
  uint8_t sensitivity = 0;
  uint8_t gravity = 0;

  static DMID2DialData shuffled_init(
    bool randomize_position = false,
    uint8_t rangemin = 10,
    uint8_t rangemax = 40,
    uint8_t sensmin = 10,
    uint8_t sensmax = 20,
    uint8_t gravmin = 4, // keep in mind; there are 5 tick per second
    uint8_t gravmax = 6
  ) {
    DMID2DialData ret {};
    ret.gravity = random(0, 256 - rangemax);
    ret.wend = ret.gravity + random(rangemin, rangemax + 1);
    ret.sensitivity = random(sensmin, sensmax + 1);
    ret.gravity = random(gravmin, min<uint8_t>(gravmax, ret.sensitivity) + 1);

    if (randomize_position) {
      ret.position = random(0, 256);
    }

    return ret;
  }
};

struct DMID2Data {
  uint8_t phase = 0;
  DMID2DialData dials[3];
  uint8_t effective_token;

  int64_t last_tick_us = -1;
  uint16_t sync_timer_left = 5000;
};


struct DMID3Data {
  uint8_t phase = 0;
  uint8_t effective_token;
  uint8_t node_idx = 0;
  int8_t nodes_status[3] = {S_HOLD, S_HOLD, S_HOLD};

  int64_t last_tick_us = -1;

  uint8_t position = 0;
  uint8_t acceleration = 0;
  uint8_t wstart = 0;
  uint8_t wend = 0;

  void shuffle_params(
    bool randomize_position = false,
    uint8_t rangemin = 10,
    uint8_t rangemax = 40,
    uint8_t accelmin = 10,
    uint8_t accelmax = 20
  ) {
    wstart = random(0, 256 - rangemax);
    wend = wstart + random(rangemin, rangemax + 1);
    acceleration = random(accelmin, accelmax + 1);

    if (randomize_position) {
      position = random(0, 256);
    }
  }
};


struct DMID4Data {
  uint8_t phase = 0;
  uint8_t effective_token = 0;
  char page[10][10];
  uint8_t code_digits_position[4][2];
  char digits[4];

  void implant_digits() {
    for (uint8_t i = 0; i < 4; i++)
      page[code_digits_position[i][0]][code_digits_position[i][1]] = digits[i];
  }

  void shuffle_page(bool implant = true) {
    for (uint8_t y = 0; y < 10; y++) {
      for (uint8_t x = 0; x < 10; x++) {
        page[y][x] = '0' + random(10);
      }
    }

    if (implant) {
      implant_digits();
    }
  }

  static DMID4Data init(const char* d) {   // d = 4 digit chars
    DMID4Data ret {};
    memcpy(ret.digits, d, 4);

    uint8_t ypool[10] = {0,1,2,3,4,5,6,7,8,9};

    for (uint8_t i = 0; i < 4; i++) {
      uint8_t j = i + random(10 - i);

      uint8_t tmp = ypool[i];
      ypool[i] = ypool[j];
      ypool[j] = tmp;

      ret.code_digits_position[i][0] = ypool[i];
      ret.code_digits_position[i][1] = random(10);
    }

    for (int8_t i = 1; i < 4; i++) {
      uint8_t key = ret.code_digits_position[i][0];
      int8_t j = i - 1;

      while (j >= 0 && ret.code_digits_position[j][0] > key) {
        ret.code_digits_position[j + 1][0] = ret.code_digits_position[j][0];
        j--;
      }

      ret.code_digits_position[j + 1][0] = key;
    }

    ret.shuffle_page();
    return ret;
  }
};


DMData construct_dmdata(
  uint8_t timer = 60,
  uint8_t dm_counts = 3,
  uint8_t available_minigames = 4
) {
  uint8_t dep_dm_limit_count = random(1, dm_counts);
  uint8_t dep_dm_count = 0;
  DMData packet {};

  for (uint8_t i = 0; i < dm_counts; i++) {
    packet.dms[i].code = random(1000, 9999);
    packet.dms[i].mid = random(available_minigames); // 0-indexed

    if (i > 0 && dep_dm_count < dep_dm_limit_count && random(0, 2)) {
      packet.dms[i].dep_to = random(i);
      dep_dm_count++;
    } else {
      packet.dms[i].dep_to = 0;
    }
  }

  packet.regenerate_token();

  return packet;
}

void buzzhandler(
  RingmodeData *buzzingdata,
  bool write_tone = true,
  uint8_t channel = BUZZER_CHANNEL,
  int16_t minfreq = 800,
  int16_t maxfreq = 2000,
  int8_t positive_delta = 20,
  int8_t negative_delta = -15,
  uint16_t delta_delay_us = 10000
) {
  const int8_t deltamap[2] = {negative_delta, positive_delta};

  if (buzzingdata->buzzer_state == -1) {
    buzzingdata->current_freq = minfreq;
    buzzingdata->direction = 1;

    if (write_tone) {
      ledcWriteTone(BUZZER_CHANNEL, 0);
    }
    return;
  };

  if (esp_timer_get_time() - buzzingdata->last_delta_us >= delta_delay_us) {
    buzzingdata->current_freq += deltamap[buzzingdata->direction];
    
    if (write_tone) {
      ledcWriteTone(channel, buzzingdata->current_freq);
    }
    
    if (buzzingdata->current_freq <= minfreq || buzzingdata->current_freq >= maxfreq) {
      buzzingdata->direction = !buzzingdata->direction;
    }
    
    buzzingdata->last_delta_us = esp_timer_get_time();
  }
}

TimestepReturn step_time_now(
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


// Inputs
// RE things
volatile int8_t re_delta = 0;
volatile uint64_t re_last_us = 0;

void IRAM_ATTR re_isr() {
  uint64_t now = esp_timer_get_time();

  if (now - re_last_us < 1500) return;
  if (digitalRead(PIN_RE_B) == HIGH) { // PIN_RE_A aren't checked because it's the condition for the interrupt
    re_delta++;
  } else {
    re_delta--;
  }
}

portMUX_TYPE re_mux = portMUX_INITIALIZER_UNLOCKED;

void register_inputs(InputData *idata) {
  const uint8_t swsmap[3] = {PIN_SW1, PIN_SW2, PIN_SW3};

  idata->re_clockwise = false;
  idata->re_counterclockwise = false;
  idata->any_sw_clicked = false;

  for (uint8_t i=0; i<3; i++) {
    idata->sws_clicked[i] = false;

    bool pressed = digitalRead(swsmap[i]) == LOW;
    idata->sws_clicked[i] = idata->sws_holding[i] && !pressed;
    idata->sws_holding[i] = pressed;
    
    idata->any_sw_clicked = idata->any_sw_clicked || idata->sws_clicked[i];
  }

  // RE THINGS
  int8_t delta;

  bool pressed = digitalRead(PIN_RE_CLICK) == LOW;
  idata->re_bttn_clicked = idata->re_bttn_holding && !pressed;
  idata->re_bttn_holding = pressed;

  portENTER_CRITICAL(&re_mux);
  delta = re_delta;
  re_delta = 0;
  portEXIT_CRITICAL(&re_mux);

  if (delta > 0) {
    idata->re_clockwise = true;
  } else if (delta < 0) {
    idata->re_counterclockwise = true;
  }
}


void dmmenu_iinterpreter(DMMenu_IIData *idata_dmmenu, InputData *idata, uint8_t token, uint8_t dmcounts = 3) {
  if (idata_dmmenu->effective_token != token) {
    *idata_dmmenu = DMMenu_IIData {};
    idata_dmmenu->effective_token = token;
  }

  if (idata->sws_clicked[0]) {
    idata_dmmenu->current_selection = (idata_dmmenu->current_selection + dmcounts - 1) % dmcounts;
  } else if (idata->sws_clicked[2]) {
    idata_dmmenu->current_selection = (idata_dmmenu->current_selection + 1) % dmcounts;
  } else if (idata->sws_clicked[1]) {
    idata_dmmenu->made_selection = true;
  }
}


// Screen updates
void standard_screen_update(
  TimestepReturn *time,
  bool use_24h,
  bool alarm_active
) {}

void dmmenu_screen_update(
  TimestepReturn *time,
  bool use24h,
  uint8_t timer_left,
  uint8_t current_selection,
  uint8_t screen_state
) {}


void failure_screen(
  uint8_t timer_left
) {}


void success_screen (
  uint8_t timer_left,
  uint16_t code
) {}


void dmid1_screen_update(
  uint8_t timer_left,
  int8_t swid_to_blink = -1 // 0-indexed
) {}

void dmid2_screen_update(
  uint8_t timer_left,
  DMID2Data *dmiddata
) {}

void dmid3_screen_update(
  uint8_t timer_left,
  DMID3Data *dmiddata // I use pointer cuz I think it's more efficient
) {}

void dmid4_screen_update(
  uint8_t timer_left,
  char (*page)[10][10]
) {}


void lockscreen(DMLockscreenData *dmld, InputData *idata) {
  dmld->current_digit = (dmld->current_digit + idata->re_clockwise) % 10;
  dmld->current_digit = (dmld->current_digit + 10 - idata->re_counterclockwise) % 10;

  if (idata->re_bttn_clicked) {
    dmld->locked_digits[dmld->digit_idx] = dmld->current_digit;
    dmld->current_digit = 0;
    dmld->digit_idx++;
  }

  if (dmld->digit_idx >= 3) {
    char digits_str[4];
    int8_t new_status = 1;

    ultoa(dmld->correct_digits, digits_str, 10);

    for (uint8_t i=0; i<3; i++) {
      if (dmld->locked_digits[i] != digits_str[i]) {
        new_status = -1;
        break;
      }
    }

    dmld->digit_idx = 2; // kinda redundant?
    dmld->status = new_status;
  }

  // INSERT RENDER STEP HERE!!!
  // RENDER BLABLABLABLABLA
}


void ringing_screen_update(
  TimestepReturn *time,
  bool use24h,
  uint8_t state
) {
}


bool standard_dm_regular_update(uint8_t dmphase, uint8_t timer_left, uint16_t self_code) {
  if (dmphase == P_CLOSING_PASS) {
    success_screen(timer_left, self_code);
    return false;
    
  } else if (dmphase == P_CLOSING_FAIL) {
    failure_screen(timer_left);
    return false;
    
  } else {
    return true;
  }
}

uint8_t standard_dm_preintro(uint8_t timer_left, uint8_t depending_to, DMLockscreenData *dmlockscreen, InputData *idata) {
  if (depending_to != 0) {
    lockscreen(dmlockscreen, idata);

    switch (dmlockscreen->status) {
      case S_PASS:
        return S_PASS;
        break;
      
      case S_FAIL:
        // FAILURE
        failure_screen(timer_left);
        return S_FAIL;
        break;
      }

  } else {
    return S_PASS;
  }

  return S_HOLD;
}

bool standard_dm_intro(uint8_t token, bool sec_passed) {
  static uint8_t effective_token;
  static bool skip_this_pass = true;

  if (effective_token != token) {
    effective_token = token;
    skip_this_pass = true;
  }

  if (sec_passed && !skip_this_pass) {
    return true;
  }

  if (skip_this_pass) {
    skip_this_pass = false;
  }
  return false;
}

void standard_dm_closing(SysFlags *flags, bool any_input) {
  if (any_input) {
    flags->mode = ALARM_DMMENU;
  }
}


// SIMON BUTTON
void dmid1_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {
  static DMID1Data dmiddata {};
  static DMLockscreenData dmlockscreen {};
  static bool failure = false;

  uint16_t self_code = dmdata->dms[dmdata->current_operating_dmn].code;
  uint8_t depending_to = dmdata->dms[dmdata->current_operating_dmn].dep_to;
  
  if (dmiddata.effective_token != dmdata->token) {
    dmiddata = DMID1Data {};
    dmiddata.effective_token = dmdata->token;
    dmlockscreen = DMLockscreenData {};
    
    for (uint8_t i = 0; i<5; i++) {
      dmiddata.orders[i] = random(0, 3); // 0-indexed
    }

    if (depending_to != 0) {
      dmlockscreen.correct_digits = dmdata->dms[depending_to].code;
    }
  }
  
  
  if (second_passed) {
    if (standard_dm_regular_update(dmiddata.phase, dmdata->timer_left, self_code)) {
      dmid1_screen_update(dmdata->timer_left, -1);
    }
  }

  switch (dmiddata.phase) {
    case P_PREINTRO:
      switch (standard_dm_preintro(dmdata->timer_left, depending_to, &dmlockscreen, idata)) {
        case S_PASS:
          dmiddata.phase = P_INTRO;
          dmid1_screen_update(dmdata->timer_left, -1);
          delay(500);
          break;
        
        case S_FAIL:
          // failure_screen(dmdata->timer_left); ALREADY HANDLED INSIDE THE FUNCTION
          dmiddata.phase = P_CLOSING_FAIL;
          break;
      }
      break;
      
    case 1:
      delay(500);
      
      dmid1_screen_update(dmdata->timer_left, dmiddata.blink_idx);
      dmiddata.blink_idx++;

      if (dmiddata.blink_idx >= 6) {
        dmiddata.phase = P_MINIGAME;
      }
      break;
    
    case P_MINIGAME:
      for (uint8_t i = 0; i<3; i++) {
        if (idata->sws_clicked[i]) {
          dmiddata.selection_orders[dmiddata.so_idx] = i;
        }
      }
    
      
      if (idata->any_sw_clicked) {
        dmid1_screen_update(dmdata->timer_left, dmiddata.orders[dmiddata.so_idx]);
        
        for (uint8_t i = 0; i <= dmiddata.so_idx; i++) {
          if (dmiddata.orders[i] != dmiddata.selection_orders[i]) {
            // FAILURE CONDITION
            failure_screen(dmdata->timer_left);
            flags->mode = ALARM_DMMENU;
            delay(1000);
          }
        }

        dmiddata.so_idx++;

        if (dmiddata.so_idx >= 6) {
          dmiddata.phase = P_CLOSING_PASS;
          success_screen(dmdata->timer_left, self_code); // closing
        }
      }

      break;
    
    case P_CLOSING_PASS:
      // Fall through cuz P_CLOSING_PASS inst is the same as P_CLOSING_FAIL
    
    case P_CLOSING_FAIL:
      standard_dm_closing(flags, idata->any_sw_clicked);
      break;
  }
}


// FREQUENCY MATCH
void dmid2_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {
  // INITIATE VARIABLES
  const uint16_t max_sync_timer = 15000;
  const uint16_t ms_per_tick = 200;

  static DMID2Data dmiddata {};
  static DMLockscreenData dmlockscreen {};

  uint16_t self_code = dmdata->dms[dmdata->current_operating_dmn].code;
  uint8_t depending_to = dmdata->dms[dmdata->current_operating_dmn].dep_to;
  
  if (dmiddata.effective_token != dmdata->token) {
    dmiddata = DMID2Data {};
    dmiddata.effective_token = dmdata->token;
    
    dmlockscreen = DMLockscreenData {};
    if (depending_to != 0) {
      dmlockscreen.correct_digits = dmdata->dms[depending_to].code;
    }
    
    for (uint8_t i = 0; i < 3; i++) {
      dmiddata.dials[i] = DMID2DialData::shuffled_init();
    }

    dmiddata.last_tick_us = esp_timer_get_time();
  }


  if (second_passed) {
    if (standard_dm_regular_update(dmiddata.phase, dmdata->timer_left, self_code)) {
      dmid2_screen_update(dmdata->timer_left, &dmiddata);
    }
  }

  switch (dmiddata.phase) {
    case P_PREINTRO:
      switch(standard_dm_preintro(dmdata->timer_left, depending_to, &dmlockscreen, idata)) {
        case S_PASS:
          dmiddata.phase = P_INTRO;
          dmid2_screen_update(dmdata->timer_left, &dmiddata);
          break;

        case S_FAIL:
          // failure_screen(dmdata->timer_left);
          dmiddata.phase = P_CLOSING_FAIL;
      }
      break;

    case P_INTRO:
      if (standard_dm_intro(dmiddata.effective_token, second_passed)) {
        dmiddata.phase = P_MINIGAME;
      }
      break;
    
    
    case P_MINIGAME: {
      bool is_onplace[3] = {false, false, false};
      uint16_t ms_passed_s_tick = static_cast<uint16_t>((esp_timer_get_time() - dmiddata.last_tick_us) / 1000);

      for (uint8_t i = 0; i<3; i++) {
        is_onplace[i] = dmiddata.dials[i].position >= dmiddata.dials[i].wstart && dmiddata.dials[i].position <= dmiddata.dials[i].wend;
      }


      for (uint8_t i = 0; i<3; i++) {
        if (idata->sws_clicked[i]) {
          dmiddata.dials[i].position += dmiddata.dials[i].sensitivity;
        }
      }
      
      
      if (dmiddata.sync_timer_left <= 0) {
        dmiddata.phase = P_CLOSING_PASS;
        success_screen(dmdata->timer_left, self_code);

      } else if (dmiddata.sync_timer_left >= 15000) {
        dmiddata.phase = P_CLOSING_FAIL;
        failure_screen(dmdata->timer_left);
      }

      if (ms_passed_s_tick >= ms_per_tick) {
        // SINGLE TICK
        if (is_onplace[0] && is_onplace[1] && is_onplace[3]) {
          dmiddata.sync_timer_left -= ms_passed_s_tick;
        
        } else if (dmiddata.sync_timer_left >= 15000) {
          dmiddata.sync_timer_left += ms_passed_s_tick;
        }

        for (uint8_t i = 0; i<3; i++) {
          dmiddata.dials[i].position -= dmiddata.dials[i].gravity;
        }

        dmid2_screen_update(dmdata->timer_left, &dmiddata);
        dmiddata.last_tick_us = esp_timer_get_time();
      }

      break;
    }
    
    
    case P_CLOSING_PASS:
      // fall through

    case P_CLOSING_FAIL:
      if (idata->any_sw_clicked) {
          flags->mode = ALARM_DMMENU;
        }
      break;
  }
}


// HITPOINT
void dmid3_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {
  static DMID3Data dmiddata = DMID3Data {};
  static DMLockscreenData dmlockscreen {};
  const uint8_t ms_per_tick = 200;

  uint16_t self_code = dmdata->dms[dmdata->current_operating_dmn].code;
  uint8_t depending_to = dmdata->dms[dmdata->current_operating_dmn].dep_to;

  if (dmiddata.effective_token != dmdata->token) {
    dmiddata = DMID3Data {};
    dmiddata.effective_token = dmdata->token;

    dmiddata.shuffle_params();
    dmiddata.last_tick_us = esp_timer_get_time();

    dmlockscreen = DMLockscreenData {};

    if (depending_to != 0) {
      dmlockscreen.correct_digits = dmdata->dms[depending_to].code;
    }
  }

  if (second_passed) {
    if (standard_dm_regular_update(dmiddata.phase, dmdata->timer_left, self_code)) {
      dmid3_screen_update(dmdata->timer_left, &dmiddata);
    }
  }


  switch (dmiddata.phase) {
    case P_PREINTRO:
      switch (standard_dm_preintro(dmdata->timer_left, depending_to, &dmlockscreen, idata)) {
        case S_PASS:
          dmiddata.phase = P_INTRO;
          dmid3_screen_update(dmdata->timer_left, &dmiddata);
          break;
        
        case S_FAIL:
          dmiddata.phase = P_CLOSING_FAIL;
          // failure_screen(dmdata->timer_left);
          break;
      }
      break;
    
    case P_INTRO:
      if (idata->any_sw_clicked) {
        dmiddata.phase = P_MINIGAME;
      }
      break;
    
    case P_MINIGAME: {
      uint16_t ms_passed_s_tick = static_cast<uint16_t>((esp_timer_get_time() - dmiddata.last_tick_us) / 1000);

      if (ms_passed_s_tick >= ms_per_tick) {
        dmiddata.position += dmiddata.acceleration;

        if (idata->any_sw_clicked) {
          dmiddata.nodes_status[dmiddata.node_idx] = dmiddata.position >= dmiddata.wstart && dmiddata.position <= dmiddata.wend;
          dmiddata.node_idx++;
          dmiddata.position = 0;
          dmiddata.shuffle_params();
        }
        
        dmid3_screen_update(dmdata->timer_left, &dmiddata);
        dmiddata.last_tick_us = esp_timer_get_time();
      }

      if (dmiddata.node_idx >= 2) {
        bool is_success = true;

        for (uint8_t i = 0; i<3; i++) {
          if (dmiddata.nodes_status[i] == S_FAIL) {
            is_success = false;
            break;
          }
        }

        if (is_success) {
          dmiddata.phase = P_CLOSING_PASS;
          success_screen(dmdata->timer_left, self_code);
        } else {
          dmiddata.phase = P_CLOSING_FAIL;
          failure_screen(dmdata->timer_left);
        }
      }
      break;
    }
    
    case P_CLOSING_PASS:
      // Fall thtough
    
    case P_CLOSING_FAIL:
      standard_dm_closing(flags, idata->any_sw_clicked);
  }
}


void dmid4_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {
  static DMID4Data dmiddata {};
  static DMLockscreenData dmlockscreen{};

  uint16_t self_code = dmdata->dms[dmdata->current_operating_dmn].code;
  uint8_t depending_to = dmdata->dms[dmdata->current_operating_dmn].dep_to;

  if (dmiddata.effective_token != dmdata->token) {
    char digits_chr[4];
    ultoa(self_code, digits_chr, 10);

    dmiddata = DMID4Data::init(digits_chr);
    dmlockscreen = DMLockscreenData {};

    if (depending_to != 0) {
      dmlockscreen.correct_digits = dmdata->dms[depending_to].code;
    }
  }

  if (second_passed) {
    if (standard_dm_regular_update(dmiddata.phase, dmdata->timer_left, self_code)) {
      dmid4_screen_update(dmdata->timer_left, &(dmiddata.page));
    }
  }

  switch (dmiddata.phase) {
    case P_PREINTRO:
      switch (standard_dm_preintro(dmdata->timer_left, depending_to, &dmlockscreen, idata)) {
        case S_PASS:
          dmiddata.phase = P_INTRO;
          dmid4_screen_update(dmdata->timer_left, &(dmiddata.page));
          break;

        case S_FAIL:
          dmiddata.phase = P_CLOSING_FAIL;
          break;
      }
      break;

    case P_INTRO:
      // Nothing here, truly
      dmiddata.phase = P_MINIGAME;
      break;
    
    case P_MINIGAME:
      if (idata->sws_holding[0] || idata->sws_holding[1]) {
        dmiddata.shuffle_page();
        dmid4_screen_update(dmdata->timer_left, &(dmiddata.page));

      } else if (idata->sws_clicked[2]) {
        dmiddata.phase = 254;
      }
      break;
    
    case 254: // Special case
      flags->mode = ALARM_DMMENU;
      break;
  }
}


void (*dmo_map[])(bool, SysFlags*, DMData*, InputData*) = {
  dmid1_orchestrator,
  dmid2_orchestrator,
  dmid3_orchestrator,
  dmid4_orchestrator
};


void setup() {
  // Setup serial
  Serial.begin(115200);
  
  pinMode(PIN_RE_A, INPUT);
  pinMode(PIN_RE_B, INPUT);
  pinMode(PIN_RE_CLICK, INPUT_PULLUP);
  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_SW3, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_RE_A), re_isr, FALLING);
  
  Serial.println("HUMAN INPUT INTERFACE INITIATED");

  // initiate display
  tft.init(76, 284);
  // tft.setOffsets(82, 16);
  tft.invertDisplay(false);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);

  Serial.println("TFT DISPLAY INITIATED");
  pinMode(PIN_BUZZER, OUTPUT);
  ledcSetup(BUZZER_CHANNEL, BUZZER_MAX_FREQ, BUZZER_RESOLUTION);
  ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);

  Serial.println("HUMAN OUTPUT INTERFACE INITIATED");
  Serial.println("HELLO WORLD :3");
}


void loop() {
  // Static var def
  static DMData wardata;
  static SysFlags flags;
  static RingmodeData mdata_ring; // mdata = mode data
  
  static InputData idata;
  static DMMenu_IIData idata_dmmenu; 

  static SysConfig config {0, 0, 0, 0, 0};
  static uint8_t last_sec = 0;


  // UPDATES
  TimestepReturn t = step_time_now(
    config.s_offset,
    config.m_offset,
    config.h_offset
  );
  
  register_inputs(&idata);
  
  if (t.min == config.alarm_m && t.h24hour == config.alarm_h24 && flags.alarm_active && flags.mode < 200) {
    flags.mode = ALARM_RINGING;
    mdata_ring = RingmodeData {};
  }

  if (flags.mode == ALARM_RINGING && idata.any_sw_clicked) {
    wardata = construct_dmdata();
    flags.mode = ALARM_DMMENU;
  }

  if (flags.mode == ALARM_DMMENU) {
    wardata.regenerate_token();

    dmmenu_iinterpreter(&idata_dmmenu, &idata, wardata.token);

    if (idata_dmmenu.made_selection) {
      wardata.current_operating_dmn = idata_dmmenu.current_selection;
      flags.mode = ALARM_DMID1 + wardata.dms[idata_dmmenu.current_selection].mid;

      // Reset dmmenu
      idata_dmmenu = DMMenu_IIData {};

    } else if (t.sec != last_sec) {
      dmmenu_screen_update(
        &t,
        flags.use_24h,
        wardata.timer_left,
        idata_dmmenu.current_selection,
        mdata_ring.screen_state
      );
    }
  }


  if (t.sec != last_sec) {
    last_sec = t.sec;

    if (flags.mode == STANDARD) {
      standard_screen_update(
        &t,
        flags.use_24h,
        flags.alarm_active
      );

    
    } else if (flags.mode >= 210) {
      wardata.timer_left--;

      if (wardata.timer_left <= 0) {
        wardata = DMData {};
        flags.mode = ALARM_RINGING;
      }
    }
  }


  if (flags.mode >= 200) { // Status prefix 2 = war mode
    buzzhandler(&mdata_ring);

    if (t.sec != last_sec) {
      mdata_ring.screen_state = not mdata_ring.screen_state;
      ringing_screen_update(
        &t,
        flags.use_24h,
        mdata_ring.screen_state
      );
    }

    if (flags.mode >= ALARM_DMID1) {
      dmo_map[flags.mode - ALARM_DMID1](t.sec != last_sec, &flags, &wardata, &idata);
    }
  }
}