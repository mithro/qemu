#ifndef BITBANG_SSI_H
#define BITBANG_SSI_H

#include "hw/ssi/ssi.h"

typedef struct bitbang_ssi_interface bitbang_ssi_interface;

typedef enum {
    BITBANG_SSI_SCLK = 0,
    BITBANG_SSI_MOSI,
    BITBANG_SSI_MISO,
} bitbang_ssi_line;

typedef enum {
   BITBANG_SSI_CPOL0 = 0, // idle == low, active == high
   BITBANG_SSI_CPOL1 = 1  // idle == high, active == low
} bitbang_ssi_cpol_mode;

typedef enum {
   BITBANG_SSI_CPHA0 = 0, // Data on 0th edge
   BITBANG_SSI_CPHA1 = 1  // Data on 1st edge
} bitbang_ssi_cpha_mode;

bitbang_ssi_interface *bitbang_ssi_init(SSIBus *bus, bitbang_ssi_cpol_mode cpol_mode, bitbang_ssi_cpha_mode cpha_mode, int transfer_size);
int bitbang_ssi_set(bitbang_ssi_interface *ssi, bitbang_ssi_line line, int level);

#endif
