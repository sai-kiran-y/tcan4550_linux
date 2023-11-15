// SPDX-License-Identifier: GPL-2.0
// CAN driver for TI TCAN4550
// Copyright (C) 2023 Carl-Magnus Moon

#include <linux/can/dev.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/netlink.h>

// TCAN4550 Registers
const static uint32_t DEVICE_ID1 = 0x0;
const static uint32_t DEVICE_ID2 = 0x4;

const static uint32_t STATUS = 0x0C;
const static uint32_t SPI_MASK = 0x10;

const static uint32_t MODES_OF_OPERATION = 0x0800;
const static uint32_t INTERRUPT_FLAGS = 0x0820;
const static uint32_t INTERRUPT_ENABLE = 0x0830;

const static uint32_t TEST = 0x1010;
const static uint32_t CCCR = 0x1018;
const static uint32_t NBTP = 0x101C;
const static uint32_t IR = 0x1050;  // interrupt register
const static uint32_t IE = 0x1054;  // interrupt enable
const static uint32_t ILE = 0x105C; // interrupt line enable
const static uint32_t RXF0C = 0x10A0;
const static uint32_t RXF0S = 0x10A4;
const static uint32_t RXF0A = 0x10A8;
const static uint32_t TXBC = 0x10C0;
const static uint32_t TXESC = 0x10C8;
const static uint32_t RXESC = 0x10BC;
const static uint32_t TXQFS = 0x10C4;
const static uint32_t TXBAR = 0x10D0;

const static uint32_t RF0N = (0x1UL << 0); // rx fifo 0 new data
const static uint32_t TC = (0x1UL << 9);   // transmission complete
const static uint32_t TFE = (0x1UL << 11); // transmit fifo empty
const static uint32_t EP = (0x1UL << 23);  // error passive
const static uint32_t EW = (0x1UL << 24);  // error warning
const static uint32_t BO = (0x1UL << 25);  // bus off

const static uint32_t INIT = 0x1; // init
const static uint32_t CCE = 0x2;  // configuration change enable
const static uint32_t CSR = 0x10; // clock stop request

const static uint32_t MODESEL_1 = 0x40;
const static uint32_t MODESEL_2 = 0x80;

// MRAM config. TODO: Move to devicetree?
const static uint32_t RX_SLOT_SIZE = 16;
const static uint32_t TX_SLOT_SIZE = 16;
const static uint32_t TX_MSG_BOXES = 32;
const static uint32_t TX_FIFO_START_ADDRESS = 0x0;
const static uint32_t RX_MSG_BOXES = 32;
const static uint32_t RX_FIFO_START_ADDRESS = 0x200;

// MRAM constants
const static uint32_t MRAM_BASE = 0x8000;
const static uint32_t MRAM_SIZE_WORDS = 512;

// Buffer configuration
#define MAX_BURST_TX_MESSAGES   8  // Max CAN messages in a SPI write
#define MAX_BURST_RX_MESSAGES   8   // Max CAN messages in a SPI read

#define TX_BUFFER_SIZE 16+1 // size of tx-buffer used between Linux networking stack and SPI. One slot is reserved to be able to keep track of if queue is full

#define ECHO_BUFFERS    1

static unsigned long flags;
static DEFINE_SPINLOCK(tx_skb_lock); // spinlock protecting tx_skb buffer
static DEFINE_MUTEX(spi_lock); // mutex protecting SPI access

const static uint32_t TCAN_ID = 0x4E414354;
const static uint32_t TCAN_ID2 = 0x30353534;

const static struct can_bittiming_const tcan4550_bittiming_const = {
    .name = KBUILD_MODNAME,
    .tseg1_min = 1,
    .tseg1_max = 255,
    .tseg2_min = 1,
    .tseg2_max = 127,
    .sjw_max = 127,
    .brp_min = 1,
    .brp_max = 511,
    .brp_inc = 1,
};

struct tcan4550_priv
{
    struct can_priv can; // must be located first in private struct
    struct device *dev;
    struct net_device *ndev;
    struct spi_device *spi;

    struct workqueue_struct *wq;
    struct work_struct tx_work;

    struct sk_buff *tx_skb_buf[TX_BUFFER_SIZE];
    int tx_head;
    int tx_tail;
    struct gpio_desc *reset_gpio;
    uint32_t rxBuffer[MAX_BURST_RX_MESSAGES*4];
    uint32_t txBuffer[MAX_BURST_TX_MESSAGES*4];
    
    unsigned char rx_txBuf[4+(MAX_BURST_RX_MESSAGES*16)];
    unsigned char rx_rxBuf[4+(MAX_BURST_RX_MESSAGES*16)];

    unsigned char tx_txBuf[4+(MAX_BURST_TX_MESSAGES*16)];
    unsigned char tx_rxBuf[4+(MAX_BURST_TX_MESSAGES*16)];
};

// TCAN function headers
static void tcan4550_set_normal_mode(struct spi_device *spi);
static void tcan4550_set_standby_mode(struct spi_device *spi);
static void tcan4550_configure_mram(struct spi_device *spi);
static void tcan4550_unlock(struct spi_device *spi);
static bool tcan4550_readIdentification(struct spi_device *spi);
static void tcan4550_setBitRate(struct spi_device *spi, uint32_t bitRateReg);
static void tcan4550_setupInterrupts(struct spi_device *spi);
static void tcan4550_hwReset(struct net_device *dev);
static void tcan4550_setupIo(struct net_device *dev);
static void tcan4550_composeMessage(struct sk_buff *skb, uint32_t *buffer);
static int tcan4550_set_mode(struct net_device *net, enum can_mode mode);
static void tcan4550_configureControlModes(struct net_device *dev);

static irqreturn_t tcan4450_handleInterrupts(int irq, void *dev);

static void tcan4550_tx_work_handler(struct work_struct *ws);
static bool tcan4550_recMsgs(struct net_device *dev);

// SPI function headers
static int spi_transfer(struct spi_device *spi, int lenBytes, unsigned char *rxBuf, unsigned char *txBuf);
static uint32_t spi_read32(struct spi_device *spi, uint32_t address);
static int spi_write32(struct spi_device *spi, uint32_t address, uint32_t data);
static int spi_write_msgs(struct tcan4550_priv *priv, uint32_t address, int32_t msgs, uint32_t *data);
static int spi_read_msgs(struct tcan4550_priv *priv, uint32_t address, int32_t msgs, uint32_t *data);

/*------------------------------------------------------------*/
/* SPI helper functions                                       */
/*------------------------------------------------------------*/
static int spi_transfer(struct spi_device *spi, int lenBytes, unsigned char *rxBuf, unsigned char *txBuf)
{
    struct spi_transfer t = {
        .tx_buf = txBuf,
        .rx_buf = rxBuf,
        .len = lenBytes,
        .cs_change = 0,
    };
    struct spi_message m;
    int ret;

    if (spi == 0 || rxBuf == 0 || txBuf == 0)
    {
        return -EINVAL;
    }

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);

    mutex_lock(&spi_lock);

    ret = spi_sync(spi, &m);
    if (ret)
    {
        dev_err(&spi->dev, "spi transfer failed: ret = %d\n", ret);
    }

    mutex_unlock(&spi_lock);

    return ret;
}

static uint32_t spi_read32(struct spi_device *spi, uint32_t address)
{
    unsigned char txBuf[8];
    unsigned char rxBuf[8];

    txBuf[0] = 0x41;
    txBuf[1] = address >> 8;
    txBuf[2] = address & 0xFF;
    txBuf[3] = 1;

    spi_transfer(spi, 8, rxBuf, txBuf);

    return (rxBuf[4] << 24) + (rxBuf[5] << 16) + (rxBuf[6] << 8) + rxBuf[7];
}

static int spi_read_msgs(struct tcan4550_priv *priv, uint32_t address, int32_t msgs, uint32_t *data)
{
    int ret;
    uint32_t i;

    if(msgs > MAX_BURST_RX_MESSAGES)
    {
        return -EINVAL;
    }

    priv->rx_txBuf[0] = 0x41;
    priv->rx_txBuf[1] = address >> 8;
    priv->rx_txBuf[2] = address & 0xFF;
    priv->rx_txBuf[3] = msgs*4;

    ret = spi_transfer(priv->spi, 4 + (msgs * 16), priv->rx_rxBuf, priv->rx_txBuf);

    for(i = 0; i < (msgs*4); i++)
    {
        data[0 + (i*4)] = priv->rx_rxBuf[7+(i*16)] + (priv->rx_rxBuf[6 + (i*16)] << 8) + (priv->rx_rxBuf[5 + (i*16)] << 16) + (priv->rx_rxBuf[4 + (i*16)] << 24);
        data[1 + (i*4)] = priv->rx_rxBuf[11 + (i*16)] + (priv->rx_rxBuf[10 + (i*16)] << 8) + (priv->rx_rxBuf[9 + (i*16)] << 16) + (priv->rx_rxBuf[8 + (i*16)] << 24);
        data[2 + (i*4)] = priv->rx_rxBuf[15 + (i*16)] + (priv->rx_rxBuf[14 + (i*16)] << 8) + (priv->rx_rxBuf[13 + (i*16)] << 16) + (priv->rx_rxBuf[12 + (i*16)] << 24);
        data[3 + (i*4)] = priv->rx_rxBuf[19 + (i*16)] + (priv->rx_rxBuf[18 + (i*16)] << 8) + (priv->rx_rxBuf[17 + (i*16)] << 16) + (priv->rx_rxBuf[16 + (i*16)] << 24);
    }

    return ret;
}

static int spi_write32(struct spi_device *spi, uint32_t address, uint32_t data)
{
    unsigned char txBuf[8];
    unsigned char rxBuf[8];

    int ret;

    txBuf[0] = 0x61;
    txBuf[1] = address >> 8;
    txBuf[2] = address & 0xFF;
    txBuf[3] = 1;
    txBuf[4] = (data >> 24) & 0xFF;
    txBuf[5] = (data >> 16) & 0xFF;
    txBuf[6] = (data >> 8) & 0xFF;
    txBuf[7] = data & 0xFF;

    ret = spi_transfer(spi, 8, rxBuf, txBuf);

    return ret;
}

static int spi_write_msgs(struct tcan4550_priv *priv, uint32_t address, int32_t msgs, uint32_t *data)
{
    uint32_t i;
    int ret;

    if(msgs > MAX_BURST_TX_MESSAGES)
    {
       return -EINVAL;
    }

    priv->tx_txBuf[0] = 0x61;
    priv->tx_txBuf[1] = address >> 8;
    priv->tx_txBuf[2] = address & 0xFF;
    priv->tx_txBuf[3] = msgs*4;

    for(i = 0; i < (msgs * 4); i++)
    {
        priv->tx_txBuf[4 + (i*4)] = ((data[i] >> 24) & 0xFF);
        priv->tx_txBuf[5 + (i*4)] = ((data[i] >> 16) & 0xFF);
        priv->tx_txBuf[6 + (i*4)] = ((data[i] >> 8) & 0xFF);
        priv->tx_txBuf[7 + (i*4)] = (data[i] & 0xFF);
    }

    ret = spi_transfer(priv->spi, 4+(msgs*16), priv->tx_rxBuf, priv->tx_txBuf);

    return ret;
}

/*------------------------------------------------------------*/
/* TCAN4550 functions                                         */
/*------------------------------------------------------------*/
static void tcan4550_set_standby_mode(struct spi_device *spi)
{
    uint32_t val;

    val = spi_read32(spi, MODES_OF_OPERATION);

    val |= MODESEL_1;
    val &= ~((uint32_t)MODESEL_2);

    spi_write32(spi, MODES_OF_OPERATION, val);
}

static void tcan4550_set_normal_mode(struct spi_device *spi)
{
    uint32_t val;

    val = spi_read32(spi, MODES_OF_OPERATION);

    val |= MODESEL_2;
    val &= ~((uint32_t)MODESEL_1);

    spi_write32(spi, MODES_OF_OPERATION, val);
}

static bool tcan4550_readIdentification(struct spi_device *spi)
{
    uint32_t id1;
    uint32_t id2;

    id1 = spi_read32(spi, DEVICE_ID1);
    id2 = spi_read32(spi, DEVICE_ID2);

    // TCAN4550 in ascii
    if ((id1 == TCAN_ID) && (id2 == TCAN_ID2))
    {
        return true;
    }

    return false;
}

static void tcan4550_setBitRate(struct spi_device *spi, uint32_t bitRateReg)
{
    spi_write32(spi, NBTP, bitRateReg);
}

static void tcan4550_configure_mram(struct spi_device *spi)
{
    uint32_t i;

    // clear MRAM to avoid risk of ECC errors 2kB = 512 words
    for (i = 0; i < MRAM_SIZE_WORDS; i++)
    {
        spi_write32(spi, MRAM_BASE + (i * 4), 0);
    }

    // configure tx-fifo
    spi_write32(spi, TXBC, TX_FIFO_START_ADDRESS + (TX_MSG_BOXES << 24));

    // configure rx-fifo
    spi_write32(spi, RXF0C, RX_FIFO_START_ADDRESS + (RX_MSG_BOXES << 16));

    // 8-byte transmit size
    spi_write32(spi, TXESC, 0);

    // 8-byte receive size
    spi_write32(spi, RXESC, 0);
}

static void tcan4550_unlock(struct spi_device *spi)
{
    uint32_t val = spi_read32(spi, CCCR);

    val |= (CCE + INIT);     // set CCE and INIT bits
    val &= ~((uint32_t)CSR); // clear CSR

    spi_write32(spi, CCCR, val);
}

// convert a struct sk_buff to a tcan4550 msg and store in buffer
void tcan4550_composeMessage(struct sk_buff *skb, uint32_t *buffer)
{
    struct can_frame *frame;
    bool extended = false;
    bool rtr = false;
    uint32_t len;
    uint32_t id;

    frame = (struct can_frame *)skb->data;

    extended = (frame->can_id & CAN_EFF_FLAG)?true:false;
    rtr = (frame->can_id & CAN_RTR_FLAG)?true:false;

    len = frame->len;
    if (len > 8)
    {
        len = 8;
    }
    
    if(extended)
    {
        id = frame->can_id & CAN_EFF_MASK;
        buffer[0] = id;
    }
    else
    {
        id = frame->can_id & CAN_SFF_MASK; 
        buffer[0] = (id << 18);
    }

    buffer[0] += (rtr << 29) + (extended << 30);    // add extended and rtr flags
    buffer[1] = (len << 16);
    buffer[2] = frame->data[0] + (frame->data[1] << 8) + (frame->data[2] << 16) + (frame->data[3] << 24);
    buffer[3] = frame->data[4] + (frame->data[5] << 8) + (frame->data[6] << 16) + (frame->data[7] << 24);
}

// actually send the messages to the CAN controller. This function is called from a work-queue
static void tcan4550_tx_work_handler(struct work_struct *ws)
{
    struct tcan4550_priv *priv = container_of(ws, struct tcan4550_priv,
                                              tx_work);

    struct net_device_stats *stats = &(priv->ndev->stats);

    uint32_t txqfs = spi_read32(priv->spi, TXQFS);
    uint32_t freeBuffers = txqfs & 0x3F;
    uint32_t writeIndex = (txqfs >> 16) & 0x1F;
    uint32_t requestMask = 0;
    uint32_t msgs = 0;
    
    uint32_t baseAddress = MRAM_BASE + TX_FIFO_START_ADDRESS + (writeIndex * TX_SLOT_SIZE);

    uint32_t msgsToTransmit = freeBuffers;
    if(msgsToTransmit > MAX_BURST_TX_MESSAGES)
    {
        msgsToTransmit = MAX_BURST_TX_MESSAGES;
    }

    // Make sure TX buffer does not wrap around
    if((writeIndex + msgsToTransmit) > TX_MSG_BOXES)
    {
        msgsToTransmit = (TX_MSG_BOXES - writeIndex);
    }

    spin_lock_irqsave(&tx_skb_lock, flags);

    // build an SPI message consisting of several CAN msgs
    while((priv->tx_head != priv->tx_tail) && (msgs < msgsToTransmit))
    {
        struct can_frame *frame = (struct can_frame *)priv->tx_skb_buf[priv->tx_tail]->data;

        tcan4550_composeMessage(priv->tx_skb_buf[priv->tx_tail], &priv->txBuffer[msgs*4]);

        // put message on echo stack
        can_put_echo_skb(priv->tx_skb_buf[priv->tx_tail], priv->ndev, 0, frame->len);
        
        // loop back the message
        // TODO: this should preferably be done when we are sure the message is actually sent in tx interrupt
        can_get_echo_skb(priv->ndev, 0, 0);

        // as we loop back the message, we also need to increase rx stats
        // Note: The original TCAN driver and also flexcan driver does this using rx_offload, other drivers such as Kvaser does not
        stats->rx_packets++;
        stats->rx_bytes += frame->len;

        requestMask += (1 << writeIndex);    // add current message to request mask

        msgs++;
        writeIndex++;

        priv->tx_tail++;
        if(priv->tx_tail >= TX_BUFFER_SIZE)
        {
            priv->tx_tail = 0;
        }

        // update stats
        stats->tx_packets++;
        stats->tx_bytes += frame->len;
    }

    spin_unlock_irqrestore(&tx_skb_lock, flags);

    if(msgs > 0)
    {
        spi_write_msgs(priv, baseAddress, msgs, priv->txBuffer);   // write message data

        spi_write32(priv->spi, TXBAR, requestMask); // request buffer transmission
    }
}

bool tcan4550_recMsgs(struct net_device *dev)
{
    uint32_t i;
    struct net_device_stats *stats = &(dev->stats);

    struct tcan4550_priv *priv = netdev_priv(dev);

    uint32_t rxf0s = spi_read32(priv->spi, RXF0S);

    uint32_t fillLevel = (rxf0s & 0x7F);          // 0-64
    uint32_t getIndex = ((rxf0s >> 8) & 0x3F);    // 0-63

    uint32_t msgsToGet = fillLevel;
    uint32_t baseAddress = MRAM_BASE + RX_FIFO_START_ADDRESS + (getIndex * RX_SLOT_SIZE);

    if(msgsToGet == 0)
    {
        return false;
    }

    // stop if hw rx buffer wraps around, we need to request the rest in a separate SPI package
    if(msgsToGet > (RX_MSG_BOXES - getIndex))
    {
        msgsToGet = (RX_MSG_BOXES - getIndex);
    }

    // do not read too many packages at once as we also need to ack rx packages to give room for new rx and perform tx
    if(msgsToGet > MAX_BURST_RX_MESSAGES)
    {
        msgsToGet = MAX_BURST_RX_MESSAGES;
    }
    
    spi_read_msgs(priv, baseAddress, msgsToGet, priv->rxBuffer);

    spi_write32(priv->spi, RXF0A, (getIndex + msgsToGet - 1)); // acknowledge the last message we have read, that will automatically free all messages read

    for(i = 0; i < msgsToGet; i++)
    {
        struct can_frame *cf;
        struct sk_buff *skb;

        skb = alloc_can_skb(dev, &cf);

        if (skb)
        {
            uint32_t *data = (uint32_t *)&priv->rxBuffer[0+(i*4)];

            cf->len = (data[1] >> 16) & 0x7F;
        
            if(data[0] & (1 << 30)) // extended
            {
                cf->can_id = (data[0] & CAN_EFF_MASK) | CAN_EFF_FLAG;
            }
            else
            {
                cf->can_id = (data[0] >> 18) & CAN_SFF_MASK; 
            }

            cf->data[0] = data[2] & 0xFF;
            cf->data[1] = (data[2] >> 8) & 0xFF;
            cf->data[2] = (data[2] >> 16) & 0xFF;
            cf->data[3] = (data[2] >> 24) & 0xFF;
            cf->data[4] = data[3] & 0xFF;
            cf->data[5] = (data[3] >> 8) & 0xFF;
            cf->data[6] = (data[3] >> 16) & 0xFF;
            cf->data[7] = (data[3] >> 24) & 0xFF;

            netif_rx(skb);  // Send message to Linux networking stack

            stats->rx_packets++;
            stats->rx_bytes += cf->len;
        }
        else
        {
            stats->rx_dropped++;
        }
    }

    return true;
}

// interrupt handler - run as an irq thread
static irqreturn_t tcan4450_handleInterrupts(int irq, void *dev)
{
    struct tcan4550_priv *priv = netdev_priv(dev);
    uint32_t ir;

    ir = spi_read32(priv->spi, IR);
    spi_write32(priv->spi, IR, ir); // acknowledge interrupts

    if(ir == 0)
    {
        return IRQ_NONE;
    }

    // rx fifo 0 new message
    if (ir & RF0N)
    {
        tcan4550_recMsgs(dev);
    }

    // tx fifo empty
    if (ir & TFE)
    {
        netif_wake_queue(dev);
    }

    // Bus off. Basically switch off everything. Linux can device will call tcan4550_set_mode with mode=RESTART if restart-ms is set up.
    if (ir & BO)
    {
        struct sk_buff *skb;
        struct can_frame *cf;

        can_bus_off(dev);   // tell Linux networking stack that we are bus off

        priv->can.can_stats.bus_off++;
        priv->can.state = CAN_STATE_BUS_OFF;

        spi_write32(priv->spi, IE, 0);  // disable all interrupts to avoid flooding

        skb = alloc_can_err_skb((struct net_device*)dev, &cf);
        if(skb)
        {
            cf->can_id |= CAN_ERR_BUSOFF;

            netif_rx(skb);
        }
        
        netif_stop_queue(dev);  // We can not handle any pacakges so stop Linux sending us pacakges
    }

    // error warning
    if (ir & EW)
    {
        priv->can.can_stats.error_warning++;
        priv->can.state = CAN_STATE_ERROR_WARNING;
    }

    // error passive
    if (ir & EP)
    {
        priv->can.can_stats.error_passive++;
        priv->can.state = CAN_STATE_ERROR_PASSIVE;
    }

    return IRQ_HANDLED;
}

void tcan4550_setupInterrupts(struct spi_device *spi)
{
    spi_write32(spi, IE, RF0N + TFE + BO + EW + EP); // rx fifo 0 new message + tx fifo empty + bus off + warning + error passive

    spi_write32(spi, ILE, 0x1); // enable interrupt line 1

    // mask all spi errors
    spi_write32(spi, SPI_MASK, 0xFFFFFFFF);

    // clear spi status register
    spi_write32(spi, STATUS, 0xFFFFFFFF);

    // clear interrupts
    spi_write32(spi, INTERRUPT_FLAGS, 0xFFFFFFFF);

    // interrupt enables
    spi_write32(spi, INTERRUPT_ENABLE, 0);
}

void tcan4550_setupIo(struct net_device *dev)
{
    struct tcan4550_priv *priv = netdev_priv(dev);

    priv->reset_gpio = devm_gpiod_get_optional(priv->dev, "reset", GPIOD_OUT_LOW); // get reset gpio from device-tree reset-gpio property, set to output low
    if (IS_ERR(priv->reset_gpio))
    {
        dev_err(priv->dev, "could not get reset gpio\n");

        priv->reset_gpio = NULL;
    }
}

void tcan4550_hwReset(struct net_device *dev)
{
    struct tcan4550_priv *priv = netdev_priv(dev);

    gpiod_set_value(priv->reset_gpio, 1);
    usleep_range(50, 100); // toggle pin for at least 30us
    gpiod_set_value(priv->reset_gpio, 0);

    usleep_range(1500, 2000); // we need to wait at least 700us for chip to become ready
}

static void tcan4550_configureControlModes(struct net_device *dev)
{
    struct tcan4550_priv *priv = netdev_priv(dev);
    uint32_t cccr = 0;
    uint32_t test = 0;

    cccr = spi_read32(priv->spi, CCCR);
    test = spi_read32(priv->spi, TEST);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) 
    {
		cccr |= (1 << 7) | (1 << 5);
		test |= (1 << 4);
	}

	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
    {
		cccr |= (1 << 5);
    }

	if (priv->can.ctrlmode & CAN_CTRLMODE_ONE_SHOT)
    {
	    cccr |= (1 << 6);
    }	

    cccr &= ~(1 << 4);  // clock stop should never be written 1 to even if reading returns 1

    spi_write32(priv->spi, CCCR, cccr);
    spi_write32(priv->spi, TEST, test);
}

// initialize tcan4550 hardware
static bool tcan4550_init(struct net_device *dev, uint32_t bitRateReg)
{
    struct tcan4550_priv *priv = netdev_priv(dev);

    tcan4550_set_standby_mode(priv->spi);
    tcan4550_unlock(priv->spi);
    tcan4550_setBitRate(priv->spi, bitRateReg);
    tcan4550_configure_mram(priv->spi);
    tcan4550_configureControlModes(dev);
    tcan4550_setupInterrupts(priv->spi);

    // after this call, the TCAN chip is ready to send/receive messages
    tcan4550_set_normal_mode(priv->spi);

    return true;
}

/*------------------------------------------------------------*/
/* Linux CAN Driver standard functions                        */
/*------------------------------------------------------------*/

// Called if user performs ifconfig canx up
static int tcan_open(struct net_device *dev)
{
    struct tcan4550_priv *priv = netdev_priv(dev);
    const struct can_bittiming *bt = &priv->can.bittiming;
    uint32_t bitRateReg = (bt->phase_seg2 - 1) + ((bt->prop_seg + bt->phase_seg1 - 1) << 8) + ((bt->brp - 1) << 16) + ((bt->sjw - 1) << 25);
    int err;

    // open the can device
    err = open_candev(dev);
    if (err)
    {
        netdev_err(dev, "failed to open can device\n");
        return err;
    }

    if (!tcan4550_init(dev, bitRateReg))
    {
        netdev_err(dev, "failed to init TCAN module\n");

        close_candev(dev);

        return -ENXIO;
    }

    // start interrupt handler, as SPI is slow, run as threaded irq
    err = request_threaded_irq(priv->spi->irq, NULL, tcan4450_handleInterrupts, IRQF_ONESHOT, dev->name, dev);
    if (err)
    {
        netdev_err(dev, "failed to register interrupt\n");

        close_candev(dev);

        return err;
    }

    priv->tx_head = 0;
    priv->tx_tail = 0;

    priv->can.state = CAN_STATE_ERROR_ACTIVE;

    dev_info(priv->dev, "hw rx buffers %d\n", RX_MSG_BOXES);
    dev_info(priv->dev, "hw tx buffers %d\n", TX_MSG_BOXES);
    
    netif_start_queue(dev); // This will make Linux network stack start send us packages

    return 0;
}

// Called if user performs ifconfig canx down
static int tcan_close(struct net_device *dev)
{
    struct tcan4550_priv *priv = netdev_priv(dev);

    netif_stop_queue(dev);
    priv->can.state = CAN_STATE_STOPPED;

    close_candev(dev);
    free_irq(priv->spi->irq, dev);

    tcan4550_set_standby_mode(priv->spi);

    return 0;
}

// Called from Linux network stack to request sending of a CAN message
// Linux call start_xmit from soft-irq context so we are not allowed to sleep here
// We do not actually send anything here, just copy frame to our sw tx buffer. 
// If sw tx buffer is full, stop queue with netif_stop_queue.
static netdev_tx_t tcan_start_xmit(struct sk_buff *skb,
                                    struct net_device *dev)
{
    struct tcan4550_priv *priv;
    uint32_t tmpHead;

    priv = netdev_priv(dev);

    // drop invalid can msgs
    if (can_dropped_invalid_skb(dev, skb))
    {
        return NETDEV_TX_OK;
    }

    spin_lock_irqsave(&tx_skb_lock, flags);

    tmpHead = priv->tx_head;
    tmpHead++;
    if(tmpHead >= TX_BUFFER_SIZE)
    {
        tmpHead = 0;
    }
    
    if(tmpHead == priv->tx_tail)
    {
        netif_stop_queue(dev);  // queue will be started again from TFE interrupt

        spin_unlock_irqrestore(&tx_skb_lock, flags);

        queue_work(priv->wq, &priv->tx_work);

        return NETDEV_TX_BUSY;
    }

    priv->tx_skb_buf[priv->tx_head] = skb;
    priv->tx_head=tmpHead;

    spin_unlock_irqrestore(&tx_skb_lock, flags);

    queue_work(priv->wq, &priv->tx_work);

    return NETDEV_TX_OK;
}

// called automatically from linux can device if bus off is detected and restart-ms is set
// or manually by calling 'ip link set canX type can restart'
static int tcan4550_set_mode(struct net_device *dev, enum can_mode mode)
{
    struct tcan4550_priv *priv = netdev_priv(dev);
    const struct can_bittiming *bt = &priv->can.bittiming;
    uint32_t bitRateReg = (bt->phase_seg2 - 1) + ((bt->prop_seg + bt->phase_seg1 - 1) << 8) + ((bt->brp - 1) << 16) + ((bt->sjw - 1) << 25);

    switch (mode) 
    {
        case CAN_MODE_START:
        priv->tx_head = 0;
        priv->tx_tail = 0;
        priv->can.state = CAN_STATE_ERROR_ACTIVE;

        // NOTE! when this call returns we will get interrupts again so be very careful what is done after this call
        if (!tcan4550_init(dev, bitRateReg))
        {
            netdev_err(dev, "failed to init TCAN module\n");

            return -ENXIO;
        }

        netif_start_queue(dev); 
        break;
        
        default:
        return -EOPNOTSUPP;
    }

    return 0;
}

static const struct net_device_ops m_can_netdev_ops = {
    .ndo_open = tcan_open,
    .ndo_stop = tcan_close,
    .ndo_start_xmit = tcan_start_xmit,
    .ndo_change_mtu = can_change_mtu,
};

// Called by Linux if it matches our driver to a entry in device tree
// or if we manually perform call dtoverlay
static int tcan_probe(struct spi_device *spi)
{
    struct net_device *ndev;
    int err;
    struct tcan4550_priv *priv;
    struct spi_delay delay =
    {
        .unit = SPI_DELAY_UNIT_USECS,
        .value = 0
    };

    ndev = alloc_candev(sizeof(struct tcan4550_priv), ECHO_BUFFERS);
    if (!ndev)
    {
        dev_err(&spi->dev, "could not allocated candev\n");
        return -ENOMEM;
    }

    priv = netdev_priv(ndev);
    spi_set_drvdata(spi, ndev);
    SET_NETDEV_DEV(ndev, &spi->dev);

    priv->dev = &spi->dev;
    priv->ndev = ndev;
    priv->spi = spi;
    priv->can.bittiming_const = &tcan4550_bittiming_const;
    priv->can.clock.freq = 40000000;    // TODO: read from devicetree
    priv->can.do_set_mode = tcan4550_set_mode;

    priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY | CAN_CTRLMODE_ONE_SHOT;

    ndev->netdev_ops = &m_can_netdev_ops;
    ndev->flags |= IFF_ECHO;    // Tell Linux we support echo

    spi->bits_per_word = 8; // Note: For instance Raspberry Pi 4 only support 8-bit transfers, imx8 support 32-bit
    spi->max_speed_hz = 18000000;   // TODO: read from devicetree
    spi->cs_setup = delay;
    spi->cs_hold = delay;
    spi->cs_inactive = delay;
    spi->word_delay = delay;

    err = spi_setup(spi);
    if (err)
    {
        dev_err(&spi->dev, "could not setup SPI\n");
        goto exit_free;
    }

    err = register_candev(ndev);
    if (err)
    {
        dev_err(&spi->dev, "registering candev failed\n");
        goto exit_free;
    }

    tcan4550_setupIo(ndev);
    usleep_range(1000, 2000);
    tcan4550_hwReset(ndev);

    // read chip identification to verify correct chip is there
    if (!tcan4550_readIdentification(spi))
    {
        if (!tcan4550_readIdentification(spi))
        {
            dev_err(&spi->dev, "failed to read TCAN4550 identification\n");
            err = -ENODEV;
            goto exit_unregister;
        }
    }

    priv->wq = alloc_workqueue("tcan4550_wq", WQ_FREEZABLE | WQ_MEM_RECLAIM, 1);
    if (!priv->wq)
    {
        dev_err(&spi->dev, "could not allocate workqueue\n");
        err = -ENOMEM;
        goto exit_unregister;
    }
    INIT_WORK(&priv->tx_work, tcan4550_tx_work_handler);

    return 0;

exit_unregister:
    unregister_candev(ndev);
exit_free:
    free_candev(ndev);

    return err;
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,18,0)
int tcan_remove(struct spi_device *spi)
#else
void tcan_remove(struct spi_device *spi)
#endif
{
    struct net_device *ndev = spi_get_drvdata(spi);
    struct tcan4550_priv *priv = netdev_priv(ndev);
    
    unregister_candev(ndev);
    free_candev(ndev);
    destroy_workqueue(priv->wq);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(5,18,0)
    return 0;
#endif
}

static const struct of_device_id tcan4550_of_match[] = {
    {
        .compatible = "ti,tcan4x5x",
    },
    {}};
MODULE_DEVICE_TABLE(of, tcan4550_of_match);

static const struct spi_device_id tcan4550_id_table[] = {
    {
        .name = "tcan4x5x",
    },
    {}};
MODULE_DEVICE_TABLE(spi, tcan4550_id_table);

static struct spi_driver tcan4550_can_driver = {
    .driver = {
        .name = "tcan4x5x",
        .of_match_table = tcan4550_of_match,
        .pm = NULL,
    },
    .id_table = tcan4550_id_table,
    .probe = tcan_probe,
    .remove = tcan_remove,
};
module_spi_driver(tcan4550_can_driver);

MODULE_AUTHOR("Carl-Magnus Moon <>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CAN bus driver for TI TCAN4550 controller");
