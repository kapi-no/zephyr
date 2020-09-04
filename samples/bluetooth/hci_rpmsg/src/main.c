/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr.h>
#include <arch/cpu.h>
#include <sys/byteorder.h>
#include <logging/log.h>
#include <sys/util.h>

#include <net/buf.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/buf.h>
#include <bluetooth/hci_raw.h>

#include "multi_instance.h"

#define LOG_LEVEL LOG_LEVEL_DBG
#define LOG_MODULE_NAME hci_rpmsg
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "Sample requires definition of shared memory for rpmsg"
#endif

#define SHM_NODE          DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR    DT_REG_ADDR(SHM_NODE)
#define VRING_SIZE        16

static K_THREAD_STACK_DEFINE(tx_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
static struct k_thread tx_thread_data;
static K_FIFO_DEFINE(tx_queue);

struct ipc_inst_t ipc = { 0 };
struct ipc_ept_t ep = { 0 };

K_SEM_DEFINE(rx_sem, 0, 1); /**< Semaphore used for TX sync. */
bool ep_binded = 0; /**< Binded indication of first instance. */

#define HCI_RPMSG_CMD 0x01
#define HCI_RPMSG_ACL 0x02
#define HCI_RPMSG_SCO 0x03
#define HCI_RPMSG_EVT 0x04

static struct net_buf *hci_rpmsg_cmd_recv(uint8_t *data, size_t remaining)
{
	struct bt_hci_cmd_hdr *hdr = (void *)data;
	struct net_buf *buf;

	if (remaining < sizeof(*hdr)) {
		LOG_ERR("Not enought data for command header");
		return NULL;
	}

	buf = bt_buf_get_tx(BT_BUF_CMD, K_NO_WAIT, hdr, sizeof(*hdr));
	if (buf) {
		data += sizeof(*hdr);
		remaining -= sizeof(*hdr);
	} else {
		LOG_ERR("No available command buffers!");
		return NULL;
	}

	if (remaining != hdr->param_len) {
		LOG_ERR("Command payload length is not correct");
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("len %u", hdr->param_len);
	net_buf_add_mem(buf, data, remaining);

	return buf;
}

static struct net_buf *hci_rpmsg_acl_recv(uint8_t *data, size_t remaining)
{
	struct bt_hci_acl_hdr *hdr = (void *)data;
	struct net_buf *buf;

	if (remaining < sizeof(*hdr)) {
		LOG_ERR("Not enought data for ACL header");
		return NULL;
	}

	buf = bt_buf_get_tx(BT_BUF_ACL_OUT, K_NO_WAIT, hdr, sizeof(*hdr));
	if (buf) {
		data += sizeof(*hdr);
		remaining -= sizeof(*hdr);
	} else {
		LOG_ERR("No available ACL buffers!");
		return NULL;
	}

	if (remaining != sys_le16_to_cpu(hdr->len)) {
		LOG_ERR("ACL payload length is not correct");
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("len %u", remaining);
	net_buf_add_mem(buf, data, remaining);

	return buf;
}

static void hci_rpmsg_rx(uint8_t *data, size_t len)
{
	uint8_t pkt_indicator;
	struct net_buf *buf = NULL;
	size_t remaining = len;

	LOG_HEXDUMP_DBG(data, len, "RPMSG data:");

	pkt_indicator = *data++;
	remaining -= sizeof(pkt_indicator);

	switch (pkt_indicator) {
	case HCI_RPMSG_CMD:
		buf = hci_rpmsg_cmd_recv(data, remaining);
		break;

	case HCI_RPMSG_ACL:
		buf = hci_rpmsg_acl_recv(data, remaining);
		break;

	default:
		LOG_ERR("Unknown HCI type %u", pkt_indicator);
		return;
	}

	if (buf) {
		net_buf_put(&tx_queue, buf);

		LOG_HEXDUMP_DBG(buf->data, buf->len, "Final net buffer:");
	}
}

static void tx_thread(void *p1, void *p2, void *p3)
{
	while (1) {
		struct net_buf *buf;
		int err;

		/* Wait until a buffer is available */
		buf = net_buf_get(&tx_queue, K_FOREVER);
		/* Pass buffer to the stack */
		err = bt_send(buf);
		if (err) {
			LOG_ERR("Unable to send (err %d)", err);
			net_buf_unref(buf);
		} else {
			LOG_HEXDUMP_DBG(buf->data, buf->len, "Sending buffer to:");
		}

		/* Give other threads a chance to run if tx_queue keeps getting
		 * new data all the time.
		 */
		k_yield();
	}
}

static int hci_rpmsg_send(struct net_buf *buf)
{
	uint8_t pkt_indicator;

	LOG_DBG("buf %p type %u len %u", buf, bt_buf_get_type(buf),
		buf->len);

	LOG_HEXDUMP_DBG(buf->data, buf->len, "Controller buffer:");

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_ACL_IN:
		pkt_indicator = HCI_RPMSG_ACL;
		break;
	case BT_BUF_EVT:
		pkt_indicator = HCI_RPMSG_EVT;
		break;
	default:
		LOG_ERR("Unknown type %u", bt_buf_get_type(buf));
		net_buf_unref(buf);
		return -EINVAL;
	}
	net_buf_push_u8(buf, pkt_indicator);

	LOG_HEXDUMP_DBG(buf->data, buf->len, "Final HCI buffer:");
	ipc_send(&ep, buf->data, buf->len);

	net_buf_unref(buf);

	return 0;
}

#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER)
void bt_ctlr_assert_handle(char *file, uint32_t line)
{
	LOG_ERR("Controller assert in: %s at %d", file, line);
}
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER */

static void msg_notification_cb(void *arg)
{
	k_sem_give(&rx_sem);
}

static void ep_handler(ipc_event_t evt, const void *data, size_t len,
		       void *arg)
{
	switch (evt) {
	case IPC_EVENT_CONNECTED:
		ep_binded = true;
		break;

	case IPC_EVENT_DATA:
		LOG_INF("Received message of %u bytes.", len);
		hci_rpmsg_rx((uint8_t *) data, len);
		break;

	default:
		break;
	}
}

const struct ipc_config_t ipc_conf =
{
    .ipm_name_rx = "IPM_0",
    .ipm_name_tx = "IPM_1",
    .vring_size = VRING_SIZE,
    .shmem_addr = SHMEM_INST_ADDR_GET(SHM_START_ADDR, VRING_SIZE, 0),
    .shmem_size = SHMEM_INST_SIZE_GET(VRING_SIZE),
};
struct ipc_ept_config_t ept_conf =
{
    .ept_no = 0,
    .cb = ep_handler,
    .arg = &ep,
};

void ipc_thread(void * arg1, void * arg2, void * arg3)
{
	uint8_t payload[256] = { 0 };

	LOG_INF("RPMsg processing thread has started");

	while (1) {
		k_sem_take(&rx_sem, K_FOREVER);

		ipc_recv(&ipc, payload);
		ipc_free(&ipc);

		k_yield();
	}
}

static int hci_rpmsg_init(void)
{
	int err;

	/* Initialize IPC instances. */
	err = ipc_init(&ipc, &ipc_conf, msg_notification_cb, NULL);
	if (err)
	{
		LOG_ERR("IPC: Error during init: %d", err);
		return err;
	}

	/* Initialize Endpoints. */
	err = ipc_ept_init(&ipc, &ep, &ept_conf);
	if (err) {
		LOG_ERR("EP: Error during init: %d", err);
		return err;
	}

	return 0;
}

void main(void)
{
	int err;

	/* incoming events and data from the controller */
	static K_FIFO_DEFINE(rx_queue);

	LOG_INF("Controller RX processing thread has started.");

	/* initialize RPMSG */
	err = hci_rpmsg_init();
	if (err != 0) {
		return;
	}

	LOG_DBG("Start");

	/* Enable the raw interface, this will in turn open the HCI driver */
	bt_enable_raw(&rx_queue);

	/* Spawn the TX thread and start feeding commands and data to the
	 * controller
	 */
	k_thread_create(&tx_thread_data, tx_thread_stack,
			K_THREAD_STACK_SIZEOF(tx_thread_stack), tx_thread,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "HCI rpmsg TX");

	while (1) {
		struct net_buf *buf;

		buf = net_buf_get(&rx_queue, K_FOREVER);
		err = hci_rpmsg_send(buf);
		if (err) {
			LOG_ERR("Failed to send (err %d)", err);
		}
	}
}

K_THREAD_DEFINE(tid_1, 1024, ipc_thread, NULL, NULL, NULL, 7, 0, 0);
