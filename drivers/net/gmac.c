/*
 * Network device driver for the GMAC ethernet controller on
 * Apple G4 Powermacs.
 *
 * Copyright (C) 2000 Paul Mackerras & Ben. Herrenschmidt
 * 
 * portions based on sunhme.c by David S. Miller
 *
 */

#include <linux/module.h>

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/pci.h>
#include <asm/prom.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/bitops.h>

#include "gmac.h"

#define DEBUG_PHY

/* Driver version 1.1, kernel 2.2.x */
#define GMAC_VERSION	"v1.1k2"

static unsigned char dummy_buf[RX_BUF_ALLOC_SIZE + RX_OFFSET + GMAC_BUFFER_ALIGN];
static struct device *gmacs = NULL;

/* Prototypes */
static int mii_read(struct gmac *gm, int phy, int r);
static int mii_write(struct gmac *gm, int phy, int r, int v);
static void mii_poll_start(struct gmac *gm);
static void mii_poll_stop(struct gmac *gm);
static void mii_interrupt(struct gmac *gm);
static int mii_lookup_and_reset(struct gmac *gm);
static void mii_setup_phy(struct gmac *gm);

static void gmac_set_power(struct gmac *gm, int power_up);
static int gmac_powerup_and_reset(struct device *dev);
static void gmac_set_duplex_mode(struct gmac *gm, int full_duplex);
static void gmac_mac_init(struct gmac *gm, unsigned char *mac_addr);
static void gmac_init_rings(struct gmac *gm, int from_irq);
static void gmac_start_dma(struct gmac *gm);
static void gmac_stop_dma(struct gmac *gm);
static void gmac_set_multicast(struct device *dev);
static int gmac_open(struct device *dev);
static int gmac_close(struct device *dev);
static void gmac_tx_timeout(struct device *dev);
static int gmac_xmit_start(struct sk_buff *skb, struct device *dev);
static void gmac_tx_cleanup(struct device *dev, int force_cleanup);
static void gmac_receive(struct device *dev);
static void gmac_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static struct net_device_stats *gmac_stats(struct device *dev);
int gmac_probe(struct device *dev);

extern int pci_device_loc(struct device_node *dev, unsigned char *bus_ptr,
		   unsigned char *devfn_ptr);

/* Stuff for talking to the physical-layer chip */
static int
mii_read(struct gmac *gm, int phy, int r)
{
	int timeout;

	GM_OUT(GM_MIF_FRAME_CTL_DATA,
		(0x01 << GM_MIF_FRAME_START_SHIFT) |
		(0x02 << GM_MIF_FRAME_OPCODE_SHIFT) |
		GM_MIF_FRAME_TURNAROUND_HI |
		(phy << GM_MIF_FRAME_PHY_ADDR_SHIFT) |
		(r << GM_MIF_FRAME_REG_ADDR_SHIFT));
		
	for (timeout = 1000; timeout > 0; --timeout) {
		udelay(20);
		if (GM_IN(GM_MIF_FRAME_CTL_DATA) & GM_MIF_FRAME_TURNAROUND_LO)
			return GM_IN(GM_MIF_FRAME_CTL_DATA) & GM_MIF_FRAME_DATA_MASK;
	}
	return -1;
}

static int
mii_write(struct gmac *gm, int phy, int r, int v)
{
	int timeout;

	GM_OUT(GM_MIF_FRAME_CTL_DATA,
		(0x01 << GM_MIF_FRAME_START_SHIFT) |
		(0x01 << GM_MIF_FRAME_OPCODE_SHIFT) |
		GM_MIF_FRAME_TURNAROUND_HI |
		(phy << GM_MIF_FRAME_PHY_ADDR_SHIFT) |
		(r << GM_MIF_FRAME_REG_ADDR_SHIFT) |
		(v & GM_MIF_FRAME_DATA_MASK));

	for (timeout = 1000; timeout > 0; --timeout) {
		udelay(20);
		if (GM_IN(GM_MIF_FRAME_CTL_DATA) & GM_MIF_FRAME_TURNAROUND_LO)
			return 0;
	}
	return -1;
}

static void 
mii_poll_start(struct gmac *gm)
{
	unsigned int tmp;
	
	/* Start the MIF polling on the external transceiver. */
	tmp = GM_IN(GM_MIF_CFG);
	tmp &= ~(GM_MIF_CFGPR_MASK | GM_MIF_CFGPD_MASK);
	tmp |= ((gm->phy_addr & 0x1f) << GM_MIF_CFGPD_SHIFT);
	tmp |= (MII_SR << GM_MIF_CFGPR_SHIFT);
	tmp |= GM_MIF_CFGPE;
	GM_OUT(GM_MIF_CFG, tmp);

	/* Let the bits set. */
	udelay(GM_MIF_POLL_DELAY);

	GM_OUT(GM_MIF_IRQ_MASK, 0xffc0);
}

static void 
mii_poll_stop(struct gmac *gm)
{
	GM_OUT(GM_MIF_IRQ_MASK, 0xffff);
	GM_BIC(GM_MIF_CFG, GM_MIF_CFGPE);
	udelay(GM_MIF_POLL_DELAY);
}

static void
mii_interrupt(struct gmac *gm)
{
	int		phy_status;
	int		lpar_ability;
	
	mii_poll_stop(gm);

	/* May the status change before polling is re-enabled ? */
	mii_poll_start(gm);
	
	/* We read the Auxilliary Status Summary register */
	phy_status = mii_read(gm, gm->phy_addr, MII_SR);
	if ((phy_status ^ gm->phy_status) & (MII_SR_ASSC | MII_SR_LKS)) {
		int		full_duplex;
		int		link_100;
#ifdef DEBUG_PHY
		printk("Link state change, phy_status: 0x%04x\n", phy_status);
#endif
		gm->phy_status = phy_status;

		lpar_ability = mii_read(gm, gm->phy_addr, MII_ANLPA);
		if (lpar_ability & MII_ANLPA_PAUS)
			GM_BIS(GM_MAC_CTRL_CONFIG, GM_MAC_CTRL_CONF_SND_PAUSE_EN);
		else
			GM_BIC(GM_MAC_CTRL_CONFIG, GM_MAC_CTRL_CONF_SND_PAUSE_EN);

		/* Link ? For now we handle only the 5201 PHY */
		if ((phy_status & MII_SR_LKS) && (phy_status & MII_SR_ASSC)) {
		    if (gm->phy_type == PHY_B5201) {
		    	int aux_stat = mii_read(gm, gm->phy_addr, MII_BCM5201_AUXCTLSTATUS);
#ifdef DEBUG_PHY
			printk("    Link up ! BCM5201 aux_stat: 0x%04x\n", aux_stat);
#endif
		    	full_duplex = ((aux_stat & MII_BCM5201_AUXCTLSTATUS_DUPLEX) != 0);
		    	link_100 = ((aux_stat & MII_BCM5201_AUXCTLSTATUS_SPEED) != 0);
		    } else {
		    	full_duplex = 1;
		    	link_100 = 1;
		    }
#ifdef DEBUG_PHY
		    printk("    full_duplex: %d, speed: %s\n", full_duplex,
		    	link_100 ? "100" : "10");
#endif
		    if (full_duplex != gm->full_duplex) {
			gm->full_duplex = full_duplex;
			gmac_set_duplex_mode(gm, gm->full_duplex);
			gmac_start_dma(gm);
		    }
		} else if (!(phy_status & MII_SR_LKS)) {
#ifdef DEBUG_PHY
		    printk("    Link down !\n");
#endif
		}
	}
}

static int
mii_lookup_and_reset(struct gmac *gm)
{
	int	i, timeout;
	int	mii_status, mii_control;

	/* Find the PHY */
	gm->phy_addr = -1;
	gm->phy_type = PHY_UNKNOWN;
	
	for(i=31; i>0; --i) {
		mii_control = mii_read(gm, i, MII_CR);
		mii_status = mii_read(gm, i, MII_SR);
		if (mii_control != -1  && mii_status != -1 &&
			(mii_control != 0xffff || mii_status != 0xffff))
			break;
	}
	gm->phy_addr = i;
	if (gm->phy_addr < 0)
		return 0;

	/* Reset it */
	mii_write(gm, gm->phy_addr, MII_CR, mii_control | MII_CR_RST);
	mdelay(10);
	for (timeout = 100; timeout > 0; --timeout) {
		mii_control = mii_read(gm, gm->phy_addr, MII_CR);
		if (mii_control == -1) {
			printk(KERN_ERR "%s PHY died after reset !\n",
				gm->dev->name);
			goto fail;
		}
		if ((mii_control & MII_CR_RST) == 0)
			break;
		mdelay(10);
	}
	if (mii_control & MII_CR_RST) {
		printk(KERN_ERR "%s PHY reset timeout !\n", gm->dev->name);
		goto fail;
	}
	mii_write(gm, gm->phy_addr, MII_CR, mii_control & ~MII_CR_ISOL);

	/* Read the PHY ID */
	gm->phy_id = (mii_read(gm, gm->phy_addr, MII_ID0) << 16) |
		mii_read(gm, gm->phy_addr, MII_ID1);
#ifdef DEBUG_PHY
	printk("%s PHY ID: 0x%08x\n", gm->dev->name, gm->phy_id);
#endif
	if ((gm->phy_id & MII_BCM5400_MASK) == MII_BCM5400_ID) {
		gm->phy_type = PHY_B5400;
		printk(KERN_ERR "%s Warning ! Unsupported BCM5400 PHY !\n",
			gm->dev->name);
	} else if ((gm->phy_id & MII_BCM5201_MASK) == MII_BCM5201_ID) {
		gm->phy_type = PHY_B5201;
	} else {
		printk(KERN_ERR "%s: Warning ! Unknown PHY ID 0x%08x !\n",
			gm->dev->name, gm->phy_id);
	}

	return 1;
	
fail:
	gm->phy_addr = -1;
	return 0;
}

/* Code to setup the PHY duplex mode and speed should be
 * added here
 */
static void
mii_setup_phy(struct gmac *gm)
{
	int data;
	
	/* Stop auto-negociation */
	data = mii_read(gm, gm->phy_addr, MII_CR);
	mii_write(gm, gm->phy_addr, MII_CR, data & ~MII_CR_ASSE);

	/* Set advertisement to 10/100 and Half/Full duplex
	 * (full capabilities) */
	data = mii_read(gm, gm->phy_addr, MII_ANA);
	data |= MII_ANA_TXAM | MII_ANA_FDAM | MII_ANA_10M;
	mii_write(gm, gm->phy_addr, MII_ANA, data);
	
	/* Restart auto-negociation */
	data = mii_read(gm, gm->phy_addr, MII_CR);
	data |= MII_CR_ASSE;
	mii_write(gm, gm->phy_addr, MII_CR, data);
	data |= MII_CR_RAN;
	mii_write(gm, gm->phy_addr, MII_CR, data);
}

static void
gmac_set_power(struct gmac *gm, int power_up)
{
	if (power_up) {
		out_le32(gm->sysregs + 0x20/4,
			in_le32(gm->sysregs + 0x20/4) | 0x02000000);
		udelay(20);
		if (gm->pci_devfn != 0xff) {
			u16 cmd;
			
			/* Make sure PCI is correctly configured */
			pcibios_read_config_word(gm->pci_bus, gm->pci_devfn,
				PCI_COMMAND, &cmd);
			cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE;
	    		pcibios_write_config_word(gm->pci_bus, gm->pci_devfn,
	    			PCI_COMMAND, cmd);
	    		pcibios_write_config_byte(gm->pci_bus, gm->pci_devfn,
	    			PCI_LATENCY_TIMER, 16);
	    		pcibios_write_config_byte(gm->pci_bus, gm->pci_devfn,
	    			PCI_CACHE_LINE_SIZE, 8);
		}
	} else {
		/* FIXME: Add PHY power down */
		gm->phy_type = 0;
		out_le32(gm->sysregs + 0x20/4,
			in_le32(gm->sysregs + 0x20/4) & ~0x02000000);
		udelay(20);
	}
}

static int
gmac_powerup_and_reset(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	int timeout;
	
	/* turn on GB clock */
	gmac_set_power(gm, 1);
	/* Perform a software reset */
	GM_OUT(GM_RESET, GM_RESET_TX | GM_RESET_RX);
	for (timeout = 100; timeout > 0; --timeout) {
		mdelay(10);
		if ((GM_IN(GM_RESET) & (GM_RESET_TX | GM_RESET_RX)) == 0) {
			/* Mask out all chips interrupts */
			GM_OUT(GM_IRQ_MASK, 0xffffffff);
			return 0;
		}
	}
	printk(KERN_ERR "%s reset failed!\n", dev->name);
	gmac_set_power(gm, 0);
	return -1;
}

/* Set the MAC duplex mode. Side effect: stops Tx MAC */
static void
gmac_set_duplex_mode(struct gmac *gm, int full_duplex)
{
	/* Stop Tx MAC */
	GM_BIC(GM_MAC_TX_CONFIG, GM_MAC_TX_CONF_ENABLE);
	while(GM_IN(GM_MAC_TX_CONFIG) & GM_MAC_TX_CONF_ENABLE)
		;
	
	if (full_duplex) {
		GM_BIS(GM_MAC_TX_CONFIG, GM_MAC_TX_CONF_IGNORE_CARRIER
			| GM_MAC_TX_CONF_IGNORE_COLL);
		GM_BIC(GM_MAC_XIF_CONFIG, GM_MAC_XIF_CONF_DISABLE_ECHO);
	} else {
		GM_BIC(GM_MAC_TX_CONFIG, GM_MAC_TX_CONF_IGNORE_CARRIER
			| GM_MAC_TX_CONF_IGNORE_COLL);
		GM_BIS(GM_MAC_XIF_CONFIG, GM_MAC_XIF_CONF_DISABLE_ECHO);
	}
}

static void
gmac_mac_init(struct gmac *gm, unsigned char *mac_addr)
{
	int i, fifo_size;

	/* Set random seed to low bits of MAC address */
	GM_OUT(GM_MAC_RANDOM_SEED, mac_addr[5] | (mac_addr[4] << 8));
	
	/* Configure the data path mode to MII/GII */
	GM_OUT(GM_PCS_DATAPATH_MODE, GM_PCS_DATAPATH_MII);
	
	/* Configure XIF to MII mode. Full duplex led is set
	 * by Apple, so...
	 */
	GM_OUT(GM_MAC_XIF_CONFIG, GM_MAC_XIF_CONF_TX_MII_OUT_EN
		| GM_MAC_XIF_CONF_FULL_DPLX_LED);

	/* Mask out all MAC interrupts */
	GM_OUT(GM_MAC_TX_MASK, 0xffff);
	GM_OUT(GM_MAC_RX_MASK, 0xffff);
	GM_OUT(GM_MAC_CTRLSTAT_MASK, 0xff);
	
	/* Setup bits of MAC */
	GM_OUT(GM_MAC_SND_PAUSE, GM_MAC_SND_PAUSE_DEFAULT);
	GM_OUT(GM_MAC_CTRL_CONFIG, GM_MAC_CTRL_CONF_RCV_PAUSE_EN);
	
	/* Configure GEM DMA */
	GM_OUT(GM_GCONF, GM_GCONF_BURST_SZ |
		(31 << GM_GCONF_TXDMA_LIMIT_SHIFT) |
		(31 << GM_GCONF_RXDMA_LIMIT_SHIFT));
	GM_OUT(GM_TX_CONF,
		GM_TX_CONF_FIFO_THR_DEFAULT << GM_TX_CONF_FIFO_THR_SHIFT |
		NTX_CONF);
/* 34 byte offset for checksum computation.  This works because ip_input() will clear out
 * the skb->csum and skb->ip_summed fields and recompute the csum if IP options are
 * present in the header.  34 == (ethernet header len) + sizeof(struct iphdr)
 */
	GM_OUT(GM_RX_CONF,
		(RX_OFFSET << GM_RX_CONF_FBYTE_OFF_SHIFT) |
		(0x22 << GM_RX_CONF_CHK_START_SHIFT) |
		(GM_RX_CONF_DMA_THR_DEFAULT << GM_RX_CONF_DMA_THR_SHIFT) |
		NRX_CONF);

	/* Configure other bits of MAC */
	GM_OUT(GM_MAC_INTR_PKT_GAP0, GM_MAC_INTR_PKT_GAP0_DEFAULT);
	GM_OUT(GM_MAC_INTR_PKT_GAP1, GM_MAC_INTR_PKT_GAP1_DEFAULT);
	GM_OUT(GM_MAC_INTR_PKT_GAP2, GM_MAC_INTR_PKT_GAP2_DEFAULT);
	GM_OUT(GM_MAC_MIN_FRAME_SIZE, GM_MAC_MIN_FRAME_SIZE_DEFAULT);
	GM_OUT(GM_MAC_MAX_FRAME_SIZE, GM_MAC_MAX_FRAME_SIZE_DEFAULT);
	GM_OUT(GM_MAC_PREAMBLE_LEN, GM_MAC_PREAMBLE_LEN_DEFAULT);
	GM_OUT(GM_MAC_JAM_SIZE, GM_MAC_JAM_SIZE_DEFAULT);
	GM_OUT(GM_MAC_ATTEMPT_LIMIT, GM_MAC_ATTEMPT_LIMIT_DEFAULT);
	GM_OUT(GM_MAC_SLOT_TIME, GM_MAC_SLOT_TIME_DEFAULT);
	GM_OUT(GM_MAC_CONTROL_TYPE, GM_MAC_CONTROL_TYPE_DEFAULT);
	
	/* Setup MAC addresses, clear filters, clear hash table */
	GM_OUT(GM_MAC_ADDR_NORMAL0, (mac_addr[4] << 8) + mac_addr[5]);
	GM_OUT(GM_MAC_ADDR_NORMAL1, (mac_addr[2] << 8) + mac_addr[3]);
	GM_OUT(GM_MAC_ADDR_NORMAL2, (mac_addr[0] << 8) + mac_addr[1]);
	GM_OUT(GM_MAC_ADDR_ALT0, 0);
	GM_OUT(GM_MAC_ADDR_ALT1, 0);
	GM_OUT(GM_MAC_ADDR_ALT2, 0);
	GM_OUT(GM_MAC_ADDR_CTRL0, 0x0001);
	GM_OUT(GM_MAC_ADDR_CTRL1, 0xc200);
	GM_OUT(GM_MAC_ADDR_CTRL2, 0x0180);
	GM_OUT(GM_MAC_ADDR_FILTER0, 0);
	GM_OUT(GM_MAC_ADDR_FILTER1, 0);
	GM_OUT(GM_MAC_ADDR_FILTER2, 0);
	GM_OUT(GM_MAC_ADDR_FILTER_MASK1_2, 0);
	GM_OUT(GM_MAC_ADDR_FILTER_MASK0, 0);
	for (i = 0; i < 27; ++i)
		GM_OUT(GM_MAC_ADDR_FILTER_HASH0 + i, 0);
	
	/* Clear stat counters */
	GM_OUT(GM_MAC_COLLISION_CTR, 0);
	GM_OUT(GM_MAC_FIRST_COLLISION_CTR, 0);
	GM_OUT(GM_MAC_EXCS_COLLISION_CTR, 0);
	GM_OUT(GM_MAC_LATE_COLLISION_CTR, 0);
	GM_OUT(GM_MAC_DEFER_TIMER_COUNTER, 0);
	GM_OUT(GM_MAC_PEAK_ATTEMPTS, 0);
	GM_OUT(GM_MAC_RX_FRAME_CTR, 0);
	GM_OUT(GM_MAC_RX_LEN_ERR_CTR, 0);
	GM_OUT(GM_MAC_RX_ALIGN_ERR_CTR, 0);
	GM_OUT(GM_MAC_RX_CRC_ERR_CTR, 0);
	GM_OUT(GM_MAC_RX_CODE_VIOLATION_CTR, 0);
	
	/* default to half duplex */
	GM_OUT(GM_MAC_TX_CONFIG, 0);
	GM_OUT(GM_MAC_RX_CONFIG, 0);
	gmac_set_duplex_mode(gm, gm->full_duplex);
	
	/* Setup pause thresholds */
	fifo_size = GM_IN(GM_RX_FIFO_SIZE);
	GM_OUT(GM_RX_PTH,
		((fifo_size - ((GM_MAC_MAX_FRAME_SIZE_ALIGN + 8) * 2 / GM_RX_PTH_UNITS))
			<< GM_RX_PTH_OFF_SHIFT) |
		((fifo_size - ((GM_MAC_MAX_FRAME_SIZE_ALIGN + 8) * 3 / GM_RX_PTH_UNITS))
			<< GM_RX_PTH_ON_SHIFT));
		
	/* Setup interrupt blanking */
	if (GM_IN(GM_BIF_CFG) & GM_BIF_CFG_M66EN)
		GM_OUT(GM_RX_BLANK, (5 << GM_RX_BLANK_INTR_PACKETS_SHIFT)
			| (8 << GM_RX_BLANK_INTR_TIME_SHIFT));
	else
		GM_OUT(GM_RX_BLANK, (5 << GM_RX_BLANK_INTR_PACKETS_SHIFT)
			| (4 << GM_RX_BLANK_INTR_TIME_SHIFT));	
}

static void
gmac_init_rings(struct gmac *gm, int from_irq)
{
	int i;
	struct sk_buff *skb;
	unsigned char *data;
	struct gmac_dma_desc *ring;
	int gfp_flags = GFP_KERNEL;

	if (from_irq || in_interrupt())
		gfp_flags = GFP_ATOMIC;

	/* init rx ring */
	ring = (struct gmac_dma_desc *) gm->rxring;
	memset(ring, 0, NRX * sizeof(struct gmac_dma_desc));
	for (i = 0; i < NRX; ++i, ++ring) {
		data = dummy_buf;
		gm->rx_buff[i] = skb = gmac_alloc_skb(RX_BUF_ALLOC_SIZE, gfp_flags);
		if (skb != 0) {
			skb->dev = gm->dev;
			skb_put(skb, ETH_FRAME_LEN + RX_OFFSET);
			skb_reserve(skb, RX_OFFSET);
			data = skb->data - RX_OFFSET;
		}
		st_le32(&ring->lo_addr, virt_to_bus(data));
		st_le32(&ring->size, RX_SZ_OWN | ((RX_BUF_ALLOC_SIZE-RX_OFFSET) << RX_SZ_SHIFT));
	}

	/* init tx ring */
	ring = (struct gmac_dma_desc *) gm->txring;
	memset(ring, 0, NTX * sizeof(struct gmac_dma_desc));

	gm->next_rx = 0;
	gm->next_tx = 0;
	gm->tx_gone = 0;

	/* set pointers in chip */
	mb();
	GM_OUT(GM_RX_DESC_HI, 0);
	GM_OUT(GM_RX_DESC_LO, virt_to_bus(gm->rxring));
	GM_OUT(GM_TX_DESC_HI, 0);
	GM_OUT(GM_TX_DESC_LO, virt_to_bus(gm->txring));
}

static void
gmac_start_dma(struct gmac *gm)
{
	/* Enable Tx and Rx */
	GM_BIS(GM_TX_CONF, GM_TX_CONF_DMA_EN);
	mdelay(20);
	GM_BIS(GM_RX_CONF, GM_RX_CONF_DMA_EN);
	mdelay(20);
	GM_BIS(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_ENABLE);
	mdelay(20);
	GM_BIS(GM_MAC_TX_CONFIG, GM_MAC_TX_CONF_ENABLE);
	mdelay(20);
	/* Kick the receiver and enable interrupts */
	GM_OUT(GM_RX_KICK, NRX);
	GM_BIC(GM_IRQ_MASK, 	GM_IRQ_TX_INT_ME |
				GM_IRQ_TX_ALL |
				GM_IRQ_RX_DONE |
				GM_IRQ_RX_TAG_ERR |
				GM_IRQ_MAC_RX |
				GM_IRQ_MIF |
				GM_IRQ_BUS_ERROR);
}

static void
gmac_stop_dma(struct gmac *gm)
{
	/* disable interrupts */
	GM_OUT(GM_IRQ_MASK, 0xffffffff);
	/* Enable Tx and Rx */
	GM_BIC(GM_TX_CONF, GM_TX_CONF_DMA_EN);
	mdelay(20);
	GM_BIC(GM_RX_CONF, GM_RX_CONF_DMA_EN);
	mdelay(20);
	GM_BIC(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_ENABLE);
	mdelay(20);
	GM_BIC(GM_MAC_TX_CONFIG, GM_MAC_TX_CONF_ENABLE);
	mdelay(20);
}

#define CRC_POLY	0xedb88320
static void
gmac_set_multicast(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	struct dev_mc_list *dmi = dev->mc_list;
	int i,j,k,b;
	unsigned long crc;
	int multicast_hash = 0;
	int multicast_all = 0;
	int promisc = 0;
	
	/* Lock out others. */
	set_bit(0, (void *) &dev->tbusy);

	if (dev->flags & IFF_PROMISC)
		promisc = 1;
	else if ((dev->flags & IFF_ALLMULTI) /* || (dev->mc_count > XXX) */) {
		multicast_all = 1;
	} else {
		u16 hash_table[16];

		for(i = 0; i < 16; i++)
			hash_table[i] = 0;

	    	for (i = 0; i < dev->mc_count; i++) {
			crc = ~0;
			for (j = 0; j < 6; ++j) {
			    b = dmi->dmi_addr[j];
			    for (k = 0; k < 8; ++k) {
				if ((crc ^ b) & 1)
				    crc = (crc >> 1) ^ CRC_POLY;
				else
				    crc >>= 1;
				b >>= 1;
			    }
			}
			j = crc >> 24;	/* bit number in multicast_filter */
			hash_table[j >> 4] |= 1 << (15 - (j & 0xf));
			dmi = dmi->next;
	    	}

	    	for (i = 0; i < 16; i++)
	    		GM_OUT(GM_MAC_ADDR_FILTER_HASH0 + (i*4), hash_table[i]);
		GM_BIS(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_HASH_ENABLE);
	    	multicast_hash = 1;
	}

	if (promisc)
		GM_BIS(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_RX_ALL);
	else
		GM_BIC(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_RX_ALL);

	if (multicast_hash)
		GM_BIS(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_HASH_ENABLE);
	else
		GM_BIC(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_HASH_ENABLE);

	if (multicast_all)
		GM_BIS(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_RX_ALL_MULTI);
	else
		GM_BIC(GM_MAC_RX_CONFIG, GM_MAC_RX_CONF_RX_ALL_MULTI);
	
	/* Let us get going again. */
	dev->tbusy = 0;
}

static int
gmac_open(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;

	MOD_INC_USE_COUNT;

	/* Power up and reset chip */
	if (gmac_powerup_and_reset(dev)) {
		MOD_DEC_USE_COUNT;
		return -EIO;
	}

	/* Get our interrupt */
	if (request_irq(dev->irq, gmac_interrupt, 0, dev->name, dev)) {
		printk(KERN_ERR "%s can't get irq %d\n", dev->name, dev->irq);
		MOD_DEC_USE_COUNT;
		return -EAGAIN;
	}

	gm->full_duplex = 0;
	gm->phy_status = 0;
	
	/* Find a PHY */
	if (!mii_lookup_and_reset(gm))
		printk(KERN_WARNING "%s WARNING ! Can't find PHY\n", dev->name);

	/* Configure the PHY */
	mii_setup_phy(gm);
	
	/* Initialize the descriptor rings */
	gmac_init_rings(gm, 0);

	/* Initialize the MAC */
	gmac_mac_init(gm, dev->dev_addr);
	
	/* Initialize the multicast tables & promisc mode if any */
	gmac_set_multicast(dev);
	
	/*
	 * Check out PHY status and start auto-poll
	 * 
	 * Note: do this before enabling interrutps
	 */
	mii_interrupt(gm);

	/* Start the chip */
	gmac_start_dma(gm);

	gm->opened = 1;

	return 0;
}

static int
gmac_close(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	int i;

	gm->opened = 0;
	
	gmac_stop_dma(gm);
	
	mii_poll_stop(gm);
	
	free_irq(dev->irq, dev);

	/* Shut down chip */
	gmac_set_power(gm, 0);

	for (i = 0; i < NRX; ++i) {
		if (gm->rx_buff[i] != 0) {
			dev_kfree_skb(gm->rx_buff[i]);
			gm->rx_buff[i] = 0;
		}
	}
	for (i = 0; i < NTX; ++i) {
		if (gm->tx_buff[i] != 0) {
			dev_kfree_skb(gm->tx_buff[i]);
			gm->tx_buff[i] = 0;
		}
	}

	MOD_DEC_USE_COUNT;
	return 0;
}

/*
 * Handle a transmit timeout
 */
static void
gmac_tx_timeout(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	int i, timeout;
	unsigned long flags;

	save_flags(flags);
	cli();
	
	printk (KERN_ERR "%s: transmit timed out, resetting\n", dev->name);

	/*
	 * Do something useful here
	 * 
	 * FIXME: check if a complete re-init of the chip isn't necessary
	 */

	/* Stop chip */
	gmac_stop_dma(gm);
	/* Empty Tx ring of any remaining gremlins */
	gmac_tx_cleanup(dev, 1);
	/* Empty Rx ring of any remaining gremlins */
	for (i = 0; i < NRX; ++i) {
		if (gm->rx_buff[i] != 0) {
			dev_kfree_skb(gm->rx_buff[i]);
			gm->rx_buff[i] = 0;
		}
	}
	/* Perform a software reset */
	GM_OUT(GM_RESET, GM_RESET_TX | GM_RESET_RX);
	for (timeout = 100; timeout > 0; --timeout) {
		mdelay(10);
		if ((GM_IN(GM_RESET) & (GM_RESET_TX | GM_RESET_RX)) == 0) {
			/* Mask out all chips interrupts */
			GM_OUT(GM_IRQ_MASK, 0xffffffff);
			break;
		}
	}
	if (!timeout)
		printk(KERN_ERR "%s reset chip failed !\n", dev->name);
	/* Create fresh rings */
	gmac_init_rings(gm, 1);
	/* re-initialize the MAC */
	gmac_mac_init(gm, dev->dev_addr);	
	/* re-initialize the multicast tables & promisc mode if any */
	gmac_set_multicast(dev);
	/* Restart PHY auto-poll */
	mii_interrupt(gm);
	/* Restart chip */
	gmac_start_dma(gm);

	restore_flags(flags);
	
	dev->tbusy = 0;
}


static int
gmac_xmit_start(struct sk_buff *skb, struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	volatile struct gmac_dma_desc *dp;
	int i;

	/* Check tbusy bit and handle eventual transmitter timeout */
	if(test_and_set_bit(0, (void *) &dev->tbusy) != 0) {
		int tickssofar = jiffies - dev->trans_start;
	    
		if (tickssofar >= 40)
			gmac_tx_timeout(dev);
		return 1;
	}

	i = gm->next_tx;
	if (gm->tx_buff[i] != 0) {
		/* buffer is full, can't send this packet at the moment */
		return 1;
	}
	gm->next_tx = (i + 1) & (NTX - 1);
	gm->tx_buff[i] = skb;

	dp = &gm->txring[i];
	/* FIXME: Interrupt on all packet for now, change this to every N packet,
	 * with N to be adjusted
	 */
	dp->flags = TX_FL_INTERRUPT;
	dp->hi_addr = 0;
	st_le32(&dp->lo_addr, virt_to_bus(skb->data));
	mb();
	st_le32(&dp->size, TX_SZ_SOP | TX_SZ_EOP | skb->len);
	mb();

	dev->trans_start = jiffies;
	GM_OUT(GM_TX_KICK, gm->next_tx);

	dev->tbusy = (gm->tx_buff[gm->next_tx] != 0);

	return 0;
}

/*
 * Handle servicing of the transmit ring by deallocating used
 * Tx packets and restoring flow control when necessary
 */
static void
gmac_tx_cleanup(struct device *dev, int force_cleanup)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	volatile struct gmac_dma_desc *dp;
	struct sk_buff *skb;
	int gone, i;

	i = gm->tx_gone;
	gone = GM_IN(GM_TX_COMP);
	
	while (force_cleanup || i != gone) {
		skb = gm->tx_buff[i];
		if (skb == NULL)
			break;
		dp = &gm->txring[i];
		if (force_cleanup)
			++gm->stats.tx_errors;
		else {
			++gm->stats.tx_packets;
			gm->stats.tx_bytes += skb->len;
		}
		gm->tx_buff[i] = NULL;
		dev_kfree_skb(skb);
		if (++i >= NTX)
			i = 0;
	}
	gm->tx_gone = i;

	if (!force_cleanup && dev->tbusy &&
	    (gm->tx_buff[gm->next_tx] == 0))
		dev->tbusy = 0;
}

static void
gmac_receive(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	int i = gm->next_rx;
	volatile struct gmac_dma_desc *dp;
	struct sk_buff *skb, *new_skb;
	int len, flags, drop, last;
	unsigned char *data;
	u16 csum;

	last = -1;
	for (;;) {
		dp = &gm->rxring[i];
		if (ld_le32(&dp->size) & RX_SZ_OWN)
			break;
		len = (ld_le32(&dp->size) >> 16) & 0x7fff;
		flags = ld_le32(&dp->flags);
		skb = gm->rx_buff[i];
		drop = 0;
		new_skb = NULL;
		csum = ld_le32(&dp->size) & RX_SZ_CKSUM_MASK;
		
		/* Handle errors */
		if ((len < ETH_ZLEN)||(flags & RX_FL_CRC_ERROR)||(!skb)) {
			++gm->stats.rx_errors;
			if (len < ETH_ZLEN)
				++gm->stats.rx_length_errors;
			if (flags & RX_FL_CRC_ERROR)
				++gm->stats.rx_crc_errors;
			if (!skb) {
				++gm->stats.rx_dropped;
				skb = gmac_alloc_skb(RX_BUF_ALLOC_SIZE, GFP_ATOMIC);
				if (skb) {
					gm->rx_buff[i] = skb;
			    		skb->dev = dev;
			    		skb_put(skb, ETH_FRAME_LEN + RX_OFFSET);
			    		skb_reserve(skb, RX_OFFSET);
				}
			}
			drop = 1;
		} else {
			/* Large packet, alloc a new skb for the ring */
			if (len > RX_COPY_THRESHOLD) {
			    new_skb = gmac_alloc_skb(RX_BUF_ALLOC_SIZE, GFP_ATOMIC);
			    if(!new_skb) {
			    	printk(KERN_INFO "%s: Out of SKBs in Rx, packet dropped !\n",
			    		dev->name);
			    	drop = 1;
			    	++gm->stats.rx_dropped;
			    	goto finish;
			    }

			    gm->rx_buff[i] = new_skb;
			    new_skb->dev = dev;
			    skb_put(new_skb, ETH_FRAME_LEN + RX_OFFSET);
			    skb_reserve(new_skb, RX_OFFSET);
			    skb_trim(skb, len);
			} else {
			    /* Small packet, copy it to a new small skb */
			    struct sk_buff *copy_skb = dev_alloc_skb(len + RX_OFFSET);

			    if(!copy_skb) {
				printk(KERN_INFO "%s: Out of SKBs in Rx, packet dropped !\n",
					dev->name);
				drop = 1;
				++gm->stats.rx_dropped;
			    	goto finish;
			    }

			    copy_skb->dev = dev;
			    skb_reserve(copy_skb, RX_OFFSET);
			    skb_put(copy_skb, len);
			    memcpy(copy_skb->data, skb->data, len);

			    new_skb = skb;
			    skb = copy_skb;
			}
		}
	finish:
		/* Need to drop packet ? */
		if (drop) {
			new_skb = skb;
			skb = NULL;
		}
		
		/* Put back ring entry */
		data = new_skb ? (new_skb->data - RX_OFFSET) : dummy_buf;
		dp->hi_addr = 0;
		st_le32(&dp->lo_addr, virt_to_bus(data));
		mb();
		st_le32(&dp->size, RX_SZ_OWN | ((RX_BUF_ALLOC_SIZE-RX_OFFSET) << RX_SZ_SHIFT));
		
		/* Got Rx packet ? */
		if (skb) {
			/* Yes, baby, keep that hot ;) */
			if(!(csum ^ 0xffff))
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			else
				skb->ip_summed = CHECKSUM_NONE;
			skb->ip_summed = CHECKSUM_NONE;
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);
			gm->stats.rx_bytes += skb->len;
			++gm->stats.rx_packets;
		}
		
		last = i;
		if (++i >= NRX)
			i = 0;
	}
	gm->next_rx = i;
	if (last >= 0) {
		mb();
		GM_OUT(GM_RX_KICK, last & 0xfffffffc);
	}
}

static void
gmac_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct device *dev = (struct device *) dev_id;
	struct gmac *gm = (struct gmac *) dev->priv;
	unsigned int status;

	if (test_and_set_bit(0, (void*)&dev->interrupt)) {
		printk(KERN_ERR "%s: Duplicate entry of the interrupt handler !\n",
			   dev->name);
		dev->interrupt = 0;
		return;
	}

	status = GM_IN(GM_IRQ_STATUS);
	if (status & (GM_IRQ_BUS_ERROR | GM_IRQ_MIF))
		GM_OUT(GM_IRQ_ACK, status & (GM_IRQ_BUS_ERROR | GM_IRQ_MIF));
	
	if (status & (GM_IRQ_RX_TAG_ERR | GM_IRQ_BUS_ERROR)) {
		printk(KERN_ERR "%s: IRQ Error status: 0x%08x\n",
			dev->name, status);
	}
	
	if (status & GM_IRQ_MIF) {
		mii_interrupt(gm);
	}

	if (status & GM_IRQ_RX_DONE)
		gmac_receive(dev);

	if (status & (GM_IRQ_TX_INT_ME | GM_IRQ_TX_ALL))
		gmac_tx_cleanup(dev, 0);

	dev->interrupt = 0;
}

static struct net_device_stats *
gmac_stats(struct device *dev)
{
	struct gmac *gm = (struct gmac *) dev->priv;
	struct net_device_stats *stats = &gm->stats;

	if (gm && gm->opened) {
		stats->rx_crc_errors += GM_IN(GM_MAC_RX_CRC_ERR_CTR);
		GM_OUT(GM_MAC_RX_CRC_ERR_CTR, 0);

		stats->rx_frame_errors += GM_IN(GM_MAC_RX_ALIGN_ERR_CTR);
		GM_OUT(GM_MAC_RX_ALIGN_ERR_CTR, 0);

		stats->rx_length_errors += GM_IN(GM_MAC_RX_LEN_ERR_CTR);
		GM_OUT(GM_MAC_RX_LEN_ERR_CTR, 0);

		stats->tx_aborted_errors += GM_IN(GM_MAC_EXCS_COLLISION_CTR);

		stats->collisions +=
			(GM_IN(GM_MAC_EXCS_COLLISION_CTR) +
			 GM_IN(GM_MAC_LATE_COLLISION_CTR));
		GM_OUT(GM_MAC_EXCS_COLLISION_CTR, 0);
		GM_OUT(GM_MAC_LATE_COLLISION_CTR, 0);
	}
	
	return stats;
}

int
gmac_probe(struct device *dev)
{
	static int gmacs_found;
	static struct device_node *next_gmac;
	struct device_node *gmac;
	struct gmac *gm;
	unsigned long rx_descpage, tx_descpage;
	unsigned char *addr;
	int i;

	/*
	 * We could (and maybe should) do this using PCI scanning
	 * for vendor/device ID 0x106b/0x21.
	 */
	if (!gmacs_found) {
		next_gmac = find_compatible_devices("network", "gmac");
		gmacs_found = 1;
	}
	if ((gmac = next_gmac) == 0)
		return -ENODEV;
	next_gmac = gmac->next;

	if (gmac->n_addrs < 1 || gmac->n_intrs < 1) {
		printk(KERN_ERR "can't use GMAC %s: %d addrs and %d intrs\n",
		       gmac->full_name, gmac->n_addrs, gmac->n_intrs);
		return -ENODEV;
	}

	rx_descpage = get_free_page(GFP_KERNEL);
	if (rx_descpage == 0) {
		printk(KERN_ERR "%s can't get a page for rx descriptors\n", dev->name);
		return -EAGAIN;
	}

	tx_descpage = get_free_page(GFP_KERNEL);
	if (tx_descpage == 0) {
		printk(KERN_ERR "%s can't get a page for tx descriptors\n", dev->name);
		free_page(rx_descpage);
		return -EAGAIN;
	}

	dev = init_etherdev(0, sizeof(struct gmac));
	memset(dev->priv, 0, sizeof(struct gmac));

	gm = (struct gmac *) dev->priv;
	dev->base_addr = gmac->addrs[0].address;
	gm->regs = (volatile unsigned int *)
		ioremap(gmac->addrs[0].address, 0x10000);
	gm->sysregs = (volatile unsigned int *) ioremap(0xf8000000, 0x1000);
	dev->irq = gmac->intrs[0].line;
	gm->dev = dev;

	if (pci_device_loc(gmac, &gm->pci_bus, &gm->pci_devfn)) {
		gm->pci_bus = gm->pci_devfn = 0xff;
		printk(KERN_ERR "Can't locate GMAC PCI entry\n");
	}

	addr = get_property(gmac, "local-mac-address", NULL);
	if (addr == NULL) {
		printk(KERN_ERR "Can't get mac-address for GMAC %s\n",
		       gmac->full_name);
		return -EAGAIN;
	}

	printk(KERN_INFO "%s: GMAC at", dev->name);
	for (i = 0; i < 6; ++i) {
		dev->dev_addr[i] = addr[i];
		printk("%c%.2x", (i? ':': ' '), addr[i]);
	}
	printk(", driver " GMAC_VERSION "\n");

	gm->tx_desc_page = tx_descpage;
	gm->rx_desc_page = rx_descpage;
	gm->rxring = (volatile struct gmac_dma_desc *) rx_descpage;
	gm->txring = (volatile struct gmac_dma_desc *) tx_descpage;

	gm->phy_addr = 0;
	gm->opened = 0;
	
	dev->open = gmac_open;
	dev->stop = gmac_close;
	dev->hard_start_xmit = gmac_xmit_start;
	dev->get_stats = gmac_stats;
	dev->set_multicast_list = &gmac_set_multicast;

	ether_setup(dev);

	gmacs = dev;

	return 0;
}

#ifdef MODULE

MODULE_AUTHOR("Paul Mackerras/Ben Herrenschmidt");
MODULE_DESCRIPTION("PowerMac GMAC driver.");

int
init_module(void)
{
	int rc;
	
	if (gmacs != NULL)
		return -EBUSY;

	/* We bump use count during probe since get_free_page can sleep
	 * which can be a race condition if module is unloaded at this
	 * point.
	 */
	MOD_INC_USE_COUNT;

	rc = gmac_probe(NULL);

	MOD_DEC_USE_COUNT;

	return rc;
}

void
cleanup_module(void)
{
	struct gmac *gm;

	/* XXX should handle more than one */
	if (gmacs == NULL)
		return;

	gm = (struct gmac *) gmacs->priv;
	unregister_netdev(gmacs);
	free_page(gm->rx_desc_page);
	free_page(gm->tx_desc_page);
	kfree(gmacs);
	gmacs = NULL;
}

#endif
