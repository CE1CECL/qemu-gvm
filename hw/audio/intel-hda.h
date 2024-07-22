 /*
   QEMU Realtek ALC885/ALC889A
   
   Copyright (c) 2023-2024 Christopher Eric Lentocha <christopherericlentocha@gmail.com>
   
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 or
   (at your option) version 3 of the License.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_INTEL_HDA_H
#define HW_INTEL_HDA_H

#include "hw/qdev-core.h"
#include "qom/object.h"

/* --------------------------------------------------------------------- */
/* hda bus                                                               */

#define TYPE_HDA_CODEC_DEVICE "hda-codec"
OBJECT_DECLARE_TYPE(HDACodecDevice, HDACodecDeviceClass,
                    HDA_CODEC_DEVICE)

#define TYPE_HDA_BUS "HDA"
OBJECT_DECLARE_SIMPLE_TYPE(HDACodecBus, HDA_BUS)


typedef void (*hda_codec_response_func)(HDACodecDevice *dev,
                                        bool solicited, uint32_t response);
typedef bool (*hda_codec_xfer_func)(HDACodecDevice *dev,
                                    uint32_t stnr, bool output,
                                    uint8_t *buf, uint32_t len);

struct HDACodecBus {
    BusState qbus;
    uint32_t next_cad;
    hda_codec_response_func response;
    hda_codec_xfer_func xfer;
};

struct HDACodecDeviceClass {
    DeviceClass parent_class;

    int (*init)(HDACodecDevice *dev);
    void (*exit)(HDACodecDevice *dev);
    void (*command)(HDACodecDevice *dev, uint32_t nid, uint32_t data);
    void (*stream)(HDACodecDevice *dev, uint32_t stnr, bool running, bool output);
};

struct HDACodecDevice {
    DeviceState         qdev;
    uint32_t            cad;    /* codec address */
};

void hda_codec_bus_init(DeviceState *dev, HDACodecBus *bus, size_t bus_size,
                        hda_codec_response_func response,
                        hda_codec_xfer_func xfer);
HDACodecDevice *hda_codec_find(HDACodecBus *bus, uint32_t cad);

void hda_codec_response(HDACodecDevice *dev, bool solicited, uint32_t response);
bool hda_codec_xfer(HDACodecDevice *dev, uint32_t stnr, bool output,
                    uint8_t *buf, uint32_t len);

/* --------------------------------------------------------------------- */

#define dprint(_dev, _level, _fmt, ...)                                 \
    do {                                                                \
        if (_dev->debug >= _level) {                                    \
            fprintf(stderr, "%s: ", _dev->name);                        \
            fprintf(stderr, _fmt, ## __VA_ARGS__);                      \
        }                                                               \
    } while (0)

/* --------------------------------------------------------------------- */

#endif
