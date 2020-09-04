/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "multi_instance.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_DRIVER)
#define LOG_MODULE_NAME bt_hci_driver_nrf53
#include "common/log.h"

#define SHM_NODE            DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR      DT_REG_ADDR(SHM_NODE)
#define VRING_SIZE          16

BUILD_ASSERT(CONFIG_HEAP_MEM_POOL_SIZE >= 1024,
	"Not enough heap memory for RPMsg queue allocation");

void bt_rpmsg_rx(uint8_t *data, size_t len);

static K_SEM_DEFINE(ready_sem, 0, 1);
static K_SEM_DEFINE(rx_sem, 0, 1);

static K_KERNEL_STACK_DEFINE(bt_rpmsg_rx_thread_stack,
			     CONFIG_BT_RPMSG_NRF53_RX_STACK_SIZE);
static struct k_thread bt_rpmsg_rx_thread_data;

struct ipc_inst_t ipc = { 0 };
struct ipc_ept_t ep = { 0 };

static void msg_notification_cb(void *arg)
{
	BT_DBG("New message in the queue");
	k_sem_give(&rx_sem);
}

static void ep_handler(ipc_event_t evt, const void *data, size_t len,
		       void *arg)
{
	switch (evt) {
	case IPC_EVENT_CONNECTED:
		BT_DBG("Remote side has connected");
		k_sem_give(&ready_sem);
		break;

	case IPC_EVENT_DATA:
		BT_DBG("Received message of %u bytes.", len);
		BT_HEXDUMP_DBG((uint8_t *)data, len, "Data:");

		bt_rpmsg_rx((uint8_t *) data, len);
		break;

	default:
		break;
	}
}

const struct ipc_config_t ipc_conf =
{
    .ipm_name_rx = "IPM_1",
    .ipm_name_tx = "IPM_0",
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

static void bt_rpmsg_rx_thread(void *p1, void *p2, void *p3)
{
	int err;
	uint8_t payload[256] = { 0 };

	while (1) {
		k_sem_take(&rx_sem, K_FOREVER);

		err = ipc_recv(&ipc, payload);
		if (err == -ENOENT) {
			BT_DBG("RX: empty message");
		} else if (err) {
			BT_ERR("RX error: %d", err);
		} else {
			ipc_free(&ipc);
		}

		k_yield();
	}
}

int bt_rpmsg_platform_init(void)
{
	int err;

	/* Setup thread for RX data processing. */
	k_thread_create(&bt_rpmsg_rx_thread_data, bt_rpmsg_rx_thread_stack,
			K_KERNEL_STACK_SIZEOF(bt_rpmsg_rx_thread_stack),
			bt_rpmsg_rx_thread, NULL, NULL, NULL,
			K_PRIO_COOP(CONFIG_BT_RPMSG_NRF53_RX_PRIO),
			0, K_NO_WAIT);

	/* Initialize IPC instances. */
	err = ipc_init(&ipc, &ipc_conf, msg_notification_cb, NULL);
	if (err)
	{
		BT_ERR("IPC: Error during init: %d", err);
		return err;
	}

	/* Initialize Endpoints. */
	err = ipc_ept_init(&ipc, &ep, &ept_conf);
	if (err) {
		BT_ERR("EP: Error during init: %d", err);
		return err;
	}

	/* Wait til nameservice ep is setup */
	k_sem_take(&ready_sem, K_FOREVER);

	BT_DBG("Initialization is completed");

	return 0;
}

int bt_rpmsg_platform_send(struct net_buf *buf)
{
	return ipc_send(&ep, buf->data, buf->len);
}
