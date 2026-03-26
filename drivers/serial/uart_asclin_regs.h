#ifndef UART_ASCLIN_REGS_H
#define UART_ASCLIN_REGS_H

#if defined(CONFIG_SOC_SERIES_TC4X)
#define ASCLIN_RXDATA_READ(base)        ((base)->RXDATA[0].U)
#define ASCLIN_TXDATA_WRITE(base, val)  ((base)->TXDATA[0].U = (val))
#else
#define ASCLIN_RXDATA_READ(base)        ((base)->RXDATA.U)
#define ASCLIN_TXDATA_WRITE(base, val)  ((base)->TXDATA.U = (val))
#endif

#endif
