/*    Copyright (C) 2005 Robert Kooima                                       */
/*                                                                           */
/*    SNTHGUI is free software;  you can redistribute it and/or modify it    */
/*    under the terms of the  GNU General Public License  as published by    */
/*    the  Free Software Foundation;  either version 2 of the License, or    */
/*    (at your option) any later version.                                    */
/*                                                                           */
/*    This program is distributed in the hope that it will be useful, but    */
/*    WITHOUT  ANY  WARRANTY;  without   even  the  implied  warranty  of    */
/*    MERCHANTABILITY or  FITNESS FOR A PARTICULAR PURPOSE.   See the GNU    */
/*    General Public License for more details.                               */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <gtk/gtk.h>
#include <sys/time.h>

#include "snth.h"

#define SEQ_NAME "LibSNTH GUI"
#define PERF 1
#define FOUT 0

/*---------------------------------------------------------------------------*/

static GMutex *mutex;

/*===========================================================================*/
/* PCM audio output                                                          */

/* Asking ALSA for a period size near a specific value seems to result in an */
/* arbitrarily odd period size output. However, asking for a specific period */
/* time gets you exactly what you want, no matter how odd the request.       */
/* Makes no sense, but if you want a specific size then ask for it by time.  */

#define PERIOD 512
#define BUFFER 1024
#define RATE   44100

static snd_pcm_uframes_t period_size = PERIOD;
static snd_pcm_uframes_t buffer_size = BUFFER;

static unsigned int      period_time = (1000000 * PERIOD / RATE);
static unsigned int      buffer_time = (1000000 * BUFFER / RATE);

static short *buffer;

static snd_pcm_t *pcm;
static snd_seq_t *seq;

/*---------------------------------------------------------------------------*/

static void do_snd_ck(const char *str, int err)
{
    if (err < 0)
    {
        fprintf(stderr, "ALSA error: '%s', %s\n", str, snd_strerror(err));
        exit(1);
    }
}

#define snd_ck(F)  do_snd_ck(#F, F);

/*---------------------------------------------------------------------------*/

static void pcm_xrun(int e)
{
    if (e == -EPIPE)
        printf("Pipe!\n");
    else
        printf("Something else!\n");

    snd_pcm_prepare(pcm);
}

#if PERF
static void pcm_chunk(snd_pcm_t *pcm, snd_pcm_sframes_t count)
{
    static int   N = 0;
    static int   n = 0;
    static float t = 0;

    struct timeval t0;
    struct timeval t1;

    int e;

    /* Get a chunk of audio from the synthesizer. */

    g_mutex_lock(mutex);
    gettimeofday(&t0, NULL);
    n += snth_get_output(buffer, count);
    gettimeofday(&t1, NULL);
    g_mutex_unlock(mutex);

    /* Send it to the PCM audio output. */

    if ((e = snd_pcm_writei(pcm, buffer, count)) < 0)
        pcm_xrun(e);

    /* Dump some performance statistics. */

    t += ((float) (t1.tv_sec  - t0.tv_sec) +
          (float) (t1.tv_usec - t0.tv_usec) / 1000000);

    if (N++ > 100)
    {
        fprintf(stderr, "%12.5f voices per sec.  Polyphony = %d\n",
                n / t, n / N);
        N = 0;
        n = 0;
        t = 0;
    }

#if FOUT
    fwrite(buffer, 4, count, stdout);
#endif
}
#else
static void pcm_chunk(snd_pcm_t *pcm, snd_pcm_sframes_t count)
{
    int e;

    /* Get a chunk of audio from the synthesizer. */

    g_mutex_lock(mutex);
    snth_get_output(buffer, count);
    g_mutex_unlock(mutex);

    /* Send it to the PCM audio output. */

    if ((e = snd_pcm_writei(pcm, buffer, count)) < 0)
        pcm_xrun(e);

#if FOUT
    fwrite(buffer, 4, count, stdout);
#endif
}
#endif
/*
static void snd_proc(snd_async_handler_t *handler)
*/
static void pcm_proc(void)
{
/*
    snd_pcm_t *pcm = snd_async_handler_get_pcm(handler);
*/
    int avail;

    /* Loop until all available PCM output buffer space is consumed. */

    for (avail  = (int) snd_pcm_avail_update(pcm);
         avail >= (int) period_size;
         avail  = (int) snd_pcm_avail_update(pcm))
    {
        snd_pcm_sframes_t foo = ((period_size > avail) ?
                                 avail : period_size);
        pcm_chunk(pcm, foo);
    }

    if (avail < 0)
        pcm_xrun(avail);
}

static void pcm_init(void)
{
/*
    snd_pcm_t           *pcm;
*/
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
/*
    snd_async_handler_t *handler;
*/
    snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
    snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

    unsigned int channels = 2;
    unsigned int rate     = RATE;

    int d = 1;

    snd_ck(snd_pcm_open(&pcm, "plughw:0,0", stream, 0));

    /* Set hardware parameters. */

    snd_ck(snd_pcm_hw_params_malloc(&hw));

    snd_ck(snd_pcm_hw_params_any                 (pcm, hw));
    snd_ck(snd_pcm_hw_params_set_access          (pcm, hw, access));
    snd_ck(snd_pcm_hw_params_set_format          (pcm, hw, format));
    snd_ck(snd_pcm_hw_params_set_channels        (pcm, hw, channels));
    snd_ck(snd_pcm_hw_params_set_rate_near       (pcm, hw, &rate, 0));

    snd_ck(snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buffer_time, &d));
    snd_ck(snd_pcm_hw_params_set_period_time_near(pcm, hw, &period_time, &d));
    snd_ck(snd_pcm_hw_params                     (pcm, hw));

    snd_ck(snd_pcm_hw_params_get_buffer_size(hw, &buffer_size));
    snd_ck(snd_pcm_hw_params_get_period_size(hw, &period_size, &d));

    snd_pcm_hw_params_free(hw);

    /* Set software parameters. */

    snd_ck(snd_pcm_sw_params_malloc(&sw));

    snd_ck(snd_pcm_sw_params_current            (pcm, sw));
    snd_ck(snd_pcm_sw_params_set_avail_min      (pcm, sw, period_size));
    snd_ck(snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer_size));
    snd_ck(snd_pcm_sw_params_set_xfer_align     (pcm, sw, 1));
    snd_ck(snd_pcm_sw_params                    (pcm, sw));

    snd_pcm_sw_params_free(sw);

    /* Acquire a working buffer. */

    buffer = (short *) calloc(period_size * channels,
                              snd_pcm_format_width(format) / 8);

    /* Begin processing audio. */
/*
    snd_ck(snd_async_add_pcm_handler(&handler, pcm, snd_proc, NULL));
*/
    pcm_chunk(pcm, period_size);
    pcm_chunk(pcm, period_size);
}

static void pcm_main(void)
{
    int n = snd_pcm_poll_descriptors_count(pcm);

    struct pollfd *fd;

    if ((fd = calloc(n, sizeof (struct pollfd))))
    {
        snd_pcm_poll_descriptors(pcm, fd, n);

        while (1)
            if (poll(fd, n, 100000) > 0)
                pcm_proc();
    }

}

/*===========================================================================*/
/* MIDI sequencer I/O                                                        */

static void seq_init(void)
{
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) == 0)
    {
        snd_seq_port_subscribe_t *subs;

        snd_seq_addr_t sender;
        snd_seq_addr_t dest;

        snd_seq_set_client_name(seq, SEQ_NAME);

        /* Create a writable port. */

        snd_seq_create_simple_port(seq, SEQ_NAME,
                                   SND_SEQ_PORT_CAP_WRITE |
                                   SND_SEQ_PORT_CAP_SUBS_WRITE,
                                   SND_SEQ_PORT_TYPE_APPLICATION);

        /* Bind 64:0->128:0.  TODO: generalize this. */

        sender.client =  64;
        sender.port   =   0;
        dest.client   = 128;
        dest.port     =   0;

        snd_seq_port_subscribe_alloca(&subs);

        snd_seq_port_subscribe_set_sender(subs, &sender);
        snd_seq_port_subscribe_set_dest  (subs, &dest);

        snd_seq_subscribe_port(seq, subs);
    }
}

static void seq_step(void)
{
    do
    {
        snd_seq_event_t *e;

        snd_seq_event_input(seq, &e);

        switch (e->type)
        {
        case SND_SEQ_EVENT_NOTEON:
            g_mutex_lock(mutex);

            snth_note_on(e->data.control.channel,
                         e->data.note.note,
                         e->data.note.velocity);

            g_mutex_unlock(mutex);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            g_mutex_lock(mutex);

            snth_note_off(e->data.control.channel,
                          e->data.note.note,
                          e->data.note.velocity);

            g_mutex_unlock(mutex);
            break;
        }

        snd_seq_free_event(e);
    }
    while (snd_seq_event_input_pending(seq, 0) > 0);
}

static void seq_main(void)
{
    int n = snd_seq_poll_descriptors_count(seq, POLLIN);

    struct pollfd *fd;

    if ((fd = calloc(n, sizeof (struct pollfd))))
    {
        snd_seq_poll_descriptors(seq, fd, n, POLLIN);

        while (1)
            if (poll(fd, n, 100000) > 0)
                seq_step();
    }

}

/*===========================================================================*/
/* GUI implementation                                                        */

static GtkObject *update;
static int        depth = 0;

static void update_event(void)
{
    depth++;
    g_signal_emit_by_name(update, "value_changed");
    depth--;
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    return FALSE;
}

static void destroy(GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
}

/*---------------------------------------------------------------------------*/
/* GObject user data manipulators.                                           */

static int idx[] = {
    0,  1,  2,  3,
    4,  5,  6,  7,
    8,  9,  10, 11,
    12, 13, 14, 15,
};

static void set_object_tone(GObject *object, int tone)
{
    g_object_set_data(object, "tone", idx + tone);
}

static void set_object_env(GObject *object, int env)
{
    g_object_set_data(object, "env", idx + env);
}

static void set_object_lfo(GObject *object, int lfo)
{
    g_object_set_data(object, "lfo", idx + lfo);
}

static int get_object_tone(GObject *object)
{
    return *((int *) g_object_get_data(object, "tone"));
}

static int get_object_env(GObject *object)
{
    return *((int *) g_object_get_data(object, "env"));
}

static int get_object_lfo(GObject *object)
{
    return *((int *) g_object_get_data(object, "lfo"));
}

/*---------------------------------------------------------------------------*/

static int mod_all(void)
{
    GdkModifierType state;

    if (gtk_get_current_event_state(&state))
        return (state & GDK_SHIFT_MASK) ? 1 : 0;

    return 0;
}

static int mod_def(void)
{
    GdkModifierType state;

    if (gtk_get_current_event_state(&state))
        return (state & GDK_CONTROL_MASK) ? 1 : 0;

    return 0;
}

/*---------------------------------------------------------------------------*/
/* Synth parameter setting callbacks.                                        */

static void set_bank(GtkAdjustment *adj, gpointer data)
{
    int8_t bank = (int8_t) gtk_adjustment_get_value(adj);

    /* Apply the adjustment value to the bank selection. */

    g_mutex_lock(mutex);
    snth_set_bank(bank);
    g_mutex_unlock(mutex);
}

static void set_patch(GtkAdjustment *adj, gpointer data)
{
    int8_t patch = (int8_t) gtk_adjustment_get_value(adj);

    /* Apply the adjustment value to the patch selection. */

    g_mutex_lock(mutex);
    snth_set_patch(patch);
    g_mutex_unlock(mutex);
}

/*---------------------------------------------------------------------------*/

static void set_patch_name(GtkEntry *entry, gpointer data)
{
    const char *name = gtk_entry_get_text(entry);

    /* Apply the text entry string to the patch name. */

    g_mutex_lock(mutex);
    snth_set_patch_name(name);
    g_mutex_unlock(mutex);
}

static void get_patch_name(GtkWidget *widget, GtkEntry *entry)
{
    const char *name;

    /* Apply the patch name to the  text entry string. */

    g_mutex_lock(mutex);
    name = snth_get_patch_name();
    g_mutex_unlock(mutex);

    gtk_entry_set_text(entry, name);
}

/*---------------------------------------------------------------------------*/

static void set_tone_wave(GtkComboBox *combo, gpointer data)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t wave = (int8_t) gtk_combo_box_get_active(combo);

    /* Apply the combo value to the tone wave. */

    g_mutex_lock(mutex);
    snth_set_tone_wave(tone, wave);
    g_mutex_unlock(mutex);
}

static void set_tone_mode(GtkComboBox *combo, gpointer data)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t mode = (int8_t) gtk_combo_box_get_active(combo);

    /* Apply the combo value to the tone mode. */

    g_mutex_lock(mutex);
    snth_set_tone_mode(tone, mode);
    g_mutex_unlock(mutex);
}

static void set_tone_level(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone  = (uint8_t) get_object_tone(G_OBJECT(value));
    int8_t level = (uint8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone level. */

    g_mutex_lock(mutex);
    snth_set_tone_level(tone, level);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, level);
}

static void set_tone_pan(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pan  = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone pan. */

    g_mutex_lock(mutex);
    snth_set_tone_pan(tone, pan);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, pan);
}

static void set_tone_delay(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone  = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t delay = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone delay. */

    g_mutex_lock(mutex);
    snth_set_tone_delay(tone, delay);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, delay);
}

/*---------------------------------------------------------------------------*/

static void set_tone_pitch_coarse(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone         = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pitch_coarse = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone coarse tuning. */

    g_mutex_lock(mutex);
    snth_set_tone_pitch_coarse(tone, pitch_coarse);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, pitch_coarse);
}

static void set_tone_pitch_fine(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone       = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pitch_fine = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone fine tuning. */

    g_mutex_lock(mutex);
    snth_set_tone_pitch_fine(tone, pitch_fine);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, pitch_fine);
}

static void set_tone_pitch_env(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone      = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pitch_env = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone pitch envelope. */

    g_mutex_lock(mutex);
    snth_set_tone_pitch_env(tone, pitch_env);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, pitch_env);
}

/*---------------------------------------------------------------------------*/

static void set_tone_filter_mode(GtkComboBox *combo, gpointer data)
{
    int8_t tone        = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t filter_mode = (int8_t) gtk_combo_box_get_active(combo);

    /* Apply the adjustment value to the tone filter mode. */

    g_mutex_lock(mutex);
    snth_set_tone_filter_mode(tone, filter_mode);
    g_mutex_unlock(mutex);
}

static void set_tone_filter_cut(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone       = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_cut = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone filter cutoff. */

    g_mutex_lock(mutex);
    snth_set_tone_filter_cut(tone, filter_cut);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, filter_cut);
}

static void set_tone_filter_res(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone       = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_res = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone filter resonance. */

    g_mutex_lock(mutex);
    snth_set_tone_filter_res(tone, filter_res);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, filter_res);
}

static void set_tone_filter_env(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone       = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_env = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone filter envelope. */

    g_mutex_lock(mutex);
    snth_set_tone_filter_env(tone, filter_env);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, filter_env);
}

static void set_tone_filter_key(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone       = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_key = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone filter key follow. */

    g_mutex_lock(mutex);
    snth_set_tone_filter_key(tone, filter_key);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, filter_key);
}

/*---------------------------------------------------------------------------*/

static void set_tone_env_a(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t a    = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone envelope attack. */

    g_mutex_lock(mutex);
    snth_set_tone_env_a(tone, env, a);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, a);
}

static void set_tone_env_d(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t d    = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone envelope decay. */

    g_mutex_lock(mutex);
    snth_set_tone_env_d(tone, env, d);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, d);
}

static void set_tone_env_s(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t s    = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone envelope sustain. */

    g_mutex_lock(mutex);
    snth_set_tone_env_s(tone, env, s);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, s);
}

static void set_tone_env_r(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t r    = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the tone envelope release. */

    g_mutex_lock(mutex);
    snth_set_tone_env_r(tone, env, r);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, r);
}

/*---------------------------------------------------------------------------*/

static void set_tone_lfo_wave(GtkComboBox *combo, gpointer data)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(combo));
    int8_t wave = (int8_t) gtk_combo_box_get_active(combo);

    /* Apply the combo value to the LFO wave. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_wave(tone, lfo, wave);
    g_mutex_unlock(mutex);
}

static void set_tone_lfo_sync(GtkToggleButton *check, gpointer data)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(check));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(check));
    int8_t sync = (int8_t) gtk_toggle_button_get_active(check);

    /* Apply the adjustment value to the LFO sync. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_sync(tone, lfo, sync);
    g_mutex_unlock(mutex);
}

static void set_tone_lfo_rate(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t rate = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO rate. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_rate(tone, lfo, rate);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, rate);
}

static void set_tone_lfo_delay(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone  = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo   = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t delay = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO delay. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_delay(tone, lfo, delay);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, delay);
}

static void set_tone_lfo_level(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone  = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo   = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t level = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO level send. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_level(tone, lfo, level);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, level);
}

static void set_tone_lfo_pan(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t pan  = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO pan send. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_pan(tone, lfo, pan);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, pan);
}

static void set_tone_lfo_pitch(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone  = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo   = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t pitch = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO pitch send. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_pitch(tone, lfo, pitch);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, pitch);
}

static void set_tone_lfo_phase(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone  = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo   = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t phase = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO phase send. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_phase(tone, lfo, phase);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, phase);
}

static void set_tone_lfo_filter(GtkAdjustment *value, GtkAdjustment *group)
{
    int8_t tone   = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo    = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t filter = (int8_t) gtk_adjustment_get_value(value);

    /* Apply the adjustment value to the LFO filter send. */

    g_mutex_lock(mutex);
    snth_set_tone_lfo_filter(tone, lfo, filter);
    g_mutex_unlock(mutex);

    if (mod_all()) gtk_adjustment_set_value(group, filter);
}

/*---------------------------------------------------------------------------*/

static void set_value_group(GtkAdjustment *group, GtkAdjustment *value)
{
    gtk_adjustment_set_value(value, gtk_adjustment_get_value(group));
}

static void set_combo_group(GtkComboBox *group, GtkComboBox *combo)
{
    gtk_combo_box_set_active(combo, gtk_combo_box_get_active(group));
}

/*---------------------------------------------------------------------------*/

static void get_tone_wave(GtkWidget *widget, GtkComboBox *combo)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t wave;

    /* Apply the LFO wave value to the combo. */

    g_mutex_lock(mutex);
    wave = snth_get_tone_wave(tone);
    g_mutex_unlock(mutex);

    gtk_combo_box_set_active(combo, wave);
}

static void get_tone_mode(GtkWidget *widget, GtkComboBox *combo)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t mode;

    /* Apply the LFO mode value to the combo. */

    g_mutex_lock(mutex);
    mode = snth_get_tone_mode(tone);
    g_mutex_unlock(mutex);

    gtk_combo_box_set_active(combo, mode);
}

static void get_tone_level(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t level;

    /* Apply the tone level value to the adjustment. */

    g_mutex_lock(mutex);
    level = snth_get_tone_level(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, level);
}

static void get_tone_pan(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pan;

    /* Apply the tone pan value to the adjustment. */

    g_mutex_lock(mutex);
    pan = snth_get_tone_pan(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, pan);
}

static void get_tone_delay(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t delay;

    /* Apply the tone delay value to the adjustment. */

    g_mutex_lock(mutex);
    delay = snth_get_tone_delay(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, delay);
}

static void get_tone_pitch_coarse(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pitch_coarse;

    /* Apply the tone coarse tuning value to the adjustment. */

    g_mutex_lock(mutex);
    pitch_coarse = snth_get_tone_pitch_coarse(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, pitch_coarse);
}

static void get_tone_pitch_fine(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pitch_fine;

    /* Apply the tone fine tuning value to the adjustment. */

    g_mutex_lock(mutex);
    pitch_fine = snth_get_tone_pitch_fine(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, pitch_fine);
}

static void get_tone_pitch_env(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t pitch_env;

    /* Apply the tone pitch envelope value to the adjustment. */

    g_mutex_lock(mutex);
    pitch_env = snth_get_tone_pitch_env(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, pitch_env);
}

static void get_tone_filter_mode(GtkWidget *widget, GtkComboBox *combo)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t filter_mode;

    /* Apply the tone filter mode value to the combo. */

    g_mutex_lock(mutex);
    filter_mode = snth_get_tone_filter_mode(tone);
    g_mutex_unlock(mutex);

    gtk_combo_box_set_active(combo, filter_mode);
}

static void get_tone_filter_cut(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_cut;

    /* Apply the tone filter cutoff value to the adjustment. */

    g_mutex_lock(mutex);
    filter_cut = snth_get_tone_filter_cut(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, filter_cut);
}

static void get_tone_filter_res(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_res;

    /* Apply the tone filter resonance value to the adjustment. */

    g_mutex_lock(mutex);
    filter_res = snth_get_tone_filter_res(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, filter_res);
}

static void get_tone_filter_env(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_env;

    /* Apply the tone filter envelope value to the adjustment. */

    g_mutex_lock(mutex);
    filter_env = snth_get_tone_filter_env(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, filter_env);
}

static void get_tone_filter_key(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t filter_key;

    /* Apply the tone filter key follow value to the adjustment. */

    g_mutex_lock(mutex);
    filter_key = snth_get_tone_filter_key(tone);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, filter_key);
}

/*---------------------------------------------------------------------------*/

static void get_tone_env_a(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t a;

    /* Apply the tone envelope attack value to the adjustment. */

    g_mutex_lock(mutex);
    a = snth_get_tone_env_a(tone, env);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, a);
}

static void get_tone_env_d(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t d;

    /* Apply the tone envelope decay value to the adjustment. */

    g_mutex_lock(mutex);
    d = snth_get_tone_env_d(tone, env);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, d);
}

static void get_tone_env_s(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t s;

    /* Apply the tone envelope sustain value to the adjustment. */

    g_mutex_lock(mutex);
    s = snth_get_tone_env_s(tone, env);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, s);
}

static void get_tone_env_r(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t env  = (int8_t) get_object_env (G_OBJECT(value));
    int8_t r;

    /* Apply the tone envelope release value to the adjustment. */

    g_mutex_lock(mutex);
    r = snth_get_tone_env_r(tone, env);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, r);
}

/*---------------------------------------------------------------------------*/

static void get_tone_lfo_wave(GtkWidget *widget, GtkComboBox *combo)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(combo));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(combo));
    int8_t wave;

    /* Apply the LFO wave value to the combo. */

    g_mutex_lock(mutex);
    wave = snth_get_tone_lfo_wave(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_combo_box_set_active(combo, wave);
}

static void get_tone_lfo_sync(GtkWidget *widget, GtkToggleButton *check)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(check));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(check));
    int8_t sync;

    /* Apply the LFO sync value to the check button. */

    g_mutex_lock(mutex);
    sync = snth_get_tone_lfo_sync(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_toggle_button_set_active(check, sync ? TRUE : FALSE);
}

static void get_tone_lfo_rate(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t rate;

    /* Apply the LFO rate value to the adjustment. */

    g_mutex_lock(mutex);
    rate = snth_get_tone_lfo_rate(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, rate);
}

static void get_tone_lfo_delay(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t delay;

    /* Apply the LFO delay value to the adjustment. */

    g_mutex_lock(mutex);
    delay = snth_get_tone_lfo_delay(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, delay);
}

static void get_tone_lfo_level(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t level;

    /* Apply the LFO level send value to the adjustment. */

    g_mutex_lock(mutex);
    level = snth_get_tone_lfo_level(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, level);
}

static void get_tone_lfo_pan(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t pan;

    /* Apply the LFO pan send value to the adjustment. */

    g_mutex_lock(mutex);
    pan = snth_get_tone_lfo_pan(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, pan);
}

static void get_tone_lfo_pitch(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t pitch;

    /* Apply the LFO pitch send value to the adjustment. */

    g_mutex_lock(mutex);
    pitch = snth_get_tone_lfo_pitch(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, pitch);
}

static void get_tone_lfo_phase(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t phase;

    /* Apply the LFO phase send value to the adjustment. */

    g_mutex_lock(mutex);
    phase = snth_get_tone_lfo_phase(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, phase);
}

static void get_tone_lfo_filter(GtkWidget *widget, GtkAdjustment *value)
{
    int8_t tone = (int8_t) get_object_tone(G_OBJECT(value));
    int8_t lfo  = (int8_t) get_object_lfo (G_OBJECT(value));
    int8_t filter;

    /* Apply the LFO filter send value to the adjustment. */

    g_mutex_lock(mutex);
    filter = snth_get_tone_lfo_filter(tone, lfo);
    g_mutex_unlock(mutex);

    gtk_adjustment_set_value(value, filter);
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_label(const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);

    /* Create a label using the give marked-up text. */

    gtk_label_set_markup(GTK_LABEL(label), text);
    gtk_widget_show(label);

    return label;
}

static GtkObject *new_value(gdouble def)
{
    return gtk_adjustment_new(def, 0, 127, 1, 10, 0);
}

static GtkObject *new_range(GObject *patch, GObject *group,
                            GCallback set,  GCallback get, gdouble def)
{
    /* Create a value adjustment. */

    GtkObject *value = new_value(def);

    /* Add handlers for getting and setting the value. */

    g_signal_connect(G_OBJECT(value), "value_changed",
                     G_CALLBACK(set), group);
    g_signal_connect(G_OBJECT(patch), "value_changed",
                     G_CALLBACK(get), value);
    g_signal_connect(G_OBJECT(group), "value_changed",
                     G_CALLBACK(set_value_group), value);

    return value;
}

static GtkWidget *new_scale(GtkAdjustment *value, const char *text)
{
    /* Created a labeled range slider. */

    GtkWidget *label = new_label(text);
    GtkWidget *group = gtk_vbox_new(FALSE, 0);
    GtkWidget *range = gtk_vscale_new(value);

    gtk_range_set_inverted(GTK_RANGE(range), TRUE);
    gtk_scale_set_digits  (GTK_SCALE(range), 0);

    gtk_box_pack_start(GTK_BOX(group), range, TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(group), label, FALSE, TRUE, 0);

    /* Display and return this widget group. */

    gtk_widget_show(label);
    gtk_widget_show(range);
    gtk_widget_show(group);

    return group;
}

static GtkWidget *new_wave(GObject *patch, GCallback set, GCallback get)
{
    /* Create a wave selection menu. */

    GtkWidget *combo = gtk_combo_box_new_text();

    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Sine");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Square");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Triangle");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Saw");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Noise");

    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

    /* Add handlers for getting and setting the value. */

    g_signal_connect(G_OBJECT(combo), "changed",       G_CALLBACK(set), NULL);
    g_signal_connect(G_OBJECT(patch), "value_changed", G_CALLBACK(get), combo);

    /* Display and return this widget. */

    gtk_widget_show(combo);

    return combo;
}

static GtkWidget *new_mode(GObject *patch, GCallback set, GCallback get)
{
    /* Create a mode selection menu. */

    GtkWidget *combo = gtk_combo_box_new_text();

    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Off");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Mix");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Mod");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Ring");

    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

    /* Add handlers for getting and setting the value. */

    g_signal_connect(G_OBJECT(combo), "changed",       G_CALLBACK(set), NULL);
    g_signal_connect(G_OBJECT(patch), "value_changed", G_CALLBACK(get), combo);

    /* Display and return this widget. */

    gtk_widget_show(combo);

    return combo;
}

static GtkWidget *new_filter(GObject *patch, GCallback set, GCallback get)
{
    /* Create a mode selection menu. */

    GtkWidget *combo = gtk_combo_box_new_text();

    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Low Pass");
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "High Pass");

    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

    /* Add handlers for getting and setting the value. */

    g_signal_connect(G_OBJECT(combo), "changed",       G_CALLBACK(set), NULL);
    g_signal_connect(G_OBJECT(patch), "value_changed", G_CALLBACK(get), combo);

    /* Display and return this widget. */

    gtk_widget_show(combo);

    return combo;
}

static GtkWidget *new_check(GObject *patch, GCallback set, GCallback get)
{
    /* Create a sync checkbox. */

    GtkWidget *check = gtk_check_button_new_with_label("Sync");

    /* Add handlers for getting and setting the value. */

    g_signal_connect(G_OBJECT(check), "toggled",       G_CALLBACK(set), NULL);
    g_signal_connect(G_OBJECT(patch), "value_changed", G_CALLBACK(get), check);

    /* Display and return this widget. */

    gtk_widget_show(check);

    return check;
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_tone_wave(GObject *patch, int tone)
{
    /* Create a new LFO wave menu. */

    GtkWidget *combo = new_wave(patch, G_CALLBACK(set_tone_wave),
                                       G_CALLBACK(get_tone_wave));
    set_object_tone(G_OBJECT(combo), tone);

    return combo;
}

static GtkWidget *new_tone_mode(GObject *patch, int tone)
{
    /* Create a new LFO wave menu. */

    GtkWidget *combo = new_mode(patch, G_CALLBACK(set_tone_mode),
                                       G_CALLBACK(get_tone_mode));
    set_object_tone(G_OBJECT(combo), tone);

    return combo;
}

static GtkWidget *new_tone_level(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_LEVEL);

    /* Create a new tone level adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_level),
                      G_CALLBACK(get_tone_level), DEF_TONE_LEVEL);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Level</small>");
}

static GtkWidget *new_tone_pan(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_PAN);

    /* Create a new tone pan adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_pan),
                      G_CALLBACK(get_tone_pan), DEF_TONE_PAN);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Pan</small>");
}

static GtkWidget *new_tone_delay(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_DELAY);

    /* Create a new tone delay adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_delay),
                      G_CALLBACK(get_tone_delay), DEF_TONE_DELAY);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Delay</small>");
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_tone_pitch_coarse(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_PITCH_COARSE);

    /* Create a new tone coarse tuning adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_pitch_coarse),
                      G_CALLBACK(get_tone_pitch_coarse),DEF_TONE_PITCH_COARSE);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Tune</small>");
}

static GtkWidget *new_tone_pitch_fine(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_PITCH_FINE);

    /* Create a new tone fine tuning adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_pitch_fine),
                      G_CALLBACK(get_tone_pitch_fine), DEF_TONE_PITCH_FINE);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Fine</small>");
}

static GtkWidget *new_tone_pitch_env(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_PITCH_ENV);

    /* Create a new tone pitch envelope adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_pitch_env),
                      G_CALLBACK(get_tone_pitch_env), DEF_TONE_PITCH_ENV);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Env</small>");
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_tone_filter_mode(GObject *patch, int tone)
{
    /* Create a new LFO wave menu. */

    GtkWidget *combo = new_filter(patch, G_CALLBACK(set_tone_filter_mode),
                                         G_CALLBACK(get_tone_filter_mode));
    set_object_tone(G_OBJECT(combo), tone);

    return combo;
}

static GtkWidget *new_tone_filter_cut(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_FILTER_CUT);

    /* Create a new tone filter cutoff adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_filter_cut),
                      G_CALLBACK(get_tone_filter_cut), DEF_TONE_FILTER_CUT);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Cut</small>");
}

static GtkWidget *new_tone_filter_res(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_FILTER_RES);

    /* Create a new tone filter resonance adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_filter_res),
                      G_CALLBACK(get_tone_filter_res), DEF_TONE_FILTER_RES);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Res</small>");
}

static GtkWidget *new_tone_filter_env(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_FILTER_ENV);

    /* Create a new tone filter envelope adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_filter_env),
                      G_CALLBACK(get_tone_filter_env), DEF_TONE_FILTER_ENV);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Env</small>");
}

static GtkWidget *new_tone_filter_key(GObject *patch, int tone)
{
    static GtkObject *group = NULL;
           GtkObject *value;

    if (group == NULL)
        group = new_value(DEF_TONE_FILTER_KEY);

    /* Create a new tone filter key follow adjustment. */

    value = new_range(patch, G_OBJECT(group),
                      G_CALLBACK(set_tone_filter_key),
                      G_CALLBACK(get_tone_filter_key), DEF_TONE_FILTER_KEY);
    set_object_tone(G_OBJECT(value), tone);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Key</small>");
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_tone_env_a(GObject *patch, int tone, int env)
{
    static GtkObject *group[3] = { NULL, NULL, NULL };
           GtkObject *value;

    if (group[env] == NULL)
        group[env] = new_value(DEF_ENV_A);

    /* Create a new tone filter key follow adjustment. */

    value = new_range(patch, G_OBJECT(group[env]),
                      G_CALLBACK(set_tone_env_a),
                      G_CALLBACK(get_tone_env_a), DEF_ENV_A);
    set_object_tone(G_OBJECT(value), tone);
    set_object_env (G_OBJECT(value), env);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>A</small>");
}

static GtkWidget *new_tone_env_d(GObject *patch, int tone, int env)
{
    static GtkObject *group[3] = { NULL, NULL, NULL };
           GtkObject *value;

    if (group[env] == NULL)
        group[env] = new_value(DEF_ENV_D);

    /* Create a new envelope decay adjustment. */

    value = new_range(patch, G_OBJECT(group[env]),
                      G_CALLBACK(set_tone_env_d),
                      G_CALLBACK(get_tone_env_d), DEF_ENV_D);
    set_object_tone(G_OBJECT(value), tone);
    set_object_env (G_OBJECT(value), env);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>D</small>");
}

static GtkWidget *new_tone_env_s(GObject *patch, int tone, int env)
{
    static GtkObject *group[3] = { NULL, NULL, NULL };
           GtkObject *value;

    if (group[env] == NULL)
        group[env] = new_value(DEF_ENV_S);

    /* Create a new envelope sustain adjustment. */

    value = new_range(patch, G_OBJECT(group[env]),
                      G_CALLBACK(set_tone_env_s),
                      G_CALLBACK(get_tone_env_s), DEF_ENV_S);
    set_object_tone(G_OBJECT(value), tone);
    set_object_env (G_OBJECT(value), env);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>S</small>");
}

static GtkWidget *new_tone_env_r(GObject *patch, int tone, int env)
{
    static GtkObject *group[3] = { NULL, NULL, NULL };
           GtkObject *value;

    if (group[env] == NULL)
        group[env] = new_value(DEF_ENV_R);

    /* Create a new envelope release adjustment. */

    value = new_range(patch, G_OBJECT(group[env]),
                      G_CALLBACK(set_tone_env_r),
                      G_CALLBACK(get_tone_env_r), DEF_ENV_R);
    set_object_tone(G_OBJECT(value), tone);
    set_object_env (G_OBJECT(value), env);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>R</small>");
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_tone_lfo_wave(GObject *patch, int tone, int lfo)
{
    /* Create a new LFO wave menu. */

    GtkWidget *combo = new_wave(patch, G_CALLBACK(set_tone_lfo_wave),
                                       G_CALLBACK(get_tone_lfo_wave));
    set_object_tone(G_OBJECT(combo), tone);
    set_object_lfo (G_OBJECT(combo), lfo);

    return combo;
}

static GtkWidget *new_tone_lfo_sync(GObject *patch, int tone, int lfo)
{
    /* Create a new LFO sync checkbox. */

    GtkWidget *check = new_check(patch, G_CALLBACK(set_tone_lfo_sync),
                                        G_CALLBACK(get_tone_lfo_sync));
    set_object_tone(G_OBJECT(check), tone);
    set_object_lfo (G_OBJECT(check), lfo);

    return check;
}

static GtkWidget *new_tone_lfo_rate(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_RATE);

    /* Create a new LFO rate adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_rate),
                      G_CALLBACK(get_tone_lfo_rate), DEF_LFO_RATE);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Rate</small>");
}

static GtkWidget *new_tone_lfo_delay(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_DELAY);

    /* Create a new LFO delay adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_delay),
                      G_CALLBACK(get_tone_lfo_delay), DEF_LFO_DELAY);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Delay</small>");
}

static GtkWidget *new_tone_lfo_level(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_LEVEL);

    /* Create a new LFO level adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_level),
                      G_CALLBACK(get_tone_lfo_level), DEF_LFO_LEVEL);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Level</small>");
}

static GtkWidget *new_tone_lfo_pan(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_PAN);

    /* Create a new LFO pan adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_pan),
                      G_CALLBACK(get_tone_lfo_pan), DEF_LFO_PAN);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Pan</small>");
}

static GtkWidget *new_tone_lfo_pitch(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_PITCH);

    /* Create a new LFO pitch adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_pitch),
                      G_CALLBACK(get_tone_lfo_pitch), DEF_LFO_PITCH);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Pitch</small>");
}

static GtkWidget *new_tone_lfo_phase(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_PHASE);

    /* Create a new LFO phase adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_phase),
                      G_CALLBACK(get_tone_lfo_phase), DEF_LFO_PHASE);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Phase</small>");
}

static GtkWidget *new_tone_lfo_filter(GObject *patch, int tone, int lfo)
{
    static GtkObject *group[2] = { NULL, NULL };
           GtkObject *value;

    if (group[lfo] == NULL)
        group[lfo] = new_value(DEF_LFO_FILTER);

    /* Create a new LFO filter adjustment. */

    value = new_range(patch, G_OBJECT(group[lfo]),
                      G_CALLBACK(set_tone_lfo_filter),
                      G_CALLBACK(get_tone_lfo_filter), DEF_LFO_FILTER);
    set_object_tone(G_OBJECT(value), tone);
    set_object_lfo (G_OBJECT(value), lfo);

    /* Return a slider using this adjustment. */

    return new_scale(GTK_ADJUSTMENT(value), "<small>Filter</small>");
}

/*---------------------------------------------------------------------------*/

static GtkWidget *new_tone_setup(GObject *patch, const char *str, int tone)
{
    /* Create tone level sliders. */

    GtkWidget *frame = gtk_frame_new(str);
    GtkWidget *vbox  = gtk_vbox_new(FALSE, 0);
    GtkWidget *hbox  = gtk_hbox_new(FALSE, 0);

    GtkWidget *mode   = new_tone_mode (patch, tone);
    GtkWidget *level  = new_tone_level(patch, tone);
    GtkWidget *pan    = new_tone_pan  (patch, tone);
    GtkWidget *delay  = new_tone_delay(patch, tone);

    /* Pack them into a labeled frame. */

    gtk_frame_set_label_widget(GTK_FRAME(frame), new_label(str));
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

    gtk_box_pack_start(GTK_BOX(hbox), level,  TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), pan,    TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), delay,  TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), mode, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE,  TRUE, 0);

    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* Show all and return the group. */

    gtk_widget_show(vbox);
    gtk_widget_show(hbox);
    gtk_widget_show(frame);

    return frame;
}

static GtkWidget *new_tone_pitch(GObject *patch, const char *str, int tone)
{
    /* Create tone pitch sliders. */

    GtkWidget *frame = gtk_frame_new(str);
    GtkWidget *vbox  = gtk_vbox_new(FALSE, 0);
    GtkWidget *hbox  = gtk_hbox_new(TRUE, 0);

    GtkWidget *wave   = new_tone_wave        (patch, tone);
    GtkWidget *coarse = new_tone_pitch_coarse(patch, tone);
    GtkWidget *fine   = new_tone_pitch_fine  (patch, tone);
    GtkWidget *env    = new_tone_pitch_env   (patch, tone);

    /* Pack them into a labeled frame. */

    gtk_frame_set_label_widget(GTK_FRAME(frame), new_label(str));
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

    gtk_box_pack_start(GTK_BOX(hbox), coarse, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), fine,   TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), env,    TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), wave, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE,  TRUE, 0);

    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* Show all and return the group. */

    gtk_widget_show(vbox);
    gtk_widget_show(hbox);
    gtk_widget_show(frame);

    return frame;
}

static GtkWidget *new_tone_filter(GObject *patch, const char *str, int tone)
{
    /* Create filter range sliders. */

    GtkWidget *frame = gtk_frame_new(str);
    GtkWidget *vbox  = gtk_vbox_new(FALSE, 0);
    GtkWidget *hbox  = gtk_hbox_new(TRUE,  0);
    GtkWidget *mode  = new_tone_filter_mode(patch, tone);
    GtkWidget *cut   = new_tone_filter_cut (patch, tone);
    GtkWidget *res   = new_tone_filter_res (patch, tone);
    GtkWidget *env   = new_tone_filter_env (patch, tone);
    GtkWidget *key   = new_tone_filter_key (patch, tone);

    /* Pack them into a labeled frame. */

    gtk_frame_set_label_widget(GTK_FRAME(frame), new_label(str));

    gtk_box_pack_start(GTK_BOX(hbox), cut, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), res, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), env, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), key, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), mode, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE,  TRUE, 0);

    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* Show all and return the group. */

    gtk_widget_show(vbox);
    gtk_widget_show(hbox);
    gtk_widget_show(frame);

    return frame;
}

static GtkWidget *new_tone_env(GObject *patch,
                               const char *str, int tone, int env)
{
    /* Create ADSR envelope range sliders. */

    GtkWidget *frame   = gtk_frame_new(str);
    GtkWidget *box     = gtk_hbox_new(TRUE, 0);
    GtkWidget *range_a = new_tone_env_a(patch, tone, env);
    GtkWidget *range_d = new_tone_env_d(patch, tone, env);
    GtkWidget *range_s = new_tone_env_s(patch, tone, env);
    GtkWidget *range_r = new_tone_env_r(patch, tone, env);

    /* Pack them into a labeled frame. */

    gtk_frame_set_label_widget(GTK_FRAME(frame), new_label(str));
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

    gtk_box_pack_start(GTK_BOX(box), range_a, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), range_d, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), range_s, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), range_r, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(frame), box);

    /* Show all and return the group. */

    gtk_widget_show(box);
    gtk_widget_show(frame);

    return frame;
}

static GtkWidget *new_tone_lfo(GObject *patch,
                               const char *str, int tone, int lfo)
{
    /* Create LFO controls. */

    GtkWidget *frame   = gtk_frame_new(str);
    GtkWidget *vbox    = gtk_vbox_new(FALSE, 0);
    GtkWidget *tbox    = gtk_hbox_new(FALSE, 0);
    GtkWidget *hbox    = gtk_hbox_new(FALSE, 0);
    GtkWidget *Lbox    = gtk_hbox_new(TRUE,  0);
    GtkWidget *Rbox    = gtk_hbox_new(TRUE,  0);

    GtkWidget *wave    = new_tone_lfo_wave  (patch, tone, lfo);
    GtkWidget *sync    = new_tone_lfo_sync  (patch, tone, lfo);
    GtkWidget *rate    = new_tone_lfo_rate  (patch, tone, lfo);
    GtkWidget *delay   = new_tone_lfo_delay (patch, tone, lfo);
    GtkWidget *level   = new_tone_lfo_level (patch, tone, lfo);
    GtkWidget *pan     = new_tone_lfo_pan   (patch, tone, lfo);
    GtkWidget *pitch   = new_tone_lfo_pitch (patch, tone, lfo);
    GtkWidget *phase   = new_tone_lfo_phase (patch, tone, lfo);
    GtkWidget *filter  = new_tone_lfo_filter(patch, tone, lfo);

    GtkWidget *sep = gtk_vseparator_new();

    /* Pack them into a labeled frame. */

    gtk_frame_set_label_widget(GTK_FRAME(frame), new_label(str));
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

    gtk_box_pack_start(GTK_BOX(Lbox), rate,   TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(Lbox), delay,  TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(Rbox), level,  TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(Rbox), pan,    TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(Rbox), pitch,  TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(Rbox), phase,  TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(Rbox), filter, TRUE,  TRUE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), Lbox,   TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sep,    FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), Rbox,   TRUE,  TRUE, 0);

    gtk_box_pack_start(GTK_BOX(tbox), wave,   TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tbox), sync,   FALSE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), tbox,   FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox,   TRUE,  TRUE, 0);

    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* Show all and return the group. */

    gtk_widget_show(sep);
    gtk_widget_show(vbox);
    gtk_widget_show(hbox);
    gtk_widget_show(Lbox);
    gtk_widget_show(Rbox);
    gtk_widget_show(tbox);
    gtk_widget_show(frame);

    return frame;
}

static GtkWidget *new_tone(GObject *patch, const char *str, int tone)
{
    GtkWidget *box    = gtk_hbox_new(FALSE, 1);
    GtkWidget *setup  = new_tone_setup (patch, str,      tone);
    GtkWidget *pitch  = new_tone_pitch (patch, "<small>Pitch</small>",  tone);
    GtkWidget *filter = new_tone_filter(patch, "<small>Filter</small>", tone);

    GtkWidget *lenv = new_tone_env(patch, "<small>Level Env</small>",  tone,0);
    GtkWidget *penv = new_tone_env(patch, "<small>Pitch Env</small>",  tone,1);
    GtkWidget *fenv = new_tone_env(patch, "<small>Filter Env</small>", tone,2);
    GtkWidget *lfo1 = new_tone_lfo(patch, "<small>LFO 0</small>",      tone,0);
    GtkWidget *lfo2 = new_tone_lfo(patch, "<small>LFO 1</small>",      tone,1);

    gtk_box_pack_start(GTK_BOX(box), setup,  TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), pitch,  TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), filter, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), lenv,   TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), penv,   TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), fenv,   TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), lfo1,   TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), lfo2,   TRUE, TRUE, 0);

    gtk_widget_show(box);

    return box;
}

static GtkWidget *new_patch(GObject *patch)
{
    GtkObject *bank = new_value(0);

    /* Build bank/patch selection controls. */

    GtkWidget *hbox = gtk_hbox_new(FALSE, 1);
    GtkWidget *bbox = gtk_hbox_new(FALSE, 1);
    GtkWidget *pbox = gtk_hbox_new(FALSE, 1);
    GtkWidget *nbox = gtk_hbox_new(FALSE, 1);

    GtkWidget *blabel = gtk_label_new("Bank");
    GtkWidget *plabel = gtk_label_new("Patch");
    GtkWidget *nlabel = gtk_label_new("Name");

    GtkWidget *bvalue = gtk_spin_button_new(GTK_ADJUSTMENT(bank),  1, 0);
    GtkWidget *pvalue = gtk_spin_button_new(GTK_ADJUSTMENT(patch), 1, 0);
    GtkWidget *nvalue = gtk_entry_new();

    /* Pack them. */

    gtk_box_pack_start(GTK_BOX(bbox), blabel, FALSE, TRUE, 2);
    gtk_box_pack_start(GTK_BOX(bbox), bvalue, TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pbox), plabel, FALSE, TRUE, 2);
    gtk_box_pack_start(GTK_BOX(pbox), pvalue, TRUE,  TRUE, 0);
    gtk_box_pack_start(GTK_BOX(nbox), nlabel, FALSE, TRUE, 2);
    gtk_box_pack_start(GTK_BOX(nbox), nvalue, TRUE,  TRUE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), bbox, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), pbox, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), nbox, TRUE,  TRUE, 0);

    /* Trigger updates to all widgets on bank/patch change. */

    g_signal_connect(G_OBJECT(bank), "value_changed",
                     G_CALLBACK(set_bank), NULL);
    g_signal_connect(G_OBJECT(patch), "value_changed",
                     G_CALLBACK(set_patch), NULL);

    g_signal_connect(G_OBJECT(nvalue), "activate",
                     G_CALLBACK(set_patch_name), NULL);
    g_signal_connect(G_OBJECT(nvalue), "focus-out-event",
                     G_CALLBACK(set_patch_name), NULL);
    g_signal_connect(G_OBJECT(patch), "value_changed",
                     G_CALLBACK(get_patch_name), nvalue);

    /* Show all and return the group. */

    gtk_widget_show(blabel);
    gtk_widget_show(plabel);
    gtk_widget_show(nlabel);
    gtk_widget_show(bvalue);
    gtk_widget_show(pvalue);
    gtk_widget_show(nvalue);

    gtk_widget_show(pbox);
    gtk_widget_show(bbox);
    gtk_widget_show(nbox);
    gtk_widget_show(hbox);

    return hbox;
}

static GtkWidget *gui_init(void)
{
    /* Build the patch editor GUI. */

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkObject *patch  = new_value(0);
    GtkWidget *box    = gtk_vbox_new(FALSE, 1);
    GtkWidget *head   = new_patch(G_OBJECT(patch));
    GtkWidget *tone0  = new_tone (G_OBJECT(patch), "<small>Tone 0</small>", 0);
    GtkWidget *tone1  = new_tone (G_OBJECT(patch), "<small>Tone 1</small>", 1);
    GtkWidget *tone2  = new_tone (G_OBJECT(patch), "<small>Tone 2</small>", 2);
    GtkWidget *tone3  = new_tone (G_OBJECT(patch), "<small>Tone 3</small>", 3);

    gtk_box_pack_start(GTK_BOX(box), head, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), tone0, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), tone1, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), tone2, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), tone3, TRUE, TRUE, 0);

    /* Add it to a top level window. */

    gtk_container_add(GTK_CONTAINER(window), box);

    gtk_window_set_title       (GTK_WINDOW(window), "Patch Editor");
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 768);

    g_signal_connect(G_OBJECT(window), "delete_event",
                     G_CALLBACK(delete_event), NULL);
    g_signal_connect(G_OBJECT(window), "destroy",
                     G_CALLBACK(destroy), NULL);

    gtk_widget_show(box);
    gtk_widget_show(window);

    /* Trigger an initial update to all widgets. */

    update = patch;
    update_event();

    return window;
}

/*---------------------------------------------------------------------------*/

static gpointer do_gui(gpointer data)
{
    gui_init();
    gtk_main();

    return NULL;
}

static gpointer do_pcm(gpointer data)
{
    pcm_init();
    pcm_main();

    return NULL;
}

static gpointer do_seq(gpointer data)
{
    seq_init();
    seq_main();

    return NULL;
}

/*---------------------------------------------------------------------------*/

#define MAXMIDI 65536

static void init(int argc, char *argv[])
{
    char midi[MAXMIDI];

    FILE  *fp;
    size_t sz;

    /* If a file was named on the command line, read SNTH state from it. */

    if ((argc > 1) && (fp = fopen(argv[1], "rb")))
    {
        if ((sz = fread(midi, 1, MAXMIDI, fp)) > 0)
            snth_midi(midi, sz);

        fclose(fp);
    }

    snth_set_channel(0);
    snth_set_bank   (0);
    snth_set_patch(0);
}

static void fini(int argc, char *argv[])
{
    char midi[MAXMIDI];

    FILE  *fp;
    size_t sz;

    /* If a file was named on the command line, write SNTH state to it. */

    if ((argc > 1) && (fp = fopen(argv[1], "wb")))
    {
        if ((sz = snth_dump_state(midi, MAXMIDI)) > 0)
            fwrite(midi, 1, sz, fp);

        fclose(fp);
    }
}

int main(int argc, char *argv[])
{
    GThread *pcm;
    GThread *seq;
    GThread *gui;

    /* Ready. */

    g_thread_init(NULL);

    mutex = g_mutex_new();

    /* Set. */

    gtk_init(&argc, &argv);
    snth_init(RATE);

    /* Go. */

    init(argc, argv);
    {
        pcm = g_thread_create(do_pcm, NULL, TRUE, NULL);
        seq = g_thread_create(do_seq, NULL, TRUE, NULL);
        gui = g_thread_create(do_gui, NULL, TRUE, NULL);

        g_thread_join(gui);
    }
    fini(argc, argv);

    return 0;
}
