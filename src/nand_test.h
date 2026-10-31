/* Read-only test of the NAND SPI clock (env daq_nandread, daq_nandread80). Not part of the real firmware. */
#ifndef NAND_TEST_H
#define NAND_TEST_H

#ifdef DAQ_NANDREAD
void nand_read_test(void);       /* never returns */
#endif

#endif
