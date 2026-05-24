// variants/crowpanel_dis05020a_v11/target.h  (FULL FILE)

#pragma once
#include <Arduino.h>
#include <Mesh.h>

#include "CrowPanelBoard.h"
#include "helpers/radiolib/CustomSX1262.h"
#include "helpers/radiolib/CustomSX1262Wrapper.h"

extern CrowPanelBoard          board;
extern mesh::RTCClock&         rtc_clock;
extern CustomSX1262Wrapper     radio_driver;

bool     radio_init();
void     radio_set_params(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
void     radio_set_tx_power(int8_t dbm);
uint32_t radio_get_rng_seed();