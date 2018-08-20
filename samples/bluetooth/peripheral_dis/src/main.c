/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <misc/printk.h>
#include <misc/byteorder.h>
#include <zephyr.h>
#include <gpio.h>
#include <board.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <gatt/dis.h>

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x0a, 0x18),
};

static const bt_addr_le_t peer_addr = {
	.type = BT_ADDR_LE_RANDOM,
	.a = {
		//.val = {0xE2, 0x4E, 0x39, 0x95, 0xC1, 0xC9}
		.val = {0xC9, 0xC1, 0x95, 0x39, 0x4E, 0xE2}
	}
};

struct bt_conn		   *default_conn;

static struct k_delayed_work advertising_work;
static struct k_delayed_work disconnect_work;
static bool is_high_duty = false;

static void advertising_start(void)
{
	//struct bt_le_adv_param *param = BT_LE_ADV_CONN;
	//param->options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME;
	//if (is_high_duty) {
	//	param->options |= BT_LE_ADV_OPT_DIR_MODE; 
	//} else {
	//	param->options |= BT_LE_ADV_OPT_DIR_MODE_LOW_DUTY; 
	//}
	//param->peer_addr = &peer_addr;
	//default_conn= bt_conn_create_slave_le(param);
	if (is_high_duty) {
		default_conn = bt_conn_create_slave_le(&peer_addr,
				BT_LE_ADV_CONN_DIR);
	} else {
		default_conn = bt_conn_create_slave_le(&peer_addr,
				BT_LE_ADV_CONN_DIR_LOW_DUTY);
	}

	//bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (!default_conn) {
		printk("Advertising failed to start\n");
		return;
	}

	printk("Advertising successfully started: %08X\n", (u32_t) default_conn);
}

static void advertising_init_wrapper(struct k_work *work)
{
	if (!default_conn) {
		advertising_start();
	}
}

static void connected(struct bt_conn *conn, u8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
		printk("Failed to connect to %s (%u)\n", addr, err);
		return;
	}

	printk("Connected %s\n", addr);
	printk("Conn: %08X\n", (u32_t) conn);
}


static void disconnected(struct bt_conn *conn, u8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s (reason %u)\n", addr, reason);
	bt_conn_unref(default_conn);
	default_conn = NULL;

	//advertising_start();
	k_delayed_work_submit(&advertising_work, 100);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

static void disconnect_init_wrapper(struct k_work *work)
{
	if (default_conn) {
		printk("%s(): Disconnecting\n", __func__);
		bt_conn_disconnect(default_conn, BT_HCI_ERR_CONN_ACCEPT_TIMEOUT);
		printk("%s(): Disconnected\n", __func__);
	}
}

static struct device       *gpio_devs[4];
static struct gpio_callback gpio_cbs[4];

void button_pressed(struct device *gpio_dev, struct gpio_callback *cb,
		    u32_t pins)
{
	if (pins & (1 << SW0_GPIO_PIN)) {
		printk("%s(): Disconnecting req\n", __func__);
		k_delayed_work_submit(&disconnect_work, 100);
	}
	if (pins & (1 << SW1_GPIO_PIN)) {
		printk("%s(): Low Advertising req\n", __func__);
		is_high_duty = false;
		k_delayed_work_submit(&advertising_work, 100);
	}
	if (pins & (1 << SW2_GPIO_PIN)) {
		printk("%s(): High Advertising req\n", __func__);
		is_high_duty = true;
		k_delayed_work_submit(&advertising_work, 100);
	}
}

void configure_buttons(void)
{
	static const u32_t pin_id[4] = { SW0_GPIO_PIN, SW1_GPIO_PIN,
		SW2_GPIO_PIN, SW3_GPIO_PIN };
	static const char *port_name[4] = { SW0_GPIO_NAME, SW1_GPIO_NAME,
		SW2_GPIO_NAME, SW3_GPIO_NAME };

	for (size_t i = 0; i < ARRAY_SIZE(pin_id); i++) {
		gpio_devs[i] = device_get_binding(port_name[i]);
		if (gpio_devs[i]) {
			printk("%s(): port %zu bound\n", __func__, i);

			gpio_pin_configure(gpio_devs[i], pin_id[i],
					   GPIO_PUD_PULL_UP | GPIO_DIR_IN |
					   GPIO_INT | GPIO_INT_EDGE |
					   GPIO_INT_ACTIVE_LOW);
			gpio_init_callback(&gpio_cbs[i], button_pressed,
					   BIT(pin_id[i]));
			gpio_add_callback(gpio_devs[i], &gpio_cbs[i]);
			gpio_pin_enable_callback(gpio_devs[i], pin_id[i]);
		}
	}
}

void main(void)
{
	int err;

	configure_buttons();
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");
	k_delayed_work_init(&advertising_work, advertising_init_wrapper);
	k_delayed_work_init(&disconnect_work, disconnect_init_wrapper);
	dis_init(CONFIG_SOC, "Manufacturer");

	bt_conn_cb_register(&conn_callbacks);

	advertising_start();
}
