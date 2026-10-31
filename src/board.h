/* Pins of the DAQ board (from DAQ_Test_Sketch/Global.h). */
#ifndef BOARD_H
#define BOARD_H

#define I2C0_SDA_PIN   8
#define I2C0_SCL_PIN   18
#define I2C0_RDY_PIN   17      /* ADS1115 ALERT/RDY, falling edge = new sample */

#define SERIAL1_TX_PIN 42
#define SERIAL1_RX_PIN 41
#define SERIAL2_TX_PIN 40
#define SERIAL2_RX_PIN 39

#define NAND_MISO_PIN  13
#define NAND_MOSI_PIN  11
#define NAND_CLK_PIN   12
#define NAND_CS_PIN    10
/* GPIO 14 and 21 are SDMMC pins on this board, so the driver does not hold WP and HOLD. */
#define NAND_WP_PIN    (-1)
#define NAND_HOLD_PIN  (-1)
/* SPI clock of the NAND. GPIO 10, 11, 12, and 13 are the IO_MUX pins of SPI2 on the ESP32-S3 (CS, MOSI, CLK, MISO), so the
   bus can run at 80 MHz. The chip allows 90 MHz (data sheet 16.2), but its output valid time (tCLQV, 9 ns at most) is longer
   than half a period at 80 MHz (6.25 ns). So 80 MHz has no guaranteed margin: it passed the read and mount tests at room
   temperature on one board (see the README). Page programs at 80 MHz are not tested yet. The SPI clock can only be 80 MHz or
   40 MHz (or less) here, because it comes from the 80 MHz APB clock. A build flag changes the value (env daq_nandread40, daq_profile40). */
#ifndef NAND_SPI_HZ
#define NAND_SPI_HZ    80000000U
#endif


/* Mode and address pins: input with pull-up. A switch to ground or a floating pin sets the level. */
#define MODE1_PIN      1       /* high = auto record at power-up, low = manual. The tactile button also pulls it low. */
#define MODE2_PIN      2       /* high = serial, low = files (USB mode, not used yet) */
#define ADDR0_PIN      9
#define ADDR1_PIN      48
#define ADDR2_PIN      38

/* Daisy chain (UART1 and UART2, 8N1). Needed line rate = 2 (safety) x 10 bit/byte x bytes/s of the busiest
   link. A board sends up to 883 samples/s of the ADS1115 and 330 of the load cell (8.56 byte per sample with the
   packet overhead, 10.4 KB/s). The link next to an end board carries all other boards in one direction:
                             boards N | ADS only | ADS + load cell
                             2        | 151 k    | 208 k  -> 460800 baud
                             3        | 302 k    | 415 k  -> 460800 baud
                             4        | 453 k    | 623 k  -> 921600 baud
                             5        | 605 k    | 831 k  -> 921600 baud
   115200 baud carries only 11.5 KB/s: not enough for 2 x margin with one other board. */
#define CHAIN_BAUD     460800

/* Second I2C bus: the load cell ADC (NAU7802, Adafruit breakout), address 0x2A. SDA, SCL, and DRDY go through the
   5 V / 3.3 V level shifter. DRDY is a push-pull output at the DVDD level, so it must not reach the GPIO with no shifter.
   The breakout VIN (DVDD) is the analog 5 V. AVDD comes from the LDO of the chip, so the AV pin stays not connected.
   The toggle test (env daq_toggle) drives the RDY pin: remove the DRDY wire before you use it. */
#define I2C1_SDA_PIN   15
#define I2C1_SCL_PIN   7
#define I2C1_RDY_PIN   16      /* NAU7802 DRDY, rising edge = new sample */

/* Micro SD card, SDMMC slot 1, 1-bit mode. */
#define SDMMC_CLK_PIN   21
#define SDMMC_CMD_PIN   47
#define SDMMC_DAT0_PIN  14

/* LED shift register (from led_sr_driver.cpp). Output QA..QH = LED index 0..7. */
#define SR_LATCH_PIN   5
#define SR_CLOCK_PIN   4
#define SR_DATA_PIN    6

enum { LED_WORKING = 1, LED_RECORDING, LED_AUTOREC, LED_WIFI, LED_NAND, LED_SD, LED_ERROR };

#endif
