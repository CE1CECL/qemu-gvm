/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * written by Gerd Hoffmann <kraxel@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "intel-hda.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "intel-hda-defs.h"
#include "audio/audio.h"
#include "trace.h"
#include "qom/object.h"

/* -------------------------------------------------------------------------- */

typedef struct desc_param {
    uint32_t id;
    uint32_t val;
} desc_param;

typedef struct desc_node {
    uint32_t nid;
    const char *name;
    const desc_param *params;
    uint32_t nparams;
    uint32_t config;
    uint32_t pinctl;
    uint32_t *conn;
    uint32_t stindex;
} desc_node;

typedef struct desc_codec {
    const char *name;
    const desc_node *nodes;
    uint32_t nnodes;
} desc_codec;

static const desc_param* hda_codec_find_param(const desc_node *node, uint32_t id)
{
    int i;

    for (i = 0; i < node->nparams; i++) {
        if (node->params[i].id == id) {
            return &node->params[i];
        }
    }
    return NULL;
}

static const desc_node* hda_codec_find_node(const desc_codec *codec, uint32_t nid)
{
    int i;

    for (i = 0; i < codec->nnodes; i++) {
        if (codec->nodes[i].nid == nid) {
            return &codec->nodes[i];
        }
    }
    return NULL;
}

static void hda_codec_parse_fmt(uint32_t format, struct audsettings *as)
{
    as->freq = (format & AC_FMT_BASE_48K) ? 44100 : 48000;

    switch ((format & AC_FMT_MULT_MASK) >> AC_FMT_MULT_SHIFT) {
    case 1: as->freq *= 2; break;
    case 2: as->freq *= 3; break;
    case 3: as->freq *= 4; break;
    }

    switch ((format & AC_FMT_DIV_MASK) >> AC_FMT_DIV_SHIFT) {
    case 1: as->freq /= 2; break;
    case 2: as->freq /= 3; break;
    case 3: as->freq /= 4; break;
    case 4: as->freq /= 5; break;
    case 5: as->freq /= 6; break;
    case 6: as->freq /= 7; break;
    case 7: as->freq /= 8; break;
    }

    switch (format & AC_FMT_BITS_MASK) {
    case AC_FMT_BITS_8:  as->fmt = AUDIO_FORMAT_S8;  break;
    case AC_FMT_BITS_16: as->fmt = AUDIO_FORMAT_S16; break;
    case AC_FMT_BITS_32: as->fmt = AUDIO_FORMAT_S32; break;
    }

    as->nchannels = ((format & AC_FMT_CHAN_MASK) >> AC_FMT_CHAN_SHIFT) + 1;
}

/* -------------------------------------------------------------------------- */
/*
 * HDA codec descriptions
 */

/* some defines */

#define QEMU_HDA_PCM_FORMATS (AC_SUPPCM_BITS_16 | 0x1fc)
#define QEMU_HDA_AMP_NONE    (0)
#define QEMU_HDA_AMP_STEPS   0x4a

#define   PARAM mixemu
#define   HDA_MIXER
#include "hda-codec-common.h"

#define   PARAM nomixemu
#include  "hda-codec-common.h"

#define HDA_TIMER_TICKS (SCALE_MS)
#define B_SIZE sizeof(st->buf)
#define B_MASK (sizeof(st->buf) - 1)

/* -------------------------------------------------------------------------- */

static const char *fmt2name[] = {
    [ AUDIO_FORMAT_U8  ] = "PCM-U8",
    [ AUDIO_FORMAT_S8  ] = "PCM-S8",
    [ AUDIO_FORMAT_U16 ] = "PCM-U16",
    [ AUDIO_FORMAT_S16 ] = "PCM-S16",
    [ AUDIO_FORMAT_U32 ] = "PCM-U32",
    [ AUDIO_FORMAT_S32 ] = "PCM-S32",
};

typedef struct HDAAudioState HDAAudioState;
typedef struct HDAAudioStream HDAAudioStream;

struct HDAAudioStream {
    HDAAudioState *state;
    const desc_node *node;
    bool output, input, running;
    uint32_t stream;
    uint32_t channel;
    uint32_t format;
    uint32_t gain_left, gain_right;
    bool mute_left, mute_right;
    struct audsettings as;
    union {
        SWVoiceIn *in;
        SWVoiceOut *out;
    } voice;
    uint8_t compat_buf[HDA_BUFFER_SIZE];
    uint32_t compat_bpos;
    uint8_t buf[8192]; /* size must be power of two */
    int64_t rpos;
    int64_t wpos;
    QEMUTimer *buft;
    int64_t buft_start;
};

#define TYPE_HDA_AUDIO "hda-audio"
OBJECT_DECLARE_SIMPLE_TYPE(HDAAudioState, HDA_AUDIO)

struct HDAAudioState {
    HDACodecDevice hda;
    const char *name;

    QEMUSoundCard card;
    const desc_codec *desc;
    HDAAudioStream st[4];
    bool running_compat[16];
    bool running_real[2 * 16];

    /* properties */
    uint32_t debug;
    bool     mixer;
    bool     use_timer;
};

static inline int64_t hda_bytes_per_second(HDAAudioStream *st)
{
    return 2LL * st->as.nchannels * st->as.freq;
}

static inline void hda_timer_sync_adjust(HDAAudioStream *st, int64_t target_pos)
{
    int64_t limit = B_SIZE / 8;
    int64_t corr = 0;

    if (target_pos > limit) {
        corr = HDA_TIMER_TICKS;
    }
    if (target_pos < -limit) {
        corr = -HDA_TIMER_TICKS;
    }
    if (target_pos < -(2 * limit)) {
        corr = -(4 * HDA_TIMER_TICKS);
    }
    if (corr == 0) {
        return;
    }

    trace_hda_audio_adjust(st->node->name, target_pos);
    st->buft_start += corr;
}

static void hda_audio_input_timer(void *opaque)
{
    HDAAudioStream *st = opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    int64_t buft_start = st->buft_start;
    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;

    int64_t wanted_rpos = hda_bytes_per_second(st) * (now - buft_start)
                          / NANOSECONDS_PER_SECOND;
    wanted_rpos &= -4; /* IMPORTANT! clip to frames */

    if (wanted_rpos <= rpos) {
        /* we already transmitted the data */
        goto out_timer;
    }

    int64_t to_transfer = MIN(wpos - rpos, wanted_rpos - rpos);
    while (to_transfer) {
        uint32_t start = (rpos & B_MASK);
        uint32_t chunk = MIN(B_SIZE - start, to_transfer);
        int rc = hda_codec_xfer(
                &st->state->hda, st->stream, false, st->buf + start, chunk);
        if (!rc) {
            break;
        }
        rpos += chunk;
        to_transfer -= chunk;
        st->rpos += chunk;
    }

out_timer:

    if (st->running) {
        timer_mod_anticipate_ns(st->buft, now + HDA_TIMER_TICKS);
    }
}

static void hda_audio_input_cb(void *opaque, int avail)
{
    HDAAudioStream *st = opaque;

    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;

    int64_t to_transfer = MIN(B_SIZE - (wpos - rpos), avail);

    while (to_transfer) {
        uint32_t start = (uint32_t) (wpos & B_MASK);
        uint32_t chunk = (uint32_t) MIN(B_SIZE - start, to_transfer);
        uint32_t read = AUD_read(st->voice.in, st->buf + start, chunk);
        wpos += read;
        to_transfer -= read;
        st->wpos += read;
        if (chunk != read) {
            break;
        }
    }

    hda_timer_sync_adjust(st, -((wpos - rpos) - (B_SIZE >> 1)));
}

static void hda_audio_output_timer(void *opaque)
{
    HDAAudioStream *st = opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    int64_t buft_start = st->buft_start;
    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;

    int64_t wanted_wpos = hda_bytes_per_second(st) * (now - buft_start)
                          / NANOSECONDS_PER_SECOND;
    wanted_wpos &= -4; /* IMPORTANT! clip to frames */

    if (wanted_wpos <= wpos) {
        /* we already received the data */
        goto out_timer;
    }

    int64_t to_transfer = MIN(B_SIZE - (wpos - rpos), wanted_wpos - wpos);
    while (to_transfer) {
        uint32_t start = (wpos & B_MASK);
        uint32_t chunk = MIN(B_SIZE - start, to_transfer);
        int rc = hda_codec_xfer(
                &st->state->hda, st->stream, true, st->buf + start, chunk);
        if (!rc) {
            break;
        }
        wpos += chunk;
        to_transfer -= chunk;
        st->wpos += chunk;
    }

out_timer:

    if (st->running) {
        timer_mod_anticipate_ns(st->buft, now + HDA_TIMER_TICKS);
    }
}

static void hda_audio_output_cb(void *opaque, int avail)
{
    HDAAudioStream *st = opaque;

    int64_t wpos = st->wpos;
    int64_t rpos = st->rpos;

    int64_t to_transfer = MIN(wpos - rpos, avail);

    if (wpos - rpos == B_SIZE) {
        /* drop buffer, reset timer adjust */
        st->rpos = 0;
        st->wpos = 0;
        st->buft_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        trace_hda_audio_overrun(st->node->name);
        return;
    }

    while (to_transfer) {
        uint32_t start = (uint32_t) (rpos & B_MASK);
        uint32_t chunk = (uint32_t) MIN(B_SIZE - start, to_transfer);
        uint32_t written = AUD_write(st->voice.out, st->buf + start, chunk);
        rpos += written;
        to_transfer -= written;
        st->rpos += written;
        if (chunk != written) {
            break;
        }
    }

    hda_timer_sync_adjust(st, (wpos - rpos) - (B_SIZE >> 1));
}

static void hda_audio_compat_input_cb(void *opaque, int avail)
{
    HDAAudioStream *st = opaque;
    int recv = 0;
    int len;
    bool rc;

    while (avail - recv >= sizeof(st->compat_buf)) {
        if (st->compat_bpos != sizeof(st->compat_buf)) {
            len = AUD_read(st->voice.in, st->compat_buf + st->compat_bpos,
                           sizeof(st->compat_buf) - st->compat_bpos);
            st->compat_bpos += len;
            recv += len;
            if (st->compat_bpos != sizeof(st->compat_buf)) {
                break;
            }
        }
        rc = hda_codec_xfer(&st->state->hda, st->stream, false,
                            st->compat_buf, sizeof(st->compat_buf));
        if (!rc) {
            break;
        }
        st->compat_bpos = 0;
    }
}

static void hda_audio_compat_output_cb(void *opaque, int avail)
{
    HDAAudioStream *st = opaque;
    int sent = 0;
    int len;
    bool rc;

    while (avail - sent >= sizeof(st->compat_buf)) {
        if (st->compat_bpos == sizeof(st->compat_buf)) {
            rc = hda_codec_xfer(&st->state->hda, st->stream, true,
                                st->compat_buf, sizeof(st->compat_buf));
            if (!rc) {
                break;
            }
            st->compat_bpos = 0;
        }
        len = AUD_write(st->voice.out, st->compat_buf + st->compat_bpos,
                        sizeof(st->compat_buf) - st->compat_bpos);
        st->compat_bpos += len;
        sent += len;
        if (st->compat_bpos != sizeof(st->compat_buf)) {
            break;
        }
    }
}

static void hda_audio_set_running(HDAAudioStream *st, bool running)
{
    if (st->node == NULL) {
        return;
    }
    if (st->running == running) {
        return;
    }
    st->running = running;
    trace_hda_audio_running(st->node->name, st->stream, st->running);
    if (st->state->use_timer) {
        if (running) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            st->rpos = 0;
            st->wpos = 0;
            st->buft_start = now;
            timer_mod_anticipate_ns(st->buft, now + HDA_TIMER_TICKS);
        } else {
            timer_del(st->buft);
        }
    }
    if (st->output) {
        AUD_set_active_out(st->voice.out, st->running);
    } else if (st->input) {
        AUD_set_active_in(st->voice.in, st->running);
    }
}

static void hda_audio_set_amp(HDAAudioStream *st)
{
    bool muted;
    uint32_t left, right;

    if (st->node == NULL) {
        return;
    }

    muted = st->mute_left && st->mute_right;
    left  = st->mute_left  ? 0 : st->gain_left;
    right = st->mute_right ? 0 : st->gain_right;

    left = left * 255 / QEMU_HDA_AMP_STEPS;
    right = right * 255 / QEMU_HDA_AMP_STEPS;

    if (!st->state->mixer) {
        return;
    }
    if (st->output) {
        AUD_set_volume_out(st->voice.out, muted, left, right);
    } else if (st->input) {
        AUD_set_volume_in(st->voice.in, muted, left, right);
    }
}

static void hda_audio_setup(HDAAudioStream *st)
{
    bool use_timer = st->state->use_timer;
    audio_callback_fn cb;

    if (st->node == NULL) {
        return;
    }

    trace_hda_audio_format(st->node->name, st->as.nchannels,
                           fmt2name[st->as.fmt], st->as.freq);

    if (st->output) {
        if (use_timer) {
            cb = hda_audio_output_cb;
            st->buft = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    hda_audio_output_timer, st);
        } else {
            cb = hda_audio_compat_output_cb;
        }
        st->voice.out = AUD_open_out(&st->state->card, st->voice.out,
                                     st->node->name, st, cb, &st->as);
    } else if (st->input) {
        if (use_timer) {
            cb = hda_audio_input_cb;
            st->buft = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    hda_audio_input_timer, st);
        } else {
            cb = hda_audio_compat_input_cb;
        }
        st->voice.in = AUD_open_in(&st->state->card, st->voice.in,
                                   st->node->name, st, cb, &st->as);
    }
}

static void hda_audio_command(HDACodecDevice *hda, uint32_t nid, uint32_t data)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    HDAAudioStream *st;
    const desc_node *node = NULL;
    const desc_param *param;
    uint32_t verb, payload, response, count, shift;

    dprint(a, 2, "%s: data: 0x%x\n",
           __func__, data);

    if (data & 0xf0900) {
        dprint(a, 2, "4/16 id/payload 0\n");
        verb = (data >> 8) & 0xf00;
        payload = data & 0xffff;
    } else {
        dprint(a, 2, "12/8 id/payload 0\n");
        verb = (data >> 8) & 0xfff;
        payload = data & 0x00ff;
    }

    node = hda_codec_find_node(a->desc, nid);
    if (node == NULL) {
	goto fail;
    }
    dprint(a, 2, "%s: nid %d (%s), verb 0x%x, payload 0x%x\n",
           __func__, nid, node->name, verb, payload);

    switch (verb) {
    case AC_VERB_PARAMETERS:
        param = hda_codec_find_param(node, payload);
        if (param == NULL) {
            goto fail;
        }
        hda_codec_response(hda, true, param->val);
        break;

    case AC_VERB_GET_SUBSYSTEM_ID:
        hda_codec_response(hda, true, 0x106b3800);
        break;

    case AC_VERB_GET_CONNECT_LIST:
        param = hda_codec_find_param(node, AC_PAR_CONNLIST_LEN);
        count = param ? param->val : 0;
        response = 0;
        shift = 0;
        while (payload < count && shift < 32) {
            response |= node->conn[payload] << shift;
            payload++;
            shift += 8;
        }
        hda_codec_response(hda, true, response);
        break;

    case AC_VERB_GET_CONFIG_DEFAULT:
        hda_codec_response(hda, true, node->config);
        break;

    case AC_VERB_GET_PIN_WIDGET_CONTROL:
        hda_codec_response(hda, true, node->pinctl);
        break;

    case AC_VERB_SET_CHANNEL_STREAMID:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        hda_audio_set_running(st, false);
        st->stream = (payload >> 4) & 0x0f;
        st->channel = payload & 0x0f;
        dprint(a, 2, "%s: stream %d, channel %d\n",
               st->node->name, st->stream, st->channel);
        hda_audio_set_running(st, a->running_real[st->output * 16 + st->stream]);
        hda_codec_response(hda, true, 0);
        break;

    case AC_VERB_GET_CONV:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        response = st->stream << 4 | st->channel;
        hda_codec_response(hda, true, response);
        break;

    case AC_VERB_SET_STREAM_FORMAT:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        st->format = payload;
        hda_codec_parse_fmt(st->format, &st->as);
        hda_audio_setup(st);
        hda_codec_response(hda, true, 0);
        break;

    case AC_VERB_GET_STREAM_FORMAT:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        hda_codec_response(hda, true, st->format);
        break;

//    case AC_VERB_GET_AMP_GAIN_MUTE:
    case 2816:
    case 2817:
    case 2818:
    case 2819:
    case 2820:
    case 2821:
    case 2822:
    case 2823:
    case 2824:
    case 2825:
    case 2826:
    case 2827:
    case 2828:
    case 2829:
    case 2830:
    case 2831:
    case 2832:
    case 2833:
    case 2834:
    case 2835:
    case 2836:
    case 2837:
    case 2838:
    case 2839:
    case 2840:
    case 2841:
    case 2842:
    case 2843:
    case 2844:
    case 2845:
    case 2846:
    case 2847:
    case 2848:
    case 2849:
    case 2850:
    case 2851:
    case 2852:
    case 2853:
    case 2854:
    case 2855:
    case 2856:
    case 2857:
    case 2858:
    case 2859:
    case 2860:
    case 2861:
    case 2862:
    case 2863:
    case 2864:
    case 2865:
    case 2866:
    case 2867:
    case 2868:
    case 2869:
    case 2870:
    case 2871:
    case 2872:
    case 2873:
    case 2874:
    case 2875:
    case 2876:
    case 2877:
    case 2878:
    case 2879:
    case 2880:
    case 2881:
    case 2882:
    case 2883:
    case 2884:
    case 2885:
    case 2886:
    case 2887:
    case 2888:
    case 2889:
    case 2890:
    case 2891:
    case 2892:
    case 2893:
    case 2894:
    case 2895:
    case 2896:
    case 2897:
    case 2898:
    case 2899:
    case 2900:
    case 2901:
    case 2902:
    case 2903:
    case 2904:
    case 2905:
    case 2906:
    case 2907:
    case 2908:
    case 2909:
    case 2910:
    case 2911:
    case 2912:
    case 2913:
    case 2914:
    case 2915:
    case 2916:
    case 2917:
    case 2918:
    case 2919:
    case 2920:
    case 2921:
    case 2922:
    case 2923:
    case 2924:
    case 2925:
    case 2926:
    case 2927:
    case 2928:
    case 2929:
    case 2930:
    case 2931:
    case 2932:
    case 2933:
    case 2934:
    case 2935:
    case 2936:
    case 2937:
    case 2938:
    case 2939:
    case 2940:
    case 2941:
    case 2942:
    case 2943:
    case 2944:
    case 2945:
    case 2946:
    case 2947:
    case 2948:
    case 2949:
    case 2950:
    case 2951:
    case 2952:
    case 2953:
    case 2954:
    case 2955:
    case 2956:
    case 2957:
    case 2958:
    case 2959:
    case 2960:
    case 2961:
    case 2962:
    case 2963:
    case 2964:
    case 2965:
    case 2966:
    case 2967:
    case 2968:
    case 2969:
    case 2970:
    case 2971:
    case 2972:
    case 2973:
    case 2974:
    case 2975:
    case 2976:
    case 2977:
    case 2978:
    case 2979:
    case 2980:
    case 2981:
    case 2982:
    case 2983:
    case 2984:
    case 2985:
    case 2986:
    case 2987:
    case 2988:
    case 2989:
    case 2990:
    case 2991:
    case 2992:
    case 2993:
    case 2994:
    case 2995:
    case 2996:
    case 2997:
    case 2998:
    case 2999:
    case 3000:
    case 3001:
    case 3002:
    case 3003:
    case 3004:
    case 3005:
    case 3006:
    case 3007:
    case 3008:
    case 3009:
    case 3010:
    case 3011:
    case 3012:
    case 3013:
    case 3014:
    case 3015:
    case 3016:
    case 3017:
    case 3018:
    case 3019:
    case 3020:
    case 3021:
    case 3022:
    case 3023:
    case 3024:
    case 3025:
    case 3026:
    case 3027:
    case 3028:
    case 3029:
    case 3030:
    case 3031:
    case 3032:
    case 3033:
    case 3034:
    case 3035:
    case 3036:
    case 3037:
    case 3038:
    case 3039:
    case 3040:
    case 3041:
    case 3042:
    case 3043:
    case 3044:
    case 3045:
    case 3046:
    case 3047:
    case 3048:
    case 3049:
    case 3050:
    case 3051:
    case 3052:
    case 3053:
    case 3054:
    case 3055:
    case 3056:
    case 3057:
    case 3058:
    case 3059:
    case 3060:
    case 3061:
    case 3062:
    case 3063:
    case 3064:
    case 3065:
    case 3066:
    case 3067:
    case 3068:
    case 3069:
    case 3070:
    case 3071:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        if (payload & AC_AMP_GET_LEFT) {
            response = st->gain_left | (st->mute_left ? AC_AMP_MUTE : 0);
        } else {
            response = st->gain_right | (st->mute_right ? AC_AMP_MUTE : 0);
        }
        hda_codec_response(hda, true, response);
        break;

//    case AC_VERB_SET_AMP_GAIN_MUTE:
    case 768:
    case 769:
    case 770:
    case 771:
    case 772:
    case 773:
    case 774:
    case 775:
    case 776:
    case 777:
    case 778:
    case 779:
    case 780:
    case 781:
    case 782:
    case 783:
    case 784:
    case 785:
    case 786:
    case 787:
    case 788:
    case 789:
    case 790:
    case 791:
    case 792:
    case 793:
    case 794:
    case 795:
    case 796:
    case 797:
    case 798:
    case 799:
    case 800:
    case 801:
    case 802:
    case 803:
    case 804:
    case 805:
    case 806:
    case 807:
    case 808:
    case 809:
    case 810:
    case 811:
    case 812:
    case 813:
    case 814:
    case 815:
    case 816:
    case 817:
    case 818:
    case 819:
    case 820:
    case 821:
    case 822:
    case 823:
    case 824:
    case 825:
    case 826:
    case 827:
    case 828:
    case 829:
    case 830:
    case 831:
    case 832:
    case 833:
    case 834:
    case 835:
    case 836:
    case 837:
    case 838:
    case 839:
    case 840:
    case 841:
    case 842:
    case 843:
    case 844:
    case 845:
    case 846:
    case 847:
    case 848:
    case 849:
    case 850:
    case 851:
    case 852:
    case 853:
    case 854:
    case 855:
    case 856:
    case 857:
    case 858:
    case 859:
    case 860:
    case 861:
    case 862:
    case 863:
    case 864:
    case 865:
    case 866:
    case 867:
    case 868:
    case 869:
    case 870:
    case 871:
    case 872:
    case 873:
    case 874:
    case 875:
    case 876:
    case 877:
    case 878:
    case 879:
    case 880:
    case 881:
    case 882:
    case 883:
    case 884:
    case 885:
    case 886:
    case 887:
    case 888:
    case 889:
    case 890:
    case 891:
    case 892:
    case 893:
    case 894:
    case 895:
    case 896:
    case 897:
    case 898:
    case 899:
    case 900:
    case 901:
    case 902:
    case 903:
    case 904:
    case 905:
    case 906:
    case 907:
    case 908:
    case 909:
    case 910:
    case 911:
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919:
    case 920:
    case 921:
    case 922:
    case 923:
    case 924:
    case 925:
    case 926:
    case 927:
    case 928:
    case 929:
    case 930:
    case 931:
    case 932:
    case 933:
    case 934:
    case 935:
    case 936:
    case 937:
    case 938:
    case 939:
    case 940:
    case 941:
    case 942:
    case 943:
    case 944:
    case 945:
    case 946:
    case 947:
    case 948:
    case 949:
    case 950:
    case 951:
    case 952:
    case 953:
    case 954:
    case 955:
    case 956:
    case 957:
    case 958:
    case 959:
    case 960:
    case 961:
    case 962:
    case 963:
    case 964:
    case 965:
    case 966:
    case 967:
    case 968:
    case 969:
    case 970:
    case 971:
    case 972:
    case 973:
    case 974:
    case 975:
    case 976:
    case 977:
    case 978:
    case 979:
    case 980:
    case 981:
    case 982:
    case 983:
    case 984:
    case 985:
    case 986:
    case 987:
    case 988:
    case 989:
    case 990:
    case 991:
    case 992:
    case 993:
    case 994:
    case 995:
    case 996:
    case 997:
    case 998:
    case 999:
    case 1000:
    case 1001:
    case 1002:
    case 1003:
    case 1004:
    case 1005:
    case 1006:
    case 1007:
    case 1008:
    case 1009:
    case 1010:
    case 1011:
    case 1012:
    case 1013:
    case 1014:
    case 1015:
    case 1016:
    case 1017:
    case 1018:
    case 1019:
    case 1020:
    case 1021:
    case 1022:
    case 1023:
        st = a->st + node->stindex;
        if (st->node == NULL) {
            goto fail;
        }
        dprint(a, 1, "amp (%s): %s%s%s%s index %d  gain %3d %s\n",
               st->node->name,
               (payload & AC_AMP_SET_OUTPUT) ? "o" : "-",
               (payload & AC_AMP_SET_INPUT)  ? "i" : "-",
               (payload & AC_AMP_SET_LEFT)   ? "l" : "-",
               (payload & AC_AMP_SET_RIGHT)  ? "r" : "-",
               (payload & AC_AMP_SET_INDEX) >> AC_AMP_SET_INDEX_SHIFT,
               (payload & AC_AMP_GAIN),
               (payload & AC_AMP_MUTE) ? "muted" : "");
        if (payload & AC_AMP_SET_LEFT) {
            st->gain_left = payload & AC_AMP_GAIN;
            st->mute_left = payload & AC_AMP_MUTE;
        }
        if (payload & AC_AMP_SET_RIGHT) {
            st->gain_right = payload & AC_AMP_GAIN;
            st->mute_right = payload & AC_AMP_MUTE;
        }
        hda_audio_set_amp(st);
        hda_codec_response(hda, true, 0);
        break;

    default:
        dprint(a, 1, "%s: not handled: nid %d (%s), verb 0x%x, payload 0x%x\n", __func__, nid, node ? node->name : "?", verb, payload);
        hda_codec_response(hda, true, 0);
        break;
//        goto fail;
    }
    return;

fail:
    dprint(a, 1, "%s: not handled: nid %d (%s), verb 0x%x, payload 0x%x\n",
           __func__, nid, node ? node->name : "?", verb, payload);
    hda_codec_response(hda, true, 0);

}

static void hda_audio_stream(HDACodecDevice *hda, uint32_t stnr, bool running, bool output)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    int s;

    a->running_compat[stnr] = running;
    a->running_real[output * 16 + stnr] = running;
    for (s = 0; s < ARRAY_SIZE(a->st); s++) {
        if (a->st[s].node == NULL) {
            continue;
        }
        if (a->st[s].output != output) {
            continue;
        }
        if (a->st[s].stream != stnr) {
            continue;
        }
        hda_audio_set_running(&a->st[s], running);
    }
}

static int hda_audio_init(HDACodecDevice *hda, const struct desc_codec *desc)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    HDAAudioStream *st;
    const desc_node *node;
    const desc_param *param;
    uint32_t i, type;

    a->desc = desc;
    a->name = object_get_typename(OBJECT(a));
    dprint(a, 1, "%s: cad %d\n", __func__, a->hda.cad);

    AUD_register_card("hda", &a->card);
    for (i = 0; i < a->desc->nnodes; i++) {
        node = a->desc->nodes + i;
        param = hda_codec_find_param(node, AC_PAR_AUDIO_WIDGET_CAP);
        if (param == NULL) {
            continue;
        }
        type = (param->val & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
        switch (type) {
        case AC_WID_AUD_OUT:
        case AC_WID_AUD_IN:
            assert(node->stindex < ARRAY_SIZE(a->st));
            st = a->st + node->stindex;
            st->state = a;
            st->node = node;
            if (type == AC_WID_AUD_OUT) {
                /* unmute output by default */
                st->gain_left = QEMU_HDA_AMP_STEPS;
                st->gain_right = QEMU_HDA_AMP_STEPS;
                st->compat_bpos = sizeof(st->compat_buf);
                st->output = true;
                st->input = false;
            } else if (type == AC_WID_AUD_IN) {
                st->input = true;
                st->output = false;
            } else {
                st->output = false;
                st->input = false;
            }
            st->format = 0xe0560;
            hda_codec_parse_fmt(st->format, &st->as);
            hda_audio_setup(st);
            break;
        }
    }
    return 0;
}

static void hda_audio_exit(HDACodecDevice *hda)
{
    HDAAudioState *a = HDA_AUDIO(hda);
    HDAAudioStream *st;
    int i;

    dprint(a, 1, "%s\n", __func__);
    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        st = a->st + i;
        if (st->node == NULL) {
            continue;
        }
        if (a->use_timer) {
            timer_del(st->buft);
        }
        if (st->output) {
            AUD_close_out(&a->card, st->voice.out);
        } else if (st->input) {
            AUD_close_in(&a->card, st->voice.in);
        }
    }
    AUD_remove_card(&a->card);
}

static int hda_audio_post_load(void *opaque, int version)
{
    HDAAudioState *a = opaque;
    HDAAudioStream *st;
    int i;

    dprint(a, 1, "%s\n", __func__);
    if (version == 1) {
        /* assume running_compat[] is for output streams */
        for (i = 0; i < ARRAY_SIZE(a->running_compat); i++)
            a->running_real[16 + i] = a->running_compat[i];
    }

    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        st = a->st + i;
        if (st->node == NULL)
            continue;
        hda_codec_parse_fmt(st->format, &st->as);
        hda_audio_setup(st);
        hda_audio_set_amp(st);
        hda_audio_set_running(st, a->running_real[st->output * 16 + st->stream]);
    }
    return 0;
}

static void hda_audio_reset(DeviceState *dev)
{
    HDAAudioState *a = HDA_AUDIO(dev);
    HDAAudioStream *st;
    int i;

    dprint(a, 1, "%s\n", __func__);
    for (i = 0; i < ARRAY_SIZE(a->st); i++) {
        st = a->st + i;
        if (st->node != NULL) {
            hda_audio_set_running(st, false);
        }
    }
}

static bool vmstate_hda_audio_stream_buf_needed(void *opaque)
{
    HDAAudioStream *st = opaque;
    return st->state && st->state->use_timer;
}

static const VMStateDescription vmstate_hda_audio_stream_buf = {
    .name = "hda-audio-stream/buffer",
    .version_id = 1,
    .needed = vmstate_hda_audio_stream_buf_needed,
    .fields = (VMStateField[]) {
        VMSTATE_BUFFER(buf, HDAAudioStream),
        VMSTATE_INT64(rpos, HDAAudioStream),
        VMSTATE_INT64(wpos, HDAAudioStream),
        VMSTATE_TIMER_PTR(buft, HDAAudioStream),
        VMSTATE_INT64(buft_start, HDAAudioStream),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hda_audio_stream = {
    .name = "hda-audio-stream",
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(stream, HDAAudioStream),
        VMSTATE_UINT32(channel, HDAAudioStream),
        VMSTATE_UINT32(format, HDAAudioStream),
        VMSTATE_UINT32(gain_left, HDAAudioStream),
        VMSTATE_UINT32(gain_right, HDAAudioStream),
        VMSTATE_BOOL(mute_left, HDAAudioStream),
        VMSTATE_BOOL(mute_right, HDAAudioStream),
        VMSTATE_UINT32(compat_bpos, HDAAudioStream),
        VMSTATE_BUFFER(compat_buf, HDAAudioStream),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * []) {
        &vmstate_hda_audio_stream_buf,
        NULL
    }
};

static const VMStateDescription vmstate_hda_audio = {
    .name = "hda-audio",
    .version_id = 2,
    .post_load = hda_audio_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(st, HDAAudioState, 4, 0,
                             vmstate_hda_audio_stream,
                             HDAAudioStream),
        VMSTATE_BOOL_ARRAY(running_compat, HDAAudioState, 16),
        VMSTATE_BOOL_ARRAY_V(running_real, HDAAudioState, 2 * 16, 2),
        VMSTATE_END_OF_LIST()
    }
};

static Property hda_audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(HDAAudioState, card),
    DEFINE_PROP_UINT32("debug", HDAAudioState, debug,   9999999999),
    DEFINE_PROP_BOOL("mixer", HDAAudioState, mixer,  true),
    DEFINE_PROP_BOOL("use-timer", HDAAudioState, use_timer,  true),
    DEFINE_PROP_END_OF_LIST(),
};

static int hda_audio_init_output(HDACodecDevice *hda)
{
    HDAAudioState *a = HDA_AUDIO(hda);

    if (!a->mixer) {
        return hda_audio_init(hda, &output_nomixemu);
    } else {
        return hda_audio_init(hda, &output_mixemu);
    }
}

static int hda_audio_init_duplex(HDACodecDevice *hda)
{
    HDAAudioState *a = HDA_AUDIO(hda);

    if (!a->mixer) {
        return hda_audio_init(hda, &duplex_nomixemu);
    } else {
        return hda_audio_init(hda, &duplex_mixemu);
    }
}

static int hda_audio_init_micro(HDACodecDevice *hda)
{
    HDAAudioState *a = HDA_AUDIO(hda);

    if (!a->mixer) {
        return hda_audio_init(hda, &micro_nomixemu);
    } else {
        return hda_audio_init(hda, &micro_mixemu);
    }
}

static void hda_audio_base_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->exit = hda_audio_exit;
    k->command = hda_audio_command;
    k->stream = hda_audio_stream;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->reset = hda_audio_reset;
    dc->vmsd = &vmstate_hda_audio;
    device_class_set_props(dc, hda_audio_properties);
}

static const TypeInfo hda_audio_info = {
    .name          = TYPE_HDA_AUDIO,
    .parent        = TYPE_HDA_CODEC_DEVICE,
    .instance_size = sizeof(HDAAudioState),
    .class_init    = hda_audio_base_class_init,
    .abstract      = true,
};

static void hda_audio_output_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_output;
    dc->desc = "HDA Audio Codec, output-only (line-out)";
}

static const TypeInfo hda_audio_output_info = {
    .name          = "hda-output",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_output_class_init,
};

static void hda_audio_duplex_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_duplex;
    dc->desc = "HDA Audio Codec, duplex (line-out, line-in)";
}

static const TypeInfo hda_audio_duplex_info = {
    .name          = "hda-duplex",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_duplex_class_init,
};

static void hda_audio_micro_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HDACodecDeviceClass *k = HDA_CODEC_DEVICE_CLASS(klass);

    k->init = hda_audio_init_micro;
    dc->desc = "HDA Audio Codec, duplex (speaker, microphone)";
}

static const TypeInfo hda_audio_micro_info = {
    .name          = "hda-micro",
    .parent        = TYPE_HDA_AUDIO,
    .class_init    = hda_audio_micro_class_init,
};

static void hda_audio_register_types(void)
{
    type_register_static(&hda_audio_info);
    type_register_static(&hda_audio_output_info);
    type_register_static(&hda_audio_duplex_info);
    type_register_static(&hda_audio_micro_info);
}

type_init(hda_audio_register_types)
