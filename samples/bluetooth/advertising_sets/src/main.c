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
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <zephyr.h>

#include <settings/settings.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

static const struct bt_data ad_set_0[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HRS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_CTS_VAL)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
		      0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12),
};

static const struct bt_data ad_set_1[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err 0x%02x)\n", err);
	} else {
		printk("Connected\n");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason 0x%02x)\n", reason);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

static void ext_adv_connected(struct bt_le_ext_adv *adv,
			      struct bt_le_ext_adv_connected_info *info)
{
	printk("Connected with %d advertising set\n",
		bt_le_ext_adv_get_index(adv));
}


static struct bt_le_ext_adv_cb ext_adv_callbacks = {
	.connected = ext_adv_connected,
};

static int bt_ext_advertising_start(struct bt_le_adv_param *param,
				    const struct bt_data *ad, size_t ad_len)
{
	int err;
	struct bt_le_ext_adv_start_param ext_adv_start_param = {0};
	struct bt_le_ext_adv *adv_set;

	err = bt_le_ext_adv_create(param, &ext_adv_callbacks, &adv_set);
	if (err) {
		printk("Could not create %d advertising set (err %d)\n",
			param->sid, err);
		return err;	
	}

	err = bt_le_ext_adv_set_data(adv_set, ad, ad_len, NULL, 0);
	if (err) {
		printk("Could not set data for %d advertising set (err %d)\n",
			param->sid, err);
		return err;	
	}

	err = bt_le_ext_adv_start(adv_set, &ext_adv_start_param);
	if (err) {
		printk("Advertising for set %d failed to start (err %d)\n",
			param->sid, err);
		return err;	
	}

	printk("Extended advertising with set %d successfully started\n",
		param->sid);

	return err;
}

static void bt_ready(void)
{
	int err;
	bt_addr_le_t addrs[10];
	size_t count;
	char addr_str[BT_ADDR_LE_STR_LEN];
	struct bt_le_adv_param param = {0};

	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	/* Create identity if necessary. */
	count = ARRAY_SIZE(addrs);
	bt_id_get(addrs, &count);
	if (count < 2) {
		err = bt_id_create(NULL, NULL);
		if (err < 0) {
			printk("ID creation failed (err %d)\n", err);
			return;
		} else {
			printk("Identity identifier: %d\n", err);
		}
	}

	/* Read the identitiy list. */
	count = ARRAY_SIZE(addrs);
	bt_id_get(addrs, &count);
	for (int i = 0; i < count; i++) {
		bt_addr_le_to_str(&addrs[i], addr_str, sizeof(addr_str));

		printk("Identity %d: %s\n", i, addr_str);
	}

	if (count != 2) {
		printk("Wrong number of identities\n");
		return;
	}

	/* Connectable set. */
	param.interval_min = BT_GAP_ADV_FAST_INT_MIN_2;
	param.interval_max = BT_GAP_ADV_FAST_INT_MAX_2;
	param.options =
		BT_LE_ADV_OPT_CONNECTABLE |
		BT_LE_ADV_OPT_USE_NAME |
		BT_LE_ADV_OPT_USE_IDENTITY;
	err = bt_ext_advertising_start(&param, ad_set_0, ARRAY_SIZE(ad_set_0));
	if (err) {
		return;
	}

	/* Non-connectable set. */
	param.id = 1;
	param.sid = 1;
	param.options =
		BT_LE_ADV_OPT_USE_NAME |
		BT_LE_ADV_OPT_USE_IDENTITY;
	err = bt_ext_advertising_start(&param, ad_set_1, ARRAY_SIZE(ad_set_1));
	if (err) {
		return;
	}
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
	.passkey_display = auth_passkey_display,
	.passkey_entry = NULL,
	.cancel = auth_cancel,
};

void main(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	bt_ready();

	bt_conn_cb_register(&conn_callbacks);
	bt_conn_auth_cb_register(&auth_cb_display);
}
