#pragma once

#include "my_types.h"

constexpr u16 DYN_STREAM_ID = 3;

void dyn_playback_init(void);
void dyn_playback_feed(const u8* payload, u32 payload_len);
void dyn_playback_set_armed(bool armed);
bool dyn_playback_is_armed(void);

// JSON: rate_hz, bits (16), gain_db, mute (bool), clip (bool).
// Только когда dyn.off; иначе false.
bool dyn_playback_set_config_json(const char* json_object);
