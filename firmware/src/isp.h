/*
 * STK500v2 ISP command handlers (CMD_*_ISP + CMD_SPI_MULTI).
 * See isp.c for the socket-wiring trick that makes this work on the
 * HVPP ZIF socket without any rewiring.
 */

#ifndef ISP_H
#define ISP_H

#include <stdint.h>
#include "hal.h"

int16_t isp_enter_progmode(void);
int16_t isp_leave_progmode(void);
int16_t isp_chip_erase(void);
int16_t isp_spi_multi(void);
int16_t isp_read_byte_cmd(void);      /* READ_FUSE/LOCK/SIGNATURE/OSCCAL_ISP */
int16_t isp_program_byte_cmd(void);   /* PROGRAM_FUSE/LOCK_ISP */
int16_t isp_read_flash(void);
int16_t isp_read_eeprom(void);
int16_t isp_program_flash(void);
int16_t isp_program_eeprom(void);

#endif
