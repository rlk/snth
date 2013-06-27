/*    Copyright (C) 2005 Robert Kooima                                       */
/*                                                                           */
/*    LIBSNTH is free software;  you can redistribute it and/or modify it    */
/*    under the terms of the  GNU General Public License  as published by    */
/*    the  Free Software Foundation;  either version 2 of the License, or    */
/*    (at your option) any later version.                                    */
/*                                                                           */
/*    This program is distributed in the hope that it will be useful, but    */
/*    WITHOUT  ANY  WARRANTY;  without   even  the  implied  warranty  of    */
/*    MERCHANTABILITY or  FITNESS FOR A PARTICULAR PURPOSE.   See the GNU    */
/*    General Public License for more details.                               */

#ifndef SNTH_H
#define SNTH_H

#include <stdint.h>

/*===========================================================================*/

#define SNTH_SYSEX 0x7D    /* Educational-use SysEx manufacturer ID */

enum {
    SNTH_WAVE_SIN,
    SNTH_WAVE_SQR,
    SNTH_WAVE_TRI,
    SNTH_WAVE_SAWU,
    SNTH_WAVE_WHT,
    SNTH_WAVE_SAWD,
    SNTH_WAVE_TRAP,
    SNTH_WAVE_DCHI,
    SNTH_WAVE_DCLO,
    SNTH_WAVE_RND,
};

enum {
    SNTH_ENV_LEVEL,
    SNTH_ENV_PITCH,
    SNTH_ENV_FILTER
};

enum {
    SNTH_MODE_OFF,
    SNTH_MODE_MIX,
    SNTH_MODE_MOD,
    SNTH_MODE_RNG
};

enum {
    SNTH_LPF,
    SNTH_HPF,
};

/*---------------------------------------------------------------------------*/

#define DEF_PATCH_NAME        "INIT PATCH"

#define DEF_TONE_WAVE         SNTH_WAVE_SIN
#define DEF_TONE_MODE         SNTH_MODE_OFF
#define DEF_TONE_LEVEL        100
#define DEF_TONE_PAN          64
#define DEF_TONE_DELAY        0

#define DEF_TONE_PITCH_COARSE 64
#define DEF_TONE_PITCH_FINE   64
#define DEF_TONE_PITCH_ENV    64

#define DEF_TONE_FILTER_MODE  SNTH_LPF
#define DEF_TONE_FILTER_CUT   127
#define DEF_TONE_FILTER_RES   0
#define DEF_TONE_FILTER_ENV   64
#define DEF_TONE_FILTER_KEY   64

#define DEF_ENV_A             10
#define DEF_ENV_D             10
#define DEF_ENV_S             100
#define DEF_ENV_R             10

#define DEF_LFO_WAVE          SNTH_WAVE_TRI
#define DEF_LFO_SYNC          1
#define DEF_LFO_RATE          64
#define DEF_LFO_DELAY         0
#define DEF_LFO_LEVEL         64
#define DEF_LFO_PAN           64
#define DEF_LFO_PITCH         64
#define DEF_LFO_PHASE         64
#define DEF_LFO_FILTER        64

#define DEF_CHANNEL_PATCH     0
#define DEF_CHANNEL_LEVEL     100
#define DEF_CHANNEL_PAN       0
#define DEF_CHANNEL_REVERB    0
#define DEF_CHANNEL_CHORUS    0

/*===========================================================================*/
/* Modifier functions                                                        */

void  snth_set_channel(uint8_t);
void  snth_set_patch  (uint8_t);
void  snth_set_bank   (uint8_t);

/*---------------------------------------------------------------------------*/

void  snth_set_channel_level (uint8_t);
void  snth_set_channel_pan   (uint8_t);
void  snth_set_channel_reverb(uint8_t);
void  snth_set_channel_chorus(uint8_t);

/*---------------------------------------------------------------------------*/

void  snth_set_patch_name(const char *);

/*---------------------------------------------------------------------------*/

void  snth_set_tone_wave (uint8_t, uint8_t);
void  snth_set_tone_mode (uint8_t, uint8_t);
void  snth_set_tone_level(uint8_t, uint8_t);
void  snth_set_tone_pan  (uint8_t, uint8_t);
void  snth_set_tone_delay(uint8_t, uint8_t);

void  snth_set_tone_pitch_coarse(uint8_t, uint8_t);
void  snth_set_tone_pitch_fine  (uint8_t, uint8_t);
void  snth_set_tone_pitch_env   (uint8_t, uint8_t);

void  snth_set_tone_filter_mode(uint8_t, uint8_t);
void  snth_set_tone_filter_cut (uint8_t, uint8_t);
void  snth_set_tone_filter_res (uint8_t, uint8_t);
void  snth_set_tone_filter_env (uint8_t, uint8_t);
void  snth_set_tone_filter_key (uint8_t, uint8_t);

void  snth_set_tone_env_a(uint8_t, uint8_t, uint8_t);
void  snth_set_tone_env_d(uint8_t, uint8_t, uint8_t);
void  snth_set_tone_env_s(uint8_t, uint8_t, uint8_t);
void  snth_set_tone_env_r(uint8_t, uint8_t, uint8_t);

void  snth_set_tone_lfo_wave  (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_sync  (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_rate  (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_delay (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_level (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_pan   (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_pitch (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_phase (uint8_t, uint8_t, uint8_t);
void  snth_set_tone_lfo_filter(uint8_t, uint8_t, uint8_t);

/*===========================================================================*/
/* Query functions                                                           */

uint8_t snth_get_channel(void);
uint8_t snth_get_patch  (void);
uint8_t snth_get_bank   (void);

/*---------------------------------------------------------------------------*/

uint8_t snth_get_channel_level (void);
uint8_t snth_get_channel_pan   (void);
uint8_t snth_get_channel_reverb(void);
uint8_t snth_get_channel_chorus(void);

/*---------------------------------------------------------------------------*/

const char *snth_get_patch_name(void);

/*---------------------------------------------------------------------------*/

uint8_t snth_get_tone_wave (uint8_t);
uint8_t snth_get_tone_mode (uint8_t);
uint8_t snth_get_tone_level(uint8_t);
uint8_t snth_get_tone_pan  (uint8_t);
uint8_t snth_get_tone_delay(uint8_t);

uint8_t snth_get_tone_pitch_coarse(uint8_t);
uint8_t snth_get_tone_pitch_fine  (uint8_t);
uint8_t snth_get_tone_pitch_env   (uint8_t);

uint8_t snth_get_tone_filter_mode(uint8_t);
uint8_t snth_get_tone_filter_cut (uint8_t);
uint8_t snth_get_tone_filter_res (uint8_t);
uint8_t snth_get_tone_filter_env (uint8_t);
uint8_t snth_get_tone_filter_key (uint8_t);

uint8_t snth_get_tone_env_a(uint8_t, uint8_t);
uint8_t snth_get_tone_env_d(uint8_t, uint8_t);
uint8_t snth_get_tone_env_s(uint8_t, uint8_t);
uint8_t snth_get_tone_env_r(uint8_t, uint8_t);

uint8_t snth_get_tone_lfo_wave  (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_sync  (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_rate  (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_delay (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_level (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_pan   (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_pitch (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_phase (uint8_t, uint8_t);
uint8_t snth_get_tone_lfo_filter(uint8_t, uint8_t);

/*===========================================================================*/
/* Control functions                                                         */

void snth_note_on (uint8_t, uint8_t, uint8_t);
void snth_note_off(uint8_t, uint8_t, uint8_t);

/*---------------------------------------------------------------------------*/

size_t snth_dump_patch(void *, size_t);
size_t snth_dump_state(void *, size_t);

int  snth_get_output(void *, size_t);
void snth_midi(const void *, size_t);

void snth_init(int);

/*===========================================================================*/

#endif
