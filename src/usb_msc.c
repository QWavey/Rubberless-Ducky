#include "usb_msc.h"
#include "diskio.h"
#include <avr32/io.h>
#include <string.h>

#include <stdint.h>
extern uint32_t mc_total_sectors;
extern uint32_t mc_partition_sectors;


#define USBB_EP_FIFO(ep)  ((volatile uint8_t *)(AVR32_USBB_SLAVE_ADDRESS + ((ep) * 0x10000)))

/* SCSI Command OpCodes */
#define SCSI_TEST_UNIT_READY        0x00
#define SCSI_REQUEST_SENSE          0x03
#define SCSI_INQUIRY                0x12
#define SCSI_MODE_SENSE_6           0x1A
#define SCSI_START_STOP_UNIT        0x1B
#define SCSI_PREVENT_ALLOW          0x1E
#define SCSI_READ_CAPACITY_10       0x25
#define SCSI_READ_10                0x28
#define SCSI_WRITE_10               0x2A
#define SCSI_MODE_SENSE_10          0x55
#define SCSI_READ_FORMAT_CAPACITIES 0x23

/* BOT States */
#define BOT_STATE_IDLE          0
#define BOT_STATE_DATA_IN       1  /* SCSI -> Host */
#define BOT_STATE_DATA_OUT      2  /* Host -> SCSI */
#define BOT_STATE_CSW           3

struct {
    uint8_t state;
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
    
    uint32_t data_remaining;
    uint32_t csw_status;

    /* Multi-block transfer state */
    uint32_t transfer_lba;
    uint16_t transfer_blocks;
    uint16_t transfer_done;
    uint8_t  sec_buf[512];
    uint16_t sub_block_ptr;
} bot;

static const uint8_t inquiry_data[] = {
    0x00, /* Direct-access block device */
    0x80, /* Removable */
    0x04, /* SPC-2 compliance */
    0x02, /* Response data format */
    31,   /* Additional length */
    0x00, 0x00, 0x00,
    'H','a','k','5',' ',' ',' ',' ',
    'R','u','b','b','e','r','D','u',
    'c','k','y',' ',' ',' ',' ',' ',
    '1','.','0','0'
};

bool usb_msc_handle_setup(uint8_t req_type, uint8_t req, uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
    if (req_type == 0xA1 && req == 0xFE) { // GET MAX LUN
        extern void ep0_send(const void*, uint16_t, uint16_t);
        uint8_t lun = 0;
        ep0_send(&lun, 1, wLength);
        return true;
    }
    if (req_type == 0x21 && req == 0xFF) { // BOT RESET
        bot.state = BOT_STATE_IDLE;
        extern void ep0_ack_status_in(void);
        ep0_ack_status_in();
        return true;
    }
    return false;
}

void usb_msc_init(void) {
    bot.state = BOT_STATE_IDLE;
}

static void msc_send_csw(uint8_t status) {
    bot.csw_status = status;
    bot.state = BOT_STATE_CSW;
}

static void ep3_stall(void) {
    AVR32_USBB.uecon3set = AVR32_USBB_UECON3SET_STALLRQS_MASK;
}

static void ep2_stall(void) {
    AVR32_USBB.uecon2set = AVR32_USBB_UECON2SET_STALLRQS_MASK;
}

static void ep3_in_xfer_sync(const uint8_t *data, uint32_t len) {
    const uint8_t *p = data;
    while (len > 0) {
        while (!(AVR32_USBB.uesta3 & AVR32_USBB_UESTA3_TXINI_MASK));
        uint32_t chunk = len;
        if (chunk > 64) chunk = 64;
        for (uint32_t i = 0; i < chunk; i++) USBB_EP_FIFO(MSC_EP_IN)[i] = p[i];
        AVR32_USBB.uesta3clr = AVR32_USBB_UESTA3CLR_TXINIC_MASK;
        AVR32_USBB.uecon3clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
        p += chunk;
        len -= chunk;
    }
}

static void process_scsi_command(void) {
    uint8_t opcode = bot.CBWCB[0];
    uint32_t alloc_len;
    
    switch (opcode) {
        case SCSI_INQUIRY: {
            alloc_len = ((uint32_t)bot.CBWCB[3] << 8) | bot.CBWCB[4];
            uint32_t len = sizeof(inquiry_data);
            if (len > alloc_len) len = alloc_len;
            if (len > bot.dCBWDataTransferLength) len = bot.dCBWDataTransferLength;
            ep3_in_xfer_sync(inquiry_data, len);
            bot.data_remaining = (len < bot.dCBWDataTransferLength) ? bot.dCBWDataTransferLength - len : 0;
            msc_send_csw(0x00);
            break;
        }
        case SCSI_REQUEST_SENSE: {
            uint8_t sense[18];
            memset(sense, 0, sizeof(sense));
            alloc_len = bot.CBWCB[4];
            if (alloc_len > 18) alloc_len = 18;
            if (alloc_len > bot.dCBWDataTransferLength) alloc_len = bot.dCBWDataTransferLength;
            sense[0]  = 0x70;
            sense[2]  = 0x00;
            sense[7]  = 10;
            ep3_in_xfer_sync(sense, alloc_len);
            bot.data_remaining = (alloc_len < bot.dCBWDataTransferLength) ? bot.dCBWDataTransferLength - alloc_len : 0;
            msc_send_csw(0x00);
            break;
        }
        case SCSI_MODE_SENSE_6: {
            uint8_t mode[4];
            mode[0] = 3;
            mode[1] = 0x00;
            mode[2] = 0x00;
            mode[3] = 0;
            alloc_len = bot.CBWCB[4];
            if (alloc_len > 4) alloc_len = 4;
            if (alloc_len > bot.dCBWDataTransferLength) alloc_len = bot.dCBWDataTransferLength;
            ep3_in_xfer_sync(mode, alloc_len);
            bot.data_remaining = (alloc_len < bot.dCBWDataTransferLength) ? bot.dCBWDataTransferLength - alloc_len : 0;
            msc_send_csw(0x00);
            break;
        }
        case SCSI_MODE_SENSE_10: {
            uint8_t mode[8];
            memset(mode, 0, sizeof(mode));
            mode[0] = 7;
            alloc_len = ((uint32_t)bot.CBWCB[7] << 8) | bot.CBWCB[8];
            if (alloc_len > 8) alloc_len = 8;
            if (alloc_len > bot.dCBWDataTransferLength) alloc_len = bot.dCBWDataTransferLength;
            ep3_in_xfer_sync(mode, alloc_len);
            bot.data_remaining = (alloc_len < bot.dCBWDataTransferLength) ? bot.dCBWDataTransferLength - alloc_len : 0;
            msc_send_csw(0x00);
            break;
        }
        case SCSI_TEST_UNIT_READY:
        case SCSI_START_STOP_UNIT:
        case SCSI_PREVENT_ALLOW:
            msc_send_csw(0x00);
            break;
        case SCSI_READ_CAPACITY_10: {
            uint32_t disk_sectors = mc_partition_sectors > 0 ? mc_partition_sectors : mc_total_sectors;
            uint32_t last_lba = disk_sectors > 0 ? disk_sectors - 1 : 0;
            uint8_t cap[8] = {
                (uint8_t)(last_lba >> 24), (uint8_t)(last_lba >> 16),
                (uint8_t)(last_lba >> 8),  (uint8_t)(last_lba),
                0x00, 0x00, 0x02, 0x00
            };
            ep3_in_xfer_sync(cap, 8);
            bot.data_remaining = (8 < bot.dCBWDataTransferLength) ? bot.dCBWDataTransferLength - 8 : 0;
            msc_send_csw(0x00);
            break;
        }

        case SCSI_READ_FORMAT_CAPACITIES: {
            uint32_t disk_sectors = mc_partition_sectors > 0 ? mc_partition_sectors : mc_total_sectors;
            uint32_t blocks = disk_sectors > 0 ? disk_sectors : 0x000FFFFF;
            uint8_t fmt_cap[12] = {
                0x00, 0x00, 0x00, 0x08,
                (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16),
                (uint8_t)(blocks >> 8),  (uint8_t)(blocks),
                0x02,
                0x00, 0x02, 0x00
            };
            uint32_t len = sizeof(fmt_cap);
            alloc_len = ((uint32_t)bot.CBWCB[7] << 8) | bot.CBWCB[8];
            if (len > alloc_len) len = alloc_len;
            if (len > bot.dCBWDataTransferLength) len = bot.dCBWDataTransferLength;
            ep3_in_xfer_sync(fmt_cap, len);
            bot.data_remaining = (len < bot.dCBWDataTransferLength) ? bot.dCBWDataTransferLength - len : 0;
            msc_send_csw(0x00);
            break;
        }
        
        case SCSI_READ_10: {
            bot.transfer_lba    = (bot.CBWCB[2] << 24) | (bot.CBWCB[3] << 16) | (bot.CBWCB[4] << 8) | bot.CBWCB[5];
            bot.transfer_blocks = (bot.CBWCB[7] << 8) | bot.CBWCB[8];
            bot.transfer_done   = 0;
            bot.sub_block_ptr   = 512; /* triggers immediate SD read */
            bot.state           = BOT_STATE_DATA_IN;
            break;
        }

        case SCSI_WRITE_10: {
            bot.transfer_lba    = (bot.CBWCB[2] << 24) | (bot.CBWCB[3] << 16) | (bot.CBWCB[4] << 8) | bot.CBWCB[5];
            bot.transfer_blocks = (bot.CBWCB[7] << 8) | bot.CBWCB[8];
            bot.transfer_done   = 0;
            bot.sub_block_ptr   = 0;
            bot.state           = BOT_STATE_DATA_OUT;
            break;
        }
        default:
            if (bot.dCBWDataTransferLength > 0) {
                if (bot.bmCBWFlags & 0x80) ep3_stall();
                else                      ep2_stall();
            }
            msc_send_csw(0x01);
            break;
    }
}

void usb_msc_task(void) {
    if (bot.state == BOT_STATE_IDLE) {
        if (AVR32_USBB.uesta2 & AVR32_USBB_UESTA2_RXOUTI_MASK) {
            uint8_t *fifo = (uint8_t*)USBB_EP_FIFO(MSC_EP_OUT);
            bot.dCBWSignature = fifo[0] | (fifo[1]<<8) | (fifo[2]<<16) | (fifo[3]<<24);
            if (bot.dCBWSignature == 0x43425355) {
                bot.dCBWTag = fifo[4] | (fifo[5]<<8) | (fifo[6]<<16) | (fifo[7]<<24);
                bot.dCBWDataTransferLength = fifo[8] | (fifo[9]<<8) | (fifo[10]<<16) | (fifo[11]<<24);
                bot.bmCBWFlags = fifo[12];
                bot.bCBWLUN = fifo[13];
                bot.bCBWCBLength = fifo[14];
                memcpy(bot.CBWCB, &fifo[15], 16);
                bot.data_remaining = bot.dCBWDataTransferLength;
                AVR32_USBB.uesta2clr = AVR32_USBB_UESTA2CLR_RXOUTIC_MASK;
                AVR32_USBB.uecon2clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
                process_scsi_command();
            } else {
                AVR32_USBB.uesta2clr = AVR32_USBB_UESTA2CLR_RXOUTIC_MASK;
                AVR32_USBB.uecon2clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
                AVR32_USBB.uecon2set = AVR32_USBB_UECON2SET_STALLRQS_MASK;
            }
        }
    } else if (bot.state == BOT_STATE_DATA_IN) {
        /* SCSI -> Host (READ_10) */
        if (bot.transfer_done >= bot.transfer_blocks) {
            bot.data_remaining = 0;
            msc_send_csw(0x00);
            return;
        }
        if (bot.sub_block_ptr >= 512) {
            /* Current sector fully sent, read next one from SD */
            if (sd_read_sector(bot.transfer_lba + bot.transfer_done, bot.sec_buf) != 0) {
                ep3_stall();
                bot.data_remaining = (uint32_t)(bot.transfer_blocks - bot.transfer_done) * 512;
                msc_send_csw(0x01);
                return;
            }
            bot.transfer_done++;
            bot.sub_block_ptr = 0;
            extern void storage_activity_mark(void);
            storage_activity_mark();
        }
        /* Try to send a 64-byte chunk of the current sector */
        if (AVR32_USBB.uesta3 & AVR32_USBB_UESTA3_TXINI_MASK) {
            volatile uint8_t *fifo = (volatile uint8_t *)USBB_EP_FIFO(MSC_EP_IN);
            for (int i = 0; i < 64; i++) {
                fifo[i] = bot.sec_buf[bot.sub_block_ptr + i];
            }
            AVR32_USBB.uesta3clr = AVR32_USBB_UESTA3CLR_TXINIC_MASK;
            AVR32_USBB.uecon3clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
            bot.sub_block_ptr += 64;
        }
    } else if (bot.state == BOT_STATE_DATA_OUT) {
        /* Host -> SCSI (WRITE_10) */
        if (bot.transfer_done >= bot.transfer_blocks) {
            bot.data_remaining = 0;
            msc_send_csw(0x00);
            return;
        }
        /* Wait for a 64-byte chunk from host */
        if (AVR32_USBB.uesta2 & AVR32_USBB_UESTA2_RXOUTI_MASK) {
            volatile uint8_t *fifo = (volatile uint8_t *)USBB_EP_FIFO(MSC_EP_OUT);
            for (int i = 0; i < 64; i++) {
                bot.sec_buf[bot.sub_block_ptr + i] = fifo[i];
            }
            AVR32_USBB.uesta2clr = AVR32_USBB_UESTA2CLR_RXOUTIC_MASK;
            AVR32_USBB.uecon2clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
            bot.sub_block_ptr += 64;
            if (bot.sub_block_ptr >= 512) {
                /* Sector buffer full, write to SD */
                if (sd_write_sector(bot.transfer_lba + bot.transfer_done, bot.sec_buf) != 0) {
                    ep2_stall();
                    bot.data_remaining = (uint32_t)(bot.transfer_blocks - bot.transfer_done) * 512;
                    msc_send_csw(0x01);
                    return;
                }
                bot.transfer_done++;
                bot.sub_block_ptr = 0;
                extern void storage_activity_mark(void);
                storage_activity_mark();
            }
        }
    } else if (bot.state == BOT_STATE_CSW) {
        if (AVR32_USBB.uesta3 & AVR32_USBB_UESTA3_TXINI_MASK) {
            uint8_t *fifo = (uint8_t*)USBB_EP_FIFO(MSC_EP_IN);
            fifo[0] = 0x55; fifo[1] = 0x53; fifo[2] = 0x42; fifo[3] = 0x53;
            fifo[4] = bot.dCBWTag & 0xFF; fifo[5] = (bot.dCBWTag>>8)&0xFF;
            fifo[6] = (bot.dCBWTag>>16)&0xFF; fifo[7] = (bot.dCBWTag>>24)&0xFF;
            fifo[8] = bot.data_remaining & 0xFF; fifo[9] = (bot.data_remaining>>8)&0xFF;
            fifo[10] = (bot.data_remaining>>16)&0xFF; fifo[11] = (bot.data_remaining>>24)&0xFF;
            fifo[12] = bot.csw_status;
            AVR32_USBB.uesta3clr = AVR32_USBB_UESTA3CLR_TXINIC_MASK;
            AVR32_USBB.uecon3clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
            bot.state = BOT_STATE_IDLE;
        }
    }
}
