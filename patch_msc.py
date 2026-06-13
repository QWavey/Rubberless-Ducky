import sys

with open('src/usb_msc.c', 'r', encoding='utf-8') as f:
    content = f.read()

# Add externs at top
externs = """
#include <stdint.h>
extern uint8_t sd_read_sector(uint32_t sector, uint8_t *buff);
extern uint8_t sd_write_sector(uint32_t sector, const uint8_t *buff);
"""
content = content.replace("#include <string.h>", "#include <string.h>\n" + externs)

# Read 10
read_10 = """
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
"""

# Write 10
write_10 = """
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
"""

content = content.replace("default:\n            // Fail", read_10 + write_10 + "        default:\n            // Fail")

with open('src/usb_msc.c', 'w', encoding='utf-8') as f:
    f.write(content)
