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
  uint8_t screen_state = 0;
  uint8_t buzzer_state = 0;
  unsigned int buzzing_duration = 0;
};


struct InputData {
  bool re_a_active = false;
  bool re_b_active = false;
  bool re_click_active = false;
  bool sws_clicked[3] = {false, false, false};
  bool sws_holding[3] = {false, false, false};
  bool input_locks[4];
};


struct DMMenu_IIData {
  uint8_t current_selection = 0;
  uint8_t made_selection = 0;
  uint8_t effective_token;
};

struct DMID1_IIData {};
struct DMID2_IIData {};
struct DMID3_IIData {};
struct DMID4_IIData {};


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
};

struct DMLockscreenData {
  uint16_t correct_digits;
  uint8_t digits[4] = {0,0,0,0};
  int8_t status = 0; // 0 = hold, 1 = passed, -1 = failed
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

  static DMID2DialData random_init(
    bool randomize_position = false,
    uint8_t rangemin = 10,
    uint8_t rangemax = 40,
    uint8_t sensmin = 10,
    uint8_t sensmax = 20,
    uint8_t gravmin = 4, // keep in mind; there are 5 tick per second
    uint8_t gravmax = 6
  ) {
    DMID2DialData ret;
    ret.wstart = random(0, 256 - rangemax);
    ret.wend = ret.wstart + random(rangemin, rangemax + 1);
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

  uint64_t last_iter_us = esp_timer_get_time();
  uint16_t sync_timer_left = 5000;
};


struct DMID3Data {};
struct DMID4Data {};


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
      packet.dms[i].dep_to = random(i) + 1;
      dep_dm_count++;
    } else {
      packet.dms[i].dep_to = 0;
    }
  }

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


// Inputs
InputData read_inputs() {
}

DMMenu_IIData dmmenu_iinterpreter(DMMenu_IIData *idata_dmmenu, uint8_t dmcounts = 3) {

} // iinterpreter = input interpreter


DMID1_IIData dmid1_iinterpreter(InputData *idata) {

}


DMID2_IIData dmid2_iinterpreter(InputData *idata) {

}


DMID3_IIData dmid3_iinterpreter(InputData *idata) {

}


DMID4_IIData dmid4_iinterpreter(InputData *idata) {

}


// Screen updates
void standard_screen_update(
  TimestepReturn *time,
  bool use_24h,
  bool alarm_active
) {

}

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


void lockscreen(DMLockscreenData *dmld) {
}


void ringing_screen_update(
  TimestepReturn *time,
  bool use24h,
  uint8_t state
) {
}


// SIMON BUTTON GAME
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
    if (dmiddata.phase == P_CLOSING_PASS) {
      success_screen(dmdata->timer_left, self_code);
    
    } else if (!dmiddata.phase == P_CLOSING_FAIL) {
      failure_screen(dmdata->timer_left);

    } else {
      dmid1_screen_update(dmdata->timer_left, 0);
    }
  }


  switch (dmiddata.phase) {
    case P_PREINTRO:
      if (depending_to != 0) {
        lockscreen(&dmlockscreen);

        switch (dmlockscreen.status) {
          case S_PASS:
            dmiddata.phase = P_INTRO;
            delay(500);
            break;
          
          case S_FAIL:
            // FAILURE
            failure_screen(dmdata->timer_left);
            dmiddata.phase = P_CLOSING_FAIL;
            break;
          }

      } else {
        dmiddata.phase = P_INTRO;
        delay(500);
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
    
      
      if (idata->sws_clicked[0] || idata->sws_clicked[1] || idata->sws_clicked[2]) {
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
      if (idata->sws_clicked[0] || idata->sws_clicked[1] || idata->sws_clicked[2]) {
        flags->mode = ALARM_DMMENU;
      }
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
  
  dmiddata.last_iter_us = esp_timer_get_time();

  if (second_passed) {
    if (dmiddata.phase == P_CLOSING_PASS) {
      success_screen(dmdata->timer_left, self_code);
    } else if (dmiddata.phase == P_CLOSING_FAIL) {
      failure_screen(dmdata->timer_left);
    } // No need to do dmid2_screen_update here, that's already regularly updated
  }


  if (dmiddata.effective_token != dmdata->token) {
    dmiddata = DMID2Data {};
    dmiddata.effective_token = dmdata->token;

    dmlockscreen = DMLockscreenData {};
    dmlockscreen.correct_digits = (*dmdata->dms)[depending_to].code;

    for (uint8_t i = 0; i < 3; i++) {
      dmiddata.dials[i] = DMID2DialData::random_init();
    }
  }

  switch (dmiddata.phase) {
    case P_PREINTRO:
      if (depending_to != 0) {
        lockscreen(&dmlockscreen);
        switch (dmlockscreen.status) {
          case S_PASS:
            dmiddata.phase = P_INTRO;
            delay(500);
            break;
          
          case S_FAIL:
            // FAILURE
            failure_screen(dmdata->timer_left);
            dmiddata.phase = P_CLOSING_FAIL;
            break;
          }

      } else {
        dmiddata.phase = P_INTRO;
      }
      break;

    case P_INTRO:
      delay(1000); // NO INTRO (just giving the player some room to breathe)
      dmiddata.phase = P_MINIGAME; 
      break;
    
    
    case P_MINIGAME:
      bool is_onplace[3] = {false, false, false};
      uint16_t ms_passed = static_cast<int>((esp_timer_get_time() - dmiddata.last_iter_us) / 1000);

      for (uint8_t i = 0; i<3; i++) {
        is_onplace[i] = dmiddata.dials[i].position >= dmiddata.dials[i].wstart && dmiddata.dials[i].position <= dmiddata.dials[i].wend;
      }


      for (uint8_t i = 0; i<3; i++) {
        if (idata->sws_clicked[i]) {
          dmiddata.dials[i].position += dmiddata.dials[i].sensitivity;
        }
      }


      if (is_onplace[0] && is_onplace[1] && is_onplace[3]) {
        dmiddata.sync_timer_left -= ms_passed;
      
      } else if (dmiddata.sync_timer_left >= 15000) {
        dmiddata.sync_timer_left += ms_passed;
      }


      if (dmiddata.sync_timer_left <= 0) {
        dmiddata.phase = P_CLOSING_PASS;
        success_screen(dmdata->timer_left, self_code);

      } else if (dmiddata.sync_timer_left >= 15000) {
        dmiddata.phase = P_CLOSING_FAIL;
        failure_screen(dmdata->timer_left);
      }

      if (ms_passed >= ms_per_tick) {
        // SINGLE TICK
        for (uint8_t i = 0; i<3; i++) {
          dmiddata.dials[i].position -= dmiddata.dials[i].gravity;
        }

        dmid2_screen_update(dmdata->timer_left, &dmiddata);
      }

      break;
    
    
    case P_CLOSING_PASS:
      // fall through

    case P_CLOSING_FAIL:
      if (idata->sws_clicked[0] || idata->sws_clicked[1] || idata->sws_clicked[2]) {
          flags->mode = ALARM_DMMENU;
        }
      break;
  }
}


void dmid3_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {}


void dmid4_orchestrator(bool second_passed, SysFlags *flags, DMData *dmdata, InputData *idata) {}


void (*dmo_map[])(bool, SysFlags*, DMData*, InputData*) = {
  dmid1_orchestrator,
  dmid2_orchestrator,
  dmid3_orchestrator,
  dmid4_orchestrator
};


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
  static DMData wardata;
  static SysFlags flags;
  static RingmodeData mdata_ring; // mdata = mode data
  
  static DMMenu_IIData idata_dmmenu; 

  static SysConfig config {0, 0, 0, 0, 0};
  static uint8_t last_sec = 0;


  // UPDATES
  TimestepReturn ret = tickhandler(
    config.s_offset,
    config.m_offset,
    config.h_offset
  );
  
  InputData idata = read_inputs();
  
  if (ret.min == config.alarm_m && ret.h24hour == config.alarm_h24 && flags.alarm_active && flags.mode < 200) {
    flags.mode = ALARM_RINGING;
    mdata_ring = RingmodeData {};
  }

  if (flags.mode == ALARM_RINGING && (idata.sws_clicked[0] || idata.sws_clicked[1] || idata.sws_clicked[2])) {
    wardata = construct_dmdata();
    flags.mode = ALARM_DMMENU;
  }

  if (flags.mode == ALARM_DMMENU) {
    if (idata_dmmenu.effective_token != wardata.token) {
      idata_dmmenu = DMMenu_IIData {};
      idata_dmmenu.effective_token = wardata.token;
    }

    idata_dmmenu = dmmenu_iinterpreter(&idata_dmmenu);

    if (idata_dmmenu.made_selection) {
      wardata.current_operating_dmn = idata_dmmenu.current_selection;
      flags.mode = ALARM_DMID1 + wardata.dms[idata_dmmenu.current_selection]->mid;

    } else if (ret.sec != last_sec) {
      dmmenu_screen_update(
        &ret,
        flags.use_24h,
        wardata.timer_left,
        idata_dmmenu.current_selection,
        mdata_ring.screen_state
      );
    }
  }


  if (ret.sec != last_sec) {
    last_sec = ret.sec;

    if (flags.mode == STANDARD) {
      standard_screen_update(
        &ret,
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
    mdata_ring.buzzer_state = buzzhandler(mdata_ring.buzzer_state, mdata_ring.buzzing_duration);

    if (ret.sec != last_sec) {
      mdata_ring.buzzing_duration += 1; // buzzing_duration variable is non-critical, so slight deviation is ok
      mdata_ring.screen_state = not mdata_ring.screen_state;
      ringing_screen_update(
        &ret,
        flags.use_24h,
        mdata_ring.screen_state
      );
  }

  if (flags.mode >= ALARM_DMID1) {
    dmo_map[flags.mode - ALARM_DMID1](ret.sec != last_sec, &flags, &wardata, &idata);
  }
}