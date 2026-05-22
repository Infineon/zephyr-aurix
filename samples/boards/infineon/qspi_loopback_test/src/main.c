#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>

#define SPI_NODE DT_NODELABEL(qspi4)

static const struct device *spi_dev = DEVICE_DT_GET(SPI_NODE);

int main(void)
{
    if (!device_is_ready(spi_dev)) {
        printk("SPI device not ready\n");
        return 0;
    }

    /* Data to send */
    uint8_t tx_data[] = "Hello QSPI ";

    struct spi_buf tx_buf = {
        .buf = tx_data,
        .len = sizeof(tx_data),
    };

    struct spi_buf_set tx = {
        .buffers = &tx_buf,
        .count = 1,
    };

    int dcount = 0;

    /* Dummy RX buffer (optional but safe) */
    uint8_t rx_data[sizeof(tx_data) / sizeof(uint8_t)] = {0};

    int data_len = sizeof(tx_data) / sizeof(uint8_t);
    struct spi_buf rx_buf = {
        .buf = rx_data,
        .len = sizeof(rx_data),
    };

    struct spi_buf_set rx = {
        .buffers = &rx_buf,
        .count = 1,
    };

    struct spi_config spi_cfg = {
        .frequency = 1000000,   // 1 MHz (adjust)
        .operation = SPI_OP_MODE_MASTER |
                     SPI_WORD_SET(8) |
                     SPI_MODE_LOOP,
        .slave = 0,
        .cs = NULL,
    };

    int ret = spi_transceive(spi_dev, &spi_cfg, &tx, &rx);

    for(int i = 0 ; i < data_len ; i++)
    {
        if(rx_data[i] == tx_data[i])
        {
            dcount++;
        }
        else
        {
            dcount = dcount;
        }
    }
    if (ret == 0) {
        printk("SPI transfer done\n");
    } else {
        printk("SPI error: %d\n", ret);
    }

    for(int j = 0 ; j < data_len ; j++)
    {
        printk("%c\n",rx_data[j]);
    }

    return 0;
}