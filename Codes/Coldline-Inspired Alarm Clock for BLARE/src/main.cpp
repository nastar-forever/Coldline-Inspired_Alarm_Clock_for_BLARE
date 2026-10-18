#include <Arduino.h>
#include <SPI.h>
#include <time.h>
#include <esp_timer.h>
#include "Adafruit_GFX.h"
#include "Adafruit_ST7789.h"

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
#define M_STANDBY 100
#define M_CONFIG 110
#define M_ALARM_RINGING 200
#define M_ALARM_DMMENU 210
#define M_ALARM_DMMENU_DEACTIVATION 211
#define M_ALARM_DMID1 221
#define M_ALARM_DMID2 222
#define M_ALARM_DMID3 223
#define M_ALARM_DMID4 224

// CONFIG POINTERS
#define C_CLOCK_H 10
#define C_CLOCK_M 11
#define C_CLOCK_S 12
#define C_CLOCK_AMPM 13
#define C_CLOCK_TRANSIT 14
#define C_CLOCK_CLOCKMODE 15
#define C_ALARM_H 20
#define C_ALARM_M 21
#define C_ALARM_AMPM 22
#define C_ALARM_TOGGLE 23
#define C_ALARM_TRANSIT 24
#define C_ALARM_CLOCKMODE 25

// MINIGAME PHASE
#define P_PREINTRO 0
#define P_INTRO 1
#define P_MINIGAME 2
#define P_CLOSING_PASS 253
#define P_CLOSING_FAIL 254
#define P_SPECIAL_CASE 255

// INPUT MAPS
#define I_SW_MOVE_UP 0
#define I_SW_SELECT 1
#define I_SW_MOVE_DOWN 2
#define I_SW_OPEN_CONFIG 1
#define I_SW_EXIT_CONFIG 1
#define I_SW_CONFIG_POINTER_INCREASE 0
#define I_SW_CONFIG_POINTER_DECREASE 2

// STATUS TEMPLATE
#define S_HOLD 0
#define S_PASS 1
#define S_FAIL -1
#define S_ACTIVE 1
#define S_DEACTIVE -1

// DEPENDING TO
#define D_NONE 255


// Create display entity
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK);

// Datacontainers
struct SysFlags {
  uint8_t mode = M_STANDBY;
  bool use_24h = false;
  bool alarm_active = false;
  bool alarm_rang_this_minute = false;
};


struct SysConfig {
  uint8_t s_offset = 0;
  uint8_t m_offset = 0;
  uint8_t h24h_offset = 0;
  uint8_t alarm_h24 = 0;
  uint8_t alarm_m = 0;
  uint8_t configure_pointer = C_CLOCK_H;
};

struct NowReturn {
  uint8_t sec;
  uint8_t min;
  uint8_t hour;
  uint8_t h24hour;
  bool is_am;
};

struct RingmodeData {
  uint8_t screen_state : 4 = 0;
  uint8_t direction : 3 = 1; // 1 = ascending, 0 = descending
  int8_t buzzer_state : 4 = S_DEACTIVE; // 1 = active; -1 = deactivate
  int16_t current_freq : 13 = BUZZER_MIN_FREQ;

  uint64_t last_delta_us = 0;

  void activate() {
    buzzer_state = S_ACTIVE;
  }

  void deactivate() {
    buzzer_state = S_DEACTIVE;
  }

  void toggle() {
    buzzer_state = buzzer_state * -1;
  }
};

struct InputData {
  int8_t re_delta = 0;
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
  uint8_t token; // 1-byte token is far more than enough.
  uint8_t timer_left = 60; // max 60 seconds to shut down?
  uint8_t current_operating_dmn = 0;

  void regenerate_token() {
    static uint8_t static_token = 0;
    static_token++;
    token = static_token;
  }
};

struct DeactivationData {
  uint8_t effective_token = 0;
  
  uint8_t code_idx = 0;
  uint8_t digit_idx = 0;
  uint8_t current_digit = 0;

  uint8_t locked_digits[3][4];
  uint16_t correct_codes[3];
};

struct DMLockscreenData {
  int8_t status = S_HOLD; // 0 = hold, 1 = passed
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

  uint64_t last_blink_us = 0;
  uint8_t blink_progression = 0;
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
    ret.sensitivity = random(sensmin, sensmax + 1);
    ret.gravity = random(gravmin, min<uint8_t>(gravmax, ret.sensitivity) + 1);
    ret.wstart = random(0, 256 - rangemax);
    ret.wend = ret.wstart + random(rangemin, rangemax + 1);
    
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
  int16_t sync_timer_left = 5000;
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

  packet.timer_left = timer;

  for (uint8_t i = 0; i < dm_counts; i++) {
    packet.dms[i].code = random(1000, 9999);
    packet.dms[i].mid = random(available_minigames); // 0-indexed

    if (i > 0 && dep_dm_count < dep_dm_limit_count && random(0, 2)) {
      packet.dms[i].dep_to = random(i);
      dep_dm_count++;
    } else {
      packet.dms[i].dep_to = D_NONE;
    }
  }

  packet.regenerate_token(); // Previously, this line isn't exist, lol.

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

  if (buzzingdata->buzzer_state == S_DEACTIVE) {
    buzzingdata->current_freq = minfreq;
    buzzingdata->direction = 1;

    if (write_tone) {
      ledcWriteTone(channel, 0);
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

NowReturn time_now(
  uint8_t s_offset,
  uint8_t m_offset,
  uint8_t h24h_offset
) {
  int64_t now = esp_timer_get_time();
  int64_t total_sec = now / TIME_ACCURACY;
  total_sec += s_offset + m_offset*60 + h24h_offset*3600;
  total_sec %= 86400; // 3600*24

  NowReturn ret;
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

  re_last_us = now;
}

portMUX_TYPE re_mux = portMUX_INITIALIZER_UNLOCKED;

void register_inputs(InputData *idata) {
  const uint8_t swsmap[3] = {PIN_SW1, PIN_SW2, PIN_SW3};

  idata->re_delta = 0;
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

  idata->re_delta = delta;
}


void dmmenu_iinterpreter(DMMenu_IIData *idata_dmmenu, InputData *idata, uint8_t token, uint8_t dmcounts = 3) {
  if (idata_dmmenu->effective_token != token) {
    *idata_dmmenu = DMMenu_IIData {};
    idata_dmmenu->effective_token = token;
  }

  dmcounts++; // ACCOUNTS FOR "DEACTIVATE OPTION"

  if (idata->sws_clicked[I_SW_MOVE_UP]) {
    idata_dmmenu->current_selection = (idata_dmmenu->current_selection + dmcounts - 1) % dmcounts;
  } else if (idata->sws_clicked[I_SW_MOVE_DOWN]) {
    idata_dmmenu->current_selection = (idata_dmmenu->current_selection + 1) % dmcounts;
  } else if (idata->sws_clicked[I_SW_SELECT]) {
    idata_dmmenu->made_selection = true;
  }
}


// Screen updates
void standby_screen_update(
  NowReturn *time,
  bool use_24h,
  bool alarm_active
) {}

void dmmenu_screen_update(
  NowReturn *time,
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

void deactivation_screen_update(
  uint8_t timer_left,
  DeactivationData *ddata
) {}


int cap_re_delta_change(int x, int8_t delta, int n) {
  return ((x + delta) % n + n) % n;
}


void lockscreen_orchestrator(DMLockscreenData *dmld, InputData *idata) {
  dmld->current_digit = cap_re_delta_change(dmld->current_digit, idata->re_delta, 10);
  // dmld->current_digit = (dmld->current_digit + 10 + idata->re_delta) % 10;

  if (idata->re_bttn_clicked) {
    dmld->locked_digits[dmld->digit_idx] = dmld->current_digit;
    dmld->current_digit = 0;
    dmld->digit_idx++;
  }

  if (dmld->digit_idx >= 4) {
    char digits_str[5];
    int8_t new_status = S_PASS;

    ultoa(dmld->correct_digits, digits_str, 10);

    for (uint8_t i=0; i<4; i++) {
      if ('0' + dmld->locked_digits[i] != digits_str[i]) {
        new_status = S_FAIL;
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
  NowReturn *time,
  bool use24h,
  uint8_t state
) {
}


void config_screen_update(
  NowReturn *t,
  SysFlags *flags,
  SysConfig *config
) {}


bool standard_dm_regular_update(uint8_t dmphase, uint8_t timer_left, uint16_t self_code) {
  if (dmphase < P_INTRO) {
    return false;
  }

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

int8_t standard_dm_preintro(uint8_t timer_left, uint8_t depending_to, DMLockscreenData *dmlockscreen, InputData *idata) {
  if (depending_to != D_NONE) {
    lockscreen_orchestrator(dmlockscreen, idata);

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
    flags->mode = M_ALARM_DMMENU;
  }
}


// SIMON BUTTON
void dmid1_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {
  static DMID1Data dmiddata {};
  static DMLockscreenData dmlockscreen {};

  uint16_t self_code = dmdata->dms[dmdata->current_operating_dmn].code;
  uint8_t depending_to = dmdata->dms[dmdata->current_operating_dmn].dep_to;
  
  if (dmiddata.effective_token != dmdata->token) {
    dmiddata = DMID1Data {};
    dmiddata.effective_token = dmdata->token;
    dmlockscreen = DMLockscreenData {};
    
    for (uint8_t i = 0; i<6; i++) {
      dmiddata.orders[i] = random(0, 3); // 0-indexed
    }

    if (depending_to != D_NONE) {
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
          dmiddata.last_blink_us = esp_timer_get_time();
          break;
        
        case S_FAIL:
          // failure_screen(dmdata->timer_left); ALREADY HANDLED INSIDE THE FUNCTION
          dmiddata.phase = P_CLOSING_FAIL;
          break;
      }
      break;
      
    case P_INTRO: {
      const uint32_t us_per_blink = 8000000;
      uint64_t time_us = esp_timer_get_time();

      bool condition1 = time_us - dmiddata.last_blink_us >= us_per_blink;

      if (condition1 && !dmiddata.blink_progression) {
        dmid1_screen_update(dmdata->timer_left, dmiddata.orders[dmiddata.blink_idx]);
        
        dmiddata.blink_progression = 1;
        dmiddata.last_blink_us = time_us;

      } else if (condition1 && dmiddata.blink_progression == 1) {
        dmid1_screen_update(dmdata->timer_left, -1);
        dmiddata.blink_progression = 2;
        dmiddata.last_blink_us = time_us;
  
      } else if (condition1 && dmiddata.blink_progression == 2) {
        dmiddata.blink_progression = 0;
        dmiddata.blink_idx++;
      }

      if (dmiddata.blink_idx >= 6) {
        dmiddata.phase = P_MINIGAME;
      }
      break;
    }
    
    case P_MINIGAME: {
      const uint32_t us_per_blink = 8000000;
      uint64_t time_us = esp_timer_get_time();
      bool condition1 = time_us - dmiddata.last_blink_us >= us_per_blink;


      for (uint8_t i = 0; i<3; i++) {
        if (idata->sws_clicked[i]) {
          dmiddata.selection_orders[dmiddata.so_idx] = i;
        }
      }

      if (condition1 && dmiddata.blink_progression == 1) {
        dmid1_screen_update(dmdata->timer_left, -1);
        dmiddata.blink_progression = 0;
      }
    
      
      if (idata->any_sw_clicked) {
        dmiddata.blink_progression = 1;

        dmid1_screen_update(dmdata->timer_left, dmiddata.selection_orders[dmiddata.so_idx]);
        
        bool failed = false;
        for (uint8_t i = 0; i <= dmiddata.so_idx; i++) {
          if (dmiddata.orders[i] != dmiddata.selection_orders[i]) {
            failed = true;
            break;
          }
        }

        if (failed) {
          failure_screen(dmdata->timer_left);
          dmiddata.phase = P_CLOSING_FAIL;
          break;
        }

        dmiddata.so_idx++;

        if (dmiddata.so_idx >= 6) {
          dmiddata.phase = P_CLOSING_PASS;
          success_screen(dmdata->timer_left, self_code); // closing
        }
      }

      break;
    }

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
    if (depending_to != D_NONE) {
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
        dmiddata.last_tick_us = esp_timer_get_time();
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
        if (is_onplace[0] && is_onplace[1] && is_onplace[2]) {
          dmiddata.sync_timer_left -= ms_passed_s_tick;
        
        } else if (dmiddata.sync_timer_left <= max_sync_timer) {
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
          flags->mode = M_ALARM_DMMENU;
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

    if (depending_to != D_NONE) {
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

      if (idata->sws_clicked[I_SW_SELECT]) {
        dmiddata.nodes_status[dmiddata.node_idx] = dmiddata.position >= dmiddata.wstart && dmiddata.position <= dmiddata.wend ? S_PASS : S_FAIL;
        dmiddata.node_idx++;
        dmiddata.position = 0;
        dmiddata.shuffle_params();
      }

      if (ms_passed_s_tick >= ms_per_tick) {
        dmiddata.position += dmiddata.acceleration;
        
        dmid3_screen_update(dmdata->timer_left, &dmiddata);
        dmiddata.last_tick_us = esp_timer_get_time();
      }

      if (dmiddata.node_idx >= 3) {
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
    char digits_char[5];
    ultoa(self_code, digits_char, 10);

    dmiddata = DMID4Data::init(digits_char);
    dmlockscreen = DMLockscreenData {};

    if (depending_to != D_NONE) {
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
    
    case P_MINIGAME: {
      static uint64_t last_shuffle_us = 0;
      const int64_t shuffle_interval_us = 100000;

      if (idata->sws_holding[I_SW_MOVE_DOWN]) {
        uint64_t now = esp_timer_get_time();

        if (now - last_shuffle_us >= shuffle_interval_us) {
          last_shuffle_us = now;
          dmiddata.shuffle_page();
          dmid4_screen_update(dmdata->timer_left, &(dmiddata.page));
        }
      } else if (idata->sws_clicked[I_SW_SELECT]) {
        dmiddata.phase = P_SPECIAL_CASE;
      }

      break;
    }
    
    case P_SPECIAL_CASE: // Special case
      flags->mode = M_ALARM_DMMENU;
      break;

    case P_CLOSING_PASS:
      // Nothing
    
    case P_CLOSING_FAIL:
      standard_dm_closing(flags, idata->any_sw_clicked);
  }
}


void deactivation_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {
  static DeactivationData ddata {};
  static bool onfailure = false;

  if (ddata.effective_token != dmdata->token) {
    ddata = DeactivationData {};
    ddata.effective_token = dmdata->token;
    onfailure = false;

    for (uint8_t i = 0; i<3; i++) {
      ddata.correct_codes[i] = dmdata->dms[i].code;
    }
  }

  if (second_passed) {
    if (onfailure) {
      failure_screen(dmdata->timer_left);
      return;
    }

    deactivation_screen_update(dmdata->timer_left, &ddata);
  }

  if (idata->re_delta) {
    ddata.current_digit = cap_re_delta_change(ddata.current_digit, idata->re_delta, 10);
  }

  if (onfailure) {
    if (idata->any_sw_clicked) {
      flags->mode = M_ALARM_DMMENU;
    }

    return;
  }

  if (idata->re_bttn_clicked) {
    ddata.locked_digits[ddata.code_idx][ddata.digit_idx] = ddata.current_digit;
    ddata.current_digit = 0;
    ddata.digit_idx++;

    if (ddata.digit_idx >= 4) {
      ddata.digit_idx = 0;
      ddata.code_idx++;

      if (ddata.code_idx >= 3) {
        bool pass = true;

        for (uint8_t ci = 0; ci<3; ci++) {
          if (!pass) break;

          char code_digits_str[5];
          ultoa(ddata.correct_codes[ci], code_digits_str, 10);

          for (uint8_t di = 0; di<4; di++) {
            if (code_digits_str[di] != '0' + ddata.locked_digits[ci][di]) {
              pass = false;
              break;
            }
          }
        }

        if (pass) {
          flags->mode = M_STANDBY;
        } else {
          onfailure = true;
          failure_screen(dmdata->timer_left);
        }
      }
    }

    deactivation_screen_update(dmdata->timer_left, &ddata);
  }
}


void config_orchestrator(SysConfig *config, SysFlags *flags, InputData *idata) {
  if (!idata->any_sw_clicked && !idata->re_delta) {
    return;
  }

  uint8_t cpntr = config->configure_pointer;
  bool in_alarm = cpntr >= C_ALARM_H;

  uint8_t base = in_alarm ? C_ALARM_H : C_CLOCK_H;
  uint8_t last = in_alarm ? C_ALARM_CLOCKMODE : C_CLOCK_CLOCKMODE;
  uint8_t size = last - base + 1;
  uint8_t idx = cpntr - base;
  // uint8_t additor = config->configure_pointer >= C_ALARM_H ? C_ALARM_H : C_CLOCK_H;
  // uint8_t capper = additor == C_ALARM_H ? C_ALARM_CLOCKMODE : C_CLOCK_CLOCKMODE; 
  
  if (idata->sws_clicked[I_SW_CONFIG_POINTER_INCREASE]) {
    idx = (idx + 1) % size;

  } else if (idata->sws_clicked[I_SW_CONFIG_POINTER_DECREASE]) {
    idx = (idx + size - 1) % size;

  } else if (idata->sws_clicked[I_SW_EXIT_CONFIG]) {
    flags->mode = M_STANDBY;
    NowReturn t = time_now(config->s_offset, config->m_offset, config->h24h_offset);
    standby_screen_update(&t, flags->use_24h, flags->alarm_active);
    return;
  }

  config->configure_pointer = idx + base;

  if (idata->any_sw_clicked) {
    NowReturn t = time_now(config->s_offset, config->m_offset, config->h24h_offset);
    config_screen_update(&t, flags, config);
  }

  if ((config->configure_pointer == C_ALARM_AMPM || config->configure_pointer == C_CLOCK_AMPM) && flags->use_24h) {
    config->configure_pointer += idata->sws_clicked[I_SW_CONFIG_POINTER_INCREASE] ? 1 : -1;
  }

  if (!idata->re_delta) {
    return;
  }

  switch (config->configure_pointer) {
    case C_CLOCK_H:
      config->h24h_offset = cap_re_delta_change(config->h24h_offset, idata->re_delta, 24);
      break;

    case C_CLOCK_M:
      config->m_offset = cap_re_delta_change(config->m_offset, idata->re_delta, 60);
      break;
    
    case C_CLOCK_S:
      config->s_offset = cap_re_delta_change(config->s_offset, idata->re_delta, 60);
      break;
    
    case C_CLOCK_AMPM:
      config->h24h_offset = (config->h24h_offset + 12) % 24;
      break;
      
    case C_ALARM_H:
      config->alarm_h24 = cap_re_delta_change(config->alarm_h24, idata->re_delta, 24);
      break;

    case C_ALARM_M:
      config->alarm_m = cap_re_delta_change(config->alarm_m, idata->re_delta, 60);
      break;

    case C_ALARM_AMPM:
      config->alarm_h24 = (config->alarm_h24 + 12) % 24;
      break;

    case C_ALARM_TOGGLE:
      flags->alarm_active = !flags->alarm_active;
      break;

    case C_CLOCK_CLOCKMODE:
      // Fall through
    case C_ALARM_CLOCKMODE:
      flags->use_24h = !flags->use_24h;
      break;


    case C_CLOCK_TRANSIT:
      // Fall through
    case C_ALARM_TRANSIT:
      config->configure_pointer = config->configure_pointer < C_ALARM_H ? C_ALARM_TRANSIT : C_CLOCK_TRANSIT;
      break;
  }

  NowReturn t = time_now(config->s_offset, config->m_offset, config->h24h_offset);
  config_screen_update(&t, flags, config);
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
  static DMData wardata {};
  static SysFlags flags {};
  static SysConfig config {};
  static RingmodeData mdata_ring {}; // mdata = mode data
  
  static InputData idata {};
  static DMMenu_IIData idata_dmmenu {};

  static uint8_t last_sec = 0;
  static uint8_t last_min = 0;


  // UPDATES
  NowReturn t = time_now(
    config.s_offset,
    config.m_offset,
    config.h24h_offset
  );
  
  register_inputs(&idata);


  if (t.min == config.alarm_m && t.h24hour == config.alarm_h24 && flags.alarm_active && flags.mode < 200 && !flags.alarm_rang_this_minute) {
    flags.mode = M_ALARM_RINGING;
    flags.alarm_rang_this_minute = true;
    mdata_ring = RingmodeData {};
  }


  if (flags.mode == M_STANDBY && idata.sws_clicked[I_SW_OPEN_CONFIG]) {
    flags.mode = M_CONFIG;
    config_screen_update(&t, &flags, &config);


  } else if (flags.mode == M_ALARM_RINGING && idata.any_sw_clicked) {
    wardata = construct_dmdata();
    flags.mode = M_ALARM_DMMENU;


  } else if (flags.mode == M_ALARM_DMMENU) {
    dmmenu_iinterpreter(&idata_dmmenu, &idata, wardata.token);

    if (idata_dmmenu.made_selection && idata_dmmenu.current_selection) { // if current selection == 0
      wardata.current_operating_dmn = idata_dmmenu.current_selection-1;
      flags.mode = M_ALARM_DMID1 + wardata.dms[idata_dmmenu.current_selection-1].mid;

      // Reset dmmenu (again, previosuly this line isn't exists)
      idata_dmmenu = DMMenu_IIData {};

    } else if (t.sec != last_sec) {
      dmmenu_screen_update(
        &t,
        flags.use_24h,
        wardata.timer_left,
        idata_dmmenu.current_selection,
        mdata_ring.screen_state
      );

    } else if (idata_dmmenu.made_selection) {
      flags.mode = M_ALARM_DMMENU_DEACTIVATION;
    }

  } else if (flags.mode == M_CONFIG) {
    config_orchestrator(&config, &flags, &idata);

  } else if (flags.mode == M_ALARM_DMMENU_DEACTIVATION) {
    deactivation_orchestrator(t.sec != last_sec, &flags, &wardata, &idata);

    if (flags.mode == M_ALARM_DMMENU) {
      wardata.regenerate_token();
    }
  }
  
  
  if (flags.mode >= M_ALARM_RINGING) { // Status prefix 2 = war mode
    mdata_ring.activate();

    buzzhandler(&mdata_ring);

    if (t.sec != last_sec && flags.mode == M_ALARM_RINGING) {
      mdata_ring.screen_state = not mdata_ring.screen_state;

      ringing_screen_update(
        &t,
        flags.use_24h,
        mdata_ring.screen_state
      );
    }

    if (flags.mode >= M_ALARM_DMID1) {
      dmo_map[flags.mode - M_ALARM_DMID1](t.sec != last_sec, &flags, &wardata, &idata);

      if (flags.mode == M_ALARM_DMMENU) { // Failure/success is marked by returning to DMMENU after DM turn
        wardata.regenerate_token();
      }
    }
  } else if (mdata_ring.buzzer_state == S_ACTIVE) {
    mdata_ring.deactivate();
    buzzhandler(&mdata_ring);
  }
  

  if (t.min != last_min) {
    flags.alarm_rang_this_minute = false;
    last_min = t.min;
  }
  
  if (t.sec != last_sec) {
    last_sec = t.sec;
  
    if (flags.mode == M_STANDBY) {
      standby_screen_update(
        &t,
        flags.use_24h,
        flags.alarm_active
      );
  
    } else if (flags.mode >= 210) {
      wardata.timer_left--;
  
      if (wardata.timer_left <= 0) {
        uint8_t token_tmp = wardata.token;
        wardata = DMData {};
        wardata.token = token_tmp;
        flags.mode = M_ALARM_RINGING;
      }
    } 
  }
}