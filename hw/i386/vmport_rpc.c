/*
 * QEMU VMPORT RPC emulation
 *
 * Copyright (C) 2015 Verizon Corporation
 *
 * Copyright (c) 2025 Christopher Eric Lentocha
 * <christopherericlentocha@gmail.com>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License Version 2 (GPLv2)
 * as published by the Free Software Foundation.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details. <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h" // Required to be the first #include
#include "hw/boards.h"
#include "hw/i386/vmport.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "sysemu/hw_accel.h"
#include "sysemu/qtest.h"
#include "sysemu/sysemu.h"
#include "trace.h"

#define VMPORT_RPC_DEBUG

#define TYPE_VMPORT_RPC "vmport_rpc"
#define VMPORT_RPC(obj) OBJECT_CHECK(VMPortRpcState, (obj), TYPE_VMPORT_RPC)

/* VMPORT RPC Command */
#define VMPORT_RPC_COMMAND 30

/* Limits on amount of non guest memory to use */
#define MAX_KEY_LEN 128
#define MIN_VAL_LEN 64
#define MAX_VAL_LEN 8192
#define MAX_NUM_KEY 256
#define MAX_BKTS 4
/* Max number of channels. */
#define GUESTMSG_MAX_CHANNEL 8

/*
 * All of VMware's rpc is based on 32bit registers.  So this is the
 * number of bytes in ebx.
 */
#define CHAR_PER_CALL sizeof(uint32_t)
/* Room for basic commands */
#define EXTRA_SEND 22
/* Status code and NULL */
#define EXTRA_RECV 2
#define MAX_SEND_BUF                                                           \
  DIV_ROUND_UP(EXTRA_SEND + MAX_KEY_LEN + MAX_VAL_LEN, CHAR_PER_CALL)
#define MAX_RECV_BUF DIV_ROUND_UP(EXTRA_RECV + MAX_VAL_LEN, CHAR_PER_CALL)
#define MIN_SEND_BUF                                                           \
  DIV_ROUND_UP(EXTRA_SEND + MAX_KEY_LEN + MIN_VAL_LEN, CHAR_PER_CALL)

/* Reply statuses */
/*  The basic request succeeded */
#define MESSAGE_STATUS_SUCCESS 0x0001
/*  vmware has a message available for its party */
#define MESSAGE_STATUS_DORECV 0x0002
/*  The channel has been closed */
#define MESSAGE_STATUS_CLOSED 0x0004
/*  vmware removed the message before the party fetched it */
#define MESSAGE_STATUS_UNSENT 0x0008
/*  A checkpoint occurred */
#define MESSAGE_STATUS_CPT 0x0010
/*  An underlying device is powering off */
#define MESSAGE_STATUS_POWEROFF 0x0020
/*  vmware has detected a timeout on the channel */
#define MESSAGE_STATUS_TIMEOUT 0x0040
/*  vmware supports high-bandwidth for sending and receiving the payload */
#define MESSAGE_STATUS_HB 0x0080

/* Max number of channels. */
#define GUESTMSG_MAX_CHANNEL 8

/* Flags to open a channel. */
#define GUESTMSG_FLAG_COOKIE 0x80000000

/* Data to guest */
#define VMWARE_PROTO_TO_GUEST 0x4f4c4354
/* Data from guest */
#define VMWARE_PROTO_FROM_GUEST 0x49435052

/*
 * Error return values used only in this file.  The routine
 * convert_local_rc() is used to convert these to an Error
 * object.
 */
#define VMPORT_DEVICE_NOT_FOUND -1
#define SEND_NOT_OPEN -2
#define SEND_SKIPPED -3
#define SEND_TRUCATED -4
#define SEND_NO_MEMORY -5
#define GUESTINFO_NOTFOUND -6
#define GUESTINFO_VALTOOLONG -7
#define GUESTINFO_KEYTOOLONG -8
#define GUESTINFO_TOOMANYKEYS -9
#define GUESTINFO_NO_MEMORY -10

/* The VMware RPC guest info storage . */
typedef struct {
  char *val_data;
  uint16_t val_len;
  uint16_t val_max;
} guestinfo_t;

/* The VMware RPC bucket control. */
typedef struct {
  uint16_t recv_len;
  uint16_t recv_idx;
  uint16_t recv_buf_max;
} bucket_control_t;

/* The VMware RPC bucket info. */
typedef struct {
  union {
    uint32_t *words;
    char *bytes;
  } recv;
  bucket_control_t ctl;
} bucket_t;

/* The VMware RPC channel control. */
typedef struct {
  uint64_t active_time;
  uint32_t chan_id;
  uint32_t cookie;
  uint32_t proto_num;
  uint16_t send_len;
  uint16_t send_idx;
  uint16_t send_buf_max;
  uint8_t recv_read;
  uint8_t recv_write;
} channel_control_t;

/* The VMware RPC channel info. */
typedef struct {
  union {
    uint32_t *words;
    char *bytes;
  } send;
  channel_control_t ctl;
  bucket_t recv_bkt[MAX_BKTS];
} channel_t;

/* The vmport_rpc object. */
typedef struct VMPortRpcState {
  ISADevice parent_obj;

  /* Properties */
  uint64_t reset_time;
  uint64_t build_number_value;
  uint64_t build_number_time;

  /* Private data */
  uint64_t ping_time;
  uint32_t open_cookie;
  channel_t chans[GUESTMSG_MAX_CHANNEL];
  GHashTable *guestinfo;
  /* Temporary cache for migration purposes */
  int32_t mig_chan_num;
  int32_t mig_bucket_num;
  uint32_t mig_guestinfo_size;
  uint32_t mig_guestinfo_off;
  uint8_t *mig_guestinfo_buf;
  channel_control_t *mig_chans;
  bucket_control_t *mig_buckets;
#ifdef VMPORT_RPC_DEBUG
  unsigned int end;
  unsigned int last;
  char out[2048];
#endif
} VMPortRpcState;

/* Basic request types */
typedef enum {
  MESSAGE_TYPE_OPEN,
  MESSAGE_TYPE_SENDSIZE,
  MESSAGE_TYPE_SENDPAYLOAD,
  MESSAGE_TYPE_RECVSIZE,
  MESSAGE_TYPE_RECVPAYLOAD,
  MESSAGE_TYPE_RECVSTATUS,
  MESSAGE_TYPE_CLOSE,
} MessageType;

/*
 * Overlay on the array that vmmouse_get_data() returns. The code is
 * easier to read using register names.
 */
typedef struct {
  uint32_t eax;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
  uint32_t esi;
  uint32_t edi;
} vregs;

#ifdef VMPORT_RPC_DEBUG
/*
 * Add helper function for tracing.  This routine will convert
 * binary data into more normal characters so that the trace data is
 * earier to read and will not have nulls in it.
 */
static void vmport_rpc_safe_print(VMPortRpcState *s, int len, const char *msg) {
  unsigned char c;
  unsigned int i, k;

  s->end = len;
  /* 1 byte can cnvert to 3 bytes, and save room at the end. */
  if (s->end > (sizeof(s->out) / 3 - 6)) {
    s->end = sizeof(s->out) / 3 - 6;
  }
  s->out[0] = '<';
  k = 1;
  for (i = 0; i < s->end; ++i) {
    c = msg[i];
    if ((c == '^') || (c == '\\') || (c == '>')) {
      s->out[k++] = '\\';
      s->out[k++] = c;
    } else if ((c >= ' ') && (c <= '~')) {
      s->out[k++] = c;
    } else if (c < ' ') {
      s->out[k++] = '^';
      s->out[k++] = c ^ 0x40;
    } else {
      snprintf(&s->out[k], sizeof(s->out) - k, "\\%02x", c);
      k += 3;
    }
  }
  s->out[k++] = '>';
  if (len > s->end) {
    s->out[k++] = '.';
    s->out[k++] = '.';
    s->out[k++] = '.';
  }
  s->out[k++] = 0;
  s->last = k;
}
#endif

/*
 * Copy message into a free receive bucket buffer which vmtools will
 * use to read from 4 (CHAR_PER_CALL) bytes at a time until done
 * with it.
 */
static int vmport_rpc_send(VMPortRpcState *s, channel_t *c, const char *msg,
                           unsigned int cur_recv_len) {
  int rc;
  unsigned int my_bkt = c->ctl.recv_write;
  unsigned int next_bkt = my_bkt + 1;
  bucket_t *b;

  if (next_bkt >= MAX_BKTS) {
    next_bkt = 0;
  }

  if (next_bkt == c->ctl.recv_read) {
#ifdef VMPORT_RPC_DEBUG
    {
      char prefix[30];

      snprintf(prefix, sizeof(prefix), "VMware _send skipped %d (%d, %d) ",
               c->ctl.chan_id, my_bkt, c->ctl.recv_read);
      prefix[sizeof(prefix) - 1] = 0;
      vmport_rpc_safe_print(s, cur_recv_len, msg);
      trace_vmport_rpc_send_skip(prefix, s->end, cur_recv_len, s->last,
                                 sizeof(s->out), s->out);
    }
#endif
    return SEND_SKIPPED;
  }

  c->ctl.recv_write = next_bkt;
  b = &c->recv_bkt[my_bkt];
#ifdef VMPORT_RPC_DEBUG
  {
    char prefix[30];

    snprintf(prefix, sizeof(prefix), "VMware _send %d (%d) ", c->ctl.chan_id,
             my_bkt);
    prefix[sizeof(prefix) - 1] = 0;
    vmport_rpc_safe_print(s, cur_recv_len, msg);
    trace_vmport_rpc_send_normal(prefix, s->end, cur_recv_len, s->last,
                                 sizeof(s->out), s->out);
  }
#endif

  if (b->ctl.recv_buf_max < MAX_RECV_BUF) {
    size_t new_recv_buf_max = DIV_ROUND_UP(cur_recv_len, CHAR_PER_CALL);

    if (new_recv_buf_max > b->ctl.recv_buf_max) {
      uint32_t *new_recv_buf =
          g_try_malloc((new_recv_buf_max + 1) * CHAR_PER_CALL);

      if (new_recv_buf) {
        g_free(b->recv.words);
        b->recv.words = new_recv_buf;
        b->ctl.recv_buf_max = new_recv_buf_max;
      }
    }
  }
  if (!b->recv.words) {
    return SEND_NO_MEMORY;
  }
  b->ctl.recv_len = cur_recv_len;
  b->ctl.recv_idx = 0;
  rc = 0;
  if (cur_recv_len > (b->ctl.recv_buf_max * CHAR_PER_CALL)) {
    trace_vmport_rpc_send_big(cur_recv_len,
                              b->ctl.recv_buf_max * CHAR_PER_CALL);
    cur_recv_len = b->ctl.recv_buf_max * CHAR_PER_CALL;
    b->recv.words[b->ctl.recv_buf_max] = 0;
    rc = SEND_TRUCATED;
  } else {
    b->recv.words[cur_recv_len / CHAR_PER_CALL] = 0;
    b->recv.words[DIV_ROUND_UP(cur_recv_len, CHAR_PER_CALL)] = 0;
  }
  memcpy(b->recv.words, msg, cur_recv_len);
  return rc;
}

static int vmport_rpc_ctrl_send(VMPortRpcState *s, char *msg) {
  int rc = SEND_NOT_OPEN;
  unsigned int i;

  if (!s) {
    return rc;
  }
  s->ping_time = get_clock() / 1000000000LL;
  for (i = 0; i < GUESTMSG_MAX_CHANNEL; ++i) {
    if (s->chans[i].ctl.proto_num == VMWARE_PROTO_TO_GUEST) {
      rc = vmport_rpc_send(s, &s->chans[i], msg, strlen(msg) + 1);
    }
  }
  return rc;
}

static void vmport_rpc_sweep(VMPortRpcState *s, unsigned long now_time) {
  unsigned int i;

  for (i = 0; i < GUESTMSG_MAX_CHANNEL; ++i) {
    if (s->chans[i].ctl.proto_num) {
      channel_t *c = &s->chans[i];
      long delta = now_time - c->ctl.active_time;

      if (delta >= 80) {
        trace_vmport_rpc_sweep(c->ctl.chan_id, delta);
        /* Return channel to free pool */
        c->ctl.proto_num = 0;
      }
    }
  }
}

static channel_t *vmport_rpc_new_chan(VMPortRpcState *s,
                                      unsigned long now_time) {
  unsigned int i;

  for (i = 0; i < GUESTMSG_MAX_CHANNEL; ++i) {
    if (!s->chans[i].ctl.proto_num) {
      channel_t *c = &s->chans[i];

      c->ctl.chan_id = i;
      c->ctl.cookie = s->open_cookie++;
      c->ctl.active_time = now_time;
      c->ctl.send_len = 0;
      c->ctl.send_idx = 0;
      c->ctl.recv_read = 0;
      c->ctl.recv_write = 0;
      if (!c->send.words) {
        uint32_t *new_send_buf =
            g_try_malloc0((MIN_SEND_BUF + 1) * CHAR_PER_CALL);
        if (new_send_buf) {
          c->send.words = new_send_buf;
          c->ctl.send_buf_max = MIN_SEND_BUF;
        }
      }
      if (!c->send.words) {
        return NULL;
      }
      return c;
    }
  }
  return NULL;
}

static void process_send_size(VMPortRpcState *s, channel_t *c, vregs *ur) {
  /* vmware tools often send a 0 byte request size. */
  c->ctl.send_len = ur->ebx;
  c->ctl.send_idx = 0;

  if (c->ctl.send_buf_max < MAX_SEND_BUF) {
    size_t new_send_max = DIV_ROUND_UP(c->ctl.send_len, CHAR_PER_CALL);

    if (new_send_max > c->ctl.send_buf_max) {
      uint32_t *new_send_buf =
          g_try_malloc0((new_send_max + 1) * CHAR_PER_CALL);

      if (new_send_buf) {
        g_free(c->send.words);
        c->send.words = new_send_buf;
        c->ctl.send_buf_max = new_send_max;
      }
    }
  }
  ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);
  trace_vmport_detail_rpc_process_send_size(c->ctl.chan_id, c->ctl.send_len);
}

/* ret_buffer is in/out param */
static int get_guestinfo(VMPortRpcState *s, char *a_info_key,
                         unsigned int a_key_len, char *ret_buffer,
                         unsigned int ret_buffer_len) {
  guestinfo_t *gi = NULL;

  trace_vmport_rpc_get_guestinfo(a_key_len, a_key_len, a_info_key);

  if (a_key_len <= MAX_KEY_LEN) {
    gpointer key = g_strndup(a_info_key, a_key_len);

    gi = (guestinfo_t *)g_hash_table_lookup(s->guestinfo, key);
    g_free(key);
  } else {
    return GUESTINFO_KEYTOOLONG;
  }
  if (gi) {
    unsigned int ret_len = 2 + gi->val_len;

    trace_vmport_rpc_get_guestinfo_found(gi->val_len, gi->val_len,
                                         gi->val_data);

    ret_buffer[0] = '1';
    ret_buffer[1] = ' ';
    if (ret_len > ret_buffer_len - 1) {
      ret_len = ret_buffer_len - 1;
    }
    memcpy(ret_buffer + 2, gi->val_data, ret_len);
    return ret_len;
  }

  return GUESTINFO_NOTFOUND;
}

static int set_guestinfo(VMPortRpcState *s, int a_key_len,
                         unsigned int a_val_len, const char *a_info_key,
                         char *val) {
  gpointer key = NULL;
  int rc = 0;

  trace_vmport_rpc_set_guestinfo(a_key_len, a_key_len, a_info_key, a_val_len,
                                 a_val_len, val);

  if (a_key_len <= MAX_KEY_LEN) {
    guestinfo_t *gi;

    key = g_strndup(a_info_key, a_key_len);
    gi = (guestinfo_t *)g_hash_table_lookup(s->guestinfo, key);
    if (a_val_len <= MAX_VAL_LEN) {
      if (gi) {
        if (a_val_len > gi->val_max) {
          char *new_val = g_try_malloc0(a_val_len);

          if (!new_val) {
            g_free(key);
            return GUESTINFO_NO_MEMORY;
          }
          g_free(gi->val_data);
          gi->val_max = a_val_len;
          gi->val_data = new_val;
        }
        gi->val_len = a_val_len;
        memcpy(gi->val_data, val, a_val_len);
      } else {
        int new_val_len = a_val_len;

        if (new_val_len < MIN_VAL_LEN) {
          new_val_len = MIN_VAL_LEN;
        }
        if (g_hash_table_size(s->guestinfo) >= MAX_NUM_KEY) {
          g_free(key);
          return GUESTINFO_TOOMANYKEYS;
        }
        gi = g_try_malloc(sizeof(guestinfo_t));
        if (!gi) {
          g_free(key);
          return GUESTINFO_NO_MEMORY;
        }
        gi->val_data = g_try_malloc0(new_val_len);
        if (!gi->val_data) {
          g_free(gi);
          g_free(key);
          return GUESTINFO_NO_MEMORY;
        }
        gi->val_len = a_val_len;
        gi->val_max = new_val_len;
        memcpy(gi->val_data, val, a_val_len);
        g_hash_table_insert(s->guestinfo, key, gi);
        key = NULL; /* Do not free key below */
      }
    } else {
      rc = GUESTINFO_VALTOOLONG;
    }
  } else {
    rc = GUESTINFO_KEYTOOLONG;
  }
  g_free(key);
  return rc;
}

static void process_send_payload(VMPortRpcState *s, channel_t *c, vregs *ur,
                                 unsigned long now_time) {
  /* Accumulate 4 (CHAR_PER_CALL) bytes of paload into send_buf
   * using offset */
  if (c->ctl.send_idx < c->ctl.send_buf_max) {
    c->send.words[c->ctl.send_idx] = ur->ebx;
  }

  c->ctl.send_idx++;
  ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);

  if (c->ctl.send_idx * CHAR_PER_CALL >= c->ctl.send_len) {

    /* We are done accumulating so handle the command */

    if (c->ctl.send_idx <= c->ctl.send_buf_max) {
      c->send.words[c->ctl.send_idx] = 0;
    }
#ifdef VMPORT_RPC_DEBUG
    {
      char prefix[30];

      snprintf(prefix, sizeof(prefix), "VMware RECV %d (%d) ", c->ctl.chan_id,
               c->ctl.recv_read);
      prefix[sizeof(prefix) - 1] = 0;
      vmport_rpc_safe_print(
          s, MIN(c->ctl.send_len, c->ctl.send_buf_max * CHAR_PER_CALL),
          c->send.bytes);
      trace_vmport_rpc_recv_normal(prefix, s->end, c->ctl.send_len, s->last,
                                   sizeof(s->out), s->out);
    }
#endif
    if (c->ctl.proto_num == VMWARE_PROTO_FROM_GUEST) {
      /*
       * Eaxmples of messages:
       *
       *   log toolbox: Version: build-341836
       *   SetGuestInfo  4 build-341836
       *   info-get guestinfo.ip
       *   info-set guestinfo.ip joe
       *
       */

      char *build = NULL;
      char *info_key = NULL;
      char *ret_msg = (char *)"1 ";
      char ret_buffer[2 + MAX_VAL_LEN + 2];
      unsigned int ret_len = strlen(ret_msg) + 1;

      if (strncmp(c->send.bytes, "log toolbox: Version: build-",
                  strlen("log toolbox: Version: build-")) == 0) {
        build = c->send.bytes + strlen("log toolbox: Version: build-");
      } else if (strncmp(c->send.bytes, "SetGuestInfo  4 build-",
                         strlen("SetGuestInfo  4 build-")) == 0) {
        build = c->send.bytes + strlen("SetGuestInfo  4 build-");
      } else if (strncmp(c->send.bytes, "info-get guestinfo.",
                         strlen("info-get guestinfo.")) == 0) {
        unsigned int a_key_len =
            c->ctl.send_len - strlen("info-get guestinfo.");
        int rc;

        info_key = c->send.bytes + strlen("info-get guestinfo.");
        if (((strncmp(c->send.bytes, "info-get guestinfo.svga.wddm.enable",
                      strlen("info-get guestinfo.svga.wddm.enable")) == 0) ||
             (strncmp(c->send.bytes, "info-get guestinfo.svga.enable",
                      strlen("info-get guestinfo.svga.enable")) == 0)) &&
            (strncmp(c->send.bytes,
                     "info-get guestinfo.svga.wddm.enableViewOnlyLargeCursor",
                     strlen("info-get "
                            "guestinfo.svga.wddm.enableViewOnlyLargeCursor")) !=
             0)) {
          ret_msg = (char *)"1 TRUE";
          ret_len = strlen(ret_msg) + 1;
        } else if (a_key_len <= MAX_KEY_LEN) {

          rc = get_guestinfo(s, info_key, a_key_len, ret_buffer,
                             sizeof(ret_buffer));
          if (rc == GUESTINFO_NOTFOUND) {
            ret_msg = (char *)"0 No value found";
            ret_len = strlen(ret_msg) + 1;
          } else {
            ret_msg = ret_buffer;
            ret_len = rc;
          }
        } else {
          ret_msg = (char *)"0 Key is too long";
          ret_len = strlen(ret_msg) + 1;
        }
      } else if (strncmp(c->send.bytes, "info-set guestinfo.",
                         strlen("info-set guestinfo.")) == 0) {
        char *val;
        unsigned int rest_len = c->ctl.send_len - strlen("info-set guestinfo.");

        info_key = c->send.bytes + strlen("info-set guestinfo.");
        val = strstr(info_key, " ");
        if (val) {
          unsigned int a_key_len = val - info_key;
          unsigned int a_val_len = rest_len - a_key_len - 1;
          int rc;

          val++;
          rc = set_guestinfo(s, a_key_len, a_val_len, info_key, val);
          switch (rc) {
          case 0:
            ret_msg = (char *)"1 ";
            break;
          case GUESTINFO_VALTOOLONG:
            ret_msg = (char *)"0 Value too long";
            break;
          case GUESTINFO_KEYTOOLONG:
            ret_msg = (char *)"0 Key is too long";
            break;
          case GUESTINFO_TOOMANYKEYS:
            ret_msg = (char *)"0 Too many keys";
            break;
          case GUESTINFO_NO_MEMORY:
            ret_msg = (char *)"0 Out of memory";
            break;
          }
          ret_len = strlen(ret_msg) + 1;
        } else {
          ret_msg = (char *)"0 Two and exactly two arguments expected";
          ret_len = strlen(ret_msg) + 1;
        }
      }

      vmport_rpc_send(s, c, ret_msg, ret_len);

      if (build) {
        s->build_number_value = strtol(build, NULL, 10);
        s->build_number_time = now_time;
      }
    } else {
      unsigned int my_bkt = c->ctl.recv_read - 1;
      bucket_t *b;

      if (my_bkt >= MAX_BKTS) {
        my_bkt = MAX_BKTS - 1;
      }
      b = &c->recv_bkt[my_bkt];
      b->ctl.recv_len = 0;
    }
  }
}

static void process_recv_size(VMPortRpcState *s, channel_t *c, vregs *ur) {
  bucket_t *b;
  int16_t recv_len;

  b = &c->recv_bkt[c->ctl.recv_read];
  recv_len = b->ctl.recv_len;
  if (recv_len) {
    ur->ecx = ((MESSAGE_STATUS_DORECV | MESSAGE_STATUS_SUCCESS) << 16) |
              (ur->ecx & 0xffff);
    ur->edx = (ur->edx & 0xffff) | (MESSAGE_TYPE_SENDSIZE << 16);
    ur->ebx = recv_len;
  } else {
    ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);
  }
  trace_vmport_detail_rpc_process_recv_size(c->ctl.chan_id, recv_len);
}

static void process_recv_payload(VMPortRpcState *s, channel_t *c, vregs *ur) {
  bucket_t *b;

  b = &c->recv_bkt[c->ctl.recv_read];
  if (b->ctl.recv_idx < b->ctl.recv_buf_max) {
    ur->ebx = b->recv.words[b->ctl.recv_idx++];
  } else {
    ur->ebx = 0;
  }
  ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);
  ur->edx = (ur->edx & 0xffff) | (MESSAGE_TYPE_SENDPAYLOAD << 16);
}

static void process_recv_status(VMPortRpcState *s, channel_t *c, vregs *ur) {
  ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);
  c->ctl.recv_read++;
  if (c->ctl.recv_read >= MAX_BKTS) {
    c->ctl.recv_read = 0;
  }
}

static void process_close(VMPortRpcState *s, channel_t *c, vregs *ur) {
  /* Return channel to free pool */
  c->ctl.proto_num = 0;
  ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);
  trace_vmport_rpc_process_close(c->ctl.chan_id);
}

static void process_packet(VMPortRpcState *s, channel_t *c, vregs *ur,
                           unsigned int sub_cmd, unsigned long now_time) {
  c->ctl.active_time = now_time;

  switch (sub_cmd) {
  case MESSAGE_TYPE_SENDSIZE:
    process_send_size(s, c, ur);
    break;

  case MESSAGE_TYPE_SENDPAYLOAD:
    process_send_payload(s, c, ur, now_time);
    break;

  case MESSAGE_TYPE_RECVSIZE:
    process_recv_size(s, c, ur);
    break;

  case MESSAGE_TYPE_RECVPAYLOAD:
    process_recv_payload(s, c, ur);
    break;

  case MESSAGE_TYPE_RECVSTATUS:
    process_recv_status(s, c, ur);
    break;

  case MESSAGE_TYPE_CLOSE:
    process_close(s, c, ur);
    break;

  default:
    ur->ecx = 0;
    break;
  }
}

static void vmport_rpc(VMPortRpcState *s, vregs *ur) {
  unsigned int sub_cmd = (ur->ecx >> 16) & 0xffff;
  channel_t *c = NULL;
  uint16_t msg_id;
  uint32_t msg_cookie;
  unsigned long now_time = get_clock() / 1000000000LL;
  long delta = now_time - s->ping_time;

  trace_vmport_detail_rpc_start(sub_cmd, ur->eax, ur->ebx, ur->ecx, ur->edx,
                                ur->esi, ur->edi);

  if (!s) {
    return;
  }
  if (delta > s->reset_time) {
    trace_vmport_rpc_ping(delta);
    vmport_rpc_ctrl_send(s, (char *)"reset");
  }
  vmport_rpc_sweep(s, now_time);
  do {
    /* Check to see if a new open request is happening... */
    if (MESSAGE_TYPE_OPEN == sub_cmd) {
      c = vmport_rpc_new_chan(s, now_time);
      if (!c) {
        trace_vmport_rpc_nofree();
        break;
      }

      /* Attach the apropriate protocol the the channel */
      c->ctl.proto_num = ur->ebx & ~GUESTMSG_FLAG_COOKIE;
      ur->ecx = (MESSAGE_STATUS_SUCCESS << 16) | (ur->ecx & 0xffff);
      ur->edx = (ur->edx & 0xffff) | (c->ctl.chan_id << 16);
      ur->edi = c->ctl.cookie & 0xffff;
      ur->esi = (c->ctl.cookie >> 16) & 0xffff;
      trace_vmport_rpc_process_open(c->ctl.chan_id, c->ctl.proto_num);
      if (c->ctl.proto_num == VMWARE_PROTO_TO_GUEST) {
        vmport_rpc_send(s, c, "reset", strlen("reset") + 1);
      }
      break;
    }

    msg_id = (ur->edx >> 16) & 0xffff;
    msg_cookie = (ur->edi & 0xffff) | (ur->esi << 16);
    if (msg_id >= GUESTMSG_MAX_CHANNEL) {
      trace_vmport_rpc_bad_chan(msg_id, GUESTMSG_MAX_CHANNEL);
      break;
    }
    c = &s->chans[msg_id];
    if (!c->ctl.proto_num) {
      trace_vmport_rpc_chan_not_open(msg_id);
      break;
    }

    /* We check the cookie here since it's possible that the
     * connection timed out on us and another channel was opened
     * if this happens, return error and the vmware tool will
     * need to reopen the connection
     */
    if (msg_cookie != c->ctl.cookie) {
      trace_vmport_rpc_bad_cookie(msg_cookie, c->ctl.cookie);
      break;
    }
    process_packet(s, c, ur, sub_cmd, now_time);
  } while (0);

  if (!c) {
    ur->ecx &= 0xffff;
  }

  trace_vmport_detail_rpc_end(sub_cmd, ur->eax, ur->ebx, ur->ecx, ur->edx,
                              ur->esi, ur->edi);
}

static void vmmouse_get_data(uint32_t *data) {
  X86CPU *cpu = X86_CPU(current_cpu);
  CPUX86State *env = &cpu->env;

  data[0] = env->regs[R_EAX];
  data[1] = env->regs[R_EBX];
  data[2] = env->regs[R_ECX];
  data[3] = env->regs[R_EDX];
  data[4] = env->regs[R_ESI];
  data[5] = env->regs[R_EDI];
}

static void vmmouse_set_data(const uint32_t *data) {
  X86CPU *cpu = X86_CPU(current_cpu);
  CPUX86State *env = &cpu->env;

  env->regs[R_EAX] = data[0];
  env->regs[R_EBX] = data[1];
  env->regs[R_ECX] = data[2];
  env->regs[R_EDX] = data[3];
  env->regs[R_ESI] = data[4];
  env->regs[R_EDI] = data[5];
}

static uint32_t vmport_rpc_ioport_read(void *opaque, uint32_t addr) {
  VMPortRpcState *s = opaque;
  union {
    uint32_t data[6];
    vregs regs;
  } ur;

  vmmouse_get_data(ur.data);

  vmport_rpc(s, &ur.regs);

  vmmouse_set_data(ur.data);
  return ur.data[0];
}

static void vmport_rpc_reset(DeviceState *d) {
  unsigned int i;
  VMPortRpcState *s = VMPORT_RPC(d);

  s->reset_time = 14;
  s->build_number_value = 0;
  s->build_number_time = 0;
  for (i = 0; i < GUESTMSG_MAX_CHANNEL; ++i) {
    unsigned int j;
    channel_t *c = &s->chans[i];

    for (j = 0; j < MAX_BKTS; ++j) {
      bucket_t *b = &c->recv_bkt[j];

      g_free(b->recv.words);
    }
    g_free(c->send.words);
    memset(c, 0, sizeof(*c));
  }
  g_hash_table_remove_all(s->guestinfo);
}

static void free_guestinfo(gpointer opaque) {
  guestinfo_t *gi = (guestinfo_t *)opaque;
  g_free(gi->val_data);
  g_free(gi);
}

static void vmport_rpc_realize(DeviceState *dev, Error **errp) {
  VMPortRpcState *s = VMPORT_RPC(dev);

  if (!vmport_register(VMPORT_RPC_COMMAND, vmport_rpc_ioport_read, s)) {
    error_set(errp, ERROR_CLASS_GENERIC_ERROR,
              "vmport_rpc needs vmport enabled");
  } else {
    s->guestinfo =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_guestinfo);
  }
}

static Property vmport_rpc_properties[] = {
    DEFINE_PROP_UINT64("reset-time", VMPortRpcState, reset_time, 14),
    DEFINE_PROP_UINT64("build-number-value", VMPortRpcState, build_number_value,
                       0),
    DEFINE_PROP_UINT64("build-number-time", VMPortRpcState, build_number_time,
                       0),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_vmport_rpc_chan = {
    .name = "vmport_rpc/chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){VMSTATE_UINT64(active_time, channel_control_t),
                               VMSTATE_UINT32(chan_id, channel_control_t),
                               VMSTATE_UINT32(cookie, channel_control_t),
                               VMSTATE_UINT32(proto_num, channel_control_t),
                               VMSTATE_UINT16(send_len, channel_control_t),
                               VMSTATE_UINT16(send_idx, channel_control_t),
                               VMSTATE_UINT16(send_buf_max, channel_control_t),
                               VMSTATE_UINT8(recv_read, channel_control_t),
                               VMSTATE_UINT8(recv_write, channel_control_t),
                               VMSTATE_END_OF_LIST()},
};

static const VMStateDescription vmstate_vmport_rpc_bucket = {
    .name = "vmport_rpc/bucket",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){VMSTATE_UINT16(recv_len, bucket_control_t),
                               VMSTATE_UINT16(recv_idx, bucket_control_t),
                               VMSTATE_UINT16(recv_buf_max, bucket_control_t),
                               VMSTATE_END_OF_LIST()},
};

static void vmport_rpc_size_mig_guestinfo(gpointer key, gpointer value,
                                          gpointer opaque) {
  VMPortRpcState *s = opaque;
  unsigned int key_len = strlen(key) + 1;
  guestinfo_t *gi = value;

  s->mig_guestinfo_size += 1 + key_len + 4 + gi->val_max;
}

static void vmport_rpc_fill_mig_guestinfo(gpointer key, gpointer value,
                                          gpointer opaque) {
  VMPortRpcState *s = opaque;
  unsigned int key_len = strlen(key) + 1;
  guestinfo_t *gi = value;

  trace_vmport_rpc_fill_mig_guestinfo(key_len, key_len, key, gi->val_len,
                                      gi->val_len, gi->val_data);
  s->mig_guestinfo_buf[s->mig_guestinfo_off++] = key_len;
  memcpy(s->mig_guestinfo_buf + s->mig_guestinfo_off, key, key_len);
  s->mig_guestinfo_off += key_len;
  s->mig_guestinfo_buf[s->mig_guestinfo_off++] = gi->val_len >> 8;
  s->mig_guestinfo_buf[s->mig_guestinfo_off++] = gi->val_len;
  s->mig_guestinfo_buf[s->mig_guestinfo_off++] = gi->val_max >> 8;
  s->mig_guestinfo_buf[s->mig_guestinfo_off++] = gi->val_max;
  memcpy(s->mig_guestinfo_buf + s->mig_guestinfo_off, gi->val_data,
         gi->val_max);
  s->mig_guestinfo_off += gi->val_max;
}

static int vmport_rpc_pre_load(void *opaque) {
  VMPortRpcState *s = opaque;

  g_free(s->mig_guestinfo_buf);
  s->mig_guestinfo_buf = NULL;
  s->mig_guestinfo_size = 0;
  s->mig_guestinfo_off = 0;
  g_free(s->mig_chans);
  s->mig_chans = NULL;
  s->mig_chan_num = 0;
  g_free(s->mig_buckets);
  s->mig_buckets = NULL;
  s->mig_bucket_num = 0;

  return 0;
}

static int vmport_rpc_pre_save(void *opaque) {
  VMPortRpcState *s = opaque;
  unsigned int i;
  unsigned int mig_chan_idx = 0;
  unsigned int mig_bucket_idx = 0;

  (void)vmport_rpc_pre_load(opaque);
  for (i = 0; i < GUESTMSG_MAX_CHANNEL; ++i) {
    channel_t *c = &s->chans[i];

    if (c->ctl.proto_num) {
      unsigned int j;

      s->mig_chan_num++;
      for (j = 0; j < MAX_BKTS; ++j) {
        bucket_t *b = &c->recv_bkt[j];

        s->mig_bucket_num++;
        s->mig_guestinfo_size += (b->ctl.recv_buf_max + 1) * CHAR_PER_CALL;
      }
      s->mig_guestinfo_size += (c->ctl.send_buf_max + 1) * CHAR_PER_CALL;
    }
  }
  g_hash_table_foreach(s->guestinfo, vmport_rpc_size_mig_guestinfo, s);
  s->mig_guestinfo_size++;
  s->mig_guestinfo_buf = g_malloc(s->mig_guestinfo_size);
  s->mig_chans = g_malloc(s->mig_chan_num * sizeof(channel_control_t));
  s->mig_buckets = g_malloc(s->mig_bucket_num * sizeof(bucket_control_t));

  for (i = 0; i < GUESTMSG_MAX_CHANNEL; ++i) {
    channel_t *c = &s->chans[i];

    if (c->ctl.proto_num) {
      unsigned int j;
      channel_control_t *cm = s->mig_chans + mig_chan_idx++;
      unsigned int send_chars = (c->ctl.send_buf_max + 1) * CHAR_PER_CALL;

      *cm = c->ctl;
      for (j = 0; j < MAX_BKTS; ++j) {
        bucket_t *b = &c->recv_bkt[j];
        bucket_control_t *bm = s->mig_buckets + mig_bucket_idx++;
        unsigned int recv_chars = (b->ctl.recv_buf_max + 1) * CHAR_PER_CALL;

        *bm = b->ctl;
        memcpy(s->mig_guestinfo_buf + s->mig_guestinfo_off, b->recv.words,
               recv_chars);
        s->mig_guestinfo_off += recv_chars;
      }
      memcpy(s->mig_guestinfo_buf + s->mig_guestinfo_off, c->send.words,
             send_chars);
      s->mig_guestinfo_off += send_chars;
    }
  }
  g_hash_table_foreach(s->guestinfo, vmport_rpc_fill_mig_guestinfo, s);
  s->mig_guestinfo_buf[s->mig_guestinfo_off++] = 0;
  return 0;
}

static int vmport_rpc_post_load(void *opaque, int version_id) {
  VMPortRpcState *s = opaque;
  unsigned int i;
  unsigned int key_len;
  unsigned int mig_bucket_idx = 0;

  s->mig_guestinfo_off = 0;
  for (i = 0; i < s->mig_chan_num; ++i) {
    channel_control_t *cm = s->mig_chans + i;
    channel_t *c = &s->chans[cm->chan_id];
    unsigned int j;
    unsigned int send_chars;

    c->ctl = *cm;
    for (j = 0; j < MAX_BKTS; ++j) {
      bucket_t *b = &c->recv_bkt[j];
      bucket_control_t *bm = s->mig_buckets + mig_bucket_idx++;
      unsigned int recv_chars;

      b->ctl = *bm;
      recv_chars = (b->ctl.recv_buf_max + 1) * CHAR_PER_CALL;
      b->recv.words =
          g_memdup(s->mig_guestinfo_buf + s->mig_guestinfo_off, recv_chars);
      s->mig_guestinfo_off += recv_chars;
    }
    send_chars = (c->ctl.send_buf_max + 1) * CHAR_PER_CALL;
    c->send.words =
        g_memdup(s->mig_guestinfo_buf + s->mig_guestinfo_off, send_chars);
    s->mig_guestinfo_off += send_chars;
  }

  do {
    key_len = s->mig_guestinfo_buf[s->mig_guestinfo_off++];
    if (key_len) {
      gpointer key =
          g_memdup(s->mig_guestinfo_buf + s->mig_guestinfo_off, key_len);
      guestinfo_t *gi = g_malloc(sizeof(guestinfo_t));
      unsigned int bhi, blow;

      s->mig_guestinfo_off += key_len;
      bhi = s->mig_guestinfo_buf[s->mig_guestinfo_off++];
      blow = s->mig_guestinfo_buf[s->mig_guestinfo_off++];
      gi->val_len = (bhi << 8) + blow;
      bhi = s->mig_guestinfo_buf[s->mig_guestinfo_off++];
      blow = s->mig_guestinfo_buf[s->mig_guestinfo_off++];
      gi->val_max = (bhi << 8) + blow;
      gi->val_data =
          g_memdup(s->mig_guestinfo_buf + s->mig_guestinfo_off, gi->val_max);
      s->mig_guestinfo_off += gi->val_max;
      trace_vmport_rpc_post_load(key_len, key_len, key, gi->val_len,
                                 gi->val_len, gi->val_data);
      g_hash_table_insert(s->guestinfo, key, gi);
    }
  } while (key_len);

  (void)vmport_rpc_pre_load(opaque);
  return 0;
}

static const VMStateDescription vmstate_vmport_rpc = {
    .name = "vmport_rpc",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = vmport_rpc_pre_save,
    .pre_load = vmport_rpc_pre_load,
    .post_load = vmport_rpc_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT64(reset_time, VMPortRpcState),
            VMSTATE_UINT64(build_number_value, VMPortRpcState),
            VMSTATE_UINT64(build_number_time, VMPortRpcState),
            VMSTATE_UINT64(ping_time, VMPortRpcState),
            VMSTATE_UINT32(open_cookie, VMPortRpcState),
            VMSTATE_INT32(mig_chan_num, VMPortRpcState),
            VMSTATE_STRUCT_VARRAY_ALLOC(mig_chans, VMPortRpcState, mig_chan_num,
                                        0, vmstate_vmport_rpc_chan,
                                        channel_control_t),
            VMSTATE_INT32(mig_bucket_num, VMPortRpcState),
            VMSTATE_STRUCT_VARRAY_ALLOC(
                mig_buckets, VMPortRpcState, mig_bucket_num, 0,
                vmstate_vmport_rpc_bucket, bucket_control_t),
            VMSTATE_UINT32(mig_guestinfo_size, VMPortRpcState),
            VMSTATE_VBUFFER_ALLOC_UINT32(mig_guestinfo_buf, VMPortRpcState, 1,
                                         NULL, mig_guestinfo_size),
            VMSTATE_END_OF_LIST()},
};

static void vmport_rpc_class_init(ObjectClass *klass, void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);

  dc->realize = vmport_rpc_realize;
  dc->reset = vmport_rpc_reset;
  dc->desc = "Enable VMware's hyper-call rpc";
  dc->props_ = vmport_rpc_properties;
  dc->vmsd = &vmstate_vmport_rpc;
}

static const TypeInfo vmport_rpc_info = {
    .name = TYPE_VMPORT_RPC,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(VMPortRpcState),
    .class_init = vmport_rpc_class_init,
};

static void vmport_rpc_register_types(void) {
  type_register_static(&vmport_rpc_info);
}

type_init(vmport_rpc_register_types)
