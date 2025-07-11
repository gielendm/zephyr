/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * @file  uhc_virtual.c
 * @brief Virtual USB host controller (UHC) driver
 *
 * Virtual device controller does not emulate any hardware
 * and can only communicate with the virtual device controllers
 * through virtual bus.
 */

#include "uhc_common.h"
#include "../uvb/uvb.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/usb/uhc.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uhc_vrt, CONFIG_UHC_DRIVER_LOG_LEVEL);

#define FRAME_MAX_TRANSFERS 16

struct uhc_vrt_config {
};

struct uhc_vrt_slot {
	sys_dnode_t node;
	struct uhc_transfer *xfer;
};

struct uhc_vrt_frame {
	struct uhc_vrt_slot slots[FRAME_MAX_TRANSFERS];
	sys_dnode_t *ptr;
	sys_dlist_t list;
	uint8_t count;
};

struct uhc_vrt_data {
	const struct device *dev;
	struct uvb_node *host_node;
	struct k_work work;
	struct k_fifo fifo;
	struct uhc_transfer *last_xfer;
	struct uhc_vrt_frame frame;
	struct k_timer sof_timer;
	uint16_t frame_number;
	uint8_t req;
};

enum uhc_vrt_event_type {
	/* SoF generator event */
	UHC_VRT_EVT_SOF,
	/* Request reply received */
	UHC_VRT_EVT_REPLY,
};

/* Structure for driver's endpoint events */
struct uhc_vrt_event {
	sys_snode_t node;
	enum uhc_vrt_event_type type;
	struct uvb_packet *pkt;
};

K_MEM_SLAB_DEFINE(uhc_vrt_slab, sizeof(struct uhc_vrt_event),
		  16, sizeof(void *));

static void vrt_event_submit(const struct device *dev,
			     const enum uhc_vrt_event_type type,
			     const void *data)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct uhc_vrt_event *event;
	int ret;

	ret = k_mem_slab_alloc(&uhc_vrt_slab, (void **)&event, K_NO_WAIT);
	__ASSERT(ret == 0, "Failed to allocate slab");

	event->type = type;
	event->pkt = (struct uvb_packet *const)data;
	k_fifo_put(&priv->fifo, event);
	k_work_submit(&priv->work);
}

static int vrt_xfer_control(const struct device *dev,
			    struct uhc_transfer *const xfer)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct net_buf *buf = xfer->buf;
	struct uvb_packet *uvb_pkt;
	uint8_t *data = NULL;
	size_t length = 0;

	if (xfer->stage == UHC_CONTROL_STAGE_SETUP) {
		LOG_DBG("Handle SETUP stage");
		uvb_pkt = uvb_alloc_pkt(UVB_REQUEST_SETUP,
					xfer->udev->addr, USB_CONTROL_EP_OUT,
					xfer->setup_pkt, sizeof(xfer->setup_pkt));
		if (uvb_pkt == NULL) {
			LOG_ERR("Failed to allocate UVB packet");
			return -ENOMEM;
		}

		priv->req = UVB_REQUEST_SETUP;

		return uvb_advert_pkt(priv->host_node, uvb_pkt);
	}

	if (buf != NULL && xfer->stage == UHC_CONTROL_STAGE_DATA) {
		if (USB_EP_DIR_IS_IN(xfer->ep)) {
			length = MIN(net_buf_tailroom(buf), xfer->mps);
			data = net_buf_tail(buf);
		} else {
			length = MIN(buf->len, xfer->mps);
			data = buf->data;
		}

		LOG_DBG("Handle DATA stage");
		uvb_pkt = uvb_alloc_pkt(UVB_REQUEST_DATA,
					xfer->udev->addr, xfer->ep,
					data, length);
		if (uvb_pkt == NULL) {
			LOG_ERR("Failed to allocate UVB packet");
			return -ENOMEM;
		}

		priv->req = UVB_REQUEST_DATA;

		return uvb_advert_pkt(priv->host_node, uvb_pkt);
	}

	if (xfer->stage == UHC_CONTROL_STAGE_STATUS) {
		uint8_t ep;

		LOG_DBG("Handle STATUS stage");
		if (USB_EP_DIR_IS_IN(xfer->ep)) {
			ep = USB_CONTROL_EP_OUT;
		} else {
			ep = USB_CONTROL_EP_IN;
		}

		uvb_pkt = uvb_alloc_pkt(UVB_REQUEST_DATA,
					xfer->udev->addr, ep,
					NULL, 0);
		if (uvb_pkt == NULL) {
			LOG_ERR("Failed to allocate UVB packet");
			return -ENOMEM;
		}

		priv->req = UVB_REQUEST_DATA;

		return uvb_advert_pkt(priv->host_node, uvb_pkt);
	}

	return -EINVAL;
}

static int vrt_xfer_bulk(const struct device *dev,
			 struct uhc_transfer *const xfer)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct net_buf *buf = xfer->buf;
	struct uvb_packet *uvb_pkt;
	uint8_t *data;
	size_t length;

	if (USB_EP_DIR_IS_IN(xfer->ep)) {
		length = MIN(net_buf_tailroom(buf), xfer->mps);
		data = net_buf_tail(buf);
	} else {
		length = MIN(buf->len, xfer->mps);
		data = buf->data;
	}

	uvb_pkt = uvb_alloc_pkt(UVB_REQUEST_DATA, xfer->udev->addr, xfer->ep,
				data, length);
	if (uvb_pkt == NULL) {
		LOG_ERR("Failed to allocate UVB packet");
		return -ENOMEM;
	}

	return uvb_advert_pkt(priv->host_node, uvb_pkt);
}

static inline uint8_t get_xfer_ep_idx(const uint8_t ep)
{
	/* We do not need to differentiate the direction for the control
	 * transfers because they are handled as a whole.
	 */
	if (USB_EP_DIR_IS_OUT(ep) || USB_EP_GET_IDX(ep) == 0) {
		return USB_EP_GET_IDX(ep & BIT_MASK(4));
	}

	return USB_EP_GET_IDX(ep & BIT_MASK(4)) + 16U;
}

static void vrt_assemble_frame(const struct device *dev)
{
	struct uhc_vrt_data *const priv = uhc_get_private(dev);
	struct uhc_vrt_frame *const frame = &priv->frame;
	struct uhc_data *const data = dev->data;
	struct uhc_transfer *tmp;
	unsigned int n = 0;
	unsigned int key;
	uint32_t bm = 0;

	sys_dlist_init(&frame->list);
	frame->ptr = NULL;
	frame->count = 0;
	key = irq_lock();

	/* TODO: add periodic transfers up to 90% */
	SYS_DLIST_FOR_EACH_CONTAINER(&data->ctrl_xfers, tmp, node) {
		uint8_t idx = get_xfer_ep_idx(tmp->ep);

		/* There could be multiple transfers queued for the same
		 * endpoint, for now we only allow one to be scheduled per frame.
		 */
		if (bm & BIT(idx)) {
			continue;
		}

		if (tmp->interval) {
			if (tmp->start_frame != priv->frame_number) {
				continue;
			}

			tmp->start_frame = priv->frame_number + tmp->interval;
			LOG_DBG("Interrupt transfer s.f. %u f.n. %u interval %u",
				tmp->start_frame, priv->frame_number, tmp->interval);
		}

		bm |= BIT(idx);
		frame->slots[n].xfer = tmp;
		sys_dlist_append(&frame->list, &frame->slots[n].node);
		n++;

		if (n >= FRAME_MAX_TRANSFERS) {
			/* No more free slots */
			break;
		}
	}

	irq_unlock(key);
}

static int vrt_schedule_frame(const struct device *dev)
{
	struct uhc_vrt_data *const priv = uhc_get_private(dev);
	struct uhc_vrt_frame *const frame = &priv->frame;
	struct uhc_vrt_slot *slot;

	if (priv->last_xfer == NULL) {
		if (frame->count >= FRAME_MAX_TRANSFERS) {
			LOG_DBG("Frame finished");
			return 0;
		}

		frame->ptr = sys_dlist_get(&frame->list);
		slot = SYS_DLIST_CONTAINER(frame->ptr, slot, node);
		if (slot == NULL) {
			LOG_DBG("No more transfers for the frame");
			return 0;
		}

		priv->last_xfer = slot->xfer;
		frame->count++;
		LOG_DBG("Next transfer is %p (count %u)",
			(void *)priv->last_xfer, frame->count);
	}

	if (USB_EP_GET_IDX(priv->last_xfer->ep) == 0) {
		return vrt_xfer_control(dev, priv->last_xfer);
	}

	return vrt_xfer_bulk(dev, priv->last_xfer);
}

static void vrt_hrslt_success(const struct device *dev,
			      struct uvb_packet *const pkt)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct uhc_transfer *const xfer = priv->last_xfer;
	struct net_buf *buf = xfer->buf;
	bool finished = false;
	size_t length;

	switch (pkt->request) {
	case UVB_REQUEST_SETUP:
		if (xfer->buf != NULL) {
			xfer->stage = UHC_CONTROL_STAGE_DATA;
		} else {
			if (xfer->no_status) {
				finished = true;
			} else {
				xfer->stage = UHC_CONTROL_STAGE_STATUS;
			}
		}

		break;
	case UVB_REQUEST_DATA:
		if (xfer->stage == UHC_CONTROL_STAGE_STATUS) {
			LOG_DBG("Status stage finished");
			finished = true;
			break;
		}

		if (USB_EP_DIR_IS_OUT(pkt->ep)) {
			length = MIN(buf->len, xfer->mps);
			net_buf_pull(buf, length);
			LOG_DBG("OUT chunk %zu out of %u", length, buf->len);
			if (buf->len == 0) {
				if (pkt->ep == USB_CONTROL_EP_OUT && !xfer->no_status) {
					xfer->stage = UHC_CONTROL_STAGE_STATUS;
				} else {
					finished = true;
				}
			}
		} else {
			length = MIN(net_buf_tailroom(buf), pkt->length);
			net_buf_add(buf, length);
			if (pkt->length > xfer->mps) {
				LOG_ERR("Ambiguous packet with the length %zu",
					pkt->length);
			}

			LOG_DBG("IN chunk %zu out of %zu", length, net_buf_tailroom(buf));
			if (pkt->length < xfer->mps || !net_buf_tailroom(buf)) {
				if (pkt->ep == USB_CONTROL_EP_IN && !xfer->no_status) {
					xfer->stage = UHC_CONTROL_STAGE_STATUS;
				} else {
					finished = true;
				}
			}
		}
		break;
	}

	if (finished) {
		LOG_DBG("Transfer finished");
		uhc_xfer_return(dev, xfer, 0);
		priv->last_xfer = NULL;
	}
}

static void vrt_xfer_drop_active(const struct device *dev, int err)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	if (priv->last_xfer) {
		uhc_xfer_return(dev, priv->last_xfer, err);
		priv->last_xfer = NULL;
	}
}

static int vrt_handle_reply(const struct device *dev,
			    struct uvb_packet *const pkt)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct uhc_vrt_frame *const frame = &priv->frame;
	struct uhc_transfer *const xfer = priv->last_xfer;
	int ret = 0;

	if (xfer == NULL) {
		LOG_ERR("No transfers to handle");
		ret = -ENODATA;
		goto handle_reply_err;
	}

	switch (pkt->reply) {
	case UVB_REPLY_NACK:
		/* Move the transfer back to the list. */
		sys_dlist_append(&frame->list, frame->ptr);
		priv->last_xfer = NULL;
		LOG_DBG("NACK 0x%02x count %u", xfer->ep, frame->count);
		break;
	case UVB_REPLY_STALL:
		vrt_xfer_drop_active(dev, -EPIPE);
		break;
	case UVB_REPLY_ACK:
		vrt_hrslt_success(dev, pkt);
		break;
	default:
		vrt_xfer_drop_active(dev, -EINVAL);
		ret = -EINVAL;
		break;
	}

handle_reply_err:
	uvb_free_pkt(pkt);
	return ret;
}

static void vrt_xfer_cleanup_cancelled(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;
	struct uhc_transfer *tmp;

	if (priv->last_xfer != NULL && priv->last_xfer->err == -ECONNRESET) {
		vrt_xfer_drop_active(dev, -ECONNRESET);
	}

	SYS_DLIST_FOR_EACH_CONTAINER(&data->ctrl_xfers, tmp, node) {
		if (tmp->err == -ECONNRESET) {
			uhc_xfer_return(dev, tmp, -ECONNRESET);
		}
	}
}

static void xfer_work_handler(struct k_work *work)
{
	struct uhc_vrt_data *priv = CONTAINER_OF(work, struct uhc_vrt_data, work);
	const struct device *dev = priv->dev;
	struct uhc_vrt_event *ev;

	while ((ev = k_fifo_get(&priv->fifo, K_NO_WAIT)) != NULL) {
		bool schedule = false;
		int err;

		switch (ev->type) {
		case UHC_VRT_EVT_SOF:
			priv->frame_number++;
			vrt_xfer_cleanup_cancelled(dev);
			vrt_assemble_frame(dev);
			schedule = true;
			break;
		case UHC_VRT_EVT_REPLY:
			err = vrt_handle_reply(dev, ev->pkt);
			if (unlikely(err)) {
				uhc_submit_event(dev, UHC_EVT_ERROR, err);
			}

			schedule = true;
			break;
		default:
			break;
		}

		if (schedule) {
			err = vrt_schedule_frame(dev);
			if (unlikely(err)) {
				uhc_submit_event(dev, UHC_EVT_ERROR, err);
			}

		}

		k_mem_slab_free(&uhc_vrt_slab, (void *)ev);
	}
}

static void sof_timer_handler(struct k_timer *timer)
{
	struct uhc_vrt_data *priv = CONTAINER_OF(timer, struct uhc_vrt_data, sof_timer);

	vrt_event_submit(priv->dev, UHC_VRT_EVT_SOF, NULL);
}

static void vrt_device_act(const struct device *dev,
			   const enum uvb_device_act act)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	enum uhc_event_type type;

	switch (act) {
	case UVB_DEVICE_ACT_RWUP:
		type = UHC_EVT_RWUP;
		break;
	case UVB_DEVICE_ACT_FS:
		type = UHC_EVT_DEV_CONNECTED_FS;
		k_timer_start(&priv->sof_timer, K_MSEC(1), K_MSEC(1));
		break;
	case UVB_DEVICE_ACT_HS:
		type = UHC_EVT_DEV_CONNECTED_HS;
		k_timer_start(&priv->sof_timer, K_MSEC(1), K_USEC(125));
		break;
	case UVB_DEVICE_ACT_REMOVED:
		type = UHC_EVT_DEV_REMOVED;
		break;
	default:
		type = UHC_EVT_ERROR;
	}

	uhc_submit_event(dev, type, 0);
}

static void uhc_vrt_uvb_cb(const void *const vrt_priv,
			   const enum uvb_event_type type,
			   const void *data)
{
	const struct device *dev = vrt_priv;

	if (type == UVB_EVT_REPLY) {
		vrt_event_submit(dev, UHC_VRT_EVT_REPLY, data);
	} else if (type == UVB_EVT_DEVICE_ACT) {
		vrt_device_act(dev, POINTER_TO_INT(data));
	} else {
		LOG_ERR("Unknown event %d for %p", type, dev);
	}
}

static int uhc_vrt_sof_enable(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	k_timer_start(&priv->sof_timer, K_MSEC(1), K_MSEC(1));

	return 0;
}

/* Disable SOF generator and suspend bus */
static int uhc_vrt_bus_suspend(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	k_timer_stop(&priv->sof_timer);

	return uvb_advert(priv->host_node, UVB_EVT_SUSPEND, NULL);
}

static int uhc_vrt_bus_reset(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	int ret;

	k_timer_stop(&priv->sof_timer);
	ret = uvb_advert(priv->host_node, UVB_EVT_RESET, NULL);
	/* TDRSTR */
	k_msleep(50);
	k_timer_start(&priv->sof_timer, K_MSEC(1), K_MSEC(1));

	return ret;
}

static int uhc_vrt_bus_resume(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	k_timer_start(&priv->sof_timer, K_MSEC(1), K_MSEC(1));

	return uvb_advert(priv->host_node, UVB_EVT_RESUME, NULL);
}

static int uhc_vrt_enqueue(const struct device *dev,
			   struct uhc_transfer *const xfer)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	if (xfer->interval) {
		xfer->start_frame = priv->frame_number + xfer->interval;
		LOG_DBG("New interrupt transfer s.f. %u f.n. %u interval %u",
			xfer->start_frame, priv->frame_number, xfer->interval);
	}

	uhc_xfer_append(dev, xfer);

	return 0;
}

static int uhc_vrt_dequeue(const struct device *dev,
			    struct uhc_transfer *const xfer)
{
	struct uhc_data *data = dev->data;
	struct uhc_transfer *tmp;
	unsigned int key;

	key = irq_lock();

	SYS_DLIST_FOR_EACH_CONTAINER(&data->ctrl_xfers, tmp, node) {
		if (xfer == tmp) {
			tmp->err = -ECONNRESET;
		}
	}

	irq_unlock(key);

	return 0;
}

static int uhc_vrt_init(const struct device *dev)
{
	return 0;
}

static int uhc_vrt_enable(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	return uvb_advert(priv->host_node, UVB_EVT_VBUS_READY, NULL);
}

static int uhc_vrt_disable(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);

	return uvb_advert(priv->host_node, UVB_EVT_VBUS_REMOVED, NULL);
}

static int uhc_vrt_shutdown(const struct device *dev)
{
	return 0;
}

static int uhc_vrt_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_vrt_unlock(const struct device *dev)
{

	return uhc_unlock_internal(dev);
}

static int uhc_vrt_driver_preinit(const struct device *dev)
{
	struct uhc_vrt_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;

	priv->dev = dev;
	k_mutex_init(&data->mutex);

	priv->host_node->priv = dev;
	k_fifo_init(&priv->fifo);
	k_work_init(&priv->work, xfer_work_handler);
	k_timer_init(&priv->sof_timer, sof_timer_handler, NULL);

	LOG_DBG("Virtual UHC pre-initialized");

	return 0;
}

static const struct uhc_api uhc_vrt_api = {
	.lock = uhc_vrt_lock,
	.unlock = uhc_vrt_unlock,
	.init = uhc_vrt_init,
	.enable = uhc_vrt_enable,
	.disable = uhc_vrt_disable,
	.shutdown = uhc_vrt_shutdown,

	.bus_reset = uhc_vrt_bus_reset,
	.sof_enable  = uhc_vrt_sof_enable,
	.bus_suspend = uhc_vrt_bus_suspend,
	.bus_resume = uhc_vrt_bus_resume,

	.ep_enqueue = uhc_vrt_enqueue,
	.ep_dequeue = uhc_vrt_dequeue,
};

#define DT_DRV_COMPAT zephyr_uhc_virtual

#define UHC_VRT_DEVICE_DEFINE(n)						\
	UVB_HOST_NODE_DEFINE(uhc_bc_##n,					\
			     DT_NODE_FULL_NAME(DT_DRV_INST(n)),			\
			     uhc_vrt_uvb_cb);					\
										\
	static const struct uhc_vrt_config uhc_vrt_config_##n = {		\
	};									\
										\
	static struct uhc_vrt_data uhc_priv_##n = {				\
		.host_node = &uhc_bc_##n,					\
	};									\
										\
	static struct uhc_data uhc_data_##n = {					\
		.priv = &uhc_priv_##n,						\
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, uhc_vrt_driver_preinit, NULL,			\
			      &uhc_data_##n, &uhc_vrt_config_##n,		\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &uhc_vrt_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_VRT_DEVICE_DEFINE)
