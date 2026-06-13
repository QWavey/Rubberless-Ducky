#include "usb_msc.h"
#include <avr32/io.h>
#include <string.h>

#include <stdint.h>
extern uint8_t sd_read_sector(uint32_t sector, uint8_t *buff);
extern uint8_t sd_write_sector(uint32_t sector, const uint8_t *buff);


#define USBB_EP_FIFO(ep)  ((volatile uint8_t *)(AVR32_USBB_SLAVE_ADDRESS + ((ep) * 0x10000)))

/* SCSI Command OpCodes */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_PREVENT_ALLOW      0x1E

/* BOT States */
#define BOT_STATE_IDLE          0
#define BOT_STATE_DATA_IN       1
#define BOT_STATE_DATA_OUT      2
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

void msc_send_csw(uint8_t status) {
    bot.csw_status = status;
    bot.state = BOT_STATE_CSW;
}

void process_scsi_command(void) {
    uint8_t opcode = bot.CBWCB[0];
    
    switch (opcode) {
        case SCSI_INQUIRY: {
            uint32_t len = sizeof(inquiry_data);
            if (len > bot.dCBWDataTransferLength) len = bot.dCBWDataTransferLength;
            // Write to EP2
            while (!(AVR32_USBB.uesta3 & AVR32_USBB_UESTA2_TXINI_MASK));
            for(uint32_t i=0; i<len; i++) USBB_EP_FIFO(MSC_EP_IN)[i] = inquiry_data[i];
            AVR32_USBB.uesta3clr = AVR32_USBB_UESTA2CLR_TXINIC_MASK;
            AVR32_USBB.uecon3clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
            bot.data_remaining -= len;
            msc_send_csw(0x00);
            break;
        }
        case SCSI_TEST_UNIT_READY:
            msc_send_csw(0x00); // Success
            break;
        case SCSI_READ_CAPACITY_10: {
            uint8_t cap[8] = {
                0x00, 0x0F, 0xFF, 0xFF, // Last LBA (dummy for 512MB)
                0x00, 0x00, 0x02, 0x00  // Block size 512 bytes
            };
            while (!(AVR32_USBB.uesta3 & AVR32_USBB_UESTA2_TXINI_MASK));
            for(int i=0; i<8; i++) USBB_EP_FIFO(MSC_EP_IN)[i] = cap[i];
            AVR32_USBB.uesta3clr = AVR32_USBB_UESTA2CLR_TXINIC_MASK;
            AVR32_USBB.uecon3clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
            msc_send_csw(0x00);
            break;
        }
        
        case SCSI_READ_10: {
            uint32_t lba = (bot.CBWCB[2] << 24) | (bot.CBWCB[3] << 16) | (bot.CBWCB[4] << 8) | bot.CBWCB[5];
            uint16_t blocks = (bot.CBWCB[7] << 8) | bot.CBWCB[8];
            uint8_t sec_buf[512];
            for (uint16_t b = 0; b < blocks; b++) {
                if (sd_read_sector(lba + b, sec_buf) != 0) {
                    AVR32_USBB.uecon3set = AVR32_USBB_UECON2SET_STALLRQS_MASK;
                    msc_send_csw(0x01);
                    return;
                }
                uint8_t *ptr = sec_buf;
                for (int p = 0; p < 512 / 64; p++) {
                    while (!(AVR32_USBB.uesta3 & AVR32_USBB_UESTA2_TXINI_MASK));
                    for (int i = 0; i < 64; i++) {
                        USBB_EP_FIFO(MSC_EP_IN)[i] = *ptr++;
                    }
                    AVR32_USBB.uesta3clr = AVR32_USBB_UESTA2CLR_TXINIC_MASK;
                    AVR32_USBB.uecon3clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
                }
            }
            bot.data_remaining = 0;
            msc_send_csw(0x00);
            break;
        }

        case SCSI_WRITE_10: {
            uint32_t lba = (bot.CBWCB[2] << 24) | (bot.CBWCB[3] << 16) | (bot.CBWCB[4] << 8) | bot.CBWCB[5];
            uint16_t blocks = (bot.CBWCB[7] << 8) | bot.CBWCB[8];
            uint8_t sec_buf[512];
            for (uint16_t b = 0; b < blocks; b++) {
                uint8_t *ptr = sec_buf;
                for (int p = 0; p < 512 / 64; p++) {
                    while (!(AVR32_USBB.uesta2 & AVR32_USBB_UESTA3_RXOUTI_MASK));
                    for (int i = 0; i < 64; i++) {
                        *ptr++ = USBB_EP_FIFO(MSC_EP_OUT)[i];
                    }
                    AVR32_USBB.uesta2clr = AVR32_USBB_UESTA3CLR_RXOUTIC_MASK;
                    AVR32_USBB.uecon2clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
                }
                if (sd_write_sector(lba + b, sec_buf) != 0) {
                    AVR32_USBB.uecon2set = AVR32_USBB_UECON3SET_STALLRQS_MASK;
                    msc_send_csw(0x01);
                    return;
                }
            }
            bot.data_remaining = 0;
            msc_send_csw(0x00);
            break;
        }
        default:
            // Fail
            AVR32_USBB.uecon3set = AVR32_USBB_UECON2SET_STALLRQS_MASK;
            msc_send_csw(0x01); // Failed
            break;
    }
}

void usb_msc_task(void) {
    if (bot.state == BOT_STATE_IDLE) {
        if (AVR32_USBB.uesta2 & AVR32_USBB_UESTA3_RXOUTI_MASK) {
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
                
                AVR32_USBB.uesta2clr = AVR32_USBB_UESTA3CLR_RXOUTIC_MASK;
                AVR32_USBB.uecon2clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
                process_scsi_command();
            } else {
                AVR32_USBB.uesta2clr = AVR32_USBB_UESTA3CLR_RXOUTIC_MASK;
                AVR32_USBB.uecon2clr = AVR32_USBB_UECON3CLR_FIFOCONC_MASK;
                AVR32_USBB.uecon2set = AVR32_USBB_UECON3SET_STALLRQS_MASK;
            }
        }
    } else if (bot.state == BOT_STATE_CSW) {
        if (AVR32_USBB.uesta3 & AVR32_USBB_UESTA2_TXINI_MASK) {
            uint8_t *fifo = (uint8_t*)USBB_EP_FIFO(MSC_EP_IN);
            fifo[0] = 0x55; fifo[1] = 0x53; fifo[2] = 0x42; fifo[3] = 0x53; // 'USBS'
            fifo[4] = bot.dCBWTag & 0xFF; fifo[5] = (bot.dCBWTag>>8)&0xFF;
            fifo[6] = (bot.dCBWTag>>16)&0xFF; fifo[7] = (bot.dCBWTag>>24)&0xFF;
            fifo[8] = bot.data_remaining & 0xFF; fifo[9] = (bot.data_remaining>>8)&0xFF;
            fifo[10] = (bot.data_remaining>>16)&0xFF; fifo[11] = (bot.data_remaining>>24)&0xFF;
            fifo[12] = bot.csw_status;
            
            AVR32_USBB.uesta3clr = AVR32_USBB_UESTA2CLR_TXINIC_MASK;
            AVR32_USBB.uecon3clr = AVR32_USBB_UECON2CLR_FIFOCONC_MASK;
            bot.state = BOT_STATE_IDLE;
        }
    }
}
