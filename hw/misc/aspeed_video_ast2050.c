/*
 * ASPEED AST2050 (G3) Video Engine (KVM screen capture).
 *
 * Register model + capture datapath. VR000 is a protection-key lock latch (write
 * 0x1A038AA8 to unlock; reads 1 unlocked / 0 locked); the rest are RW while
 * unlocked. Datasheet cites: AST2050/AST1100 A3 datasheet V1.05 §20 (p.232-255),
 * extracted in qemu-model/peripherals/video/DATASHEET-VIDEO.md.
 *
 * Capture semantics modelled (the "internal VGA" KVM path):
 *  - VR004[0] 0->1 triggers mode detection (p.235). The modelled internal-VGA
 *    source presents a fixed 640x480@60 scanout (classic VGA timing: 800x525
 *    total, hsync 96 / vsync 2, positive polarities), reported through the
 *    read-back registers VR090/VR094 (frame edges), VR098 (V lines + stable
 *    bits), VR09C (sync widths) and VR0A0 (H total), then mode-detection-ready
 *    (VR308[4]) is raised on INT#7 (VR304/VR308, p.249-250).
 *  - VR004[1]/[4] 0->1 triggers capture + compression (p.234-236). The engine
 *    reads the source frame out of the VGA carve-out at the top of BMC DRAM
 *    (the internal VGA controller's frame buffer; VR008[2]=0 selects the
 *    integrated VGA source, p.236-237), JPEG-compresses it, writes the frame to
 *    the compressed-video-stream buffer VR054 (p.243), updates the read-back
 *    counters VR070 (stream size) / VR078 (frame-end offset, what the driver
 *    reads as the frame size) / VR07C (frame counter) (p.246-247), and raises
 *    capture-complete + compression-complete (VR308[1]/[3]) on INT#7. While a
 *    frame is in flight the VR004[16]/[18] engine-status bits read 0 (busy);
 *    idle reads 1 (p.234).
 *
 * Modelling contracts (documented approximations, see
 * qemu-model/peripherals/video/DOC.md):
 *  - The internal-VGA scanout is a linear 32bpp XRGB8888 frame at the base of
 *    the VGA carve-out (stride = width*4). On silicon the mode/format follow
 *    the host's VGA mode-set; the BMC-only machine has no VGA controller model,
 *    so this fixed scanout stands in for "the host set 640x480x32".
 *  - The bitstream is a baseline JPEG (YCbCr 4:4:4) using the AST2050 ROM
 *    quantization tables selected by VR060[15:11] (one of 8) and the standard
 *    Annex-K Huffman tables. In pure-JPEG mode (VR060[0]=1, as the AST2050
 *    aspeed-video driver sets) the engine emits ONLY the entropy-coded stream --
 *    no JFIF header, no EOI -- because register 0x040 is the CRC buffer, not a
 *    header buffer; the driver rebuilds the header in software. This matches
 *    real silicon. Without VR060[0] the model emits a self-contained JFIF (for
 *    a G4-class driver that expects the engine to prepend the header).
 *
 * This code is licensed under the GPL version 2 or later.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/aspeed_video_ast2050.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/dma.h"
#include <math.h>

#define VR_PROTECT   0x000
#define VIDEO_UNLOCK 0x1A038AA8u

/* VR004 — sequence control (datasheet p.234-236) */
#define VR_SEQ_CTRL             0x004
#define  SEQ_TRIG_MODE_DET      BIT(0)
#define  SEQ_TRIG_CAPTURE       BIT(1)
#define  SEQ_TRIG_COMP          BIT(4)
#define  SEQ_CAP_IDLE           BIT(16)   /* R: capture engine 1=idle */
#define  SEQ_COMP_IDLE          BIT(18)   /* R: compression engine 1=idle */

/* VR008 — video control (p.236-238) */
#define VR_CTRL                 0x008
#define  CTRL_SRC_EXTERNAL      BIT(2)    /* 0 = integrated VGA controller */

/* Capture/compression windows + buffers (p.241-244) */
#define VR_CAP_WINDOW           0x030     /* H pixels [27:16], V lines [10:0] */
#define VR_COMP_WINDOW          0x034
#define VR_COMP_ADDR            0x054     /* compressed-video-stream buffer */
#define VR_STREAM_BUF_SIZE      0x058     /* packet number [5:3] / size [2:0] */

/* VR060 — compression control (p.244-246) */
#define VR_COMP_CTRL            0x060
#define  COMP_CTRL_JPEG_ONLY    BIT(0)     /* [0] 1 = pure-JPEG (headerless) mode */
#define  COMP_CTRL_DCT_LUM_SHIFT 11       /* [15:11] luminance quant select */

/* Read-back counters (p.246-247) */
#define VR_COMP_STREAM_SIZE     0x070     /* total compressed size [19:0] */
#define VR_FRAME_END_OFFSET     0x078     /* frame-end offset [21:3] */
#define VR_FRAME_COUNTER        0x07C

/* Mode-detection read-back (p.247-248) */
#define VR_SRC_LR_EDGE          0x090
#define VR_SRC_TB_EDGE          0x094
#define VR_MODE_DETECT_STS      0x098
#define VR_SYNC_STATUS          0x09C
#define VR_H_TOTAL_PIXELS       0x0A0

/* Interrupt enable/status (p.249-250; VR308 is W1C) */
#define VR_INT_CTRL             0x304
#define VR_INT_STATUS           0x308
#define  INT_CAPTURE_COMPLETE   BIT(1)
#define  INT_COMP_COMPLETE      BIT(3)
#define  INT_MODE_DETECT_RDY    BIT(4)

/* Memory-restriction window (p.251): datasheet init values. */
#define VR_MEM_RESTRICT_START   0x310
#define VR_MEM_RESTRICT_END     0x314
#define  MEM_RESTRICT_END_INIT  0x0FFF0000u

#define R(off) ((off) >> 2)

/*
 * Modelled internal-VGA scanout: 640x480@60 with classic VGA timing
 * (800x525 total, hsync 96 px, vsync 2 lines, both sync polarities positive).
 * The aspeed-video driver derives width/height from the VR090/VR094 frame
 * edges and the porches from VR098/VR09C/VR0A0.
 */
#define VGA_WIDTH        640
#define VGA_HEIGHT       480
#define VGA_H_TOTAL      800
#define VGA_V_TOTAL      525
#define VGA_HSYNC        96
#define VGA_VSYNC        2
#define VGA_FRAME_LEFT   144   /* hsync (96) + back porch (48) */
#define VGA_FRAME_RIGHT  (VGA_FRAME_LEFT + VGA_WIDTH - 1)     /* 783 */
#define VGA_FRAME_TOP    35    /* vsync (2) + back porch (33) */
#define VGA_FRAME_BOTTOM (VGA_FRAME_TOP + VGA_HEIGHT - 1)     /* 514 */

/* VR098 bits (p.248, matching the aspeed-video driver's field layout) */
#define MD_STS_HSYNC_RDY  BIT(31)
#define MD_STS_VSYNC_RDY  BIT(30)
#define MD_STS_V_STABLE   BIT(14)
#define MD_STS_H_STABLE   BIT(13)

/*
 * A triggered capture+compression completes after 2 ms of virtual time — a
 * plausible engine latency that also keeps the busy status bits observable
 * and bounds free-running stream captures.
 */
#define FRAME_DELAY_NS   (2 * 1000 * 1000)

/* Max source geometry the engine supports (datasheet §20.2: 1920x1200). */
#define MAX_WIDTH        1920
#define MAX_HEIGHT       1200

/* ------------------------------------------------------------------------- */
/* Minimal baseline JPEG encoder (YCbCr 4:4:4, interleaved 8x8 MCUs).         */
/*                                                                            */
/* Quantization: the AST2050 internal-ROM tables selected by VR060[15:11]     */
/* (one of 8), transcribed from the Linux aspeed-video driver's jpeg_dct[]    */
/* below -- NOT generic Annex-K tables scaled by quality, so the entropy      */
/* matches what the driver's software JFIF header declares. Entropy coding:    */
/* Annex K.3 typical Huffman tables (identical to the driver's jpeg_quant).   */
/* ------------------------------------------------------------------------- */

typedef struct JpegBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    uint32_t bitbuf;
    int bitcnt;
    size_t body_off;    /* offset of the entropy-coded segment (after SOS) */
    size_t body_len;    /* length of the entropy-coded segment (before EOI) */
} JpegBuf;

static const uint8_t jpeg_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/*
 * AST2050 ROM quantization tables (raster order), extracted from the Linux
 * aspeed-video driver's jpeg_dct[0..7] DQT segments (VR060[15:11] selects one of
 * 8, p.244-246). These ARE the engine's fixed internal-ROM tables -- not the
 * generic Annex-K tables scaled by quality -- so the headerless entropy the G3
 * emits is decoded by exactly these tables. The driver's software JFIF header
 * carries the very same tables, so QEMU's stream + the driver's header compose
 * into a standards-compliant JPEG, matching real silicon.
 * Regenerate with tools: tmp/vga-fix/gen-quant-tables.py.
 */
static const uint8_t ast2050_quant_luma[8][64] = {
    { /* sel 0 */
         20,  13,  12,  20,  30,  50,  63,  76,
         15,  15,  17,  23,  32,  72,  75,  68,
         17,  16,  20,  30,  50,  71,  86,  70,
         17,  21,  27,  36,  63, 108, 100,  77,
         22,  27,  46,  70,  85, 136, 128,  96,
         30,  43,  68,  80, 101, 130, 141, 115,
         61,  80,  97, 108, 128, 151, 150, 126,
         90, 115, 118, 122, 140, 125, 128, 123,
    },
    { /* sel 1 */
         17,  12,  10,  17,  26,  43,  55,  66,
         13,  13,  15,  20,  28,  63,  65,  60,
         15,  14,  17,  26,  43,  62,  75,  61,
         15,  18,  24,  31,  55,  95,  87,  67,
         19,  24,  40,  61,  74, 119, 112,  84,
         26,  38,  60,  70,  88, 113, 123, 100,
         53,  70,  85,  95, 112, 132, 131, 110,
         78, 100, 103, 107, 122, 109, 112, 108,
    },
    { /* sel 2 */
         14,   9,   9,  14,  21,  36,  46,  55,
         10,  10,  12,  17,  23,  52,  54,  49,
         12,  11,  14,  21,  36,  51,  62,  50,
         12,  15,  19,  26,  46,  78,  72,  56,
         16,  19,  33,  50,  61,  98,  93,  69,
         21,  31,  49,  58,  73,  94, 102,  83,
         44,  58,  70,  78,  93, 109, 108,  91,
         65,  83,  86,  88, 101,  90,  93,  89,
    },
    { /* sel 3 */
         11,   7,   7,  11,  17,  28,  36,  43,
          8,   8,  10,  13,  18,  41,  43,  39,
         10,   9,  11,  17,  28,  40,  49,  40,
         10,  12,  15,  20,  36,  62,  57,  44,
         12,  15,  26,  40,  48,  78,  74,  55,
         17,  25,  39,  46,  58,  74,  81,  66,
         35,  46,  56,  62,  74,  86,  86,  72,
         51,  66,  68,  70,  80,  71,  74,  71,
    },
    { /* sel 4 */
          9,   6,   5,   9,  13,  22,  28,  34,
          6,   6,   7,  10,  14,  32,  33,  30,
          7,   7,   9,  13,  22,  32,  38,  31,
          7,   9,  12,  16,  28,  48,  45,  34,
         10,  12,  20,  31,  38,  61,  57,  43,
         13,  19,  30,  36,  45,  58,  63,  51,
         27,  36,  43,  48,  57,  68,  67,  56,
         40,  51,  53,  55,  63,  56,  57,  55,
    },
    { /* sel 5 */
          6,   4,   3,   6,   9,  15,  19,  22,
          4,   4,   5,   7,   9,  21,  22,  20,
          5,   4,   6,   9,  15,  21,  25,  21,
          5,   6,   8,  10,  19,  32,  30,  23,
          6,   8,  13,  21,  25,  40,  38,  28,
          9,  13,  20,  24,  30,  39,  42,  34,
         18,  24,  29,  32,  38,  45,  45,  37,
         27,  34,  35,  36,  42,  37,  38,  37,
    },
    { /* sel 6 */
          3,   2,   1,   3,   4,   7,   9,  11,
          2,   2,   2,   3,   4,  10,  11,  10,
          2,   2,   3,   4,   7,  10,  12,  10,
          2,   3,   4,   5,   9,  16,  15,  11,
          3,   4,   6,  10,  12,  20,  19,  14,
          4,   6,  10,  12,  15,  19,  21,  17,
          9,  12,  14,  16,  19,  22,  22,  18,
         13,  17,  17,  18,  21,  18,  19,  18,
    },
    { /* sel 7 */
          2,   1,   1,   2,   3,   5,   6,   7,
          1,   1,   1,   2,   3,   7,   7,   6,
          1,   1,   2,   3,   5,   7,   8,   7,
          1,   2,   2,   3,   6,  10,  10,   7,
          2,   2,   4,   7,   8,  13,  12,   9,
          3,   4,   6,   8,  10,  13,  14,  11,
          6,   8,   9,  10,  12,  15,  15,  12,
          9,  11,  11,  12,  14,  12,  12,  12,
    },
};
static const uint8_t ast2050_quant_chroma[8][64] = {
    { /* sel 0 */
         31,  33,  45,  88, 185, 185, 185, 185,
         33,  39,  48, 123, 184, 185, 185, 185,
         45,  48, 105, 166, 185, 185, 185, 185,
         88, 123, 166, 185, 185, 185, 185, 185,
        185, 184, 185, 185, 185, 185, 185, 185,
        185, 185, 185, 185, 185, 185, 185, 185,
        185, 185, 185, 185, 185, 185, 185, 185,
        185, 185, 185, 185, 185, 185, 185, 185,
    },
    { /* sel 1 */
         27,  29,  39,  76, 160, 160, 160, 160,
         29,  34,  42, 107, 160, 160, 160, 160,
         39,  42,  91, 160, 160, 160, 160, 160,
         76, 107, 160, 160, 160, 160, 160, 160,
        160, 160, 160, 160, 160, 160, 160, 160,
        160, 160, 160, 160, 160, 160, 160, 160,
        160, 160, 160, 160, 160, 160, 160, 160,
        160, 160, 160, 160, 160, 160, 160, 160,
    },
    { /* sel 2 */
         22,  24,  32,  63, 133, 133, 133, 133,
         24,  28,  34,  88, 133, 133, 133, 133,
         32,  34,  75, 133, 133, 133, 133, 133,
         63,  88, 133, 133, 133, 133, 133, 133,
        133, 133, 133, 133, 133, 133, 133, 133,
        133, 133, 133, 133, 133, 133, 133, 133,
        133, 133, 133, 133, 133, 133, 133, 133,
        133, 133, 133, 133, 133, 133, 133, 133,
    },
    { /* sel 3 */
         18,  19,  26,  51, 108, 108, 108, 108,
         19,  22,  28,  72, 108, 108, 108, 108,
         26,  28,  61, 108, 108, 108, 108, 108,
         51,  72, 108, 108, 108, 108, 108, 108,
        108, 108, 108, 108, 108, 108, 108, 108,
        108, 108, 108, 108, 108, 108, 108, 108,
        108, 108, 108, 108, 108, 108, 108, 108,
        108, 108, 108, 108, 108, 108, 108, 108,
    },
    { /* sel 4 */
         13,  14,  19,  38,  80,  80,  80,  80,
         14,  17,  21,  53,  80,  80,  80,  80,
         19,  21,  45,  80,  80,  80,  80,  80,
         38,  53,  80,  80,  80,  80,  80,  80,
         80,  80,  80,  80,  80,  80,  80,  80,
         80,  80,  80,  80,  80,  80,  80,  80,
         80,  80,  80,  80,  80,  80,  80,  80,
         80,  80,  80,  80,  80,  80,  80,  80,
    },
    { /* sel 5 */
          9,  10,  13,  26,  55,  55,  55,  55,
         10,  11,  14,  37,  55,  55,  55,  55,
         13,  14,  31,  55,  55,  55,  55,  55,
         26,  37,  55,  55,  55,  55,  55,  55,
         55,  55,  55,  55,  55,  55,  55,  55,
         55,  55,  55,  55,  55,  55,  55,  55,
         55,  55,  55,  55,  55,  55,  55,  55,
         55,  55,  55,  55,  55,  55,  55,  55,
    },
    { /* sel 6 */
          4,   5,   6,  13,  27,  27,  27,  27,
          5,   5,   7,  18,  27,  27,  27,  27,
          6,   7,  15,  27,  27,  27,  27,  27,
         13,  18,  27,  27,  27,  27,  27,  27,
         27,  27,  27,  27,  27,  27,  27,  27,
         27,  27,  27,  27,  27,  27,  27,  27,
         27,  27,  27,  27,  27,  27,  27,  27,
         27,  27,  27,  27,  27,  27,  27,  27,
    },
    { /* sel 7 */
          3,   3,   4,   8,  18,  18,  18,  18,
          3,   3,   4,  12,  18,  18,  18,  18,
          4,   4,  10,  18,  18,  18,  18,  18,
          8,  12,  18,  18,  18,  18,  18,  18,
         18,  18,  18,  18,  18,  18,  18,  18,
         18,  18,  18,  18,  18,  18,  18,  18,
         18,  18,  18,  18,  18,  18,  18,  18,
         18,  18,  18,  18,  18,  18,  18,  18,
    },
};

/* Annex K.3 typical Huffman tables: 16 BITS counts then values. */
static const uint8_t jpeg_dc_luma_bits[16] = {
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t jpeg_dc_luma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const uint8_t jpeg_dc_chroma_bits[16] = {
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const uint8_t jpeg_dc_chroma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const uint8_t jpeg_ac_luma_bits[16] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
static const uint8_t jpeg_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const uint8_t jpeg_ac_chroma_bits[16] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const uint8_t jpeg_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

typedef struct HuffTable {
    uint16_t code[256];
    uint8_t size[256];
} HuffTable;

/* Build canonical Huffman codes from BITS/HUFFVAL (ITU-T T.81 Annex C). */
static void jpeg_build_huff(HuffTable *h, const uint8_t bits[16],
                            const uint8_t *vals, int nvals)
{
    uint16_t code = 0;
    int k = 0;

    memset(h, 0, sizeof(*h));
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1] && k < nvals; i++, k++) {
            h->code[vals[k]] = code;
            h->size[vals[k]] = len;
            code++;
        }
        code <<= 1;
    }
}

static void jpeg_put_byte(JpegBuf *b, uint8_t v)
{
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8192;
        b->data = g_realloc(b->data, b->cap);
    }
    b->data[b->len++] = v;
}

static void jpeg_put_bytes(JpegBuf *b, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        jpeg_put_byte(b, p[i]);
    }
}

static void jpeg_put_marker(JpegBuf *b, uint8_t m)
{
    jpeg_put_byte(b, 0xFF);
    jpeg_put_byte(b, m);
}

static void jpeg_put_u16(JpegBuf *b, uint16_t v)
{
    jpeg_put_byte(b, v >> 8);
    jpeg_put_byte(b, v & 0xFF);
}

/* Entropy-coded bit writer with 0xFF byte stuffing. */
static void jpeg_put_bits(JpegBuf *b, uint32_t bits, int nbits)
{
    b->bitbuf = (b->bitbuf << nbits) | (bits & ((1u << nbits) - 1));
    b->bitcnt += nbits;
    while (b->bitcnt >= 8) {
        uint8_t byte = (b->bitbuf >> (b->bitcnt - 8)) & 0xFF;
        jpeg_put_byte(b, byte);
        if (byte == 0xFF) {
            jpeg_put_byte(b, 0x00);
        }
        b->bitcnt -= 8;
    }
}

static void jpeg_flush_bits(JpegBuf *b)
{
    /* Pad the final partial byte with 1-bits (ITU-T T.81 F.1.2.3). */
    if (b->bitcnt > 0) {
        jpeg_put_bits(b, 0xFF, 8 - b->bitcnt);
    }
}

/* Reference double-precision 8x8 forward DCT (ITU-T T.81 A.3.3). */
static void jpeg_fdct(const int16_t in[64], double out[64])
{
    static double cs[8][8];
    static bool cs_init;

    if (!cs_init) {
        for (int u = 0; u < 8; u++) {
            for (int x = 0; x < 8; x++) {
                cs[u][x] = cos((2 * x + 1) * u * M_PI / 16.0);
            }
        }
        cs_init = true;
    }

    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            double sum = 0.0;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    sum += in[y * 8 + x] * cs[u][x] * cs[v][y];
                }
            }
            double cu = (u == 0) ? M_SQRT1_2 : 1.0;
            double cv = (v == 0) ? M_SQRT1_2 : 1.0;
            out[v * 8 + u] = 0.25 * cu * cv * sum;
        }
    }
}

/* Magnitude category + offset code per ITU-T T.81 F.1.2 / F.1.4. */
static int jpeg_category(int v, uint32_t *code)
{
    int a = v < 0 ? -v : v;
    int n = 0;

    while (a) {
        a >>= 1;
        n++;
    }
    *code = v >= 0 ? (uint32_t)v : (uint32_t)(v - 1) & ((1u << n) - 1);
    return n;
}

/* Encode one quantized 8x8 block; returns the new DC predictor. */
static int jpeg_encode_block(JpegBuf *b, const int16_t block[64],
                             const uint8_t quant[64], const HuffTable *dc,
                             const HuffTable *ac, int dc_pred)
{
    double dct[64];
    int16_t q[64];
    uint32_t code;
    int n;

    jpeg_fdct(block, dct);
    for (int i = 0; i < 64; i++) {
        int r = jpeg_zigzag[i];
        double v = dct[r] / quant[r];
        q[i] = (int16_t)lrint(v);
    }

    /* DC: difference from predictor. */
    int diff = q[0] - dc_pred;
    n = jpeg_category(diff, &code);
    jpeg_put_bits(b, dc->code[n], dc->size[n]);
    if (n) {
        jpeg_put_bits(b, code, n);
    }

    /* AC: run-length of zeros + category. */
    int run = 0;
    for (int i = 1; i < 64; i++) {
        if (q[i] == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            jpeg_put_bits(b, ac->code[0xF0], ac->size[0xF0]);  /* ZRL */
            run -= 16;
        }
        n = jpeg_category(q[i], &code);
        int sym = (run << 4) | n;
        jpeg_put_bits(b, ac->code[sym], ac->size[sym]);
        jpeg_put_bits(b, code, n);
        run = 0;
    }
    if (run) {
        jpeg_put_bits(b, ac->code[0x00], ac->size[0x00]);      /* EOB */
    }
    return q[0];
}

/*
 * Encode an XRGB8888 frame (little-endian words: B,G,R,X) as a baseline JFIF
 * JPEG, YCbCr 4:4:4, 1x1 sampling, interleaved MCUs. Returns a JpegBuf the
 * caller g_free()s .data of.
 */
static JpegBuf jpeg_encode_frame(const uint8_t *rgb, unsigned width,
                                 unsigned height, unsigned q_index)
{
    JpegBuf b = { 0 };
    unsigned sel = MIN(q_index, 7u);        /* G3 has 8 ROM quant tables */
    const uint8_t *qluma = ast2050_quant_luma[sel];
    const uint8_t *qchroma = ast2050_quant_chroma[sel];
    HuffTable dc_l, ac_l, dc_c, ac_c;

    jpeg_build_huff(&dc_l, jpeg_dc_luma_bits, jpeg_dc_luma_vals, 12);
    jpeg_build_huff(&ac_l, jpeg_ac_luma_bits, jpeg_ac_luma_vals, 162);
    jpeg_build_huff(&dc_c, jpeg_dc_chroma_bits, jpeg_dc_chroma_vals, 12);
    jpeg_build_huff(&ac_c, jpeg_ac_chroma_bits, jpeg_ac_chroma_vals, 162);

    /* SOI + JFIF APP0 */
    jpeg_put_marker(&b, 0xD8);
    jpeg_put_marker(&b, 0xE0);
    jpeg_put_u16(&b, 16);
    jpeg_put_bytes(&b, (const uint8_t *)"JFIF\0", 5);
    jpeg_put_bytes(&b, (const uint8_t[]){ 1, 1, 0, 0, 1, 0, 1, 0, 0 }, 9);

    /* DQT: table 0 (luma), table 1 (chroma), zigzag order. */
    jpeg_put_marker(&b, 0xDB);
    jpeg_put_u16(&b, 2 + 2 * 65);
    jpeg_put_byte(&b, 0x00);
    for (int i = 0; i < 64; i++) {
        jpeg_put_byte(&b, qluma[jpeg_zigzag[i]]);
    }
    jpeg_put_byte(&b, 0x01);
    for (int i = 0; i < 64; i++) {
        jpeg_put_byte(&b, qchroma[jpeg_zigzag[i]]);
    }

    /* SOF0: 8-bit, 3 components, 1x1 sampling (4:4:4). */
    jpeg_put_marker(&b, 0xC0);
    jpeg_put_u16(&b, 8 + 3 * 3);
    jpeg_put_byte(&b, 8);
    jpeg_put_u16(&b, height);
    jpeg_put_u16(&b, width);
    jpeg_put_byte(&b, 3);
    jpeg_put_bytes(&b, (const uint8_t[]){ 1, 0x11, 0 }, 3);
    jpeg_put_bytes(&b, (const uint8_t[]){ 2, 0x11, 1 }, 3);
    jpeg_put_bytes(&b, (const uint8_t[]){ 3, 0x11, 1 }, 3);

    /* DHT: DC/AC luma (class 0/1, id 0), DC/AC chroma (id 1). */
    static const struct {
        uint8_t id;
        const uint8_t *bits;
        const uint8_t *vals;
        int nvals;
    } dht[4] = {
        { 0x00, jpeg_dc_luma_bits, jpeg_dc_luma_vals, 12 },
        { 0x10, jpeg_ac_luma_bits, jpeg_ac_luma_vals, 162 },
        { 0x01, jpeg_dc_chroma_bits, jpeg_dc_chroma_vals, 12 },
        { 0x11, jpeg_ac_chroma_bits, jpeg_ac_chroma_vals, 162 },
    };
    for (int t = 0; t < 4; t++) {
        jpeg_put_marker(&b, 0xC4);
        jpeg_put_u16(&b, 2 + 1 + 16 + dht[t].nvals);
        jpeg_put_byte(&b, dht[t].id);
        jpeg_put_bytes(&b, dht[t].bits, 16);
        jpeg_put_bytes(&b, dht[t].vals, dht[t].nvals);
    }

    /* SOS */
    jpeg_put_marker(&b, 0xDA);
    jpeg_put_u16(&b, 6 + 2 * 3);
    jpeg_put_byte(&b, 3);
    jpeg_put_bytes(&b, (const uint8_t[]){ 1, 0x00 }, 2);
    jpeg_put_bytes(&b, (const uint8_t[]){ 2, 0x11 }, 2);
    jpeg_put_bytes(&b, (const uint8_t[]){ 3, 0x11 }, 2);
    jpeg_put_bytes(&b, (const uint8_t[]){ 0, 63, 0 }, 3);

    /*
     * The entropy-coded segment starts here. The real G3 writes only this to
     * the stream buffer (register 0x040 is the CRC buffer, not a JPEG-header
     * buffer, so no JFIF header is DMA-prepended) -- record its span so
     * do_frame() can emit just the entropy in pure-JPEG mode, matching silicon.
     */
    b.body_off = b.len;

    /* Entropy-coded data: interleaved Y/Cb/Cr 8x8 blocks per MCU. */
    int dcy = 0, dcb = 0, dcr = 0;
    for (unsigned my = 0; my < height; my += 8) {
        for (unsigned mx = 0; mx < width; mx += 8) {
            int16_t yb[64], cb[64], cr[64];
            for (int py = 0; py < 8; py++) {
                unsigned sy = MIN(my + py, height - 1);
                for (int px = 0; px < 8; px++) {
                    unsigned sx = MIN(mx + px, width - 1);
                    const uint8_t *p = rgb + (sy * width + sx) * 4;
                    int bl = p[0], g = p[1], r = p[2];
                    /* JFIF full-range RGB -> YCbCr (BT.601). */
                    int y  = (77 * r + 150 * g + 29 * bl) >> 8;
                    int u  = ((-43 * r - 85 * g + 128 * bl) >> 8) + 128;
                    int v  = ((128 * r - 107 * g - 21 * bl) >> 8) + 128;
                    yb[py * 8 + px] = MIN(MAX(y, 0), 255) - 128;
                    cb[py * 8 + px] = MIN(MAX(u, 0), 255) - 128;
                    cr[py * 8 + px] = MIN(MAX(v, 0), 255) - 128;
                }
            }
            dcy = jpeg_encode_block(&b, yb, qluma, &dc_l, &ac_l, dcy);
            dcb = jpeg_encode_block(&b, cb, qchroma, &dc_c, &ac_c, dcb);
            dcr = jpeg_encode_block(&b, cr, qchroma, &dc_c, &ac_c, dcr);
        }
    }
    jpeg_flush_bits(&b);
    b.body_len = b.len - b.body_off;    /* entropy length, before EOI */
    jpeg_put_marker(&b, 0xD9);  /* EOI */
    return b;
}

/* ------------------------------------------------------------------------- */
/* Video engine behaviour                                                     */
/* ------------------------------------------------------------------------- */

static void aspeed_video_ast2050_update_irq(AspeedVideoAST2050State *s)
{
    qemu_set_irq(s->irq, !!(s->regs[R(VR_INT_STATUS)] & s->regs[R(VR_INT_CTRL)]));
}

static bool aspeed_video_ast2050_signal(AspeedVideoAST2050State *s)
{
    /* VR008[2]=0 selects the integrated VGA controller (p.236-237); no
     * external DVO source is modelled. */
    return s->vga_signal && !(s->regs[R(VR_CTRL)] & CTRL_SRC_EXTERNAL);
}

/*
 * Mode detection (VR004[0] 0->1, p.235): report the modelled internal-VGA
 * 640x480@60 timing through the read-back registers and raise
 * mode-detection-ready. With no signal the detection never completes (the
 * driver's wait then times out -> V4L2 "no signal"), matching a dead source.
 */
static void aspeed_video_ast2050_mode_detect(AspeedVideoAST2050State *s)
{
    if (!aspeed_video_ast2050_signal(s)) {
        /* No sync: mark the source undetected (VR090[15:12] no-* flags). */
        s->regs[R(VR_SRC_LR_EDGE)] = 0xF000;
        s->regs[R(VR_SRC_TB_EDGE)] = 0;
        s->regs[R(VR_MODE_DETECT_STS)] = 0;
        return;
    }

    s->regs[R(VR_SRC_LR_EDGE)] = (VGA_FRAME_RIGHT << 16) | VGA_FRAME_LEFT;
    s->regs[R(VR_SRC_TB_EDGE)] = (VGA_FRAME_BOTTOM << 16) | VGA_FRAME_TOP;
    s->regs[R(VR_MODE_DETECT_STS)] = MD_STS_HSYNC_RDY | MD_STS_VSYNC_RDY |
        (VGA_V_TOTAL << 16) | MD_STS_V_STABLE | MD_STS_H_STABLE | VGA_H_TOTAL;
    s->regs[R(VR_SYNC_STATUS)] = (VGA_VSYNC << 16) | VGA_HSYNC;
    s->regs[R(VR_H_TOTAL_PIXELS)] = VGA_H_TOTAL;

    s->regs[R(VR_INT_STATUS)] |= INT_MODE_DETECT_RDY;
    aspeed_video_ast2050_update_irq(s);
}

/*
 * Capture + compress one frame: read the internal-VGA scanout from the VGA
 * carve-out at the top of DRAM, JPEG-encode it, write the stream to the
 * VR054 buffer, update the read-back counters, raise completion on INT#7.
 */
static void aspeed_video_ast2050_do_frame(void *opaque)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(opaque);
    uint64_t dram_size = memory_region_size(s->dram_mr);
    uint32_t window = s->regs[R(VR_COMP_WINDOW)];
    unsigned width = (window >> 16) & 0xFFF;
    unsigned height = window & 0x7FF;
    uint32_t comp_bus = s->regs[R(VR_COMP_ADDR)];
    uint32_t sbuf = s->regs[R(VR_STREAM_BUF_SIZE)];
    uint32_t comp_max = (4u << ((sbuf >> 3) & 0x7)) * (1024u << (sbuf & 0x7));

    s->frame_pending = false;

    if (!aspeed_video_ast2050_signal(s)) {
        /* Dead source: nothing to capture, no completion. */
        return;
    }

    if (width < 8 || width > MAX_WIDTH || height < 8 || height > MAX_HEIGHT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: capture triggered with bad compression window "
                      "0x%08x\n", __func__, window);
        return;
    }
    if (s->vga_mem_size > dram_size ||
        (uint64_t)width * height * 4 > s->vga_mem_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %ux%u frame does not fit the %u-byte VGA carve-out\n",
                      __func__, width, height, s->vga_mem_size);
        return;
    }
    if (comp_bus < s->dram_base || comp_bus - s->dram_base >= dram_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: compressed-stream buffer 0x%08x outside DRAM\n",
                      __func__, comp_bus);
        return;
    }

    /*
     * Modelled scanout contract: linear XRGB8888 at the base of the VGA
     * carve-out, stride = width*4 (see file header).
     */
    uint64_t src_off = dram_size - s->vga_mem_size;
    size_t src_len = (size_t)width * height * 4;
    g_autofree uint8_t *rgb = g_malloc(src_len);
    MemTxResult res = dma_memory_read(&s->dram_as, src_off, rgb, src_len,
                                      MEMTXATTRS_UNSPECIFIED);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: VGA scanout read failed @0x%"
                      PRIx64 "\n", __func__, src_off);
        return;
    }

    uint32_t comp_ctrl = s->regs[R(VR_COMP_CTRL)];
    unsigned q_index = (comp_ctrl >> COMP_CTRL_DCT_LUM_SHIFT) & 0xF;
    JpegBuf jpeg = jpeg_encode_frame(rgb, width, height, q_index);

    /*
     * Pure-JPEG mode (VR060[0], as the AST2050 aspeed-video driver sets): the
     * G3 writes ONLY the entropy-coded stream to the buffer -- no JFIF header,
     * no EOI -- because register 0x040 is the CRC buffer, not a header buffer.
     * The driver reconstructs the JFIF header in software. Emit exactly that
     * headerless stream so QEMU matches silicon. Without the bit (an ast2400-
     * style driver), emit the whole self-contained JFIF as before.
     */
    const uint8_t *out = jpeg.data;
    size_t out_len = jpeg.len;
    if (comp_ctrl & COMP_CTRL_JPEG_ONLY) {
        out = jpeg.data + jpeg.body_off;
        out_len = jpeg.body_len;
    }

    if (out_len > comp_max) {
        /* Real engine behaviour: an undersized stream buffer yields a
         * truncated (incomplete) JPEG. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %zu-byte frame truncated to %u-byte stream buffer\n",
                      __func__, out_len, comp_max);
        out_len = comp_max;
    }
    uint64_t comp_off = comp_bus - s->dram_base;
    if (out_len > dram_size - comp_off) {
        out_len = dram_size - comp_off;
    }
    res = dma_memory_write(&s->dram_as, comp_off, out, out_len,
                           MEMTXATTRS_UNSPECIFIED);
    g_free(jpeg.data);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: stream buffer write failed @0x%"
                      PRIx64 "\n", __func__, comp_off);
        return;
    }

    /* Read-back counters (p.246-247). VR078 [21:3] is the frame-end offset
     * from the stream-buffer base — what the aspeed-video driver reads as the
     * frame size — so round the JPEG up to the 8-byte granularity. */
    s->regs[R(VR_FRAME_END_OFFSET)] = ROUND_UP(out_len, 8) & 0x3FFFF8;
    s->regs[R(VR_COMP_STREAM_SIZE)] = (out_len / 4) & 0xFFFFF;
    s->regs[R(VR_FRAME_COUNTER)]++;

    s->regs[R(VR_INT_STATUS)] |= INT_CAPTURE_COMPLETE | INT_COMP_COMPLETE;
    aspeed_video_ast2050_update_irq(s);
}

static uint64_t aspeed_video_ast2050_read(void *opaque, hwaddr offset, unsigned size)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_VIDEO_AST2050_NR_REGS) {
        return 0;
    }
    if (offset == VR_SEQ_CTRL) {
        /* VR004[16]/[18] read the engine status: 0 busy / 1 idle (p.234). */
        uint32_t idle = s->frame_pending ? 0 : (SEQ_CAP_IDLE | SEQ_COMP_IDLE);
        return (s->regs[reg] & ~(SEQ_CAP_IDLE | SEQ_COMP_IDLE)) | idle;
    }
    return s->regs[reg];
}

static void aspeed_video_ast2050_write(void *opaque, hwaddr offset, uint64_t data,
                                       unsigned size)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(opaque);
    unsigned reg = offset >> 2;

    if (reg >= ASPEED_VIDEO_AST2050_NR_REGS) {
        return;
    }
    if (offset == VR_PROTECT) {
        /* lock latch: reads back 1 when unlocked, 0 otherwise */
        s->regs[reg] = (data == VIDEO_UNLOCK) ? 1 : 0;
        return;
    }
    if (!s->regs[R(VR_PROTECT)]) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write while locked at 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return;
    }

    switch (offset) {
    case VR_SEQ_CTRL: {
        uint32_t old = s->regs[reg];
        uint32_t set = (uint32_t)data & ~old;

        /* [16]/[18] are read-only engine status. */
        s->regs[reg] = data & ~(SEQ_CAP_IDLE | SEQ_COMP_IDLE);

        if (set & SEQ_TRIG_MODE_DET) {
            aspeed_video_ast2050_mode_detect(s);
        }
        /* 0->1 on the capture/compression triggers starts a frame (p.235;
         * the driver raises both bits in one write). */
        if ((set & (SEQ_TRIG_CAPTURE | SEQ_TRIG_COMP)) &&
            (s->regs[reg] & SEQ_TRIG_CAPTURE) &&
            (s->regs[reg] & SEQ_TRIG_COMP) &&
            !s->frame_pending) {
            s->frame_pending = true;
            timer_mod(&s->frame_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_DELAY_NS);
        }
        break;
    }
    case VR_INT_STATUS:
        /* W1C (p.250: "Clear this register by writing 1"). */
        s->regs[reg] &= ~data;
        aspeed_video_ast2050_update_irq(s);
        break;
    case VR_INT_CTRL:
        s->regs[reg] = data;
        aspeed_video_ast2050_update_irq(s);
        break;
    case VR_FRAME_END_OFFSET:
    case VR_COMP_STREAM_SIZE:
    case VR_FRAME_COUNTER:
    case VR_SRC_LR_EDGE:
    case VR_SRC_TB_EDGE:
    case VR_MODE_DETECT_STS:
    case VR_SYNC_STATUS:
    case VR_H_TOTAL_PIXELS:
        /* Read-back registers; drop guest writes. */
        break;
    default:
        s->regs[reg] = data;
        break;
    }
}

static const MemoryRegionOps aspeed_video_ast2050_ops = {
    .read = aspeed_video_ast2050_read,
    .write = aspeed_video_ast2050_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void aspeed_video_ast2050_reset(DeviceState *dev)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(dev);

    timer_del(&s->frame_timer);
    s->frame_pending = false;
    memset(s->regs, 0, sizeof(s->regs));
    /* Memory-restriction window datasheet init values (p.251). */
    s->regs[R(VR_MEM_RESTRICT_START)] = 0;
    s->regs[R(VR_MEM_RESTRICT_END)] = MEM_RESTRICT_END_INIT;
}

static void aspeed_video_ast2050_realize(DeviceState *dev, Error **errp)
{
    AspeedVideoAST2050State *s = ASPEED_VIDEO_AST2050(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->dram_mr) {
        error_setg(errp, TYPE_ASPEED_VIDEO_AST2050 ": 'dram' link not set");
        return;
    }
    address_space_init(&s->dram_as, s->dram_mr, "video-dram");

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_video_ast2050_ops, s,
                          TYPE_ASPEED_VIDEO_AST2050, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    timer_init_ns(&s->frame_timer, QEMU_CLOCK_VIRTUAL,
                  aspeed_video_ast2050_do_frame, s);
}

static const VMStateDescription vmstate_aspeed_video_ast2050 = {
    .name = TYPE_ASPEED_VIDEO_AST2050,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedVideoAST2050State,
                             ASPEED_VIDEO_AST2050_NR_REGS),
        VMSTATE_TIMER(frame_timer, AspeedVideoAST2050State),
        VMSTATE_BOOL(frame_pending, AspeedVideoAST2050State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property aspeed_video_ast2050_properties[] = {
    DEFINE_PROP_LINK("dram", AspeedVideoAST2050State, dram_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("dram-base", AspeedVideoAST2050State, dram_base, 0),
    DEFINE_PROP_UINT32("vga-mem-size", AspeedVideoAST2050State, vga_mem_size,
                       8 * 1024 * 1024),
    DEFINE_PROP_BOOL("vga-signal", AspeedVideoAST2050State, vga_signal, true),
};

static void aspeed_video_ast2050_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = aspeed_video_ast2050_realize;
    device_class_set_legacy_reset(dc, aspeed_video_ast2050_reset);
    dc->desc = "ASPEED AST2050 Video Engine";
    dc->vmsd = &vmstate_aspeed_video_ast2050;
    device_class_set_props(dc, aspeed_video_ast2050_properties);
}

static const TypeInfo aspeed_video_ast2050_info = {
    .name = TYPE_ASPEED_VIDEO_AST2050,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedVideoAST2050State),
    .class_init = aspeed_video_ast2050_class_init,
};

static void aspeed_video_ast2050_register_types(void)
{
    type_register_static(&aspeed_video_ast2050_info);
}

type_init(aspeed_video_ast2050_register_types)
