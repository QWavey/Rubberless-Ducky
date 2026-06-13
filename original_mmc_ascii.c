/*-----------------------------------------------------------------------*/
/* PFF - Low level disk control module for AT32UC3B1 (Bit-Bang)          */
/*-----------------------------------------------------------------------*/

#include <avr32/io.h>
#include <stdint.h>
#include "diskio.h"

#define SD_PORT_MISO 0
#define SD_PIN_MISO  25
#define SD_PORT_MOSI 0
#define SD_PIN_MOSI  14
#define SD_PORT_SCK  0
#define SD_PIN_SCK   15
#define SD_PORT_CS   0
#define SD_PIN_CS    0

#define _GPIO_PORT(n) ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void SELECT(void)   { _GPIO_PORT(SD_PORT_CS)->ovrc = (1u << SD_PIN_CS); }
static inline void DESELECT(void) { _GPIO_PORT(SD_PORT_CS)->ovrs = (1u << SD_PIN_CS); }

static uint8_t sd_spi_transfer(uint8_t out) {
    uint8_t in = 0;
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);

    /* Unrolled bit-bang SPI for maximum speed */
#define SPI_BIT(bit_idx) \
    if (out & (1 << (7 - bit_idx))) pa->ovrs = (1u << SD_PIN_MOSI); \
    else                            pa->ovrc = (1u << SD_PIN_MOSI); \
    pa->ovrs = (1u << SD_PIN_SCK); /* SCK HIGH */ \
    in <<= 1; \
    if (pa->pvr & (1u << SD_PIN_MISO)) in |= 1; \
    pa->ovrc = (1u << SD_PIN_SCK); /* SCK LOW */

    SPI_BIT(0);
    SPI_BIT(1);
    SPI_BIT(2);
    SPI_BIT(3);
    SPI_BIT(4);
    SPI_BIT(5);
    SPI_BIT(6);
    SPI_BIT(7);
#undef SPI_BIT

    return in;
}

static uint8_t rcv_spi(void) {
    return sd_spi_transfer(0xFF);
}

/*-----------------------------------------------------------------------*/
/* Send a command packet to MMC                                          */
/*-----------------------------------------------------------------------*/
#define CMD0	(0)			/* GO_IDLE_STATE */
#define CMD1	(1)			/* SEND_OP_COND */
#define CMD8	(8)			/* SEND_IF_COND */
#define CMD16	(16)		/* SET_BLOCKLEN */
#define CMD17	(17)		/* READ_SINGLE_BLOCK */
#define CMD55	(55)		/* APP_CMD */
#define ACMD41	(0x80+41)	/* SEND_OP_COND (SDC) */
#define CMD58	(58)		/* READ_OCR */

static BYTE send_cmd(BYTE cmd, DWORD arg) {
    BYTE n, res;

    if (cmd & 0x80) {	/* ACMD<n> is the command sequense of CMD55-CMD<n> */
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) return res;
    }

    /* Select the card */
    DESELECT();
    rcv_spi();
    SELECT();
    rcv_spi();

    /* Send a command packet */
    sd_spi_transfer(0x40 | cmd);				/* Start + Command index */
    sd_spi_transfer((BYTE)(arg >> 24));		/* Argument[31..24] */
    sd_spi_transfer((BYTE)(arg >> 16));		/* Argument[23..16] */
    sd_spi_transfer((BYTE)(arg >> 8));			/* Argument[15..8] */
    sd_spi_transfer((BYTE)arg);				/* Argument[7..0] */
    n = 0x01;							/* Dummy CRC + Stop */
    if (cmd == CMD0) n = 0x95;			/* Valid CRC for CMD0(0) */
    if (cmd == CMD8) n = 0x87;			/* Valid CRC for CMD8(0x1AA) */
    sd_spi_transfer(n);

    /* Receive a command response */
    n = 200;								/* Wait for a valid response in timeout of 200 attempts */
    do {
        res = rcv_spi();
    } while ((res & 0x80) && --n);

    return res;			/* Return with the response value */
}

/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/
static BYTE CardType;
uint8_t dbg_cmd0 = 0xFF;
uint8_t dbg_cmd8 = 0xFF;
uint8_t dbg_acmd41 = 0xFF;

static uint8_t sector_cache[512];
static DWORD cached_sector = 0xFFFFFFFF;

static inline uint32_t get_cpu_count(void) {
    return __builtin_mfsr(AVR32_COUNT);
}

extern void usb_device_task(void);

static void dly_100us(void) {
    /* 4,800 cycles = 100 microseconds at 48 MHz */
    uint32_t start = get_cpu_count();
    while ((get_cpu_count() - start) < 4800) {
        usb_device_task();
    }
}

DSTATUS disk_initialize(void) {
    BYTE n, cmd, ty, ocr[4];
    UINT tmr;
    volatile avr32_gpio_port_t *pa = _GPIO_PORT(0);

    cached_sector = 0xFFFFFFFF; /* Invalidate cache */
    

    /* Power up the SD card via PA17 (Active LOW P-Channel MOSFET) */
    pa->gpers |= (1u << 17);
    pa->oders |= (1u << 17);
    pa->ovrc   = (1u << 17); /* Drive LOW to turn ON power */
    
    /* Give power time to stabilize: 2ms */
    for (int ms = 0; ms < 20; ms++) dly_100us();
    
    /* Initialize SPI pins */
    pa->gpers |= (1u << SD_PIN_MISO) | (1u << SD_PIN_MOSI) | (1u << SD_PIN_SCK);
    
    pa->oderc = (1u << SD_PIN_MISO);
    pa->puers = (1u << SD_PIN_MISO);
    
    /* DAT1/DAT2 pullups (PA18 and PA19) */
    pa->gpers |= (1u << 18) | (1u << 19);
    pa->oderc  = (1u << 18) | (1u << 19);
    pa->puers  = (1u << 18) | (1u << 19);
    
    pa->oders = (1u << SD_PIN_MOSI);
    pa->ovrs  = (1u << SD_PIN_MOSI);
    
    pa->oders = (1u << SD_PIN_SCK);
    pa->ovrc  = (1u << SD_PIN_SCK);
    
    volatile avr32_gpio_port_t *pcs = _GPIO_PORT(0);
    pcs->gpers = (1u << SD_PIN_CS);
    pcs->oders = (1u << SD_PIN_CS);
    pcs->ovrs  = (1u << SD_PIN_CS);

    uint8_t res = 0xFF;
    for (int retry = 0; retry < 100; retry++) {
        /* 80 dummy clocks with CS=H */
        pcs->ovrs = (1u << SD_PIN_CS);
        for (n = 0; n < 10; n++) rcv_spi();
        
        /* Exact sequence from the scanner */
        pcs->ovrc = (1u << SD_PIN_CS);
        sd_spi_transfer(0x40);
        sd_spi_transfer(0x00);
        sd_spi_transfer(0x00);
        sd_spi_transfer(0x00);
        sd_spi_transfer(0x00);
        sd_spi_transfer(0x95);
        
        res = 0xFF;
        for (int i = 0; i < 20; i++) {
            res = sd_spi_transfer(0xFF);
            if ((res & 0x80) == 0) break;
        }
        
        if (res == 0x01) break;
    }
    dbg_cmd0 = res;
    
    if (dbg_cmd0 == 1) {			/* Enter Idle state */
        dbg_cmd8 = send_cmd(CMD8, 0x1AA);
        if (dbg_cmd8 == 1) {	/* SDv2 */
            for (n = 0; n < 4; n++) ocr[n] = rcv_spi();		/* Get trailing return value of R7 resp */
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {			/* The card can work at vdd range of 2.7-3.6V */
                for (tmr = 10000; tmr && (dbg_acmd41 = send_cmd(ACMD41, 1UL << 30)); tmr--) {
                    dly_100us();
                }
                if (tmr && send_cmd(CMD58, 0) == 0) {		/* Check CCS bit in the OCR */
                    for (n = 0; n < 4; n++) ocr[n] = rcv_spi();
                    ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;	/* SDv2 (HC or SC) */
                }
            }
        } else {							/* SDv1 or MMCv3 */
            if ((dbg_acmd41 = send_cmd(ACMD41, 0)) <= 1) 	{
                ty = CT_SD1; cmd = ACMD41;	/* SDv1 */
            } else {
                ty = CT_MMC; cmd = CMD1;	/* MMCv3 */
            }
            for (tmr = 10000; tmr && send_cmd(cmd, 0); tmr--) {
                dly_100us();
            }
            if (!tmr || send_cmd(CMD16, 512) != 0)			/* Set R/W block length to 512 */
                ty = 0;
        }
    }
    CardType = ty;
    DESELECT();
    rcv_spi();

    return ty ? 0 : STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Read partial sector                                                   */
/*-----------------------------------------------------------------------*/
DRESULT disk_readp (
    BYTE *buff,		/* Pointer to the read buffer (NULL:Read bytes are forwarded to the stream) */
    DWORD sector,	/* Sector number (LBA) */
    UINT offset,	/* Byte offset to read from (0..511) */
    UINT count		/* Number of bytes to read (ofs + cnt mus be <= 512) */
)
{
    if (sector != cached_sector) {
        DRESULT res = RES_ERROR;
        DWORD cmd_sector = (CardType & CT_BLOCK) ? sector : (sector * 512);

        if (send_cmd(CMD17, cmd_sector) == 0) {		/* READ_SINGLE_BLOCK */
            BYTE rc;
            UINT bc = 40000;
            do {							/* Wait for data packet */
                rc = rcv_spi();
            } while (rc == 0xFF && --bc);

            if (rc == 0xFE) {				/* A data packet arrived */
                /* Read the entire 512-byte sector into cache */
                for (int i = 0; i < 512; i++) {
                    sector_cache[i] = rcv_spi();
                }

                /* Skip 2-byte CRC */
                rcv_spi();
                rcv_spi();
                
                cached_sector = sector;
                res = RES_OK;
            }
        }

        DESELECT();
        rcv_spi();
        
        if (res != RES_OK) return res;
    }

    if (buff) {
        for (UINT i = 0; i < count; i++) {
            buff[i] = sector_cache[offset + i];
        }
    }

    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* Read 512-byte Sector (For MSC)                                        */
/*-----------------------------------------------------------------------*/
DRESULT sd_read_sector(DWORD sector, BYTE *buff)
{
    DWORD cmd_sector = (CardType & CT_BLOCK) ? sector : (sector * 512);

    if (send_cmd(CMD17, cmd_sector) == 0) {
        BYTE rc;
        UINT bc = 40000;
        do { rc = rcv_spi(); } while (rc == 0xFF && --bc);

        if (rc == 0xFE) {
            for (int i = 0; i < 512; i++) {
                buff[i] = rcv_spi();
            }
            rcv_spi(); /* CRC */
            rcv_spi();
            DESELECT();
            rcv_spi();
            return RES_OK;
        }
    }
    DESELECT();
    rcv_spi();
    return RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* Write 512-byte Sector (For MSC)                                       */
/*-----------------------------------------------------------------------*/
#define CMD24   (24)    /* WRITE_BLOCK */

DRESULT sd_write_sector(DWORD sector, const BYTE *buff)
{
    DWORD cmd_sector = (CardType & CT_BLOCK) ? sector : (sector * 512);

    if (send_cmd(CMD24, cmd_sector) == 0) {
        sd_spi_transfer(0xFF); /* Dummy byte */
        sd_spi_transfer(0xFE); /* Data Token */
        
        for (int i = 0; i < 512; i++) {
            sd_spi_transfer(buff[i]);
        }
        
        sd_spi_transfer(0xFF); /* Dummy CRC */
        sd_spi_transfer(0xFF);
        
        BYTE resp = rcv_spi();
        if ((resp & 0x1F) == 0x05) { /* Accepted */
            UINT bc = 0xFFFFF; /* Wait for busy flag to clear */
            while (rcv_spi() == 0 && --bc) {
                usb_device_task(); /* Keep USB alive during busy */
            }
            DESELECT();
            rcv_spi();
            return bc ? RES_OK : RES_ERROR;
        }
    }
    DESELECT();
    rcv_spi();
    return RES_ERROR;
}
