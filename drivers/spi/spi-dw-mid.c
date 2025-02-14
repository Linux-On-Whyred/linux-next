// SPDX-License-Identifier: GPL-2.0-only
/*
 * Special handling for DW core on Intel MID platform
 *
 * Copyright (c) 2009, 2014 Intel Corporation.
 */

#include <linux/spi/spi.h>
#include <linux/types.h>

#include "spi-dw.h"

#ifdef CONFIG_SPI_DW_MID_DMA
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/irqreturn.h>
#include <linux/pci.h>
#include <linux/platform_data/dma-dw.h>

#define RX_BUSY		0
#define TX_BUSY		1

static bool mid_spi_dma_chan_filter(struct dma_chan *chan, void *param)
{
	struct dw_dma_slave *s = param;

	if (s->dma_dev != chan->device->dev)
		return false;

	chan->private = s;
	return true;
}

static int mid_spi_dma_init_mfld(struct device *dev, struct dw_spi *dws)
{
	struct dw_dma_slave slave = {
		.src_id = 0,
		.dst_id = 0
	};
	struct pci_dev *dma_dev;
	dma_cap_mask_t mask;

	/*
	 * Get pci device for DMA controller, currently it could only
	 * be the DMA controller of Medfield
	 */
	dma_dev = pci_get_device(PCI_VENDOR_ID_INTEL, 0x0827, NULL);
	if (!dma_dev)
		return -ENODEV;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	/* 1. Init rx channel */
	slave.dma_dev = &dma_dev->dev;
	dws->rxchan = dma_request_channel(mask, mid_spi_dma_chan_filter, &slave);
	if (!dws->rxchan)
		goto err_exit;

	/* 2. Init tx channel */
	slave.dst_id = 1;
	dws->txchan = dma_request_channel(mask, mid_spi_dma_chan_filter, &slave);
	if (!dws->txchan)
		goto free_rxchan;

	dws->master->dma_rx = dws->rxchan;
	dws->master->dma_tx = dws->txchan;

	return 0;

free_rxchan:
	dma_release_channel(dws->rxchan);
	dws->rxchan = NULL;
err_exit:
	return -EBUSY;
}

static int mid_spi_dma_init_generic(struct device *dev, struct dw_spi *dws)
{
	dws->rxchan = dma_request_slave_channel(dev, "rx");
	if (!dws->rxchan)
		return -ENODEV;

	dws->txchan = dma_request_slave_channel(dev, "tx");
	if (!dws->txchan) {
		dma_release_channel(dws->rxchan);
		dws->rxchan = NULL;
		return -ENODEV;
	}

	dws->master->dma_rx = dws->rxchan;
	dws->master->dma_tx = dws->txchan;

	return 0;
}

static void mid_spi_dma_exit(struct dw_spi *dws)
{
	if (dws->txchan) {
		dmaengine_terminate_sync(dws->txchan);
		dma_release_channel(dws->txchan);
	}

	if (dws->rxchan) {
		dmaengine_terminate_sync(dws->rxchan);
		dma_release_channel(dws->rxchan);
	}

	dw_writel(dws, DW_SPI_DMACR, 0);
}

static irqreturn_t dma_transfer(struct dw_spi *dws)
{
	u16 irq_status = dw_readl(dws, DW_SPI_ISR);

	if (!irq_status)
		return IRQ_NONE;

	dw_readl(dws, DW_SPI_ICR);
	spi_reset_chip(dws);

	dev_err(&dws->master->dev, "%s: FIFO overrun/underrun\n", __func__);
	dws->master->cur_msg->status = -EIO;
	spi_finalize_current_transfer(dws->master);
	return IRQ_HANDLED;
}

static bool mid_spi_can_dma(struct spi_controller *master,
		struct spi_device *spi, struct spi_transfer *xfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);

	return xfer->len > dws->fifo_len;
}

static enum dma_slave_buswidth convert_dma_width(u8 n_bytes) {
	if (n_bytes == 1)
		return DMA_SLAVE_BUSWIDTH_1_BYTE;
	else if (n_bytes == 2)
		return DMA_SLAVE_BUSWIDTH_2_BYTES;

	return DMA_SLAVE_BUSWIDTH_UNDEFINED;
}

/*
 * dws->dma_chan_busy is set before the dma transfer starts, callback for tx
 * channel will clear a corresponding bit.
 */
static void dw_spi_dma_tx_done(void *arg)
{
	struct dw_spi *dws = arg;

	clear_bit(TX_BUSY, &dws->dma_chan_busy);
	if (test_bit(RX_BUSY, &dws->dma_chan_busy))
		return;

	dw_writel(dws, DW_SPI_DMACR, 0);
	spi_finalize_current_transfer(dws->master);
}

static struct dma_async_tx_descriptor *dw_spi_dma_prepare_tx(struct dw_spi *dws,
		struct spi_transfer *xfer)
{
	struct dma_slave_config txconf;
	struct dma_async_tx_descriptor *txdesc;

	if (!xfer->tx_buf)
		return NULL;

	memset(&txconf, 0, sizeof(txconf));
	txconf.direction = DMA_MEM_TO_DEV;
	txconf.dst_addr = dws->dma_addr;
	txconf.dst_maxburst = 16;
	txconf.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	txconf.dst_addr_width = convert_dma_width(dws->n_bytes);
	txconf.device_fc = false;

	dmaengine_slave_config(dws->txchan, &txconf);

	txdesc = dmaengine_prep_slave_sg(dws->txchan,
				xfer->tx_sg.sgl,
				xfer->tx_sg.nents,
				DMA_MEM_TO_DEV,
				DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!txdesc)
		return NULL;

	txdesc->callback = dw_spi_dma_tx_done;
	txdesc->callback_param = dws;

	return txdesc;
}

/*
 * dws->dma_chan_busy is set before the dma transfer starts, callback for rx
 * channel will clear a corresponding bit.
 */
static void dw_spi_dma_rx_done(void *arg)
{
	struct dw_spi *dws = arg;

	clear_bit(RX_BUSY, &dws->dma_chan_busy);
	if (test_bit(TX_BUSY, &dws->dma_chan_busy))
		return;

	dw_writel(dws, DW_SPI_DMACR, 0);
	spi_finalize_current_transfer(dws->master);
}

static struct dma_async_tx_descriptor *dw_spi_dma_prepare_rx(struct dw_spi *dws,
		struct spi_transfer *xfer)
{
	struct dma_slave_config rxconf;
	struct dma_async_tx_descriptor *rxdesc;

	if (!xfer->rx_buf)
		return NULL;

	memset(&rxconf, 0, sizeof(rxconf));
	rxconf.direction = DMA_DEV_TO_MEM;
	rxconf.src_addr = dws->dma_addr;
	rxconf.src_maxburst = 16;
	rxconf.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	rxconf.src_addr_width = convert_dma_width(dws->n_bytes);
	rxconf.device_fc = false;

	dmaengine_slave_config(dws->rxchan, &rxconf);

	rxdesc = dmaengine_prep_slave_sg(dws->rxchan,
				xfer->rx_sg.sgl,
				xfer->rx_sg.nents,
				DMA_DEV_TO_MEM,
				DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!rxdesc)
		return NULL;

	rxdesc->callback = dw_spi_dma_rx_done;
	rxdesc->callback_param = dws;

	return rxdesc;
}

static int mid_spi_dma_setup(struct dw_spi *dws, struct spi_transfer *xfer)
{
	u16 imr = 0, dma_ctrl = 0;

	dw_writel(dws, DW_SPI_DMARDLR, 0xf);
	dw_writel(dws, DW_SPI_DMATDLR, 0x10);

	if (xfer->tx_buf) {
		dma_ctrl |= SPI_DMA_TDMAE;
		imr |= SPI_INT_TXOI;
	}
	if (xfer->rx_buf) {
		dma_ctrl |= SPI_DMA_RDMAE;
		imr |= SPI_INT_RXUI | SPI_INT_RXOI;
	}
	dw_writel(dws, DW_SPI_DMACR, dma_ctrl);

	/* Set the interrupt mask */
	spi_umask_intr(dws, imr);

	dws->transfer_handler = dma_transfer;

	return 0;
}

static int mid_spi_dma_transfer(struct dw_spi *dws, struct spi_transfer *xfer)
{
	struct dma_async_tx_descriptor *txdesc, *rxdesc;

	/* Prepare the TX dma transfer */
	txdesc = dw_spi_dma_prepare_tx(dws, xfer);

	/* Prepare the RX dma transfer */
	rxdesc = dw_spi_dma_prepare_rx(dws, xfer);

	/* rx must be started before tx due to spi instinct */
	if (rxdesc) {
		set_bit(RX_BUSY, &dws->dma_chan_busy);
		dmaengine_submit(rxdesc);
		dma_async_issue_pending(dws->rxchan);
	}

	if (txdesc) {
		set_bit(TX_BUSY, &dws->dma_chan_busy);
		dmaengine_submit(txdesc);
		dma_async_issue_pending(dws->txchan);
	}

	return 0;
}

static void mid_spi_dma_stop(struct dw_spi *dws)
{
	if (test_bit(TX_BUSY, &dws->dma_chan_busy)) {
		dmaengine_terminate_sync(dws->txchan);
		clear_bit(TX_BUSY, &dws->dma_chan_busy);
	}
	if (test_bit(RX_BUSY, &dws->dma_chan_busy)) {
		dmaengine_terminate_sync(dws->rxchan);
		clear_bit(RX_BUSY, &dws->dma_chan_busy);
	}

	dw_writel(dws, DW_SPI_DMACR, 0);
}

static const struct dw_spi_dma_ops mfld_dma_ops = {
	.dma_init	= mid_spi_dma_init_mfld,
	.dma_exit	= mid_spi_dma_exit,
	.dma_setup	= mid_spi_dma_setup,
	.can_dma	= mid_spi_can_dma,
	.dma_transfer	= mid_spi_dma_transfer,
	.dma_stop	= mid_spi_dma_stop,
};

static void dw_spi_mid_setup_dma_mfld(struct dw_spi *dws)
{
	dws->dma_ops = &mfld_dma_ops;
}

static const struct dw_spi_dma_ops generic_dma_ops = {
	.dma_init	= mid_spi_dma_init_generic,
	.dma_exit	= mid_spi_dma_exit,
	.dma_setup	= mid_spi_dma_setup,
	.can_dma	= mid_spi_can_dma,
	.dma_transfer	= mid_spi_dma_transfer,
	.dma_stop	= mid_spi_dma_stop,
};

static void dw_spi_mid_setup_dma_generic(struct dw_spi *dws)
{
	dws->dma_ops = &generic_dma_ops;
}
#else	/* CONFIG_SPI_DW_MID_DMA */
static inline void dw_spi_mid_setup_dma_mfld(struct dw_spi *dws) {}
static inline void dw_spi_mid_setup_dma_generic(struct dw_spi *dws) {}
#endif

/* Some specific info for SPI0 controller on Intel MID */

/* HW info for MRST Clk Control Unit, 32b reg per controller */
#define MRST_SPI_CLK_BASE	100000000	/* 100m */
#define MRST_CLK_SPI_REG	0xff11d86c
#define CLK_SPI_BDIV_OFFSET	0
#define CLK_SPI_BDIV_MASK	0x00000007
#define CLK_SPI_CDIV_OFFSET	9
#define CLK_SPI_CDIV_MASK	0x00000e00
#define CLK_SPI_DISABLE_OFFSET	8

int dw_spi_mid_init_mfld(struct dw_spi *dws)
{
	void __iomem *clk_reg;
	u32 clk_cdiv;

	clk_reg = ioremap(MRST_CLK_SPI_REG, 16);
	if (!clk_reg)
		return -ENOMEM;

	/* Get SPI controller operating freq info */
	clk_cdiv = readl(clk_reg + dws->bus_num * sizeof(u32));
	clk_cdiv &= CLK_SPI_CDIV_MASK;
	clk_cdiv >>= CLK_SPI_CDIV_OFFSET;
	dws->max_freq = MRST_SPI_CLK_BASE / (clk_cdiv + 1);

	iounmap(clk_reg);

	/* Register hook to configure CTRLR0 */
	dws->update_cr0 = dw_spi_update_cr0;

	dw_spi_mid_setup_dma_mfld(dws);
	return 0;
}

int dw_spi_mid_init_generic(struct dw_spi *dws)
{
	/* Register hook to configure CTRLR0 */
	dws->update_cr0 = dw_spi_update_cr0;

	dw_spi_mid_setup_dma_generic(dws);
	return 0;
}
