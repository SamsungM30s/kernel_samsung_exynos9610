/*
 * u_ether.c -- Ethernet-over-USB link layer utilities for Gadget stack
 *
 * Copyright (C) 2003-2005,2008 David Brownell
 * Copyright (C) 2003-2004 Robert Schwebel, Benedikt Spranger
 * Copyright (C) 2008 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* #define VERBOSE_DEBUG */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
#include <linux/hrtimer.h>
#endif

#include "u_ether.h"
#include "rndis.h"


/*
 * This component encapsulates the Ethernet link glue needed to provide
 * one (!) network link through the USB gadget stack, normally "usb0".
 *
 * The control and data models are handled by the function driver which
 * connects to this code; such as CDC Ethernet (ECM or EEM),
 * "CDC Subset", or RNDIS.  That includes all descriptor and endpoint
 * management.
 *
 * Link level addressing is handled by this component using module
 * parameters; if no such parameters are provided, random link level
 * addresses are used.  Each end of the link uses one address.  The
 * host end address is exported in various ways, and is often recorded
 * in configuration databases.
 *
 * The driver which assembles each configuration using such a link is
 * responsible for ensuring that each configuration includes at most one
 * instance of is network link.  (The network layer provides ways for
 * this single "physical" link to be used by multiple virtual links.)
 */

#define UETH__VERSION	"29-May-2008"

/* Experiments show that both Linux and Windows hosts allow up to 16k
 * frame sizes. Set the max MTU size to 15k+52 to prevent allocating 32k
 * blocks and still have efficient handling. */
#define GETHER_MAX_MTU_SIZE 15412
#define GETHER_MAX_ETH_FRAME_LEN (GETHER_MAX_MTU_SIZE + ETH_HLEN)

static struct workqueue_struct	*uether_wq;

struct eth_dev {
	/* lock is held while accessing port_usb
	 */
	spinlock_t		lock;
	struct gether		*port_usb;

	struct net_device	*net;
	struct usb_gadget	*gadget;

	spinlock_t		req_lock;	/* guard {rx,tx}_reqs */
	struct list_head	tx_reqs, rx_reqs;
	unsigned		tx_qlen;
/* Minimum number of TX USB request queued to UDC */
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
#define TX_REQ_THRESHOLD	1
#else
#define TX_REQ_THRESHOLD	5
#endif
	int			no_tx_req_used;
	int			tx_skb_hold_count;
	size_t		tx_req_bufsize;		/* prevent CID 103507 */
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
	struct hrtimer	tx_timer;
	bool en_timer;
#define TX_TIMEOUT_NSECS	11000000
#endif

	struct sk_buff_head	rx_frames;

	unsigned		qmult;

	unsigned		header_len;
	unsigned		ul_max_pkts_per_xfer;
	unsigned		dl_max_pkts_per_xfer;
	struct sk_buff		*(*wrap)(struct gether *, struct sk_buff *skb);
	int			(*unwrap)(struct gether *,
						struct sk_buff *skb,
						struct sk_buff_head *list);

	struct work_struct	work;
	struct work_struct	rx_work;

	unsigned long		todo;
#define	WORK_RX_MEMORY		0

	bool			zlp;
	u8			host_mac[ETH_ALEN];
	u8			dev_mac[ETH_ALEN];
	int 			no_of_zlp;
};

/*-------------------------------------------------------------------------*/

#define RX_EXTRA	20	/* bytes guarding against rx overflows */

#define DEFAULT_QLEN	2	/* double buffering by default */

/* for dual-speed hardware, use deeper queues at high/super speed */
static inline int qlen(struct usb_gadget *gadget, unsigned qmult)
{
	if (gadget_is_dualspeed(gadget))
		return qmult * DEFAULT_QLEN;
	else
		return DEFAULT_QLEN;
}

/*-------------------------------------------------------------------------*/

/* REVISIT there must be a better way than having two sets
 * of debug calls ...
 */

#undef DBG
#undef VDBG
#undef ERROR
#undef INFO

#define xprintk(d, level, fmt, args...) \
	printk(level "%s: " fmt, (d)->net->name, ## args)

#ifdef DEBUG
#undef DEBUG
#define DBG(dev, fmt, args...) \
	xprintk(dev, KERN_DEBUG, fmt, ## args)
#else
#define DBG(dev, fmt, args...) \
	do { } while (0)
#endif /* DEBUG */

#ifdef VERBOSE_DEBUG
#define VDBG	DBG
#else
#define VDBG(dev, fmt, args...) \
	do { } while (0)
#endif /* DEBUG */

#define ERROR(dev, fmt, args...) \
	xprintk(dev, KERN_ERR, fmt, ## args)
#define INFO(dev, fmt, args...) \
	xprintk(dev, KERN_INFO, fmt, ## args)

/*-------------------------------------------------------------------------*/

/* NETWORK DRIVER HOOKUP (to the layer above this driver) */

static int ueth_change_mtu(struct net_device *net, int new_mtu)
{
	struct eth_dev	*dev = netdev_priv(net);
	unsigned long	flags;
	int		status = 0;

	/* don't change MTU on "live" link (peer won't know) */
	spin_lock_irqsave(&dev->lock, flags);
	if (dev->port_usb)
		status = -EBUSY;
	else if (new_mtu <= ETH_HLEN || new_mtu > GETHER_MAX_ETH_FRAME_LEN)
		status = -ERANGE;
	else
		net->mtu = new_mtu;
	spin_unlock_irqrestore(&dev->lock, flags);

	return status;
}

static void eth_get_drvinfo(struct net_device *net, struct ethtool_drvinfo *p)
{
	struct eth_dev *dev = netdev_priv(net);

	strlcpy(p->driver, "g_ether", sizeof(p->driver));
	strlcpy(p->version, UETH__VERSION, sizeof(p->version));
	strlcpy(p->fw_version, dev->gadget->name, sizeof(p->fw_version));
	strlcpy(p->bus_info, dev_name(&dev->gadget->dev), sizeof(p->bus_info));
}

/* REVISIT can also support:
 *   - WOL (by tracking suspends and issuing remote wakeup)
 *   - msglevel (implies updated messaging)
 *   - ... probably more ethtool ops
 */

static const struct ethtool_ops ops = {
	.get_drvinfo = eth_get_drvinfo,
	.get_link = ethtool_op_get_link,
};

static void defer_kevent(struct eth_dev *dev, int flag)
{
	if (test_and_set_bit(flag, &dev->todo))
		return;
	if (!schedule_work(&dev->work))
		ERROR(dev, "kevent %d may have been dropped\n", flag);
	else
		DBG(dev, "kevent %d scheduled\n", flag);
}

static void rx_complete(struct usb_ep *ep, struct usb_request *req);

static int
rx_submit(struct eth_dev *dev, struct usb_request *req, gfp_t gfp_flags)
{
	struct sk_buff	*skb;
	int		retval = -ENOMEM;
	size_t		size = 0;
	struct usb_ep	*out;
	unsigned long	flags;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->port_usb)
		out = dev->port_usb->out_ep;
	else
		out = NULL;

	if (!out)
	{
		spin_unlock_irqrestore(&dev->lock, flags);
		return -ENOTCONN;
	}

	/* Padding up to RX_EXTRA handles minor disagreements with host.
	 * Normally we use the USB "terminate on short read" convention;
	 * so allow up to (N*maxpacket), since that memory is normally
	 * already allocated.  Some hardware doesn't deal well with short
	 * reads (e.g. DMA must be N*maxpacket), so for now don't trim a
	 * byte off the end (to force hardware errors on overflow).
	 *
	 * RNDIS uses internal framing, and explicitly allows senders to
	 * pad to end-of-packet.  That's potentially nice for speed, but
	 * means receivers can't recover lost synch on their own (because
	 * new packets don't only start after a short RX).
	 */
	size += sizeof(struct ethhdr) + dev->net->mtu + RX_EXTRA;
	size += dev->port_usb->header_len;
	size += out->maxpacket - 1;
	size -= size % out->maxpacket;

	if (dev->ul_max_pkts_per_xfer)
		size *= dev->ul_max_pkts_per_xfer;

	if (dev->port_usb->is_fixed)
		size = max_t(size_t, size, dev->port_usb->fixed_out_len);
	spin_unlock_irqrestore(&dev->lock, flags);

	DBG(dev, "%s: size: %zd\n", __func__, size);
	skb = alloc_skb(size + NET_IP_ALIGN, gfp_flags);
	if (skb == NULL) {
		DBG(dev, "no rx skb\n");
		goto enomem;
	}

	/* Some platforms perform better when IP packets are aligned,
	 * but on at least one, checksumming fails otherwise.  Note:
	 * RNDIS headers involve variable numbers of LE32 values.
	 */
	skb_reserve(skb, NET_IP_ALIGN);

	req->buf = skb->data;
	req->length = size;
	req->complete = rx_complete;
	req->context = skb;

	retval = usb_ep_queue(out, req, gfp_flags);
	if (retval == -ENOMEM)
enomem:
		defer_kevent(dev, WORK_RX_MEMORY);
	if (retval) {
		DBG(dev, "rx submit --> %d\n", retval);
		if (skb)
			dev_kfree_skb_any(skb);
	}
	return retval;
}

static void rx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct sk_buff	*skb = req->context;
	struct eth_dev	*dev = ep->driver_data;
	int		status = req->status;
	bool		queue = 0;

	switch (status) {

	/* normal completion */
	case 0:
		skb_put(skb, req->actual);

		if (dev->unwrap) {
			unsigned long	flags;

			spin_lock_irqsave(&dev->lock, flags);
			if (dev->port_usb) {
				status = dev->unwrap(dev->port_usb,
							skb,
							&dev->rx_frames);
				if (status == -EINVAL)
					dev->net->stats.rx_errors++;
				else if (status == -EOVERFLOW)
					dev->net->stats.rx_over_errors++;
			} else {
				dev_kfree_skb_any(skb);
				status = -ENOTCONN;
			}
			spin_unlock_irqrestore(&dev->lock, flags);
		} else {
			skb_queue_tail(&dev->rx_frames, skb);
		}
		if (!status)
			queue = 1;
		break;

	/* software-driven interface shutdown */
	case -ECONNRESET:		/* unlink */
	case -ESHUTDOWN:		/* disconnect etc */
		VDBG(dev, "rx shutdown, code %d\n", status);
		goto quiesce;

	/* for hardware automagic (such as pxa) */
	case -ECONNABORTED:		/* endpoint reset */
		DBG(dev, "rx %s reset\n", ep->name);
		defer_kevent(dev, WORK_RX_MEMORY);
quiesce:
		dev_kfree_skb_any(skb);
		goto clean;

	/* data overrun */
	case -EOVERFLOW:
		dev->net->stats.rx_over_errors++;
		/* FALLTHROUGH */

	default:
		queue = 1;
		dev_kfree_skb_any(skb);
		dev->net->stats.rx_errors++;
		DBG(dev, "rx status %d\n", status);
		break;
	}

clean:
	spin_lock(&dev->req_lock);
	list_add(&req->list, &dev->rx_reqs);
	spin_unlock(&dev->req_lock);

	if (queue)
		queue_work(uether_wq, &dev->rx_work);
}


static int prealloc(struct list_head *list, struct usb_ep *ep, unsigned n, int tx_size)
{
	unsigned		i;
	struct usb_request	*req;

	if (!n)
		return -ENOMEM;

	/* queue/recycle up to N requests */
	i = n;
	list_for_each_entry(req, list, list) {
		if (i-- == 0)
			goto extra;
	}
	while (i--) {
		req = usb_ep_alloc_request(ep, GFP_KERNEL);
		if (!req)
			return list_empty(list) ? -ENOMEM : i;
		if (tx_size) {
			if (!req->buf)
				req->buf = kmalloc(tx_size, GFP_KERNEL);
			if (!req->buf) {
				usb_ep_free_request(ep, req);
				return list_empty(list) ? -ENOMEM : i;
			}
		}
		list_add(&req->list, list);
	}
	return 0;

extra:
	/* free extras */
	for (;;) {
		struct list_head	*next;

		next = req->list.next;
		list_del(&req->list);
		usb_ep_free_request(ep, req);

		if (next == list)
			break;

		req = container_of(next, struct usb_request, list);
	}
	return 0;
}

static int alloc_requests(struct eth_dev *dev, struct gether *link, unsigned n)
{
	int	status;

	if (!link->is_fixed) {
		dev->tx_req_bufsize = (dev->dl_max_pkts_per_xfer *
				(dev->net->mtu
				+ sizeof(struct ethhdr)
				/* size of rndis_packet_msg_type */
				+ 44
				+ 22));
	} else
		dev->tx_req_bufsize = 0;
//	spin_lock(&dev->req_lock);
	status = prealloc(&dev->tx_reqs, link->in_ep, n, dev->tx_req_bufsize);
	if (status < 0)
		goto fail;
	else if (status > 0)
		printk("usb: %s prepare  [%d] dev->tx_reqs  \n", __func__, status);
	status = prealloc(&dev->rx_reqs, link->out_ep, n, 0);
	if (status < 0)
		goto fail;
	else if (status > 0)
		printk("usb: %s prepare [%d] dev->rx_reqs \n", __func__, status);
	goto done;
fail:
	DBG(dev, "can't alloc requests\n");
done:
//	spin_unlock(&dev->req_lock);
	return status;
}

static void rx_fill(struct eth_dev *dev, gfp_t gfp_flags)
{
	struct usb_request	*req;
	unsigned long		flags;
	int			req_cnt = 0;

	/* fill unused rxq slots with some skb */
	spin_lock_irqsave(&dev->req_lock, flags);
	while (!list_empty(&dev->rx_reqs)) {
		/* break the nexus of continuous completion and re-submission*/
		if (++req_cnt > qlen(dev->gadget, dev->qmult))
			break;

		req = container_of(dev->rx_reqs.next,
				struct usb_request, list);
		list_del_init(&req->list);
		spin_unlock_irqrestore(&dev->req_lock, flags);

		if (rx_submit(dev, req, gfp_flags) < 0) {
			spin_lock_irqsave(&dev->req_lock, flags);
			list_add(&req->list, &dev->rx_reqs);
			spin_unlock_irqrestore(&dev->req_lock, flags);
			defer_kevent(dev, WORK_RX_MEMORY);
			return;
		}

		spin_lock_irqsave(&dev->req_lock, flags);
	}
	spin_unlock_irqrestore(&dev->req_lock, flags);
}

static void process_rx_w(struct work_struct *work)
{
	struct eth_dev	*dev = container_of(work, struct eth_dev, rx_work);
	struct sk_buff	*skb;
	int		status = 0;

	if (!dev->port_usb)
		return;

	while ((skb = skb_dequeue(&dev->rx_frames))) {
		if (status < 0
				|| ETH_HLEN > skb->len
				|| skb->len > ETH_FRAME_LEN) {
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
		/*
		  Need to revisit net->mtu	does not include header size incase of changed MTU
		*/
			if (!strcmp(dev->port_usb->func.name, "ncm")) {
				if (status < 0
					|| ETH_HLEN > skb->len
					|| skb->len > (dev->net->mtu + ETH_HLEN)) {
					printk(KERN_ERR "usb: %s  drop incase of NCM rx length %d\n", __func__, skb->len);
				} else {
					printk(KERN_ERR "usb: %s  Dont drop incase of NCM rx length %d\n", __func__, skb->len);
					goto process_frame;
				}
			}
#endif
			dev->net->stats.rx_errors++;
			dev->net->stats.rx_length_errors++;
#ifndef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
			DBG(dev, "rx length %d\n", skb->len);
#else
			pr_debug("usb: %s Drop rx length %d\n", __func__, skb->len);
#endif

			DBG(dev, "rx length %d\n", skb->len);
			dev_kfree_skb_any(skb);
			continue;
		}
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
process_frame:
#endif
		skb->protocol = eth_type_trans(skb, dev->net);
		dev->net->stats.rx_packets++;
		dev->net->stats.rx_bytes += skb->len;

		status = netif_rx_ni(skb);
	}

	if (netif_running(dev->net))
		rx_fill(dev, GFP_KERNEL);
}

static void eth_work(struct work_struct *work)
{
	struct eth_dev	*dev = container_of(work, struct eth_dev, work);

	if (test_and_clear_bit(WORK_RX_MEMORY, &dev->todo)) {
		if (netif_running(dev->net))
			rx_fill(dev, GFP_KERNEL);
	}

	if (dev->todo)
		DBG(dev, "work done, flags = 0x%lx\n", dev->todo);
}

static void tx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct sk_buff	*skb = req->context;
	struct eth_dev	*dev = ep->driver_data;
#ifndef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
	//struct net_device *net = dev->net;
	struct usb_request *new_req;
#endif
	struct usb_ep *in;
#ifndef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
	int length;
	int retval;
#endif

	switch (req->status) {
	default:
		dev->net->stats.tx_errors++;
		VDBG(dev, "tx err %d\n", req->status);
#ifdef CONFIG_USB_NCM_SUPPORT_MTU_CHANGE
		printk(KERN_ERR"usb:%s tx err %d\n", __func__, req->status);
#endif
		/* FALLTHROUGH */
	case -ECONNRESET:		/* unlink */
	case -ESHUTDOWN:		/* disconnect etc */
		break;
	case 0:
		if (!req->zero && !dev->zlp)
			dev->net->stats.tx_bytes += req->length-1;
		else
			dev->net->stats.tx_bytes += req->length;
	}
	dev->net->stats.tx_packets++;

	spin_lock(&dev->req_lock);
	req->length = 0;
	list_add_tail(&req->list, &dev->tx_reqs);

	/* list and and just return worh usb reset or shutdown */
	if (req->status == -ESHUTDOWN) {
		spin_unlock(&dev->req_lock);
		return;
	}

	if (dev->port_usb->multi_pkt_xfer) {
#ifndef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
		dev->no_tx_req_used--;
#endif
		in = dev->port_usb->in_ep;

#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
		spin_unlock(&dev->req_lock);
#else
		if (!list_empty(&dev->tx_reqs)) {
			new_req = container_of(dev->tx_reqs.next,
					struct usb_request, list);
			list_del(&new_req->list);
			spin_unlock(&dev->req_lock);
			if (new_req->length > 0) {
				length = new_req->length;

				new_req->zero = 0;
				if ((length % in->maxpacket) == 0) {
					new_req->zero = 1;
					dev->no_of_zlp++;
				}

				/* NCM requires no zlp if transfer is dwNtbInMaxSize */
				if (dev->port_usb->is_fixed) {
					if (length == dev->port_usb->fixed_in_len) {
						new_req->zero = 0;
						dev->no_of_zlp--;
					}
				}

				/* use zlp framing on tx for strict CDC-Ether
				 * conformance, though any robust network rx
				 * path ignores extra padding. and some hardware
				 * doesn't like to write zlps.
				 */
				if (new_req->zero && !dev->zlp &&
						(length % in->maxpacket) == 0) {
					length++;
				}

				new_req->length = length;
#ifdef CONFIG_USB_RNDIS_MULTIPACKET
				new_req->complete = tx_complete;
#endif
				retval = usb_ep_queue(in, new_req, GFP_ATOMIC);
				switch (retval) {
				default:
					printk(KERN_ERR"usb: dropped tx_complete_newreq(%pK)\n", new_req);
					DBG(dev, "tx queue err %d\n", retval);
					new_req->length = 0;
					spin_lock(&dev->req_lock);
					list_add_tail(&new_req->list, &dev->tx_reqs);
					spin_unlock(&dev->req_lock);
					break;
				case 0:
					spin_lock(&dev->req_lock);
					dev->no_tx_req_used++;
					spin_unlock(&dev->req_lock);
					//net->trans_start = jiffies;
				}
			} else {
				spin_lock(&dev->req_lock);
				list_add_tail(&new_req->list, &dev->tx_reqs);
				spin_unlock(&dev->req_lock);
			}
		} else {
			spin_unlock(&dev->req_lock);
		}
#endif
	} else {
		spin_unlock(&dev->req_lock);
		dev_kfree_skb_any(skb);
	}

	if (netif_carrier_ok(dev->net))
		netif_wake_queue(dev->net);
}

static inline int is_promisc(u16 cdc_filter)
{
	return cdc_filter & USB_CDC_PACKET_TYPE_PROMISCUOUS;
}

#if 0
static void alloc_tx_buffer(struct eth_dev *dev)
{
	struct list_head	*act;
	struct usb_request	*req;

	dev->tx_req_bufsize = (dev->dl_max_pkts_per_xfer *
				(dev->net->mtu
				+ sizeof(struct ethhdr)
				/* size of rndis_packet_msg_type */
				+ 44
				+ 22));

	list_for_each(act, &dev->tx_reqs) {
		req = container_of(act, struct usb_request, list);
		if (!req->buf)
			req->buf = kmalloc(dev->tx_req_bufsize,
						GFP_ATOMIC);
	}
}
#endif

#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
static int tx_task(struct eth_dev *dev, struct usb_request *req)
{
	struct usb_ep *in = dev->port_usb->in_ep;
	int length = req->length;
	int retval;

	req->complete = tx_complete;

	req->zero = 0;

	if ((length % in->maxpacket) == 0) {
		req->zero = 1;
		dev->no_of_zlp++;
	}

	/* NCM requires no zlp if transfer is dwNtbInMaxSize */
	if (dev->port_usb) {
		if (dev->port_usb->is_fixed) {
			if (length == dev->port_usb->fixed_in_len) {
				req->zero = 0;
				dev->no_of_zlp--;
			}
		}
	}

	if (req->zero && !dev->zlp && (length % in->maxpacket) == 0) {
		length++;
	}
	req->length = length;

	/* throttle highspeed IRQ rate back slightly */
	if (gadget_is_dualspeed(dev->gadget) &&
		(dev->gadget->speed == USB_SPEED_HIGH)) {
		dev->tx_qlen++;
		if (dev->tx_qlen == (dev->qmult/2)) {
			req->no_interrupt = 0;
			dev->tx_qlen = 0;
		} else {
			req->no_interrupt = 1;
		}
	} else {
		req->no_interrupt = 0;
	}
	retval = usb_ep_queue(in, req, GFP_ATOMIC);

	return retval;
}

static enum hrtimer_restart tx_timeout(struct hrtimer *data)
{
	struct eth_dev *dev = container_of(data, struct eth_dev, tx_timer);
	struct usb_request *req = NULL;

	int retval;
	unsigned long flags;

	spin_lock_irqsave(&dev->req_lock, flags);

	/*
	* this freelist can be empty if an interrupt triggered disconnect()
	* and reconfigured the gadget (shutting down this queue) after the
	* network stack decided to xmit but before we got the spinlock.
	*/

	if (list_empty(&dev->tx_reqs)) {
		spin_unlock_irqrestore(&dev->req_lock, flags);
		printk("\n\n%s: TX REQS list empty!\n\n", __func__);
		return HRTIMER_NORESTART;
	}

	req = container_of(dev->tx_reqs.next, struct usb_request, list);

	list_del(&req->list);

	/* temporarily stop TX queue when the freelist empties */
	if (list_empty(&dev->tx_reqs))
		netif_stop_queue(dev->net);

	spin_unlock_irqrestore(&dev->req_lock, flags);

	retval = tx_task(dev, req);

    if (retval) {
		req->length = 0;
		dev->net->stats.tx_dropped++;
		spin_lock_irqsave(&dev->req_lock, flags);
		if (list_empty(&dev->tx_reqs))
			netif_start_queue(dev->net);
		list_add(&req->list, &dev->tx_reqs);
		spin_unlock_irqrestore(&dev->req_lock, flags);
	}

	return HRTIMER_NORESTART;
}
#endif
static netdev_tx_t eth_start_xmit(struct sk_buff *skb,
					struct net_device *net)
{
	struct eth_dev		*dev = netdev_priv(net);
	int			length = 0;
	int			retval;
	struct usb_request	*req = NULL;
	unsigned long		flags;
	struct usb_ep		*in;
	u16			cdc_filter;
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
	if (dev->en_timer) {
		hrtimer_cancel(&dev->tx_timer);
		dev->en_timer = 0;
	}
#endif
	spin_lock_irqsave(&dev->lock, flags);
	if (dev->port_usb) {
		in = dev->port_usb->in_ep;
		cdc_filter = dev->port_usb->cdc_filter;
	} else {
		in = NULL;
		cdc_filter = 0;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (!in) {
		if (skb)
			dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

#if 0
	/* Allocate memory for tx_reqs to support multi packet transfer */
	if (dev->port_usb->multi_pkt_xfer && !dev->tx_req_bufsize)
		alloc_tx_buffer(dev);
#endif

	/* apply outgoing CDC or RNDIS filters */
	if (skb && !is_promisc(cdc_filter)) {
		u8		*dest = skb->data;

		if (is_multicast_ether_addr(dest)) {
			u16	type;

			/* ignores USB_CDC_PACKET_TYPE_MULTICAST and host
			 * SET_ETHERNET_MULTICAST_FILTERS requests
			 */
			if (is_broadcast_ether_addr(dest))
				type = USB_CDC_PACKET_TYPE_BROADCAST;
			else
				type = USB_CDC_PACKET_TYPE_ALL_MULTICAST;
			if (!(cdc_filter & type)) {
				dev_kfree_skb_any(skb);
				return NETDEV_TX_OK;
			}
		}
		/* ignores USB_CDC_PACKET_TYPE_DIRECTED */
	}

	spin_lock_irqsave(&dev->req_lock, flags);
	/*
	 * this freelist can be empty if an interrupt triggered disconnect()
	 * and reconfigured the gadget (shutting down this queue) after the
	 * network stack decided to xmit but before we got the spinlock.
	 */
	if (list_empty(&dev->tx_reqs)) {
		spin_unlock_irqrestore(&dev->req_lock, flags);
		return NETDEV_TX_BUSY;
	}

	req = container_of(dev->tx_reqs.next, struct usb_request, list);
	list_del(&req->list);

	/* temporarily stop TX queue when the freelist empties */
	if (list_empty(&dev->tx_reqs) && (dev->tx_skb_hold_count >= (dev->dl_max_pkts_per_xfer -1)))
		netif_stop_queue(net);
	spin_unlock_irqrestore(&dev->req_lock, flags);

	/* no buffer copies needed, unless the network stack did it
	 * or the hardware can't use skb buffers.
	 * or there's not enough space for extra headers we need
	 */
	if (dev->wrap) {
		unsigned long	flags;

		spin_lock_irqsave(&dev->lock, flags);
		if (dev->port_usb)
			skb = dev->wrap(dev->port_usb, skb);
		spin_unlock_irqrestore(&dev->lock, flags);
		if (!skb) {
			/* Multi frame CDC protocols may store the frame for
			 * later which is not a dropped frame.
			 */
			if (dev->port_usb->supports_multi_frame)
				goto multiframe;
			goto drop;
		}
	}

	spin_lock_irqsave(&dev->req_lock, flags);
	dev->tx_skb_hold_count++;
	spin_unlock_irqrestore(&dev->req_lock, flags);

	if (dev->port_usb->multi_pkt_xfer) {
		/* Add RNDIS Header */
		memcpy(req->buf + req->length, dev->port_usb->header,
						dev->header_len);
		/* Increment req length by header size */
		req->length += dev->header_len;
		/* Copy received IP data from SKB */
		memcpy(req->buf + req->length, skb->data, skb->len);
		/* Increment req length by skb data length */
		req->length = req->length + skb->len;
		length = req->length;
		dev_kfree_skb_any(skb);

		spin_lock_irqsave(&dev->req_lock, flags);
		if (dev->tx_skb_hold_count < dev->dl_max_pkts_per_xfer) {
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
			list_add(&req->list, &dev->tx_reqs);
			spin_unlock_irqrestore(&dev->req_lock, flags);
			hrtimer_start(&dev->tx_timer, ktime_set(0, TX_TIMEOUT_NSECS),
					HRTIMER_MODE_REL);
			dev->en_timer = 1;
			goto success;
#else
			if (dev->no_tx_req_used > TX_REQ_THRESHOLD) {
				list_add(&req->list, &dev->tx_reqs);
				spin_unlock_irqrestore(&dev->req_lock, flags);
				goto success;
			}
#endif
		}

#ifndef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
		dev->no_tx_req_used++;
#endif
		spin_unlock_irqrestore(&dev->req_lock, flags);

		spin_lock_irqsave(&dev->lock, flags);
		dev->tx_skb_hold_count = 0;
		spin_unlock_irqrestore(&dev->lock, flags);
	} else {
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
		req->length = skb->len;
#else
		length = skb->len;
#endif
		req->buf = skb->data;
		req->context = skb;
	}
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
	retval = tx_task(dev, req);
#else
	req->complete = tx_complete;

	req->zero = 0;
	if ((length % in->maxpacket) == 0) {
		req->zero = 1;
		dev->no_of_zlp++;
	}

	/* NCM requires no zlp if transfer is dwNtbInMaxSize */
	if (dev->port_usb) {
		if (dev->port_usb->is_fixed) {
			if (length == dev->port_usb->fixed_in_len) {
				req->zero = 0;
				dev->no_of_zlp--;
			}
		}
	}

	/* use zlp framing on tx for strict CDC-Ether conformance,
	 * though any robust network rx path ignores extra padding.
	 * and some hardware doesn't like to write zlps.
	 */
	if (req->zero && !dev->zlp && (length % in->maxpacket) == 0) {
		length++;
	}

	req->length = length;

	/* throttle highspeed IRQ rate back slightly */
	if (gadget_is_dualspeed(dev->gadget) &&
			 (dev->gadget->speed == USB_SPEED_HIGH)) {
		dev->tx_qlen++;
		if (dev->tx_qlen == (dev->qmult/2)) {
			req->no_interrupt = 0;
			dev->tx_qlen = 0;
		} else {
			req->no_interrupt = 1;
		}
	} else {
		req->no_interrupt = 0;
	}

	retval = usb_ep_queue(in, req, GFP_ATOMIC);
#endif

	if (retval) {
		if (!dev->port_usb->multi_pkt_xfer)
			dev_kfree_skb_any(skb);
drop:
		dev->net->stats.tx_dropped++;
		dev->no_tx_req_used--;
		req->length = 0;
multiframe:
		spin_lock_irqsave(&dev->req_lock, flags);
		if (list_empty(&dev->tx_reqs))
			netif_start_queue(net);
		list_add(&req->list, &dev->tx_reqs);
		spin_unlock_irqrestore(&dev->req_lock, flags);
	}
success:
	return NETDEV_TX_OK;
}

/*-------------------------------------------------------------------------*/

static void eth_start(struct eth_dev *dev, gfp_t gfp_flags)
{
	DBG(dev, "%s\n", __func__);

	/* fill the rx queue */
	rx_fill(dev, gfp_flags);

	/* and open the tx floodgates */
	dev->tx_qlen = 0;
	netif_wake_queue(dev->net);
}

static int eth_open(struct net_device *net)
{
	struct eth_dev	*dev = netdev_priv(net);
	struct gether	*link;

	DBG(dev, "%s\n", __func__);
	if (netif_carrier_ok(dev->net))
		eth_start(dev, GFP_KERNEL);

	spin_lock_irq(&dev->lock);
	link = dev->port_usb;
	if (link && link->open)
		link->open(link);
	spin_unlock_irq(&dev->lock);

	return 0;
}

static int eth_stop(struct net_device *net)
{
	struct eth_dev	*dev = netdev_priv(net);
	unsigned long	flags;

	VDBG(dev, "%s\n", __func__);
	netif_stop_queue(net);

	DBG(dev, "stop stats: rx/tx %ld/%ld, errs %ld/%ld\n",
		dev->net->stats.rx_packets, dev->net->stats.tx_packets,
		dev->net->stats.rx_errors, dev->net->stats.tx_errors
		);

	/* ensure there are no more active requests */
	spin_lock_irqsave(&dev->lock, flags);
	if (dev->port_usb) {
		struct gether	*link = dev->port_usb;
		const struct usb_endpoint_descriptor *in;
		const struct usb_endpoint_descriptor *out;

		if (link->close)
			link->close(link);

		/* NOTE:  we have no abort-queue primitive we could use
		 * to cancel all pending I/O.  Instead, we disable then
		 * reenable the endpoints ... this idiom may leave toggle
		 * wrong, but that's a self-correcting error.
		 *
		 * REVISIT:  we *COULD* just let the transfers complete at
		 * their own pace; the network stack can handle old packets.
		 * For the moment we leave this here, since it works.
		 */
		in = link->in_ep->desc;
		out = link->out_ep->desc;
		usb_ep_disable(link->in_ep);
		usb_ep_disable(link->out_ep);
		if (netif_carrier_ok(net)) {
			DBG(dev, "host still using in/out endpoints\n");
			link->in_ep->desc = in;
			link->out_ep->desc = out;
			usb_ep_enable(link->in_ep);
			usb_ep_enable(link->out_ep);
		}
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}

/*-------------------------------------------------------------------------*/
#ifndef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
static u8 host_ethaddr[ETH_ALEN];
#endif
static int get_ether_addr(const char *str, u8 *dev_addr)
{
	if (str) {
		unsigned	i;

		for (i = 0; i < 6; i++) {
			unsigned char num;

			if ((*str == '.') || (*str == ':'))
				str++;
			num = hex_to_bin(*str++) << 4;
			num |= hex_to_bin(*str++);
			dev_addr[i] = num;
		}
		if (is_valid_ether_addr(dev_addr))
			return 0;
	}
	eth_random_addr(dev_addr);
	return 1;
}

static int get_ether_addr_str(u8 dev_addr[ETH_ALEN], char *str, int len)
{
	if (len < 18)
		return -EINVAL;

	snprintf(str, len, "%pM", dev_addr);
	return 18;
}
#ifndef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
static int get_host_ether_addr(u8 *str, u8 *dev_addr)
{
	memcpy(dev_addr, str, ETH_ALEN);
	if (is_valid_ether_addr(dev_addr))
		return 0;

	random_ether_addr(dev_addr);
	memcpy(str, dev_addr, ETH_ALEN);
	return 1;
}
#endif

static const struct net_device_ops eth_netdev_ops = {
	.ndo_open		= eth_open,
	.ndo_stop		= eth_stop,
	.ndo_start_xmit		= eth_start_xmit,
	.ndo_change_mtu		= ueth_change_mtu,
	.ndo_set_mac_address 	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static struct device_type gadget_type = {
	.name	= "gadget",
};

/**
 * gether_setup_name - initialize one ethernet-over-usb link
 * @g: gadget to associated with these links
 * @ethaddr: NULL, or a buffer in which the ethernet address of the
 *	host side of the link is recorded
 * @netname: name for network device (for example, "usb")
 * Context: may sleep
 *
 * This sets up the single network link that may be exported by a
 * gadget driver using this framework.  The link layer addresses are
 * set up using module parameters.
 *
 * Returns an eth_dev pointer on success, or an ERR_PTR on failure.
 */
 struct eth_dev *gether_setup_name(struct usb_gadget *g,
	const char *dev_addr, const char *host_addr,
	u8 ethaddr[ETH_ALEN], unsigned qmult, const char *netname)
{
struct eth_dev		*dev;
struct net_device	*net;
int			status;

net = alloc_etherdev(sizeof *dev);
if (!net)
	return ERR_PTR(-ENOMEM);

dev = netdev_priv(net);
spin_lock_init(&dev->lock);
spin_lock_init(&dev->req_lock);
INIT_WORK(&dev->work, eth_work);
INIT_WORK(&dev->rx_work, process_rx_w);
INIT_LIST_HEAD(&dev->tx_reqs);
INIT_LIST_HEAD(&dev->rx_reqs);
skb_queue_head_init(&dev->rx_frames);

/* network device setup */
dev->net = net;
dev->qmult = qmult;
snprintf(net->name, sizeof(net->name), "%s%%d", netname);

if (get_ether_addr(dev_addr, net->dev_addr)) {
	net->addr_assign_type = NET_ADDR_RANDOM;
	dev_warn(&g->dev, "using random %s ethernet address\n", "self");
} else {
	net->addr_assign_type = NET_ADDR_SET;
}

#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
memcpy(dev->host_mac, ethaddr, ETH_ALEN);
printk(KERN_DEBUG "usb: set unique host mac\n");
#else
if (get_ether_addr(host_addr, dev->host_mac))
	dev_warn(&g->dev, "using random %s ethernet address\n", "host");
else
	dev_warn(&g->dev, "using previous %s ethernet address\n", "host");
#endif

if (ethaddr)
	memcpy(ethaddr, dev->host_mac, ETH_ALEN);

net->netdev_ops = &eth_netdev_ops;
net->ethtool_ops = &ops;

/* MTU range: 14 - 15412 */
net->min_mtu = ETH_HLEN;
net->max_mtu = GETHER_MAX_MTU_SIZE;

dev->gadget = g;
SET_NETDEV_DEV(net, &g->dev);
SET_NETDEV_DEVTYPE(net, &gadget_type);

status = register_netdev(net);
if (status < 0) {
	dev_dbg(&g->dev, "register_netdev failed, %d\n", status);
	free_netdev(net);
	dev = ERR_PTR(status);
} else {
	DBG(dev, "MAC %pM\n", net->dev_addr);
	DBG(dev, "HOST MAC %pM\n", dev->host_mac);
	netif_carrier_off(net);
}

return dev;
}
EXPORT_SYMBOL_GPL(gether_setup_name);

struct net_device *gether_setup_name_default(const char *netname)
{
struct net_device	*net;
struct eth_dev		*dev;

net = alloc_etherdev(sizeof(*dev));
if (!net)
	return ERR_PTR(-ENOMEM);

dev = netdev_priv(net);
spin_lock_init(&dev->lock);
spin_lock_init(&dev->req_lock);
INIT_WORK(&dev->work, eth_work);
INIT_WORK(&dev->rx_work, process_rx_w);
INIT_LIST_HEAD(&dev->tx_reqs);
INIT_LIST_HEAD(&dev->rx_reqs);

#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
hrtimer_init(&dev->tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
dev->tx_timer.function = tx_timeout;
#endif

skb_queue_head_init(&dev->rx_frames);

/* network device setup */
dev->net = net;
dev->qmult = QMULT_DEFAULT;
dev->tx_req_bufsize = 0;
snprintf(net->name, sizeof(net->name), "%s%%d", netname);

eth_random_addr(dev->dev_mac);
pr_warn("using random %s ethernet address\n", "self");
eth_random_addr(dev->host_mac);
pr_warn("using random %s ethernet address\n", "host");

net->addr_assign_type = NET_ADDR_RANDOM;
net->netdev_ops = &eth_netdev_ops;
net->ethtool_ops = &ops;

SET_NETDEV_DEVTYPE(net, &gadget_type);

/* MTU range: 14 - 15412 */
net->min_mtu = ETH_HLEN;
net->max_mtu = GETHER_MAX_MTU_SIZE;

return net;
}
EXPORT_SYMBOL_GPL(gether_setup_name_default);

int gether_register_netdev(struct net_device *net)
{
struct eth_dev *dev;
struct usb_gadget *g;
int status;
struct sockaddr sa;

if (!net->dev.parent)
	return -EINVAL;

dev = netdev_priv(net);
g = dev->gadget;

memcpy(net->dev_addr, dev->dev_mac, ETH_ALEN);

status = register_netdev(net);
if (status < 0) {
	dev_dbg(&g->dev, "register_netdev failed, %d\n", status);
	return status;
}

INFO(dev, "HOST MAC %pM\n", dev->host_mac);
INFO(dev, "MAC %pM\n", dev->dev_mac);

netif_carrier_off(net);

sa.sa_family = net->type;
memcpy(sa.sa_data, dev->dev_mac, ETH_ALEN);
rtnl_lock();
status = dev_set_mac_address(net, &sa);
rtnl_unlock();
if (status)
	pr_warn("cannot set self ethernet address: %d\n", status);
else
	DBG(dev, "MAC %pM\n", dev->dev_mac);

return status;
}

EXPORT_SYMBOL_GPL(gether_register_netdev);

void gether_set_gadget(struct net_device *net, struct usb_gadget *g)
{
	struct eth_dev *dev;

	dev = netdev_priv(net);
	dev->gadget = g;
	SET_NETDEV_DEV(net, &g->dev);
}
EXPORT_SYMBOL_GPL(gether_set_gadget);

int gether_set_dev_addr(struct net_device *net, const char *dev_addr)
{
	struct eth_dev *dev;
	u8 new_addr[ETH_ALEN];

	dev = netdev_priv(net);
	if (get_ether_addr(dev_addr, new_addr))
		return -EINVAL;
	memcpy(dev->dev_mac, new_addr, ETH_ALEN);
	net->addr_assign_type = NET_ADDR_SET;
	return 0;
}
EXPORT_SYMBOL_GPL(gether_set_dev_addr);

int gether_get_dev_addr(struct net_device *net, char *dev_addr, int len)
{
	struct eth_dev *dev;

	dev = netdev_priv(net);
	return get_ether_addr_str(dev->dev_mac, dev_addr, len);
}
EXPORT_SYMBOL_GPL(gether_get_dev_addr);

int gether_set_host_addr(struct net_device *net, const char *host_addr)
{
	struct eth_dev *dev;
	u8 new_addr[ETH_ALEN];

	dev = netdev_priv(net);
	if (get_ether_addr(host_addr, new_addr))
		return -EINVAL;
	memcpy(dev->host_mac, new_addr, ETH_ALEN);
	return 0;
}
EXPORT_SYMBOL_GPL(gether_set_host_addr);

int gether_get_host_addr(struct net_device *net, char *host_addr, int len)
{
	struct eth_dev *dev;

	dev = netdev_priv(net);
	return get_ether_addr_str(dev->host_mac, host_addr, len);
}
EXPORT_SYMBOL_GPL(gether_get_host_addr);

int gether_get_host_addr_cdc(struct net_device *net, char *host_addr, int len)
{
	struct eth_dev *dev;

	if (len < 13)
		return -EINVAL;

	dev = netdev_priv(net);
	snprintf(host_addr, len, "%pm", dev->host_mac);

	return strlen(host_addr);
}
EXPORT_SYMBOL_GPL(gether_get_host_addr_cdc);

void gether_get_host_addr_u8(struct net_device *net, u8 host_mac[ETH_ALEN])
{
	struct eth_dev *dev;

	dev = netdev_priv(net);
	memcpy(host_mac, dev->host_mac, ETH_ALEN);
}
EXPORT_SYMBOL_GPL(gether_get_host_addr_u8);

void gether_set_qmult(struct net_device *net, unsigned qmult)
{
	struct eth_dev *dev;

	dev = netdev_priv(net);
	dev->qmult = qmult;
}
EXPORT_SYMBOL_GPL(gether_set_qmult);

unsigned gether_get_qmult(struct net_device *net)
{
	struct eth_dev *dev;

	dev = netdev_priv(net);
	return dev->qmult;
}
EXPORT_SYMBOL_GPL(gether_get_qmult);

int gether_get_ifname(struct net_device *net, char *name, int len)
{
	rtnl_lock();
	strlcpy(name, netdev_name(net), len);
	rtnl_unlock();
	return strlen(name);
}
EXPORT_SYMBOL_GPL(gether_get_ifname);

/**
 * gether_cleanup - remove Ethernet-over-USB device
 * Context: may sleep
 *
 * This is called to free all resources allocated by @gether_setup().
 */
void gether_cleanup(struct eth_dev *dev)
{
	if (!dev)
		return;

	unregister_netdev(dev->net);
	flush_work(&dev->work);
	free_netdev(dev->net);
}
EXPORT_SYMBOL_GPL(gether_cleanup);

/**
 * gether_connect - notify network layer that USB link is active
 * @link: the USB link, set up with endpoints, descriptors matching
 *	current device speed, and any framing wrapper(s) set up.
 * Context: irqs blocked
 *
 * This is called to activate endpoints and let the network layer know
 * the connection is active ("carrier detect").  It may cause the I/O
 * queues to open and start letting network packets flow, but will in
 * any case activate the endpoints so that they respond properly to the
 * USB host.
 *
 * Verify net_device pointer returned using IS_ERR().  If it doesn't
 * indicate some error code (negative errno), ep->driver_data values
 * have been overwritten.
 */
struct net_device *gether_connect(struct gether *link)
{
	struct eth_dev		*dev = link->ioport;
	int			result = 0;
	struct rndis_packet_msg_type *header = NULL;

	if (!dev)
		return ERR_PTR(-EINVAL);
	link->header = kzalloc(sizeof(*header), GFP_ATOMIC);

	if (!link->header) {
		pr_err("RNDIS header memory allocation failed.\n");
		result = -ENOMEM;
		goto fail;
	}

	link->in_ep->driver_data = dev;
	result = usb_ep_enable(link->in_ep);
	if (result != 0) {
		DBG(dev, "enable %s --> %d\n",
			link->in_ep->name, result);
		goto fail0;
	}

	link->out_ep->driver_data = dev;
	result = usb_ep_enable(link->out_ep);
	if (result != 0) {
		DBG(dev, "enable %s --> %d\n",
			link->out_ep->name, result);
		(void) usb_ep_disable(link->in_ep);
		goto fail0;
	}

#if 0
	if (result == 0)
		result = alloc_requests(dev, link, qlen(dev->gadget,
					dev->qmult));
#endif


	dev->zlp = link->is_zlp_ok;
	DBG(dev, "qlen %d\n", qlen(dev->gadget, dev->qmult));

	dev->header_len = link->header_len;
	dev->unwrap = link->unwrap;
	dev->wrap = link->wrap;

	spin_lock(&dev->lock);
	dev->tx_skb_hold_count = 0;
	dev->no_tx_req_used = 0;
	dev->no_of_zlp = 0;
	dev->port_usb = link;
	if (netif_running(dev->net)) {
		if (link->open)
			link->open(link);
	} else {
		if (link->close)
			link->close(link);
	}
	spin_unlock(&dev->lock);

	netif_carrier_on(dev->net);
	if (netif_running(dev->net))
		eth_start(dev, GFP_ATOMIC);

fail0:
	/* caller is responsible for cleanup on error */
	if (result < 0) {
		kfree(link->header);
fail:
		return ERR_PTR(result);
	}
	return dev->net;
}

EXPORT_SYMBOL_GPL(gether_alloc_request);
/* gether_alloc_request - get usb request queue */
int gether_alloc_request(struct gether *link)
{
	struct eth_dev		*dev = link->ioport;
	int			result = 0;

	/* update multi packet number */
	if (!link->is_fixed) {
		dev->ul_max_pkts_per_xfer = link->ul_max_pkts_per_xfer;
		dev->dl_max_pkts_per_xfer = link->dl_max_pkts_per_xfer;
	}

	result = alloc_requests(dev, link, qlen(dev->gadget,
				dev->qmult));
	printk("usb: %s qlen %d\n", __func__, qlen(dev->gadget, dev->qmult));
	return result;
}

EXPORT_SYMBOL_GPL(gether_free_request);
void gether_free_request(struct gether *link)
{
	struct eth_dev		*dev = link->ioport;
	struct usb_request	*req;

	printk("usb: %s : \n", __func__);
	spin_lock(&dev->req_lock);
	while (!list_empty(&dev->tx_reqs)) {
		req = container_of(dev->tx_reqs.next,
					struct usb_request, list);
		list_del(&req->list);
		spin_unlock(&dev->req_lock);
		if (link->multi_pkt_xfer)
			kfree(req->buf);
		usb_ep_free_request(link->in_ep, req);
		spin_lock(&dev->req_lock);
	}
	spin_unlock(&dev->req_lock);

	spin_lock(&dev->req_lock);
	while (!list_empty(&dev->rx_reqs)) {
		req = container_of(dev->rx_reqs.next,
					struct usb_request, list);
		list_del(&req->list);
		spin_unlock(&dev->req_lock);
		usb_ep_free_request(link->out_ep, req);
		spin_lock(&dev->req_lock);
	}
	spin_unlock(&dev->req_lock);
}

EXPORT_SYMBOL_GPL(gether_connect);

/**
 * gether_disconnect - notify network layer that USB link is inactive
 * @link: the USB link, on which gether_connect() was called
 * Context: irqs blocked
 *
 * This is called to deactivate endpoints and let the network layer know
 * the connection went inactive ("no carrier").
 *
 * On return, the state is as if gether_connect() had never been called.
 * The endpoints are inactive, and accordingly without active USB I/O.
 * Pointers to endpoint descriptors and endpoint private data are nulled.
 */
void gether_disconnect(struct gether *link)
{
	struct eth_dev		*dev = link->ioport;
//	struct usb_request	*req;
	struct sk_buff		*skb;

	WARN_ON(!dev);
	if (!dev)
		return;

	DBG(dev, "%s\n", __func__);

	netif_stop_queue(dev->net);
	netif_carrier_off(dev->net);
	printk(KERN_ERR"usb: %s No of ZLPS (%d)\n", __func__, dev->no_of_zlp);

	/* disable endpoints, forcing (synchronous) completion
	 * of all pending i/o.  then free the request objects
	 * and forget about the endpoints.
	 */
	usb_ep_disable(link->in_ep);
	link->in_ep->desc = NULL;

	usb_ep_disable(link->out_ep);

#if 0
	spin_lock(&dev->req_lock);
	while (!list_empty(&dev->tx_reqs)) {
		req = container_of(dev->tx_reqs.next,
					struct usb_request, list);
		list_del(&req->list);

		spin_unlock(&dev->req_lock);
		if (link->multi_pkt_xfer)
			kfree(req->buf);
		usb_ep_free_request(link->in_ep, req);
		spin_lock(&dev->req_lock);
	}
	kfree(link->header);
	link->header = NULL;
	spin_unlock(&dev->req_lock);
	link->in_ep->desc = NULL;

	usb_ep_disable(link->out_ep);
	spin_lock(&dev->req_lock);
	while (!list_empty(&dev->rx_reqs)) {
		req = container_of(dev->rx_reqs.next,
					struct usb_request, list);
		list_del(&req->list);

		spin_unlock(&dev->req_lock);
		usb_ep_free_request(link->out_ep, req);
		spin_lock(&dev->req_lock);
	}
	spin_unlock(&dev->req_lock);
#endif

	spin_lock(&dev->rx_frames.lock);
	while ((skb = __skb_dequeue(&dev->rx_frames)))
		dev_kfree_skb_any(skb);
	spin_unlock(&dev->rx_frames.lock);

	link->out_ep->desc = NULL;

	/* finish forgetting about this USB link episode */
	dev->header_len = 0;
	dev->unwrap = NULL;
	dev->wrap = NULL;

	spin_lock(&dev->lock);
	dev->port_usb = NULL;
	spin_unlock(&dev->lock);
#ifdef CONFIG_USB_RNDIS_MULTIPACKET_WITH_TIMER
	if (dev->en_timer) {
		hrtimer_cancel(&dev->tx_timer);
		dev->en_timer = 0;
	}
#endif
}
EXPORT_SYMBOL_GPL(gether_disconnect);

static int __init gether_init(void)
{
	uether_wq  = create_singlethread_workqueue("uether");
	if (!uether_wq) {
		pr_err("%s: Unable to create workqueue: uether\n", __func__);
		return -ENOMEM;
	}
	return 0;
}
module_init(gether_init);

static void __exit gether_exit(void)
{
	destroy_workqueue(uether_wq);

}
module_exit(gether_exit);
MODULE_AUTHOR("David Brownell");
MODULE_DESCRIPTION("ethernet over USB driver");
MODULE_LICENSE("GPL v2");
