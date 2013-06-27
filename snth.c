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

#define	_ISOC9X_SOURCE	1
#define _ISOC99_SOURCE	1

#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

#include "snth.h"

#define F2I(x) lrintf(x)
#define FRAC(x) ((x) - (int) (x))

/*===========================================================================*/

#define MAXFRAME   512
#define MAXCHANNEL  16
#define MAXPATCH   128
#define MAXPITCH   128
#define MAXNOTE    256
#define MAXSTR     256
#define MAXWAVE      5
#define MAXMODE      4
#define MAXTONE      4
#define MAXENV       3
#define MAXLFO       2
#define MAXSINE    256

#define NO_NOTE 0xFFFF

/*---------------------------------------------------------------------------*/

struct snth_env
{
    /* Envelope config */

    uint8_t a;
    uint8_t d;
    uint8_t s;
    uint8_t r;

    /* Envelope evaluator state cache */

    float am;
    float ab;
    float dm;
    float db;
    float sb;
    float rm;
    float rb;

    uint16_t flags;
};

struct snth_lfo
{
    /* LFO config */

    uint8_t wave;
    uint8_t sync;
    uint8_t rate;
    uint8_t delay;

    uint8_t level;
    uint8_t pan;
    uint8_t pitch;
    uint8_t phase;
    uint8_t filter;

    /* LFO evaluator state cache */

    float freq;
    float dm;

    uint16_t flags;
};

struct snth_tone
{
    /* Tone config */

    uint8_t wave;
    uint8_t mode;
    uint8_t level;
    uint8_t pan;
    uint8_t delay;

    /* Pitch config */

    uint8_t pitch_coarse;
    uint8_t pitch_fine;
    uint8_t pitch_env;

    /* Filter config */

    uint8_t filter_mode;
    uint8_t filter_cut;
    uint8_t filter_res;
    uint8_t filter_env;
    uint8_t filter_key;

    struct snth_env env[MAXENV];
    struct snth_lfo lfo[MAXLFO];

    uint16_t flags;
};

struct snth_patch
{
    char name[MAXSTR];

    struct snth_tone tone[MAXTONE];
};

/*---------------------------------------------------------------------------*/

/* Oscillator option flags. */

#define FL_ENV0   1
#define FL_ENV1   2
#define FL_ENV2   4
#define FL_LFO0   8
#define FL_LFO1   16
#define FL_PITCH  32
#define FL_PAN    64
#define FL_FILTER 128

struct snth_filter
{
    /* Filter evaluator state */

    float b0;
    float b1;
    float b2;
    float b3;
    float b4;
};

struct snth_osc
{
    /* Oscillator evaluator state */

    int   time;
    int   state;
    float rm[3];
    float rb[3];

    float osc_phase;
    float lfo_phase[2];

    struct snth_filter filter;
};

struct snth_note
{
    /* Note config */

    int     start;
    uint8_t pitch;
    uint8_t level;
    uint8_t chan;

    /* Note evaluator state */

    struct snth_osc osc[MAXTONE];
};

struct snth_channel
{
    /* Channel config */

    uint8_t patch;
    uint8_t level;
    uint8_t pan;
    uint8_t reverb;
    uint8_t chorus;

    uint16_t note[128];
};

/*---------------------------------------------------------------------------*/

struct snth_frame
{
    float L;
    float R;
};

/*===========================================================================*/
/* Global synthesizer state                                                  */

static int rate = 44100;

static float  sine_tab_k[MAXSINE];
static float  sine_tab_d[MAXSINE];
static float  freq_tab_k[MAXPATCH];
static float  freq_tab_d[MAXPATCH];

static uint16_t curr_note = 0;
static uint8_t  curr_chan = 0;
static int      curr_time = 0;

static float modula [MAXFRAME];
static float outputL[MAXFRAME];
static float outputR[MAXFRAME];

static struct snth_channel channel[MAXCHANNEL];
static struct snth_patch   patch  [MAXPATCH];
static struct snth_note    note   [MAXNOTE];

/*---------------------------------------------------------------------------*/

static void snth_set_tone_env_cache(uint8_t, uint8_t, uint8_t);
static void snth_set_tone_lfo_cache(uint8_t, uint8_t, uint8_t);

/*---------------------------------------------------------------------------*/
/* Convert from 7-bit MIDI values to [0,1], [-1,+1], or frame time.          */

#define TO_01(b) (b ? ((float) b -  1) / 126 :  0.0f)
#define TO_11(b) (b ? ((float) b - 64) /  63 : -1.0f)

#define TO_DT(b) (rate * 4 * TO_01(b) * TO_01(b))

/*===========================================================================*/

#ifdef __SSE__

static void vec_set(float *v, int n, float k)
{
    __m128 *dst = (__m128 *) v;
    __m128  val = _mm_set1_ps(k);

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
        dst[i] = val;
}

static void vec_acc(float *v, const float *w, int n, float k)
{
          __m128 *dst =       (__m128 *) v;
    const __m128 *src = (const __m128 *) w;

    __m128 mul = _mm_set1_ps(k);
    __m128 tmp;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        tmp    = _mm_mul_ps(src[i], mul);
        dst[i] = _mm_add_ps(dst[i], tmp);
    }
}

static void vec_add(float *v, const float *u, const float *w, int n)
{
          __m128 *dst  =       (__m128 *) v;
    const __m128 *src1 = (const __m128 *) u;
    const __m128 *src2 = (const __m128 *) w;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
        dst[i] = _mm_add_ps(src1[i], src2[i]);
}

static void vec_mul(float *v, const float *u, const float *w, int n)
{
          __m128 *dst  =       (__m128 *) v;
    const __m128 *src1 = (const __m128 *) u;
    const __m128 *src2 = (const __m128 *) w;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
        dst[i] = _mm_mul_ps(src1[i], src2[i]);
}

static void vec_fm(float *v, const float *u, const float *w, int n)
{
          __m128 *dst  =       (__m128 *) v;
    const __m128 *src1 = (const __m128 *) u;
    const __m128 *src2 = (const __m128 *) w;

    const __m128 v1 = _mm_set1_ps(1);

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
        dst[i] = _mm_mul_ps(src1[i], _mm_add_ps(src2[i], v1));
}

static void vec_mod(float *v, const float *w, int n, float k)
{
          __m128 *dst =       (__m128 *) v;
    const __m128 *src = (const __m128 *) w;

    __m128 mul = _mm_set1_ps(k);
    __m128 tmp;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        tmp    = _mm_mul_ps(src[i], mul);
        dst[i] = _mm_mul_ps(dst[i], tmp);
    }
}

static void vec_clamp(float *v, const float *w, int n, float k0, float k1)
{
          __m128 *dst =       (__m128 *) v;
    const __m128 *src = (const __m128 *) w;

    __m128 min = _mm_set1_ps(k0);
    __m128 max = _mm_set1_ps(k1);
    __m128 tmp;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
         tmp    = _mm_min_ps(src[i], max);
         dst[i] = _mm_max_ps(tmp,    min);
    }
}

#else /* __SSE __ */

static void vec_set(float *v, int n, float k)
{
    int i;

    for (i = 0; i < n; ++i)
        v[i] = k;
}

static void vec_acc(float *v, const float *w, int n, float k)
{
    int i;

    for (i = 0; i < n; ++i)
        v[i] += w[i] * k;
}

static void vec_mod(float *v, const float *w, int n, float k)
{
    int i;

    for (i = 0; i < n; ++i)
        v[i] *= w[i] * k;
}

static void vec_clamp(float *v, const float *w, int n, float a, float z)
{
    int i;

    for (i = 0; i < n; ++i)
        if      (w[i] < a) v[i] = a;
        else if (w[i] > z) v[i] = z;
        else               v[i] = w[i];
}

#endif /* __SSE__ */

/*===========================================================================*/
/* Compute all waveforms over [0,1].                                         */

static float sin_wave(float t)
{
    return sinf(6.2831853f * t);
}

static float sqr_wave(float t)
{
    if (t < 0.5f)
        return  1;
    else
        return -1;
}

static float tri_wave(float t)
{
    if      (t < 0.25f) return       t          * 4;
    else if (t < 0.75f) return  1 - (t - 0.25f) * 4;
    else                return -1 + (t - 0.75f) * 4;
}

static float saw_wave(float t)
{
    if (t < 0.5f)
        return 2 *  t;
    else
        return 2 * (t - 1);
}

static float wht_wave(float t)
{
    return 2 * (float) rand() / RAND_MAX - 1;
}

/*---------------------------------------------------------------------------*/

static float snth_freq(float n)
{
    return 440.0f * powf(2.0f, (n - 69.0f) / 12.0f);
}

static float *make_wave(int length, float (*func)(float))
{
    float *p;
    int    i;

    if ((p = (float *) malloc(length * sizeof (float))))
        for (i = 0; i < length; ++i)
            p[i] = func((float) i / length);

    return p;
}

/*===========================================================================*/

/* The following filters are based on Paul Kellett's implementation of the   */
/* Moog VCF approximation devised by Tim Stilson and Julius Smith.           */

#define NEW 1

#ifdef OLD
static float snth_get_filter(struct snth_filter *F, float in, float c, float r)
{
    float b0, b1, b2, b3, b4;

    float q = 1.0f - c;
    float p = c + 0.8f * c * q;
    float f = p + p - 1.0f;
    float k = 1.0f + 0.5f * q * (1.0f - q + 5.6f * q * q);

    b0 = in - r * k * F->b4;

    b1 = b0 * p + (F->b0 * p - F->b1 * f);
    b2 = b1 * p + (F->b1 * p - F->b2 * f);
    b3 = b2 * p + (F->b2 * p - F->b3 * f);
    b4 = b3 * p + (F->b3 * p - F->b4 * f);

    F->b0 = b0;
    F->b1 = b1;
    F->b2 = b2;
    F->b3 = b3;
    F->b4 = b4 - b4 * b4 * b4 * 0.166667f;

    return F->b4;
}

static void snth_get_lpf(float *wave, int n,
                         struct snth_filter *F, const float *cut, float res)
{
    int i;

    for (i = 0; i < n; ++i)
        wave[i] = snth_get_filter(F, wave[i], cut[i], res);
}
#endif

#ifdef NEW
void snth_get_lpf(struct snth_filter *F, float *wave, int n,
                         const float *b, const float *k)
{
    int i;

    /* Filtering is a fundamentally serial operation. */

    for (i = 0; i < n; ++i)
    {
        float B = b[i];
        float A = b[i] + b[i] - 1;

        float t1 = F->b0 * B - F->b1 * A;
        float t2 = F->b1 * B - F->b2 * A;
        float t3 = F->b2 * B - F->b3 * A;
        float t4 = F->b3 * B - F->b4 * A;

        /* Feedback. */

        float b0 = wave[i] - k[i] * F->b4;

        /* Four cascaded one-pole filters. */

        float b1 = b0 * B + t1;
        float b2 = b1 * B + t2;
        float b3 = b2 * B + t3;
        float b4 = b3 * B + t4;

        /* Retain clipped filter state. */

        F->b0 = b0;
        F->b1 = b1;
        F->b2 = b2;
        F->b3 = b3;
        F->b4 = b4 - b4 * b4 * b4 * 0.166667f;

        /* Output. */

        wave[i] = F->b4;
    }
}

static void snth_get_hpf(struct snth_filter *F, float *wave, int n,
                         const float *b, const float *k)
{
    int i;

    /* Filtering is a fundamentally serial operation. */

    for (i = 0; i < n; ++i)
    {
        const float B = b[i];
        const float A = b[i] + b[i] - 1;

        /* Feedback. */

        float b0 = wave[i] - k[i] * F->b4;

        /* Four cascaded one-pole filters. */

        float b1 = (b0 + F->b0) * B - F->b1 * A;
        float b2 = (b1 + F->b1) * B - F->b2 * A;
        float b3 = (b2 + F->b2) * B - F->b3 * A;
        float b4 = (b3 + F->b3) * B - F->b4 * A;

        /* Retain clipped filter state. */

        F->b0 = b0;
        F->b1 = b1;
        F->b2 = b2;
        F->b3 = b3;
        F->b4 = b4 - b4 * b4 * b4 * 0.166667f;

        /* Output. */

        wave[i] -= F->b4;
    }
}

static void snth_get_filter(float *wave, int n, int m,
                            struct snth_filter *F, const float *cut, float res)
{
    const __m128 *c   = (const __m128 *) cut;

    static __m128 b[MAXFRAME >> 2];
    static __m128 k[MAXFRAME >> 2];

    const __m128 c05 = _mm_set1_ps(0.5f);
    const __m128 c08 = _mm_set1_ps(0.8f);
    const __m128 c10 = _mm_set1_ps(1.0f);
    const __m128 c56 = _mm_set1_ps(5.6f);
    const __m128 r   = _mm_set1_ps(res);

    __m128 tmp;
    __m128 val;

    int i;

    /* Precompute all filter coefficients. */

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        /* t = 1 - c */

        tmp  = _mm_sub_ps(c10, c[i]);

        /* b = c + 0.8 * c * t */

        val  = _mm_mul_ps(tmp, c[i]);
        val  = _mm_mul_ps(val, c08);
        b[i] = _mm_add_ps(val, c[i]);

        /* k = r * (1 + 0.5 * t * (1 - t + 5.6 * t * t)) */

        val  = _mm_mul_ps(tmp, tmp);
        val  = _mm_mul_ps(val, c56);
        val  = _mm_sub_ps(val, tmp);
        val  = _mm_add_ps(val, c10);
        val  = _mm_mul_ps(val, tmp);
        val  = _mm_mul_ps(val, c05);
        val  = _mm_add_ps(val, c10);
        k[i] = _mm_mul_ps(val, r);
    }

    /* Apply the filter. */

    switch (m)
    {
    case SNTH_LPF:
        snth_get_lpf(F, wave, n, (const float *) b, (const float *) k);
        break;
    case SNTH_HPF:
        snth_get_hpf(F, wave, n, (const float *) b, (const float *) k);
        break;
    }
}
#endif
/*
static void snth_get_filter(float *wave, int n, int m,
                            struct snth_filter *F, const float *cut, float res)
{
    int i;

    for (i = 0; i < n; ++i)
    {
        F->b0 = wave[i] * cut[i] * cut[i] + F->b0 * (1 - cut[i] * cut[i]) - F->b0 * res;
        wave[i] = F->b0;
    }
}
*/
/*---------------------------------------------------------------------------*/
#ifdef __SSE__

static void get_sin_wave(__m128 *dst, const __m128 *src, int n)
{
    const __m128 pi  = _mm_set1_ps(3.1415926535897932f);
    const __m128 pi2 = _mm_set1_ps(6.2831853071795864f);

    const __m128 r3f = _mm_set1_ps(0.1666666666666666f);
    const __m128 r5f = _mm_set1_ps(0.0083333333333333f);
    const __m128 r7f = _mm_set1_ps(0.0001984126984126f);
/*
    const __m128 r9f = _mm_set1_ps(0.0000027557319223f);
    const __m128 rBf = _mm_set1_ps(0.0000000250521083f);
*/
    __m128 sqr;
    __m128 sin;
    __m128 tmp;
    __m128 num;
    __m128 nlt;
    __m128 ngt;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        /* Normalize the phase to -pi through +pi. */

        num = _mm_mul_ps(pi2, src[i]);
        num = _mm_sub_ps(num, pi);

        /* Wrap -pi/2 < t < +pi/2. */

        ngt = _mm_sub_ps(pi,  num);
        nlt = _mm_sub_ps(ngt, pi2);
        num = _mm_min_ps(num, ngt);
        num = _mm_max_ps(num, nlt);

        /* Evaluate the Taylor polynomial... */

        sqr = _mm_mul_ps(num, num);

        /* sin = x */

        sin = num;

        /* sin = x - x^3/3! */

        num = _mm_mul_ps(num, sqr);
        tmp = _mm_mul_ps(num, r3f);
        sin = _mm_sub_ps(sin, tmp);

        /* sin = x - x^3/3! + x^5/5! */

        num = _mm_mul_ps(num, sqr);
        tmp = _mm_mul_ps(num, r5f);
        sin = _mm_add_ps(sin, tmp);

        /* sin = x - x^3/3! + x^5/5! - x^7/7! */

        num = _mm_mul_ps(num, sqr);
        tmp = _mm_mul_ps(num, r7f);
        sin = _mm_sub_ps(sin, tmp);

        /* sin = x - x^3/3! + x^5/5! - x^7/7! + x^9/9!*/
        /*
        num = _mm_mul_ps(num, sqr);
        tmp = _mm_mul_ps(num, r9f);
        sin = _mm_add_ps(sin, tmp);
        */
        /* sin = x - x^3/3! + x^5/5! - x^7/7! + x^9/9! - x^11/11!*/
        /*
        num = _mm_mul_ps(num, sqr);
        tmp = _mm_mul_ps(num, rBf);
        sin = _mm_add_ps(sin, tmp);
        */
        dst[i] = sin;
    }
}

static void get_sqr_wave(__m128 *dst, const __m128 *src, int n)
{
    const __m128 vh = _mm_set1_ps(0.5f);
    const __m128 v1 = _mm_set1_ps(1.0f);
    const __m128 v2 = _mm_set1_ps(2.0f);

    __m128 tmp;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        tmp    = _mm_cmplt_ps(src[i], vh);
        tmp    = _mm_and_ps(tmp, v2);
        dst[i] = _mm_sub_ps(tmp, v1);
    }
}

static void get_tri_wave(__m128 *dst, const __m128 *src, int n)
{
    const __m128 v2 = _mm_set1_ps(2.0f);
    const __m128 v4 = _mm_set1_ps(4.0f);

    __m128 t1;
    __m128 t2;
    __m128 t3;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        t1     = _mm_mul_ps(v4, src[i]);
        t2     = _mm_sub_ps(v2, t1);
        t3     = _mm_sub_ps(t1, v4);
        t1     = _mm_min_ps(t1, t2);
        dst[i] = _mm_max_ps(t1, t3);
    }
}

static void get_saw_wave(__m128 *dst, const __m128 *src, int n)
{
    const __m128 one = _mm_set1_ps(1.0f);

    __m128 tmp;

    int i;

    for (i = (n >> 2) - 1; i >= 0; --i)
    {
        tmp    = _mm_add_ps(src[i], src[i]);
        dst[i] = _mm_sub_ps(tmp, one);
    }
}

static void get_wht_wave(__m128 *dst, int n)
{
    float *v = (float *) dst;

    int i;

    for (i = n - 1; i >= 0; --i)
        v[i] = 2.0f * rand() / RAND_MAX - 1.0f;
}

static void snth_get_wave(float *wave, const float *phase, int n, int mode)
{
          __m128 *dst =       (__m128 *) wave;
    const __m128 *src = (const __m128 *) phase;

    switch (mode)
    {
    case SNTH_WAVE_SIN:  get_sin_wave(dst, src, n); break;
    case SNTH_WAVE_SQR:  get_sqr_wave(dst, src, n); break;
    case SNTH_WAVE_TRI:  get_tri_wave(dst, src, n); break;
    case SNTH_WAVE_SAWU: get_saw_wave(dst, src, n); break;
    case SNTH_WAVE_WHT:  get_wht_wave(dst,      n); break;
    }
}

#else
static void snth_get_wave(float *wave, const float *phase, int n, int mode)
{
}
#endif

/*---------------------------------------------------------------------------*/

void snth_fix_phase(float *phase, int n)
{
    float *p = phase;
    int i;

    for (i = 0; i < n; i += 4, p += 4)
    {
        p[0] = FRAC(p[0]);
        p[1] = FRAC(p[1]);
        p[2] = FRAC(p[2]);
        p[3] = FRAC(p[3]);
    }
}

static void snth_get_phase_variable(float *phase, const float *freq, int n,
                                    float w, float *osc_phase)
{
          __m128 *dst =       (__m128 *) phase;
    const __m128 *src = (const __m128 *) freq;

    const __m128 o = _mm_set_ps(1, 1, 1, 1);
    const __m128 z = _mm_set_ps(0, w, w, w);
    const __m128 d = _mm_set_ps(w, w, w, w);

    __m128 p  = _mm_set1_ps(*osc_phase);
    __m128 m0 = _mm_set1_ps(0);
    __m128 m1 = _mm_set1_ps(1);
    __m128 t;

    __m128 ft;
    __m128 f0;
    __m128 f1 = _mm_setzero_ps();
    __m128 f2 = _mm_setzero_ps();
    __m128 f3 = _mm_setzero_ps();

    int i;

    for (i = 0; i < n >> 2; ++i)
    {
        /* Load and normalize the next 4 frequency values. */

        f0 = _mm_mul_ps(src[i], d);
        ft = _mm_mul_ps(src[i], z);

        /* Shift them 1, 2 and 3 spaces to the left. Pad with zero. */

        f1 = _mm_shuffle_ps(ft, ft, 0x93);
        f2 = _mm_movelh_ps(f2, ft);
        f3 = _mm_movelh_ps(f3, f1);

        /* Accumulate all shifted frequency vectors, giving phase. */

        p  = _mm_add_ps(p, f0);
        p  = _mm_add_ps(p, f1);
        p  = _mm_add_ps(p, f2);
        p  = _mm_add_ps(p, f3);

        /* Wrap all phases at one.  Store. */
/*
        t  = _mm_cmpge_ps(p, o);
        t  = _mm_and_ps(t, o);
        p  = _mm_sub_ps(p, t);
*/
        dst[i] = p;

        /* Distribute the last phase across the accumulation vector. */

        p  = _mm_shuffle_ps(p, p, 0xFF);
    }

    _mm_store_ss(osc_phase, p);
}

static void snth_get_phase_constant(float *phase, float freq, int n,
                                    float w, float *osc_phase)
{
    const __m128 o = _mm_set_ps(1, 1, 1, 1);

    __m128 *dst = (__m128 *) phase;

    __m128 t;
    __m128 f = _mm_set1_ps(freq * w * 4);
    __m128 p = _mm_set_ps(*osc_phase,
                          *osc_phase - freq * w * 1,
                          *osc_phase - freq * w * 2,
                          *osc_phase - freq * w * 3);
    int i;

    for (i = 0; i < n >> 2; ++i)
    {
        /* Accumulate the frequency. */

        p  = _mm_add_ps(p, f);

        /* Wrap phase at one.  Store. */
/*
        t  = _mm_cmpge_ps(p, o);
        t  = _mm_and_ps(t, o);
        p  = _mm_sub_ps(p, t);
*/
        dst[i] = p;
    }

    _mm_store_ss(osc_phase, _mm_shuffle_ps(p, p, 0xFF));
}

static void snth_get_env(float *level, int n, float am, float ab,
                                              float dm, float db,
                                                        float sb,
                                              float rm, float rb, float time)
{
    int i;

    const __m128 t  = _mm_set_ps(time + 3, time + 2, time + 1, time + 0);
    const __m128 dt = _mm_set1_ps(4);
    const __m128 v0 = _mm_set1_ps(0);

    /* Move all ADSR parameters into SSE vectors. */
    
    const __m128 amv = _mm_set1_ps(am);
    const __m128 abv = _mm_set1_ps(ab);
    const __m128 dmv = _mm_set1_ps(dm);
    const __m128 dbv = _mm_set1_ps(db);
    const __m128 s   = _mm_set1_ps(sb);
    const __m128 rmv = _mm_set1_ps(rm);
    const __m128 rbv = _mm_set1_ps(rb);

    __m128 *dst = (__m128 *) level;

    __m128 a, da;
    __m128 d, dd;
    __m128 r, dr;
    __m128 x;

    /* Procompute the slopes and starting points of the ADSR lines. */

    a  = _mm_mul_ps(amv, t);
    a  = _mm_add_ps(abv, a);
    da = _mm_mul_ps(amv, dt);

    d  = _mm_mul_ps(dmv, t);
    d  = _mm_add_ps(dbv, d);
    dd = _mm_mul_ps(dmv, dt);

    r  = _mm_mul_ps(rmv, t);
    r  = _mm_add_ps(rbv, r);
    dr = _mm_mul_ps(rmv, dt);

    /* Trace the ADSR lines, collapsing them down to a single envelope. */

    for (i = 0; i < n >> 2; ++i)
    {
        x      = _mm_max_ps(d, s);
        x      = _mm_min_ps(x, a);
        x      = _mm_min_ps(x, r);
        dst[i] = _mm_max_ps(x, v0);
        a      = _mm_add_ps(a, da);
        d      = _mm_add_ps(d, dd);
        r      = _mm_add_ps(r, dr);
    }
}

static void snth_get_lfo(float *param, int n, int mode,
                         float freq, float dm, float time, float *lfo_phase)
{
    /* Compute the phase and waveform of this LFO.  Param buffer is scratch. */

    snth_get_phase_constant(param, freq, n, 1.0f / rate, lfo_phase);
    snth_get_wave(param, param, n, mode);

    /* Apply the LFO delay. */

    if (dm > 0)
    {
        const __m128 o = _mm_set1_ps(1);
        const __m128 d = _mm_set1_ps(4 * dm);

        __m128 *buf = (__m128 *) param;

        __m128 k = _mm_set_ps((time + 3) * dm,
                              (time + 2) * dm,
                              (time + 1) * dm,
                              (time + 0) * dm);
        int i;

        for (i = 0; i < (n >> 2); ++i)
        {
            k      = _mm_min_ps(k, o);
            buf[i] = _mm_mul_ps(k, buf[i]);
            k      = _mm_add_ps(k, d);
        }
    }
}

/*
static void snth_get_env(float *level, int n,
                         const struct snth_env *env,
                         float *env_level, int *env_stage)
{
    float l = *env_level;
    int   s = *env_stage;

    float k = env->ks;
    int   i = 0;

    switch (s)
    {
    case 1:

        while (i < n)
            if (l < 1)
                level[i++] = (l += env->da);
            else
            {
                s = 2;
                break;
            }

    case 2:

        while (i < n)
            if (l > k)
                level[i++] = (l += env->dd);
            else
            {
                s = 3;
                break;
            }

    case 3:

        while (i < n)
            level[i++] = k;

    case 4:

        while (i < n)
            if (l > 0)
                level[i++] = (l += env->dr);
            else
            {
                s = 5;
                break;
            }

    default:

        while (i < n)
            level[i++] = 0;
    }

    *env_level = l;
    *env_stage = (s < 5) ? s : 0;
}

static void snth_get_lfo(float *wave, int n,
                         const struct snth_lfo *lfo,
                         float *lfo_phase, float *lfo_level)
{
    float k = *lfo_phase;
    float l = *lfo_level;

    int i;

    for (i = 0; i < n; ++i)
    {
        wave[i] = wave_table[lfo->wave][F2I(k)] * l;

        k += lfo->dr;

        if (k >= wave_count[lfo->wave])
            k -= wave_count[lfo->wave];

        if (l < 1)
            l += lfo->dl;
        else
            l  = 1;
    }

    *lfo_phase = k;
    *lfo_level = l;
}
*/

static void snth_get_freq(float *freq, const float *pitch, int n)
{
    int i;

    /* SSE is not well-suited for table lookups. */

    for (i = 0; i < n; i += 4)
    {
              float *dst = (float *) (freq  + i);
        const float *src = (float *) (pitch + i);

        /* Truncate all pitch values down to frequency table indices. */

        int i0 = (int) src[0];
        int i1 = (int) src[1];
        int i2 = (int) src[2];
        int i3 = (int) src[3];

        /* Linearly interpolate frequency table values. */

        dst[0] = (freq_tab_k[i0] + freq_tab_d[i0] * (src[0] - i0));
        dst[1] = (freq_tab_k[i1] + freq_tab_d[i1] * (src[1] - i1));
        dst[2] = (freq_tab_k[i2] + freq_tab_d[i2] * (src[2] - i2));
        dst[3] = (freq_tab_k[i3] + freq_tab_d[i3] * (src[3] - i3));
    }
}

/*
static void snth_get_phase(float *phase, const float *freq, int n, int m,
                           float *osc_phase)
{
    float k = *osc_phase;
    int   i;

    for (i = 0; i < n; ++i)
    {
        k += (freq[i] / m);

        if (k >= 1)
            k -= 1;

        phase[i] = k;
    }

    *osc_phase = k;
}
*/
/*
static float snth_get_pan(float pan)
{
    if (pan <= -1) return 0;
    if (pan >=  1) return 1;

    return sine_table[F2I((pan + 1) * MAXSINE / 2)];
}
*/

/*---------------------------------------------------------------------------*/

static int snth_get_osc(struct snth_osc  *O,
                        const struct snth_tone *T,
                        int n, int p, int l, int mode0, int mode1)
{
    const struct snth_env *E = T->env;
    const struct snth_lfo *L = T->lfo;

    /* Working buffers */

    static float env_level[3][MAXFRAME];
    static float lfo_param[2][MAXFRAME];

    static float pitch[MAXFRAME];
    static float phase[MAXFRAME];
    static float level[MAXFRAME];
    static float freq [MAXFRAME];
    static float wave [MAXFRAME];
    static float cut  [MAXFRAME];

    /* Tone parameters */

    const float note = p + T->pitch_coarse - 64 + TO_11(T->pitch_fine);
    const float time = (float) O->time;

    /* Evaluate the envelopes. */

    if (T->flags & FL_ENV0)
        snth_get_env(env_level[0], n, E[0].am, E[0].ab, E[0].dm, E[0].db,
                     E[0].sb, O->rm[0], O->rb[0], time);
    if (T->flags & FL_ENV1)
        snth_get_env(env_level[1], n, E[1].am, E[1].ab, E[1].dm, E[1].db,
                     E[1].sb, O->rm[1], O->rb[1], time);
    if (T->flags & FL_ENV2)
        snth_get_env(env_level[2], n, E[2].am, E[2].ab, E[2].dm, E[2].db,
                     E[2].sb, O->rm[2], O->rb[2], time);

    /* Evaluate the LFOs. */

    if (T->flags & FL_LFO0)
        snth_get_lfo(lfo_param[0], n,
                     L[0].wave, L[0].freq, L[0].dm, time, O->lfo_phase + 0);
    if (T->flags & FL_LFO1)
        snth_get_lfo(lfo_param[1], n,
                     L[1].wave, L[1].freq, L[1].dm, time, O->lfo_phase + 1);

    /* Evaluate the frequency and phase. */

    if (T->flags & FL_PITCH)
    {
        vec_set(pitch, n, note);

        if ((T->flags & FL_LFO0) && (L[0].pitch   != DEF_LFO_PITCH))
            vec_acc(pitch, lfo_param[0], n, L[0].pitch   - 64);
        if ((T->flags & FL_LFO1) && (L[1].pitch   != DEF_LFO_PITCH))
            vec_acc(pitch, lfo_param[1], n, L[1].pitch   - 64);
        if ((T->flags & FL_ENV1) && (T->pitch_env != DEF_TONE_PITCH_ENV))
            vec_acc(pitch, env_level[1], n, T->pitch_env - 64);

        vec_clamp(pitch, pitch, n, 0, 127);

        snth_get_freq(freq, pitch, n);

        if (mode0 == SNTH_MODE_MOD)
            vec_fm(freq, freq, modula, n);

        snth_get_phase_variable(phase, freq, n, 1.0f / rate, &O->osc_phase);
    }
    else
    {
        float freq;

        if      (note > 127) freq = 12543.8539514160f;
        else if (note <   0) freq =     8.1757989156f;
        else                 freq = snth_freq(note);

        snth_get_phase_constant(phase, freq, n, 1.0f / rate, &O->osc_phase);
    }

    /* Evaluate the waveform. */

    snth_fix_phase(phase, n);
    snth_get_wave(wave, phase, n, T->wave);

    if (mode0 == SNTH_MODE_RNG)
        vec_mul(wave, wave, modula, n);

    /* Apply the filter. */

    if (T->flags & FL_FILTER)
    {
        const float res = TO_01(T->filter_res);

        vec_set(cut, n, TO_01(T->filter_cut) +
                        TO_11(T->filter_key) * TO_01(l));

        if ((T->flags & FL_LFO0) && (L[0].filter   != DEF_LFO_FILTER))
            vec_acc(cut, lfo_param[0], n, TO_11(T->lfo[0].filter));
        if ((T->flags & FL_LFO1) && (L[1].filter   != DEF_LFO_FILTER))
            vec_acc(cut, lfo_param[1], n, TO_11(T->lfo[1].filter));
        if ((T->flags & FL_ENV2) && (T->filter_env != DEF_TONE_FILTER_ENV))
            vec_acc(cut, env_level[2], n, TO_11(T->filter_env));

        vec_clamp(cut, cut, n, 0, 1);

        snth_get_filter(wave, n, T->filter_mode, &O->filter, cut, res);
    }

    /* Evaluate the level. */

    vec_set(level, n, TO_01(T->level) * TO_01(l));

    if ((T->flags & FL_LFO0) && (L[0].level != DEF_LFO_LEVEL))
        vec_acc(level, lfo_param[0], n, TO_11(T->lfo[0].level));
    if ((T->flags & FL_LFO1) && (L[1].level != DEF_LFO_LEVEL))
        vec_acc(level, lfo_param[1], n, TO_11(T->lfo[1].level));
    if ((T->flags & FL_ENV0))
        vec_mod(level, env_level[0], n, 1);

    /* Evaluate the final output. */

    if (mode1 == SNTH_MODE_MIX)
    {
        vec_mul(wave, wave, level, n);

        vec_acc(outputL, wave, n, 1);
        vec_acc(outputR, wave, n, 1);
    }
    else
        vec_mul(modula, wave, level, n);

    O->time += n;

    O->osc_phase    = FRAC(O->osc_phase);
    O->lfo_phase[0] = FRAC(O->lfo_phase[0]);
    O->lfo_phase[1] = FRAC(O->lfo_phase[1]);

    if (env_level[0][n - 1] > 0)
        O->state = 1;
    else
        O->state = 0;

    return O->state;
}

static int snth_get_note(struct snth_note *N, int n)
{
    const struct snth_channel *C = channel + N->chan;
    const struct snth_tone    *T = patch[C->patch].tone;

    const int e0 = N->osc[0].state;
    const int e1 = N->osc[1].state;
    const int e2 = N->osc[2].state;
    const int e3 = N->osc[3].state;

    const int mx =                  SNTH_MODE_OFF;
    const int m0 = e0 ? T[0].mode : SNTH_MODE_OFF;
    const int m1 = e1 ? T[1].mode : SNTH_MODE_OFF;
    const int m2 = e2 ? T[2].mode : SNTH_MODE_OFF;
    const int m3 = e3 ? T[3].mode : SNTH_MODE_OFF;

    const int d0 = TO_DT(T[0].delay);
    const int d1 = TO_DT(T[1].delay);
    const int d2 = TO_DT(T[2].delay);
    const int d3 = TO_DT(T[3].delay);

    int c = 0;

    if (m0 && e0 && curr_time - N->start >= d0)
        c += snth_get_osc(N->osc + 0, T + 0, n, N->pitch, N->level, mx, m0);
    if (m1 && e1 && curr_time - N->start >= d1)
        c += snth_get_osc(N->osc + 1, T + 1, n, N->pitch, N->level, m0, m1);
    if (m2 && e2 && curr_time - N->start >= d2)
        c += snth_get_osc(N->osc + 2, T + 2, n, N->pitch, N->level, m1, m2);
    if (m3 && e3 && curr_time - N->start >= d3)
        c += snth_get_osc(N->osc + 3, T + 3, n, N->pitch, N->level, m2, m3);

    /* If none of the oscillators are sounding, kill the note. */

    if (e0 == 0 && e1 == 0 && e2 == 0 && e3 == 0)
        N->level = 0;

    return c;
}

static int snth_get_buffer(int n)
{
    int c = 0;
    int i;

    /* Initialize the working audio buffer. */

    memset(outputL, 0, n * sizeof (float));
    memset(outputR, 0, n * sizeof (float));

    /* Iterate over all notes, processing the active ones. */

    for (i = 0; i < MAXNOTE; ++i)
        if (note[i].level)
            c += snth_get_note(note + i, n);

    curr_time += n;

    return c;
}

int snth_get_output(void *buffer, size_t frames)
{
    assert((frames % 4) == 0);

    int16_t *L = (int16_t *) buffer + 0;
    int16_t *R = (int16_t *) buffer + 1;

    size_t count;
    int c = 0;
    int m = 0;

    /* Continue processing audio until the given buffer is full. */

    for (count = 0; count < frames; count += MAXFRAME)
    {
        size_t i, n = ((frames - count) < MAXFRAME ?
                       (frames - count) : MAXFRAME);

        /* Process a chunk of audio. */

        c = snth_get_buffer((int) n);

        if (m < c)
            m = c;

        /* Copy clamped audio to the output buffer. */

        if (c)
        {
            vec_clamp(outputL, outputL, n, -1, 1);
            vec_clamp(outputR, outputR, n, -1, 1);
        }

        for (i = 0; i < n; L += 2, R += 2, ++i)
        {
            *L = (int16_t) F2I(outputL[i] * 32767);
            *R = (int16_t) F2I(outputR[i] * 32767);
        }
    }

    return c;
}

/*===========================================================================*/

void snth_set_channel(uint8_t i)
{
    assert(i < MAXCHANNEL);
    curr_chan = i;
}

void snth_set_patch(uint8_t i)
{
    assert(i < MAXPATCH);
    channel[curr_chan].patch = i;
}

void snth_set_bank(uint8_t i)
{
}

/*---------------------------------------------------------------------------*/

void snth_set_patch_name(const char *name)
{
    strncpy(patch[channel[curr_chan].patch].name, name, MAXSTR);
}

/*---------------------------------------------------------------------------*/

static void snth_set_tone_cache(uint8_t i, uint8_t j)
{
    struct snth_tone *t = patch[i].tone + j;

    uint8_t  m = j ? patch[i].tone[j - 1].mode : SNTH_MODE_OFF;
    uint16_t f = 0;

    /* Determine the envelope enable states. */

    if (t->env[0].flags)                                         f |= FL_ENV0;
    if (t->env[1].flags && t->pitch_env  != DEF_TONE_PITCH_ENV)  f |= FL_ENV1;
    if (t->env[2].flags && t->filter_env != DEF_TONE_FILTER_ENV) f |= FL_ENV2;
    
    /* Determine the LFO enable states. */

    if (t->lfo[0].flags) f |= FL_LFO0;
    if (t->lfo[1].flags) f |= FL_LFO1;

    /* Determine the varying-parameter enable states. */

    if (m               == SNTH_MODE_MOD ||
        t->lfo[0].pitch != DEF_LFO_PITCH ||
        t->lfo[1].pitch != DEF_LFO_PITCH || f & FL_ENV1) f |= FL_PITCH;

    if (t->lfo[0].pan   != DEF_LFO_PAN ||
        t->lfo[1].pan   != DEF_LFO_PAN) f |= FL_PAN;

    if (t->filter_mode != DEF_TONE_FILTER_MODE ||
        t->filter_cut  != DEF_TONE_FILTER_CUT  ||
        t->filter_res  != DEF_TONE_FILTER_RES  ||
        t->filter_key  != DEF_TONE_FILTER_KEY  || f & FL_ENV2) f |= FL_FILTER;

    t->flags = f;
}

void snth_set_tone_wave(uint8_t tone, uint8_t wave)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].wave = wave;
}

void snth_set_tone_mode(uint8_t tone, uint8_t mode)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].mode = mode;
}

void snth_set_tone_level(uint8_t tone, uint8_t level)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].level = level;
}

void snth_set_tone_pan(uint8_t tone, uint8_t pan)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].pan = pan;
}

void snth_set_tone_delay(uint8_t tone, uint8_t delay)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].delay = delay;
}

void snth_set_tone_pitch_coarse(uint8_t tone, uint8_t pitch_coarse)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].pitch_coarse = pitch_coarse;
}

void snth_set_tone_pitch_fine(uint8_t tone, uint8_t pitch_fine)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].pitch_fine = pitch_fine;
}

void snth_set_tone_pitch_env(uint8_t tone, uint8_t pitch_env)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].pitch_env = pitch_env;
    snth_set_tone_cache(channel[curr_chan].patch, tone);
}

void snth_set_tone_filter_mode(uint8_t tone, uint8_t filter_mode)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].filter_mode = filter_mode;
    snth_set_tone_cache(channel[curr_chan].patch, tone);
}

void snth_set_tone_filter_cut(uint8_t tone, uint8_t filter_cut)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].filter_cut = filter_cut;
    snth_set_tone_cache(channel[curr_chan].patch, tone);
}

void snth_set_tone_filter_res(uint8_t tone, uint8_t filter_res)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].filter_res = filter_res;
    snth_set_tone_cache(channel[curr_chan].patch, tone);
}

void snth_set_tone_filter_env(uint8_t tone, uint8_t filter_env)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].filter_env = filter_env;
    snth_set_tone_cache(channel[curr_chan].patch, tone);
}

void snth_set_tone_filter_key(uint8_t tone, uint8_t filter_key)
{
    assert(tone < MAXTONE);
    patch[channel[curr_chan].patch].tone[tone].filter_key = filter_key;
    snth_set_tone_cache(channel[curr_chan].patch, tone);
}

/*---------------------------------------------------------------------------*/

static void snth_set_tone_env_cache(uint8_t i, uint8_t j, uint8_t k)
{
    struct snth_env *e = patch[i].tone[j].env + k;

    float at = TO_DT(e->a);
    float dt = TO_DT(e->d);
    float sb = TO_01(e->s);
    float rt = TO_DT(e->r);

    /* Recompute the envelope state cache. */

    if (at > 0)
    {
        e->am = 1 / at;
        e->ab = 0;
    }
    else
    {
        e->am = 0;
        e->ab = 1;
    }

    if (dt > 0)
    {
        e->dm =         -(1 - sb) / dt;
        e->db = 1 + at * (1 - sb) / dt;
    }
    else
    {
        e->dm = 0;
        e->db = sb;
    }

    if (rt > 0)
    {
        e->rm = -sb / rt;
        e->rb = 0;
    }
    else
    {
        e->rm = 0;
        e->rb = 0;
    }

    e->sb = sb;

    e->flags = (uint16_t) (e->a || e->d || e->s || e->r);

    snth_set_tone_cache(i, j);
}

void snth_set_tone_env_a(uint8_t tone, uint8_t env, uint8_t a)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    patch[channel[curr_chan].patch].tone[tone].env[env].a = a;
    snth_set_tone_env_cache(channel[curr_chan].patch, tone, env);
}

void snth_set_tone_env_d(uint8_t tone, uint8_t env, uint8_t d)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    patch[channel[curr_chan].patch].tone[tone].env[env].d = d;
    snth_set_tone_env_cache(channel[curr_chan].patch, tone, env);
}

void snth_set_tone_env_s(uint8_t tone, uint8_t env, uint8_t s)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    patch[channel[curr_chan].patch].tone[tone].env[env].s = s;
    snth_set_tone_env_cache(channel[curr_chan].patch, tone, env);
}

void snth_set_tone_env_r(uint8_t tone, uint8_t env, uint8_t r)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    patch[channel[curr_chan].patch].tone[tone].env[env].r = r;
    snth_set_tone_env_cache(channel[curr_chan].patch, tone, env);
}

/*---------------------------------------------------------------------------*/

static void snth_set_tone_lfo_cache(uint8_t i, uint8_t j, uint8_t k)
{
    struct snth_lfo *l = patch[i].tone[j].lfo + k;

    float rt = TO_DT(l->rate);
    float dt = TO_DT(l->delay);

    l->freq = (rt > 0) ? (float) rate / rt : 0;
    l->dm   = (dt > 0) ? (float) 1.0f / dt : 0;

    l->flags = (uint16_t) ((l->rate > 0) && (l->level  != DEF_LFO_LEVEL ||
                                             l->pan    != DEF_LFO_PAN   ||
                                             l->pitch  != DEF_LFO_PITCH ||
                                             l->phase  != DEF_LFO_PHASE ||
                                             l->filter != DEF_LFO_FILTER));
    snth_set_tone_cache(i, j);
}

void snth_set_tone_lfo_wave(uint8_t tone, uint8_t lfo, uint8_t wave)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].wave = wave;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_sync(uint8_t tone, uint8_t lfo, uint8_t sync)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].sync = sync;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_rate(uint8_t tone, uint8_t lfo, uint8_t rate)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].rate = rate;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_delay(uint8_t tone, uint8_t lfo, uint8_t delay)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].delay = delay;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_level(uint8_t tone, uint8_t lfo, uint8_t level)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].level = level;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_pan(uint8_t tone, uint8_t lfo, uint8_t pan)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].pan = pan;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_pitch(uint8_t tone, uint8_t lfo, uint8_t pitch)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].pitch = pitch;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_phase(uint8_t tone, uint8_t lfo, uint8_t phase)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].phase = phase;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

void snth_set_tone_lfo_filter(uint8_t tone, uint8_t lfo, uint8_t filter)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    patch[channel[curr_chan].patch].tone[tone].lfo[lfo].filter = filter;
    snth_set_tone_lfo_cache(channel[curr_chan].patch, tone, lfo);
}

/*===========================================================================*/

uint8_t snth_get_channel(void)
{
    return curr_chan;
}

uint8_t snth_get_patch(void)
{
    return channel[curr_chan].patch;
}

uint8_t snth_get_bank(void)
{
    return 0;
}

/*---------------------------------------------------------------------------*/

const char *snth_get_patch_name(void)
{
    return patch[channel[curr_chan].patch].name;
}

/*---------------------------------------------------------------------------*/

uint8_t snth_get_tone_wave(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].wave;
}

uint8_t snth_get_tone_mode(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].mode;
}

uint8_t snth_get_tone_level(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].level;
}

uint8_t snth_get_tone_pan(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].pan;
}

uint8_t snth_get_tone_delay(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].delay;
}

uint8_t snth_get_tone_pitch_coarse(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].pitch_coarse;
}

uint8_t snth_get_tone_pitch_fine(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].pitch_fine;
}

uint8_t snth_get_tone_pitch_env(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].pitch_env;
}

uint8_t snth_get_tone_filter_mode(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].filter_mode;
}

uint8_t snth_get_tone_filter_cut(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].filter_cut;
}

uint8_t snth_get_tone_filter_res(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].filter_res;
}

uint8_t snth_get_tone_filter_env(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].filter_env;
}

uint8_t snth_get_tone_filter_key(uint8_t tone)
{
    assert(tone < MAXTONE);
    return patch[channel[curr_chan].patch].tone[tone].filter_key;
}

/*---------------------------------------------------------------------------*/

uint8_t snth_get_tone_env_a(uint8_t tone, uint8_t env)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    return patch[channel[curr_chan].patch].tone[tone].env[env].a;
}

uint8_t snth_get_tone_env_d(uint8_t tone, uint8_t env)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    return patch[channel[curr_chan].patch].tone[tone].env[env].d;
}

uint8_t snth_get_tone_env_s(uint8_t tone, uint8_t env)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    return patch[channel[curr_chan].patch].tone[tone].env[env].s;
}

uint8_t snth_get_tone_env_r(uint8_t tone, uint8_t env)
{
    assert(tone < MAXTONE);
    assert(env  < MAXENV);
    return patch[channel[curr_chan].patch].tone[tone].env[env].r;
}

/*---------------------------------------------------------------------------*/

uint8_t snth_get_tone_lfo_wave(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].wave;
}

uint8_t snth_get_tone_lfo_sync(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].sync;
}

uint8_t snth_get_tone_lfo_rate(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].rate;
}

uint8_t snth_get_tone_lfo_delay(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].delay;
}

uint8_t snth_get_tone_lfo_level(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].level;
}

uint8_t snth_get_tone_lfo_pan(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].pan;
}

uint8_t snth_get_tone_lfo_pitch(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].pitch;
}

uint8_t snth_get_tone_lfo_phase(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].phase;
}

uint8_t snth_get_tone_lfo_filter(uint8_t tone, uint8_t lfo)
{
    assert(tone < MAXTONE);
    assert(lfo  < MAXLFO);
    return patch[channel[curr_chan].patch].tone[tone].lfo[lfo].filter;
}

/*===========================================================================*/

static void snth_osc_on(struct snth_osc *O, const struct snth_lfo *L)
{
    int i;

    /* Initialize the envelope states. */

    for (i = 0; i < MAXENV; ++i)
    {
        O->time  = 0;
        O->state = 1;
        O->rm[i] = 0;
        O->rb[i] = 1;
    }

    /* Initialize the oscillator states. */

    O->osc_phase    = 0;
    O->lfo_phase[0] = (L[0].sync) ? 0 : FRAC(curr_time * L[0].freq / rate);
    O->lfo_phase[1] = (L[1].sync) ? 0 : FRAC(curr_time * L[1].freq / rate);

    /* Initialize the filter. */

    memset(&O->filter, 0, sizeof (struct snth_filter));
}

static void snth_osc_off(struct snth_osc *O, const struct snth_env *E)
{
    int i;

    for (i = 0; i < MAXENV; ++i)
        if (E[i].rm < 0)
        {
            O->rm[i] = E[i].rm;
            O->rb[i] = E[i].sb - E[i].rm * O->time;
        }
        else
        {
            O->rm[i] = 0;
            O->rb[i] = 0;
        }
}

/*---------------------------------------------------------------------------*/

void snth_note_on(uint8_t chan, uint8_t pitch, uint8_t level)
{
    assert(chan  < MAXCHANNEL);
    assert(pitch < 128);

    struct snth_tone *T = patch[channel[chan].patch].tone;
    struct snth_note *N = note + curr_note;

    channel[chan].note[pitch] = curr_note;

    /* Initialize a new note. */

    N->start = curr_time;
    N->pitch = pitch;
    N->level = level;
    N->chan  = chan;

    /* Initialize an oscillator for each active tone of this patch. */

    if (T[0].mode) snth_osc_on(N->osc + 0, T[0].lfo);
    if (T[1].mode) snth_osc_on(N->osc + 1, T[1].lfo);
    if (T[2].mode) snth_osc_on(N->osc + 2, T[2].lfo);
    if (T[3].mode) snth_osc_on(N->osc + 3, T[3].lfo);

    /* Advance the note ring index. */

    curr_note = (uint16_t) ((curr_note + 1) % MAXNOTE);
}

void snth_note_off(uint8_t chan, uint8_t pitch, uint8_t level)
{
    assert(chan  < MAXCHANNEL);
    assert(pitch < 128);

    struct snth_tone *T = patch[channel[chan].patch].tone;

    /* If there is in fact a note playing... */

    if (channel[chan].note[pitch] != NO_NOTE)
    {
        /* Stop all oscillators currently playing this note. */

        snth_osc_off(note[channel[chan].note[pitch]].osc + 0, T[0].env);
        snth_osc_off(note[channel[chan].note[pitch]].osc + 1, T[1].env);
        snth_osc_off(note[channel[chan].note[pitch]].osc + 2, T[2].env);
        snth_osc_off(note[channel[chan].note[pitch]].osc + 3, T[3].env);
    }

    channel[chan].note[pitch] = NO_NOTE;
}

/*===========================================================================*/
/* Default state check                                                       */

static int snth_stat_env(uint8_t i, uint8_t j, uint8_t k)
{
    const struct snth_env *e = patch[i].tone[j].env + k;

    /* Indicate whether all parameters of an envelope have default state. */

    return ((e->a != DEF_ENV_A) ||
            (e->d != DEF_ENV_D) ||
            (e->s != DEF_ENV_S) ||
            (e->r != DEF_ENV_R));
}

static int snth_stat_lfo(uint8_t i, uint8_t j, uint8_t k)
{
    const struct snth_lfo *l = patch[i].tone[j].lfo + k;

    /* Indicate whether all parameters of an LFO have default state. */

    return ((l->wave   != DEF_LFO_WAVE)   ||
            (l->sync   != DEF_LFO_SYNC)   ||
            (l->rate   != DEF_LFO_RATE)   ||
            (l->delay  != DEF_LFO_DELAY)  ||
            (l->level  != DEF_LFO_LEVEL)  ||
            (l->pan    != DEF_LFO_PAN)    ||
            (l->pitch  != DEF_LFO_PITCH)  ||
            (l->phase  != DEF_LFO_PHASE)  ||
            (l->filter != DEF_LFO_FILTER));
}

static int snth_stat_tone(uint8_t i, uint8_t j)
{
    const struct snth_tone *t = patch[i].tone + j;

    uint8_t def_tone_mode = j ? DEF_TONE_MODE : SNTH_MODE_MIX;

    /* Indicate whether all parameters of a tone have default state. */

    return ((t->wave         != DEF_TONE_WAVE)  ||
            (t->mode         != def_tone_mode)  ||
            (t->level        != DEF_TONE_LEVEL) ||
            (t->pan          != DEF_TONE_PAN)   ||
            (t->delay        != DEF_TONE_DELAY) ||

            (t->pitch_coarse != DEF_TONE_PITCH_COARSE) ||
            (t->pitch_fine   != DEF_TONE_PITCH_FINE)   ||
            (t->pitch_env    != DEF_TONE_PITCH_ENV)    ||

            (t->filter_mode  != DEF_TONE_FILTER_MODE)  ||
            (t->filter_cut   != DEF_TONE_FILTER_CUT)   ||
            (t->filter_res   != DEF_TONE_FILTER_RES)   ||
            (t->filter_env   != DEF_TONE_FILTER_ENV)   ||
            (t->filter_key   != DEF_TONE_FILTER_KEY));
}

static int snth_stat_patch(uint8_t i)
{
    uint8_t j;
    uint8_t k;

    /* Indicate whether all parameters of a patch have default state. */

    if (strcmp(patch[i].name, DEF_PATCH_NAME))
        return 1;

    for (j = 0; j < MAXTONE; ++j)
    {
        if (snth_stat_tone(i, j))
            return 1;

        for (k = 0; k < MAXENV; ++k)
            if (snth_stat_env(i, j, k))
                return 1;

        for (k = 0; k < MAXLFO; ++k)
            if (snth_stat_lfo(i, j, k))
                return 1;
    }

    return 0;
}

/*===========================================================================*/
/* System dump                                                               */

static size_t dump_str(uint8_t *p, size_t c, size_t n,
                       uint8_t k, const char *v, const char *d)
{
    size_t l = strlen(v);

    /* Dump a string if it fits in the buffer and has non-default value. */

    if (c + l + 2 < n && strcmp(v, d))
    {
        p[c] = k;
        strcpy((char *) (p + c + 1), v);

        return c + l + 2;
    }
    return c;
}

static size_t dump_val(uint8_t *p, size_t c, size_t n,
                       uint8_t k, uint8_t v, uint8_t d)
{
    /* Dump a parameter if it fits in the buffer and has non-default value. */

    if (c + 1 < n && v != d)
    {
        p[c + 0] = k;
        p[c + 1] = v;

        return c + 2;
    }
    return c;
}

/*---------------------------------------------------------------------------*/

static size_t dump_env(uint8_t *p, size_t c, size_t n,
                       uint8_t i, uint8_t j, uint8_t k)
{
    struct snth_env *e = patch[i].tone[j].env + k;

    uint8_t tt = (uint8_t) (j << 4);
    uint8_t ee = (uint8_t) (k << 2);

    /* Dump envelope parameters. */

    c = dump_val(p, c, n, 0x40 | tt | ee, e->a, DEF_ENV_A);
    c = dump_val(p, c, n, 0x41 | tt | ee, e->d, DEF_ENV_D);
    c = dump_val(p, c, n, 0x42 | tt | ee, e->s, DEF_ENV_S);
    c = dump_val(p, c, n, 0x43 | tt | ee, e->r, DEF_ENV_R);

    return c;
}

static size_t dump_lfo(uint8_t *p, size_t c, size_t n,
                       uint8_t i, uint8_t j, uint8_t k)
{
    struct snth_lfo *l = patch[i].tone[j].lfo + k;

    uint8_t tt = (uint8_t) (j << 4);
    uint8_t ll = (uint8_t) (k << 3);

    uint8_t     wave_sync =      l->wave |      (l->sync ? 0x10 : 0);  
    uint8_t def_wave_sync = DEF_LFO_WAVE | (DEF_LFO_SYNC ? 0x10 : 0);  

    /* Dump LFO parameters. */

    c = dump_val(p, c, n, 0x80 | tt | ll, wave_sync, def_wave_sync);
    c = dump_val(p, c, n, 0x81 | tt | ll, l->rate,   DEF_LFO_RATE);
    c = dump_val(p, c, n, 0x82 | tt | ll, l->delay,  DEF_LFO_DELAY);
    c = dump_val(p, c, n, 0x83 | tt | ll, l->level,  DEF_LFO_LEVEL);
    c = dump_val(p, c, n, 0x84 | tt | ll, l->pan,    DEF_LFO_PAN);
    c = dump_val(p, c, n, 0x85 | tt | ll, l->pitch,  DEF_LFO_PITCH);
    c = dump_val(p, c, n, 0x86 | tt | ll, l->phase,  DEF_LFO_PHASE);
    c = dump_val(p, c, n, 0x87 | tt | ll, l->filter, DEF_LFO_FILTER);

    return c;
}

static size_t dump_tone(uint8_t *p, size_t c, size_t n, uint8_t i, uint8_t j)
{
    struct snth_tone *t = patch[i].tone + j;
    uint8_t          tt = (uint8_t) (j << 4);

    uint8_t def_tone_mode = j ? DEF_TONE_MODE : SNTH_MODE_MIX;

    /* Dump tone parameters. */

    c = dump_val(p, c, n, 0xC0 | tt, t->wave,  DEF_TONE_WAVE);
    c = dump_val(p, c, n, 0xC1 | tt, t->mode,  def_tone_mode);
    c = dump_val(p, c, n, 0xC2 | tt, t->level, DEF_TONE_LEVEL);
    c = dump_val(p, c, n, 0xC3 | tt, t->pan,   DEF_TONE_PAN);
    c = dump_val(p, c, n, 0xC4 | tt, t->delay, DEF_TONE_DELAY);

    /* Dump tone pitch parameters. */

    c = dump_val(p, c, n, 0xC8 | tt, t->pitch_coarse, DEF_TONE_PITCH_COARSE);
    c = dump_val(p, c, n, 0xC9 | tt, t->pitch_fine,   DEF_TONE_PITCH_FINE);
    c = dump_val(p, c, n, 0xCA | tt, t->pitch_env,    DEF_TONE_PITCH_ENV);

    /* Dump tone filter parameters. */

    c = dump_val(p, c, n, 0xCB | tt, t->filter_mode,  DEF_TONE_FILTER_MODE);
    c = dump_val(p, c, n, 0xCC | tt, t->filter_cut,   DEF_TONE_FILTER_CUT);
    c = dump_val(p, c, n, 0xCD | tt, t->filter_res,   DEF_TONE_FILTER_RES);
    c = dump_val(p, c, n, 0xCE | tt, t->filter_env,   DEF_TONE_FILTER_ENV);
    c = dump_val(p, c, n, 0xCF | tt, t->filter_key,   DEF_TONE_FILTER_KEY);

    return c;
}

static size_t dump_patch(uint8_t *p, size_t c, size_t n, uint8_t i)
{
    uint8_t j;
    uint8_t k;

    /* Dump the patch name. */

    c = dump_str(p, c, n, 0x30, patch[i].name, DEF_PATCH_NAME);

    /* Dump all patch parameters. */

    for (j = 0; j < MAXTONE; ++j)
    {
        c = dump_tone(p, c, n, i, j);

        for (k = 0; k < MAXENV; ++k) c = dump_env(p, c, n, i, j, k);
        for (k = 0; k < MAXLFO; ++k) c = dump_lfo(p, c, n, i, j, k);
    }

    return c;
}

/*---------------------------------------------------------------------------*/

size_t snth_dump_patch(void *d, size_t n)
{
    uint8_t *p = (uint8_t *) d;
    uint8_t  j;
    uint8_t  k;
    size_t   c = 0;

    /* Dump the SysEx header. */

    if (c < n) p[c++] = 0xF0;
    if (c < n) p[c++] = SNTH_SYSEX;

    /* Dump the patch. */

    c = dump_patch(p, c, n, channel[curr_chan].patch);

    /* Dump the SysEx footer. */

    if (c < n) p[c++] = 0xF7;

    return c;
}

size_t snth_dump_state(void *d, size_t n)
{
    uint8_t *p = (uint8_t *) d;
    uint8_t  i;

    size_t c = 0;

    /* Dump the SysEx header. */

    if (c < n) p[c++] = 0xF0;
    if (c < n) p[c++] = SNTH_SYSEX;

    /* Dump the complete system state. */
    
    for (i = 0; i < MAXPATCH; ++i)
        if (snth_stat_patch(i))
        {
            c = dump_val(p, c, n, 0x02, i, 0xFF);
            c = dump_patch(p, c, n, i);
        }

    /* Dump the SysEx footer. */

    if (c < n) p[c++] = 0xF7;

    return c;
}

/*===========================================================================*/
/* System Exclusives                                                         */

static size_t snth_midi_sysex_global(const uint8_t *p, size_t i)
{
    switch (p[i] & 0x0F)
    {
    case 0x00: snth_set_channel(p[i + 1]); break;
    case 0x01: snth_set_bank   (p[i + 1]); break;
    case 0x02: snth_set_patch  (p[i + 1]); break;
    }
    return i + 2;
}

static size_t snth_midi_sysex_channel(const uint8_t *p, size_t i)
{
    return i + 2;
}

static size_t snth_midi_sysex_effects(const uint8_t *p, size_t i)
{
    return i + 2;
}

static size_t snth_midi_sysex_patch(const uint8_t *p, size_t i)
{
    size_t l;

    switch (p[i] & 0x0F)
    {
    case 0x00: 
        snth_set_patch_name((const char *) (p + i + 1));
        return   i + strlen((const char *) (p + i + 1)) + 2;
    }
    return i + 2;
}

static size_t snth_midi_sysex_tone(const uint8_t *p, size_t i)
{
    uint8_t t = (p[i + 0] & 0x30) >> 4;
    uint8_t v =  p[i + 1];

    /* Apply a general tone SysEx. */

    switch (p[i] & 0x0F)
    {
    case 0x00: snth_set_tone_wave        (t, v); break;
    case 0x01: snth_set_tone_mode        (t, v); break;
    case 0x02: snth_set_tone_level       (t, v); break;
    case 0x03: snth_set_tone_pan         (t, v); break;
    case 0x04: snth_set_tone_delay       (t, v); break;

    case 0x08: snth_set_tone_pitch_coarse(t, v); break;
    case 0x09: snth_set_tone_pitch_fine  (t, v); break;
    case 0x0A: snth_set_tone_pitch_env   (t, v); break;

    case 0x0B: snth_set_tone_filter_mode (t, v); break;
    case 0x0C: snth_set_tone_filter_cut  (t, v); break;
    case 0x0D: snth_set_tone_filter_res  (t, v); break;
    case 0x0E: snth_set_tone_filter_env  (t, v); break;
    case 0x0F: snth_set_tone_filter_key  (t, v); break;
    }

    return i + 2;
}

static size_t snth_midi_sysex_env(const uint8_t *p, size_t i)
{
    uint8_t t = (p[i + 0] & 0x30) >> 4;
    uint8_t e = (p[i + 0] & 0x0C) >> 2;
    uint8_t v =  p[i + 1];

    /* Apply a tone envelope SysEx. */

    switch (p[i] & 0x03)
    {
    case 0x00: snth_set_tone_env_a(t, e, v); break;
    case 0x01: snth_set_tone_env_d(t, e, v); break;
    case 0x02: snth_set_tone_env_s(t, e, v); break;
    case 0x03: snth_set_tone_env_r(t, e, v); break;
    }

    return i + 2;
}

static size_t snth_midi_sysex_lfo(const uint8_t *p, size_t i)
{
    uint8_t t = (p[i + 0] & 0x30) >> 4;
    uint8_t l = (p[i + 0] & 0x08) >> 3;
    uint8_t v =  p[i + 1];

    /* Apply a tone LFO SysEx. */

    switch (p[i] & 0x07)
    {
    case 0x00: snth_set_tone_lfo_wave  (t, l, v & 0x0F);
               snth_set_tone_lfo_sync  (t, l, v & 0xF0); break;

    case 0x01: snth_set_tone_lfo_rate  (t, l, v); break;
    case 0x02: snth_set_tone_lfo_delay (t, l, v); break;
    case 0x03: snth_set_tone_lfo_level (t, l, v); break;
    case 0x04: snth_set_tone_lfo_pan   (t, l, v); break;
    case 0x05: snth_set_tone_lfo_pitch (t, l, v); break;
    case 0x06: snth_set_tone_lfo_phase (t, l, v); break;
    case 0x07: snth_set_tone_lfo_filter(t, l, v); break;
    }

    return i + 2;
}

static size_t snth_midi_sysex(const uint8_t *p, size_t i)
{
    i++;

    /* Process SysEx messages carrying the recognized ID.  Skip others. */

    if (p[i] == SNTH_SYSEX)
    {
        i++;

        while (p[i] != 0xF7)
        {
            if      ((p[i] & 0xF0) == 0x00) i = snth_midi_sysex_global (p, i);
            else if ((p[i] & 0xF0) == 0x10) i = snth_midi_sysex_channel(p, i);
            else if ((p[i] & 0xF0) == 0x20) i = snth_midi_sysex_effects(p, i);
            else if ((p[i] & 0xF0) == 0x30) i = snth_midi_sysex_patch  (p, i);
            else if ((p[i] & 0xC0) == 0x40) i = snth_midi_sysex_env    (p, i);
            else if ((p[i] & 0xC0) == 0x80) i = snth_midi_sysex_lfo    (p, i);
            else if ((p[i] & 0xC0) == 0xC0) i = snth_midi_sysex_tone   (p, i);
        }
    }
    else
        while (p[i] != 0xF7)
            i++;

    return i + 1;
}

/*---------------------------------------------------------------------------*/
/* MIDI input                                                                */

static size_t snth_midi_note_off(const uint8_t *p, size_t i)
{
    uint8_t c = p[i + 0] & 0x0F;
    uint8_t n = p[i + 1];
    uint8_t v = p[i + 2];

    snth_note_off(c, n, v);

    return i + 3;
}

static size_t snth_midi_note_on(const uint8_t *p, size_t i)
{
    uint8_t c = p[i + 0] & 0x0F;
    uint8_t n = p[i + 1];
    uint8_t v = p[i + 2];

    snth_note_on(c, n, v);

    return i + 3;
}

void snth_midi(const void *d, size_t n)
{
    const uint8_t *p = (const uint8_t *) d;

    size_t i = 0;

    while (i < n)
        if      ((p[i])        == 0xF0) i = snth_midi_sysex   (p, i);
        else if ((p[i] & 0xF0) == 0x80) i = snth_midi_note_off(p, i);
        else if ((p[i] & 0xF0) == 0x90) i = snth_midi_note_on (p, i);
}

/*===========================================================================*/

static void snth_init_channel(uint8_t i)
{
    /* Set channel defaults. */

    channel[i].patch  = i;
    channel[i].level  = DEF_CHANNEL_LEVEL;
    channel[i].pan    = DEF_CHANNEL_PAN;
    channel[i].reverb = DEF_CHANNEL_REVERB;
    channel[i].chorus = DEF_CHANNEL_CHORUS;
}

static void snth_init_env(uint8_t i, uint8_t j, uint8_t k)
{
    struct snth_env *e = patch[i].tone[j].env + k;

    /* Set envelope defaults. */

    e->a = DEF_ENV_A;
    e->d = DEF_ENV_D;
    e->s = DEF_ENV_S;
    e->r = DEF_ENV_R;
    
    snth_set_tone_env_cache(i, j, k);
}

static void snth_init_lfo(uint8_t i, uint8_t j, uint8_t k)
{
    struct snth_lfo *l = patch[i].tone[j].lfo + k;

    /* Set LFO defaults. */

    l->wave   = DEF_LFO_WAVE;
    l->sync   = DEF_LFO_SYNC;
    l->rate   = DEF_LFO_RATE;
    l->delay  = DEF_LFO_DELAY;
    l->level  = DEF_LFO_LEVEL;
    l->pan    = DEF_LFO_PAN;
    l->pitch  = DEF_LFO_PITCH;
    l->phase  = DEF_LFO_PHASE;
    l->filter = DEF_LFO_FILTER;

    snth_set_tone_lfo_cache(i, j, k);
}

static void snth_init_tone(uint8_t i, uint8_t j)
{
    struct snth_tone *t = patch[i].tone + j;

    int def_tone_mode = j ? DEF_TONE_MODE : SNTH_MODE_MIX;

    /* Set tone defaults. */

    t->wave         = DEF_TONE_WAVE;
    t->mode         = def_tone_mode;
    t->level        = DEF_TONE_LEVEL;
    t->pan          = DEF_TONE_PAN;
    t->delay        = DEF_TONE_DELAY;

    t->pitch_coarse = DEF_TONE_PITCH_COARSE;
    t->pitch_fine   = DEF_TONE_PITCH_FINE;
    t->pitch_env    = DEF_TONE_PITCH_ENV;

    t->filter_cut   = DEF_TONE_FILTER_CUT;
    t->filter_res   = DEF_TONE_FILTER_RES;
    t->filter_env   = DEF_TONE_FILTER_ENV;
    t->filter_key   = DEF_TONE_FILTER_KEY;

    snth_set_tone_cache(i, j);
}

static void snth_init_patch(uint8_t i)
{
    uint8_t j;
    uint8_t k;

    /* Set patch defaults. */

    strncpy(patch[i].name, DEF_PATCH_NAME, MAXSTR);

    for (j = 0; j < MAXTONE; ++j)
    {
        snth_init_tone(i, j);

        for (k = 0; k < MAXENV; ++k) snth_init_env(i, j, k);
        for (k = 0; k < MAXLFO; ++k) snth_init_lfo(i, j, k);
    }
}

void snth_init(int r)
{
    int i;

    rate = r;

    /* Compute the sine table. */

    for (i = 0; i < MAXSINE; ++i)
    {
        float k0 = sinf(6.283185307f *  i      / MAXSINE);
        float k1 = sinf(6.283185307f * (i + 1) / MAXSINE);

        sine_tab_k[i] = k0;
        sine_tab_d[i] = k1 - k0;
    }

    /* Compute the frequency table. */

    for (i = 0; i < MAXPITCH; ++i)
    {
        float k0 = 440.0f * powf(2, (float) (i - 69) / 12);
        float k1 = 440.0f * powf(2, (float) (i - 68) / 12);

        freq_tab_k[i] = k0;
        freq_tab_d[i] = k1 - k0;
    }

    /* Initialize all channels and patches. */

    for (i = 0; i < MAXCHANNEL; ++i)
        snth_init_channel(i);
    for (i = 0; i < MAXPATCH; ++i)
        snth_init_patch(i);

    /* Initialize all notes. */

    memset(note, 0, MAXNOTE * sizeof (struct snth_note));

    curr_chan = 0;
}

/*===========================================================================*/

