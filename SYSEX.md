# MIDI Implementation and API mapping

    SYSEX

        Glob    0000----

            set channel           0000--00
            set bank              0000--01
            set patch             0000--02

        Chan    0001----

            set_channel_level     0001--00
            set_channel_pan       0001--01
            set_channel_reverb    0001--10
            set_channel_chorus    0001--11

        Effects 0010----

        Patch   0011----

            set_patch_name        0011---0

    Tone  11TT----

            set_tone_wave         11TT0000 0000wwww
            set_tone_mode         11TT0001
            set_tone_level        11TT0010
            set_tone_pan          11TT0011
            set_tone_delay        11TT0100

            End of Exclusive      11110111

            set_tone_pitch_coarse 11TT1000
            set_tone_pitch_fine   11TT1001
            set_tone_pitch_env    11TT1010

            set_tone_filter_mode  11TT1011
            set_tone_filter_cut   11TT1100
            set_tone_filter_res   11TT1101
            set_tone_filter_env   11TT1110
            set_tone_filter_key   11TT1111

    ENV   01TTEEKK

            E = 00  Level
                01  Pitch
                10  Filter
                11  -
            K = 00  A
                01  D
                10  S
                11  R

            set_tone_env_a        01TTEE00
            set_tone_env_d        01TTEE01
            set_tone_env_s        01TTEE10
            set_tone_env_r        01TTEE11

    LFO   10TTLKKK

            L =   0  LFO1
                  1  LFO2
            K = 000  wave/sync
                001  rate
                010  delay
                011  level
                100  pan
                101  pitch
                110  phase
                111  filter

            set_tone_lfo_wave     10TTL000 000swwww
            set_tone_lfo_rate     10TTL001
            set_tone_lfo_delay    10TTL010
            set_tone_lfo_level    10TTL011
            set_tone_lfo_pan      10TTL100
            set_tone_lfo_pitch    10TTL101
            set_tone_lfo_phase    10TTL110
            set_tone_lfo_filter   10TTL111
