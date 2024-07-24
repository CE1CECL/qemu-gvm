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

static const desc_param glue(paramaters_zero_, PARAM)[] = {
    {
        .id  = 0x0,
        .val = 0x10ec0885,
    },{
        .id  = 0x1,
        .val = 0x106b3800,
    },{
        .id  = 0x2,
        .val = 0x100103,
    },{
        .id  = 0x4,
        .val = 0x10001,
    },
};

static const desc_param glue(paramaters_one_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x1,
    },{
        .id  = 0x4,
        .val = 0x20025,
    },{
        .id  = 0xf,
        .val = 0xf,
    },{
        .id  = 0x1,
        .val = 0x106b3800,
    },
};

static const desc_param glue(paramaters_two_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x11,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_three_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x11,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_four_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x11,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_five_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x11,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_six_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x211,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_seven_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x10011b,
    },{
        .id  = 0xe,
        .val = 0x1,
    },{
        .id  = 0xd,
        .val = 0x80032e10,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_eight_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x10011b,
    },{
        .id  = 0xe,
        .val = 0x1,
    },{
        .id  = 0xd,
        .val = 0x80032e10,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_nine_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x10011b,
    },{
        .id  = 0xe,
        .val = 0x1,
    },{
        .id  = 0xd,
        .val = 0x80032e10,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_ten_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0xf00000,
    },
};

static const desc_param glue(paramaters_eleven_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0xf00000,
    },
};

static const desc_param glue(paramaters_twelve_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0xf00000,
    },
};

static const desc_param glue(paramaters_thirteen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0xf00000,
    },
};

static const desc_param glue(paramaters_fourteen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x40018f,
    },{
        .id  = 0xc,
        .val = 0x373c,
    },{
        .id  = 0xe,
        .val = 0x5,
    },{
        .id  = 0x12,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_fifteen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x40018f,
    },{
        .id  = 0xc,
        .val = 0x373c,
    },{
        .id  = 0xe,
        .val = 0x5,
    },{
        .id  = 0x12,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_sixteen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0xc,
        .val = 0x3c,
    },
};

static const desc_param glue(paramaters_seventeen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0xc,
        .val = 0x3c,
    },
};

static const desc_param glue(paramaters_eighteen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x40018f,
    },{
        .id  = 0xc,
        .val = 0x373c,
    },{
        .id  = 0xd,
        .val = 0x270300,
    },{
        .id  = 0xe,
        .val = 0x5,
    },
};

static const desc_param glue(paramaters_nineteen_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0xc,
        .val = 0x373c,
    },
};

static const desc_param glue(paramaters_twenty_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0xf00040,
    },
};

static const desc_param glue(paramaters_twenty_one_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x600080,
    },
};

static const desc_param glue(paramaters_twenty_two_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010b,
    },{
        .id  = 0xe,
        .val = 0xb,
    },{
        .id  = 0xd,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_twenty_three_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010b,
    },{
        .id  = 0xe,
        .val = 0xb,
    },{
        .id  = 0xd,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_twenty_four_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010b,
    },{
        .id  = 0xe,
        .val = 0xb,
    },{
        .id  = 0xd,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_twenty_five_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x11,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_twenty_six_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010f,
    },{
        .id  = 0xe,
        .val = 0x2,
    },
};

static const desc_param glue(paramaters_a_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x100391,
    },{
        .id  = 0xe,
        .val = 0x1,
    },{
        .id  = 0xb,
        .val = 0x1,
    },{
        .id  = 0xa,
        .val = 0x20040,
    },
};

static const desc_param glue(paramaters_b_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010b,
    },{
        .id  = 0xe,
        .val = 0xa,
    },{
        .id  = 0xd,
        .val = 0x80051f17,
    },{
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x5,
        .val = 0x0,
    },
};

static const desc_param glue(paramaters_c_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010f,
    },{
        .id  = 0xe,
        .val = 0x2,
    },{
        .id  = 0x12,
        .val = 0x34040,
    },{
        .id  = 0xd,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_d_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010f,
    },{
        .id  = 0xe,
        .val = 0x2,
    },{
        .id  = 0x12,
        .val = 0x34040,
    },{
        .id  = 0xd,
        .val = 0x80000000,
    },
};

static const desc_param glue(paramaters_e_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010f,
    },{
        .id  = 0xe,
        .val = 0x2,
    },
};

static const desc_param glue(paramaters_f_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x20010f,
    },{
        .id  = 0xe,
        .val = 0x2,
    },
};

static const desc_param glue(paramaters_one_a_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x40018f,
    },{
        .id  = 0xc,
        .val = 0x373c,
    },{
        .id  = 0xd,
        .val = 0x270300,
    },{
        .id  = 0xe,
        .val = 0x5,
    },
};

static const desc_param glue(paramaters_one_b_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0xc,
        .val = 0x373c,
    },
};

static const desc_param glue(paramaters_one_c_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x400001,
    },{
        .id  = 0xc,
        .val = 0x20,
    },
};

static const desc_param glue(paramaters_one_d_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x400000,
    },{
        .id  = 0xc,
        .val = 0x20,
    },
};

static const desc_param glue(paramaters_one_e_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x400300,
    },{
        .id  = 0xc,
        .val = 0x10,
    },{
        .id  = 0xe,
        .val = 0x1,
    },
};

static const desc_param glue(paramaters_one_f_, PARAM)[] = {
    {
        .id  = 0x5,
        .val = 0x0,
    },{
        .id  = 0x9,
        .val = 0x400200,
    },{
        .id  = 0xc,
        .val = 0x20,
    },
};

static const desc_node glue(nodes_realtek_alc_, PARAM)[] = {
    {
        .nid     = 0x0,
        .name    = "zero",
        .params  = glue(paramaters_zero_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_zero_, PARAM)),
    },{
        .nid     = 0x1,
        .name    = "one",
        .params  = glue(paramaters_one_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_, PARAM)),
    },{
        .nid     = 0x2,
        .name    = "two",
        .params  = glue(paramaters_two_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_two_, PARAM)),
    },{
        .nid     = 0x3,
        .name    = "three",
        .params  = glue(paramaters_three_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_three_, PARAM)),
    },{
        .nid     = 0x4,
        .name    = "four",
        .params  = glue(paramaters_four_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_four_, PARAM)),
    },{
        .nid     = 0x5,
        .name    = "five",
        .params  = glue(paramaters_five_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_five_, PARAM)),
    },{
        .nid     = 0x6,
        .name    = "six",
        .params  = glue(paramaters_six_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_six_, PARAM)),
    },{
        .nid     = 0x7,
        .name    = "seven",
        .params  = glue(paramaters_seven_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_seven_, PARAM)),
        .stindex = 0x1,
        .conn    = 0x24,
    },{
        .nid     = 0x8,
        .name    = "eight",
        .params  = glue(paramaters_eight_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_eight_, PARAM)),
        .stindex = 0x1,
        .conn    = 0x23,
    },{
        .nid     = 0x9,
        .name    = "nine",
        .params  = glue(paramaters_nine_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_nine_, PARAM)),
        .stindex = 0x1,
        .conn    = 0x22,
    },{
        .nid     = 0xa,
        .name    = "a",
        .params  = glue(paramaters_a_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_a_, PARAM)),
        .stindex = 0x1,
        .conn    = 0x1f,
    },{
        .nid     = 0xb,
        .name    = "b",
        .params  = glue(paramaters_b_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_b_, PARAM)),
        .conn    = 0x1b1a1918,
    },{
        .nid     = 0xc,
        .name    = "c",
        .params  = glue(paramaters_c_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_c_, PARAM)),
        .conn    = 0xb02,
    },{
        .nid     = 0xd,
        .name    = "d",
        .params  = glue(paramaters_d_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_d_, PARAM)),
        .conn    = 0xb03,
    },{
        .nid     = 0xe,
        .name    = "e",
        .params  = glue(paramaters_e_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_e_, PARAM)),
        .conn    = 0xb04,
    },{
        .nid     = 0xf,
        .name    = "f",
        .params  = glue(paramaters_f_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_f_, PARAM)),
        .conn    = 0xb05,
    },{
        .nid     = 0x10,
        .name    = "ten",
        .params  = glue(paramaters_ten_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_ten_, PARAM)),
    },{
        .nid     = 0x11,
        .name    = "eleven",
        .params  = glue(paramaters_eleven_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_eleven_, PARAM)),
    },{
        .nid     = 0x12,
        .name    = "twelve",
        .params  = glue(paramaters_twelve_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twelve_, PARAM)),
    },{
        .nid     = 0x13,
        .name    = "thirteen",
        .params  = glue(paramaters_thirteen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_thirteen_, PARAM)),
    },{
        .nid     = 0x14,
        .name    = "fourteen",
        .params  = glue(paramaters_fourteen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_fourteen_, PARAM)),
        .conn    = 0xf0e0d0c,
        .config  = 0x90100140,
        .pinctl  = 0x40,
    },{
        .nid     = 0x15,
        .name    = "fifteen",
        .params  = glue(paramaters_fifteen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_fifteen_, PARAM)),
        .conn    = 0xf0e0d0c,
        .config  = 0x12b4050,
        .pinctl  = 0xc4,
    },{
        .nid     = 0x16,
        .name    = "sixteen",
        .params  = glue(paramaters_sixteen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_sixteen_, PARAM)),
        .config  = 0x400000f0,
        .pinctl  = 0x20,
    },{
        .nid     = 0x17,
        .name    = "seventeen",
        .params  = glue(paramaters_seventeen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_seventeen_, PARAM)),
        .config  = 0x400000f0,
        .pinctl  = 0x20,
    },{
        .nid     = 0x18,
        .name    = "eighteen",
        .params  = glue(paramaters_eighteen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_eighteen_, PARAM)),
        .conn    = 0xf0e0d0c,
        .config  = 0x90a00110,
        .pinctl  = 0x24,
    },{
        .nid     = 0x19,
        .name    = "nineteen",
        .params  = glue(paramaters_nineteen_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_nineteen_, PARAM)),
        .config  = 0x400000f0,
        .pinctl  = 0x24,
    },{
        .nid     = 0x1a,
        .name    = "one_a",
        .params  = glue(paramaters_one_a_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_a_, PARAM)),
        .conn    = 0xf0e0d0c,
        .config  = 0x18b3020,
        .pinctl  = 0x20,
    },{
        .nid     = 0x1b,
        .name    = "one_b",
        .params  = glue(paramaters_one_b_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_b_, PARAM)),
        .config  = 0x400000f0,
        .pinctl  = 0x20,
    },{
        .nid     = 0x1c,
        .name    = "one_c",
        .params  = glue(paramaters_one_c_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_c_, PARAM)),
        .config  = 0x400000f0,
        .pinctl  = 0x20,
    },{
        .nid     = 0x1d,
        .name    = "one_d",
        .params  = glue(paramaters_one_d_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_d_, PARAM)),
        .config  = 0x400000f0,
        .pinctl  = 0x20,
    },{
        .nid     = 0x1e,
        .name    = "one_e",
        .params  = glue(paramaters_one_e_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_e_, PARAM)),
        .config  = 0x14be060,
        .conn    = 0x6,
        .pinctl  = 0x40,
    },{
        .nid     = 0x1f,
        .name    = "one_f",
        .params  = glue(paramaters_one_f_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_one_f_, PARAM)),
        .config  = 0x1cbe030,
        .pinctl  = 0x20,
    },{
        .nid     = 0x20,
        .name    = "twenty",
        .params  = glue(paramaters_twenty_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_, PARAM)),
    },{
        .nid     = 0x21,
        .name    = "twenty_one",
        .params  = glue(paramaters_twenty_one_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_one_, PARAM)),
    },{
        .nid     = 0x22,
        .name    = "twenty_two",
        .params  = glue(paramaters_twenty_two_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_two_, PARAM)),
        .conn    = 0x1b1a1918,
    },{
        .nid     = 0x23,
        .name    = "twenty_three",
        .params  = glue(paramaters_twenty_three_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_three_, PARAM)),
        .conn    = 0x1b1a1918,
    },{
        .nid     = 0x24,
        .name    = "twenty_four",
        .params  = glue(paramaters_twenty_four_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_four_, PARAM)),
        .conn    = 0x1b1a1918,
    },{
        .nid     = 0x25,
        .name    = "twenty_five",
        .params  = glue(paramaters_twenty_five_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_five_, PARAM)),
    },{
        .nid     = 0x26,
        .name    = "twenty_six",
        .params  = glue(paramaters_twenty_six_, PARAM),
        .nparams = ARRAY_SIZE(glue(paramaters_twenty_six_, PARAM)),
        .conn    = 0xb25,
    },
};

static const desc_codec glue(duplex_, PARAM) = {
    .name   = "duplex",
    .nodes  = glue(nodes_realtek_alc_, PARAM),
    .nnodes = ARRAY_SIZE(glue(nodes_realtek_alc_, PARAM)),
};

static const desc_codec glue(micro_, PARAM) = {
    .name   = "micro",
    .nodes  = glue(nodes_realtek_alc_, PARAM),
    .nnodes = ARRAY_SIZE(glue(nodes_realtek_alc_, PARAM)),
};

static const desc_codec glue(output_, PARAM) = {
    .name   = "output",
    .nodes  = glue(nodes_realtek_alc_, PARAM),
    .nnodes = ARRAY_SIZE(glue(nodes_realtek_alc_, PARAM)),
};

#undef PARAM
#undef HDA_MIXER
