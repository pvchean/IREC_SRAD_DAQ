/* Pins of the DAQ board (from DAQ_Test_Sketch/Global.h). */
#ifndef BOARD_H
#define BOARD_H

#define I2C0_SDA_PIN   8
#define I2C0_SCL_PIN   18
#define I2C0_RDY_PIN   17      /* ADS1115 ALERT/RDY, falling edge = new sample */

#define NAND_MISO_PIN  13
#define NAND_MOSI_PIN  11
#define NAND_CLK_PIN   12
#define NAND_CS_PIN    10
/* GPIO 14 and 21 are SDMMC pins on this board, so the driver does not hold WP and HOLD. */
#define NAND_WP_PIN    (-1)
#define NAND_HOLD_PIN  (-1)

#endif
