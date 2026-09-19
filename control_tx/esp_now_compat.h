#pragma once

#include <stdint.h>
#include <esp_idf_version.h>
#include <esp_now.h>

// Keep the selected type in a header so Arduino's generated sketch prototypes
// also see the correct signature on each supported IDF version.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
using SoccerSendInfo = esp_now_send_info_t;
#else
using SoccerSendInfo = uint8_t;
#endif
