#include "diskio.h"
#include <avr32/io.h>
#include <stdint.h>
#include <string.h>

#define SD_PIN_MISO 25  /* PA25 CAN be used as GPIO BEFORE usb_device_init() is called! */
#define SD_PIN_MOSI 14
#define SD_PIN_SCK  15
#define SD_PIN_CS   17  /* FOUND BY SCANNER! */
#define SD_PIN_PWR  18

#define _GPIO_PORT(n) ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void SELECT(void)   { _GPIO_PORT(0)->ovrc = (1u << SD_PIN_CS); }
static inline void DESELECT(void) { _GPIO_PORT(0)->ovrs = (1u << SD_PIN_CS); }

static uint8_t spi_fast = 0;

static void spi_delay(void) {
    if (spi_fast) return; /* Full speed! */
    for (volatile int i = 0; i < 50; i++); /* ~125kHz */
}

static BYTE sd_spi_transfer(BYTE out) {
    BYTE in = 0;
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);

#define SPI_BIT(bit_idx) \
    if (out & (1 << (7 - bit_idx))) pa->ovrs = (1u << SD_PIN_MOSI); \
    else                            pa->ovrc = (1u << SD_PIN_MOSI); \
    spi_delay(); \
    pa->ovrs = (1u << SD_PIN_SCK); /* SCK HIGH */ \
    spi_delay(); \
    in <<= 1; \
    if (pa->pvr & (1u << SD_PIN_MISO)) in |= 1; \
    pa->ovrc = (1u << SD_PIN_SCK); /* SCK LOW */ \
    spi_delay();

    SPI_BIT(0); SPI_BIT(1); SPI_BIT(2); SPI_BIT(3);
    SPI_BIT(4); SPI_BIT(5); SPI_BIT(6); SPI_BIT(7);
    return in;
}

static BYTE rcv_spi(void) { return sd_spi_transfer(0xFF); }

static void dly_100us(void) {
    for (volatile int i = 0; i < 480; i++);
}

static BYTE send_cmd(BYTE cmd, DWORD arg) {
    BYTE n, res;
    if (cmd & 0x80) {
        cmd &= 0x7F;
        res = send_cmd(55, 0);
        if (res > 1) return res;
    }
    DESELECT();
    rcv_spi();
    SELECT();
    rcv_spi();
    sd_spi_transfer(0x40 | cmd);  /* SD cmds always have top two bits = 01 */
    sd_spi_transfer((BYTE)(arg >> 24));
    sd_spi_transfer((BYTE)(arg >> 16));
    sd_spi_transfer((BYTE)(arg >> 8));
    sd_spi_transfer((BYTE)arg);
    n = 0x01;
    if (cmd == 0)  n = 0x95;  /* CMD0  CRC (only one needed in SPI mode) */
    if (cmd == 8)  n = 0x87;  /* CMD8  CRC */
    sd_spi_transfer(n);
    if (cmd == 12) rcv_spi();  /* CMD12 needs one extra byte before response */
    n = 10;
    do {
        res = rcv_spi();
    } while ((res & 0x80) && --n);
    return res;
}

static volatile uint32_t cached_sector = 0xFFFFFFFF;
static uint8_t sector_cache[512];
static BYTE CardType;

/* ---- Multi-sector cache ----
 * PA25 = USBB D-. After usb_device_init(), SPI MISO is dead.
 * Every sector PetitFS reads before USB init is cached here.
 * SCSI READ_10 serves from this cache.
 * FIFO eviction: when full, oldest slot is reused. */
#define MC_MAX 32
static uint32_t mc_sectors[MC_MAX];
static uint8_t  mc_data[MC_MAX][512];
static uint16_t mc_count = 0;
static uint16_t mc_next = 0;       /* FIFO eviction slot */

uint32_t mc_total_sectors = 0;     /* Real card capacity from CSD */
uint32_t mc_partition_sectors = 0; /* Partition size from VBR BPB */

static void mc_store(uint32_t sector, const uint8_t *data) {
    for (uint16_t i = 0; i < mc_count; i++) {
        if (mc_sectors[i] == sector) return; /* already cached */
    }
    uint16_t slot;
    if (mc_count < MC_MAX) {
        slot = mc_count;
        mc_count++;
    } else {
        slot = mc_next;
        mc_next = (mc_next + 1) % MC_MAX;
    }
    mc_sectors[slot] = sector;
    memcpy(mc_data[slot], data, 512);
    if (sector + 1 > mc_total_sectors) mc_total_sectors = sector + 1;
}

void mc_reset(void) {
    mc_count = 0;
    mc_next = 0;
    cached_sector = 0xFFFFFFFF;
}

static int mc_find(uint32_t sector, uint8_t *buff) {
    for (uint16_t i = 0; i < mc_count; i++) {
        if (mc_sectors[i] == sector) {
            memcpy(buff, mc_data[i], 512);
            return 0;
        }
    }
    return -1;
}

DSTATUS disk_initialize(void) {
    BYTE n, cmd, ty, ocr[4];
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);
    cached_sector = 0xFFFFFFFF;
    mc_count = 0;
    mc_total_sectors = 0;

    /* DAT1/DAT2 Pullups */
    pa->gpers = (1u << 18) | (1u << 19);
    pa->oderc = (1u << 18) | (1u << 19); // Input
    pa->puers = (1u << 18) | (1u << 19); // Pullup
    
    for (int ms = 0; ms < 100; ms++) dly_100us(); /* 10ms boot delay */
    
    /* Setup SPI pins */
    pa->gpers = (1u << SD_PIN_MISO) | (1u << SD_PIN_MOSI) | (1u << SD_PIN_SCK) | (1u << SD_PIN_CS);
    pa->oderc = (1u << SD_PIN_MISO);
    pa->puers = (1u << SD_PIN_MISO);
    pa->oders = (1u << SD_PIN_MOSI) | (1u << SD_PIN_SCK) | (1u << SD_PIN_CS);
    pa->ovrs  = (1u << SD_PIN_MOSI);
    pa->ovrc  = (1u << SD_PIN_SCK);
    pa->ovrs  = (1u << SD_PIN_CS);

    for (int ms = 0; ms < 500; ms++) dly_100us(); /* Let lines settle */

    for (n = 10; n; n--) rcv_spi();

    ty = 0;
    if (send_cmd(0, 0) == 1) {         /* CMD0  - GO_IDLE_STATE */
        if (send_cmd(8, 0x1AA) == 1) {    /* CMD8  - SEND_IF_COND */
            for (n = 0; n < 4; n++) ocr[n] = rcv_spi();
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                for (int t = 10000; t && send_cmd(0x80 | 41, 1UL << 30); t--) dly_100us(); /* ACMD41 */
                if (send_cmd(58, 0) == 0) {  /* CMD58 - READ_OCR */
                    for (n = 0; n < 4; n++) ocr[n] = rcv_spi();
                    ty = (ocr[0] & 0x40) ? 12 : 10;
                }
            }
        } else {
            if (send_cmd(0x80 | 41, 0) <= 1) {
                ty = 2; cmd = 0x80 | 41;
            } else {
                ty = 1; cmd = 1;
            }
            for (int t = 10000; t && send_cmd(cmd, 0); t--) dly_100us();
            if (send_cmd(16, 512) != 0) ty = 0;
        }
    }
    CardType = ty;
    DESELECT();
    rcv_spi();

    /* Read real card capacity from CSD while SPI is still alive */
    if (ty) {
        spi_fast = 1;
        if (send_cmd(9, 0) == 0) {
            uint16_t t = 40000;
            while (rcv_spi() != 0xFE && --t);
            if (t) {
                uint8_t csd[16];
                for (uint8_t i = 0; i < 16; i++) csd[i] = rcv_spi();
                rcv_spi(); rcv_spi();
                if ((csd[0] >> 6) == 1) {
                    /* CSD v2 (SDHC/SDXC) */
                    uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16)
                                    | ((uint32_t)csd[8] << 8)
                                    | csd[9];
                    mc_total_sectors = (c_size + 1) * 1024;
                } else {
                    /* CSD v1 */
                    uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10)
                                    | ((uint32_t)csd[7] << 2)
                                    | (csd[8] >> 6);
                    uint32_t c_size_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
                    uint32_t block_len = 1 << (csd[5] & 0x0F);
                    mc_total_sectors = (c_size + 1) * (1 << (c_size_mult + 2)) * block_len / 512;
                }
            }
            DESELECT();
            rcv_spi();
        }
    }

    return ty ? 0 : STA_NOINIT;
}

DRESULT disk_readp(BYTE* buff, DWORD sector, UINT offset, UINT count) {
    if (cached_sector != sector) {
        /* SDHC/SDXC (CardType==12) use block address; all others need byte address */
        DWORD cmd_sector = (CardType == 12) ? sector : (sector * 512);
        if (send_cmd(17, cmd_sector) == 0) {
            uint16_t t = 40000;
            while (rcv_spi() != 0xFE && --t);
            if (t) {
                for (int i = 0; i < 512; i++) sector_cache[i] = rcv_spi();
                rcv_spi(); rcv_spi();
                cached_sector = sector;
                /* Store in multi-sector cache for post-USB reads */
                mc_store(sector, sector_cache);
            } else {
                return RES_ERROR;
            }
        } else {
            return RES_ERROR;
        }
        DESELECT();
        rcv_spi();
    }
    if (buff) {
        for (UINT i = 0; i < count; i++) {
            buff[i] = sector_cache[offset + i];
        }
    }
    return RES_OK;
}

DRESULT sd_read_sector(DWORD sector, BYTE *buff) {
    if (mc_find(sector, buff) == 0) return RES_OK;
    return disk_readp(buff, sector, 0, 512);
}

DRESULT sd_write_sector(DWORD sector, const BYTE *buff) {
    DWORD cmd_sector = (CardType == 12) ? sector : (sector * 512);
    if (send_cmd(24, cmd_sector) == 0) {
        sd_spi_transfer(0xFE);
        for (int i = 0; i < 512; i++) sd_spi_transfer(buff[i]);
        sd_spi_transfer(0xFF); sd_spi_transfer(0xFF);
        BYTE resp = rcv_spi();
        if ((resp & 0x1F) == 0x05) {
            UINT bc = 40000;
            while (rcv_spi() == 0 && --bc);
            DESELECT(); rcv_spi();
            cached_sector = 0xFFFFFFFF;
            return RES_OK;
        }
    }
    DESELECT(); rcv_spi();
    return RES_ERROR;
}

