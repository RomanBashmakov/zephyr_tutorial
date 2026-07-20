/*
 * Copyright (c) 2016 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Демонстрация совмещённой работы:
 *   1. LoRa-трансивер SX1272 по SPI (Zephyr LoRa subsystem).
 *   2. Bluetooth LE HID-клавиатуры "stm32_button".
 *
 * По нажатию механической кнопки (sw0 / BOOT на PH3):
 *   - зажигается светодиод led0;
 *   - по BLE HID отправляется нажатие клавиши Enter,
 *     а при отжатии — отпускание (нуль-отчёт).
 *
 * Имя Bluetooth-устройства задаётся в prj.conf:
 *   CONFIG_BT_DEVICE_NAME="stm32_button"
 *
 * Радиомодуль STM32WB55 (CPU2) подключается через встроенный
 * IPM-драйвер (drivers/bluetooth/hci/ipm_stm32wb.c); узел ble_rf
 * уже включён в dts-include stm32wb.dtsi и выбран через
 * zephyr,bt-hci = &ble_rf, поэтому CONFIG_BT_STM32_IPM
 * активируется автоматически при CONFIG_BT=y.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

/* Низкоуровневый API модуля loramac-node: Radio.Read()/Write() поверх SPI. */
#include <radio.h>

/* ------------------------------------------------------------------ *
 *  SX1272: адреса регистров и константы                               *
 * ------------------------------------------------------------------ */
#define SX1272_REG_LR_OPMODE         0x01
#define SX1272_REG_LR_FRFMSB         0x06
#define SX1272_REG_LR_FRFMID         0x07
#define SX1272_REG_LR_FRFLSB         0x08
#define SX1272_REG_LR_PACONFIG       0x09
#define SX1272_REG_LR_LNA            0x0C
#define SX1272_REG_LR_MODEMCONFIG1   0x1D
#define SX1272_REG_LR_MODEMCONFIG2   0x1E
#define SX1272_REG_LR_SYNCWORD       0x39
#define SX1272_REG_LR_VERSION        0x42
#define SX1272_VERSION_ID            0x22

#define LORA_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE),
	     "Не найден узел LoRa (alias lora0) в devicetree. "
	     "Проверьте app.overlay: узел sx1272 с compatible = \"semtech,sx1272\".");

/* ------------------------------------------------------------------ *
 *  Механическая кнопка (alias sw0) + антидребезг                      *
 *  sw0 = boot_button на PH3 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN).      *
 * ------------------------------------------------------------------ */
#define SW0_NODE DT_ALIAS(sw0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SW0_NODE),
	     "Не найден узел кнопки (alias sw0) в devicetree. "
	     "Проверьте DTS платы: узел gpio-keys с алиасом sw0.");

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* Светодиод led0 (синий User LED на PE4). */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define BUTTON_DEBOUNCE_MS  20
static struct k_work_delayable button_debounce_work;

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* ================================================================== *
 *  Bluetooth LE HID (клавиатура)                                      *
 * ================================================================== */

/* HID Usage ID клавиши Enter (Keyboard/Keypad Page, Usage 0x28). */
#define HID_KEY_ENTER  0x28

/* Отчёт HID-клавиатуры:
 *   byte 0 — модификаторы (Ctrl/Shift/Alt/GUI),
 *   byte 1 — зарезервирован,
 *   bytes 2..7 — до 6 одновременно нажатых клавиш.
 */
struct hid_keyboard_report {
	uint8_t modifiers;
	uint8_t reserved;
	uint8_t key[6];
} __packed;

/* HIDS Information: версия USB HID Spec, код страны, флаги. */
enum {
	HIDS_REMOTE_WAKE = BIT(0),
	HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

struct hids_info {
	uint16_t version; /* версия базовой спецификации USB HID */
	uint8_t code;     /* код локализации страны */
	uint8_t flags;
} __packed;

struct hids_report {
	uint8_t id;   /* идентификатор отчёта */
	uint8_t type; /* тип отчёта: Input/Output/Feature */
} __packed;

enum {
	HIDS_INPUT = 0x01,
	HIDS_OUTPUT = 0x02,
	HIDS_FEATURE = 0x03,
};

static struct hids_info info = {
	.version = 0x0000,
	.code = 0x00,
	.flags = HIDS_NORMALLY_CONNECTABLE,
};

static struct hids_report input_report_ref = {
	.id = 0x01,
	.type = HIDS_INPUT,
};

/* Дескриптор отчёта (Report Map) для стандартной HID-клавиатуры.
 * Описывает одно приложение Usage(Keyboard) с:
 *   - байтом модификаторов (8 бит),
 *   - байтом-заполнителем,
 *   - массивом из 6 кодов клавиш.
 */
static const uint8_t report_map[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */
	0x85, 0x01, /*   Report Id (1) */
	0x05, 0x07, /*   Usage Page (Keyboard/Keypad) */
	0x19, 0xE0, /*   Usage Minimum (0xE0 - Left Control) */
	0x29, 0xE7, /*   Usage Maximum (0xE7 - Right GUI) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data,Var,Abs) - модификаторы */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x08, /*   Report Size (8) */
	0x81, 0x01, /*   Input (Cnst,Arr,Abs) - зарезервировано */
	0x95, 0x05, /*   Report Count (5) */
	0x75, 0x01, /*   Report Size (1) */
	0x05, 0x08, /*   Usage Page (LEDs) */
	0x19, 0x01, /*   Usage Minimum (Num Lock) */
	0x29, 0x05, /*   Usage Maximum (Kana) */
	0x91, 0x02, /*   Output (Data,Var,Abs) - индикаторы */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x03, /*   Report Size (3) */
	0x91, 0x01, /*   Output (Cnst,Arr,Abs) - заполнитель */
	0x95, 0x06, /*   Report Count (6) */
	0x75, 0x08, /*   Report Size (8) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x73, /*   Logical Maximum (0x73) */
	0x05, 0x07, /*   Usage Page (Keyboard/Keypad) */
	0x19, 0x00, /*   Usage Minimum (0) */
	0x29, 0x73, /*   Usage Maximum (0x73) */
	0x81, 0x00, /*   Input (Data,Arr) - массив клавиш */
	0xC0,       /* End Collection */
};

/* Текущее состояние клавиш (отправляется по notify). */
static struct hid_keyboard_report keyboard_state;

/* Признак того, что хост включил уведомления (CCC = NOTIFY). */
static bool hid_notify_enabled;

/* Активное подключение (для bt_gatt_notify). */
static struct bt_conn *default_conn;

/* Разрешения: HID требует шифрованного канала, но не обязательно
 * аутентифицированного. Используем READ_ENCRYPT/WRITE_ENCRYPT,
 * чтобы не усложнять спаривание (Just Works).
 */
#define HIDS_PERM_READ  BT_GATT_PERM_READ_ENCRYPT
#define HIDS_PERM_WRITE BT_GATT_PERM_WRITE_ENCRYPT

/* --- Callback'и чтения/записи атрибутов HIDS --- */

static ssize_t read_hids_info(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr,
			      void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 attr->user_data, sizeof(struct hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 (void *)report_map, sizeof(report_map));
}

static ssize_t read_input_report(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &keyboard_state, sizeof(keyboard_state));
}

static ssize_t read_report_ref(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 attr->user_data, sizeof(struct hids_report));
}

static void input_ccc_changed(const struct bt_gatt_attr *attr,
			      uint16_t value)
{
	hid_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("HID notify %s", hid_notify_enabled ? "включён" : "выключен");
}

static ssize_t write_ctrl_point(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len,
				uint16_t offset, uint8_t flags)
{
	/* Control Point используется хостом для команд suspend/resume.
	 * Принимаем значение, но не обрабатываем детально.
	 */
	uint8_t *val = attr->user_data;

	if (offset + len > sizeof(uint8_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	memcpy(val + offset, buf, len);
	return len;
}

static uint8_t ctrl_point;

/* HID Service Declaration.
 * Порядок атрибутов важен: индекс входного отчёта (с notify) используется
 * в button_debounce_handler для bt_gatt_notify().
 *
 *   attrs[0]  - Primary Service
 *   attrs[1]  - Characteristic decl (Info)
 *   attrs[2]  - Info value
 *   attrs[3]  - Characteristic decl (Report Map)
 *   attrs[4]  - Report Map value
 *   attrs[5]  - Characteristic decl (Report, read+notify)
 *   attrs[6]  - Report value   <-- notify отправляем сюда
 *   attrs[7]  - CCC descriptor
 *   attrs[8]  - Report Reference descriptor
 *   attrs[9]  - Characteristic decl (Control Point)
 *   attrs[10] - Control Point value
 */
BT_GATT_SERVICE_DEFINE(hids_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO,
			       BT_GATT_CHRC_READ,
			       HIDS_PERM_READ,
			       read_hids_info, NULL, &info),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP,
			       BT_GATT_CHRC_READ,
			       HIDS_PERM_READ,
			       read_report_map, NULL, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       HIDS_PERM_READ,
			       read_input_report, NULL, &keyboard_state),
	BT_GATT_CCC(input_ccc_changed,
		    HIDS_PERM_READ | HIDS_PERM_WRITE),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF,
			   BT_GATT_PERM_READ,
			   read_report_ref, NULL, &input_report_ref),

	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT,
			       NULL, write_ctrl_point, &ctrl_point),
);

/* Индекс атрибута значения входного отчёта в массиве hids_svc.attrs[]
 * (см. комментарий выше). bt_gatt_notify() принимает указатель на атрибут.
 */
#define HIDS_INPUT_REPORT_ATTR_IDX  6

/* Отправка HID-отчёта хосту. Если ключ == 0 — это отпускание всех клавиш. */
static void hid_send_key(uint8_t hid_key)
{
	if (!default_conn || !hid_notify_enabled) {
		LOG_DBG("HID-отправка пропущена: нет подключения/notify");
		return;
	}

	memset(&keyboard_state, 0, sizeof(keyboard_state));
	if (hid_key != 0) {
		keyboard_state.key[0] = hid_key;
	}

	int err = bt_gatt_notify(default_conn,
				 &hids_svc.attrs[HIDS_INPUT_REPORT_ATTR_IDX],
				 &keyboard_state, sizeof(keyboard_state));
	if (err) {
		LOG_WRN("bt_gatt_notify() failed: %d", err);
	}
}

/* ------------------------------------------------------------------ *
 *  Advertising и callbacks подключения                                *
 * ------------------------------------------------------------------ */

/* Advertising data: флаги LE + UUID HID-сервиса (0x1812). */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS,
		      (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

/* Scan response: полное имя устройства из CONFIG_BT_DEVICE_NAME. */
static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Подключение не удалось: err 0x%02x %s",
			err, bt_hci_err_to_str(err));
		return;
	}

	if (default_conn) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	LOG_INF("Bluetooth подключён %s", bt_conn_dst_str(conn));

	default_conn = bt_conn_ref(conn);

	/* HID требует защищённого канала — запрашиваем уровень L2. */
	if (bt_conn_set_security(conn, BT_SECURITY_L2)) {
		LOG_WRN("Не удалось задать уровень безопасности");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Bluetooth отключён, причина 0x%02x %s",
		reason, bt_hci_err_to_str(reason));

	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
		hid_notify_enabled = false;
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (!err) {
		LOG_INF("Безопасность: уровень %u", level);
	} else {
		LOG_WRN("Сбой безопасности: уровень %u, err %s (%d)",
			level, bt_security_err_to_str(err), err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}

	LOG_INF("Bluetooth инициализирован");

	/* Запускаем рекламу: подключаемую (CONNECTABLE) + scan response. */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
			      ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Запуск рекламы не удался: %d", err);
		return;
	}

	LOG_INF("Реклама запущена: устройство видно как \"%s\"",
		CONFIG_BT_DEVICE_NAME);
}

/* ================================================================== *
 *  LoRa (SX1272): вывод регистров и callback приёма                   *
 * ================================================================== */

static void sx1272_dump_registers(void)
{
	const uint8_t version = Radio.Read(SX1272_REG_LR_VERSION);

	printk("=== Регистры SX1272 (чтение по SPI) ===\n");
	printk("REG_VERSION       (0x42) = 0x%02x  (%s)\n", version,
		version == SX1272_VERSION_ID ? "SX1272 OK" : "НЕВЕРНЫЙ ID!");

	printk("REG_OPMODE        (0x01) = 0x%02x\n", Radio.Read(SX1272_REG_LR_OPMODE));

	uint32_t frf = ((uint32_t)Radio.Read(SX1272_REG_LR_FRFMSB) << 16) |
		       ((uint32_t)Radio.Read(SX1272_REG_LR_FRFMID) << 8) |
		       ((uint32_t)Radio.Read(SX1272_REG_LR_FRFLSB));
	uint32_t freq_hz = (uint32_t)((uint64_t)frf * 32000000ULL >> 19);
	printk("REG_FRF MSB/MID/LSB (0x06..08): frf=0x%06x -> %u Гц\n",
		frf, freq_hz);

	printk("REG_PACONFIG      (0x09) = 0x%02x\n", Radio.Read(SX1272_REG_LR_PACONFIG));
	printk("REG_LNA           (0x0C) = 0x%02x\n", Radio.Read(SX1272_REG_LR_LNA));
	printk("REG_MODEMCONFIG1  (0x1D) = 0x%02x\n", Radio.Read(SX1272_REG_LR_MODEMCONFIG1));
	printk("REG_MODEMCONFIG2  (0x1E) = 0x%02x\n", Radio.Read(SX1272_REG_LR_MODEMCONFIG2));
	printk("REG_SYNCWORD      (0x39) = 0x%02x\n", Radio.Read(SX1272_REG_LR_SYNCWORD));
}

static void on_lora_packet_recv(const struct device *dev, uint8_t *data,
				uint16_t size, int16_t rssi, int8_t snr,
				void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_INF("LoRa RX: RSSI=%d dBm, SNR=%d dB, %u байт", rssi, snr, size);
	LOG_HEXDUMP_INF(data, size, "payload");
}

/* ================================================================== *
 *  Кнопка: антидребезг + отправка HID Enter                           *
 * ================================================================== */

static void button_debounce_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int val = gpio_pin_get_dt(&button);
	if (val < 0) {
		LOG_ERR("Не удалось прочитать состояние кнопки: %d", val);
		return;
	}

	if (val) {
		gpio_pin_set_dt(&led, 1);
		LOG_INF("Кнопка нажата -> HID Enter");
		hid_send_key(HID_KEY_ENTER);
	} else {
		gpio_pin_set_dt(&led, 0);
		LOG_INF("Кнопка отжата -> HID release");
		hid_send_key(0);
	}
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_reschedule(&button_debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

static int button_setup(void)
{
	int ret;

	if (!device_is_ready(button.port)) {
		LOG_ERR("GPIO-устройство кнопки %s не готово", button.port->name);
		return -ENODEV;
	}

	k_work_init_delayable(&button_debounce_work, button_debounce_handler);

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("gpio_pin_configure_dt() failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("gpio_pin_interrupt_configure_dt() failed: %d", ret);
		return ret;
	}

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	ret = gpio_add_callback(button.port, &button_cb_data);
	if (ret < 0) {
		LOG_ERR("gpio_add_callback() failed: %d", ret);
		return ret;
	}

	LOG_INF("Кнопка готова: прерывания + антидребезг %d мс",
		BUTTON_DEBOUNCE_MS);
	return 0;
}

static int led_setup(void)
{
	if (!device_is_ready(led.port)) {
		LOG_ERR("GPIO-устройство светодиода %s не готово", led.port->name);
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Не удалось настроить светодиод: %d", ret);
		return ret;
	}

	LOG_INF("Светодиод готов: зажигается по нажатию кнопки");
	return 0;
}

/* ================================================================== *
 *  main                                                               *
 * ================================================================== */

int main(void)
{
	const struct device *const lora_dev = DEVICE_DT_GET(LORA_NODE);
	struct lora_modem_config config = {0};
	int ret;

	LOG_INF("Старт: LoRa SX1272 + BLE HID \"%s\"",
		CONFIG_BT_DEVICE_NAME);

	/* --- Светодиод --- */
	ret = led_setup();
	if (ret < 0) {
		LOG_ERR("Инициализация светодиода не удалась: %d", ret);
	}

	/* --- Кнопка --- */
	ret = button_setup();
	if (ret < 0) {
		LOG_ERR("Инициализация кнопки не удалась: %d", ret);
	}

	/* --- Bluetooth LE (асинхронная инициализация, callback bt_ready) --- */
	ret = bt_enable(bt_ready);
	if (ret) {
		LOG_ERR("Bluetooth init failed: %d", ret);
	}

	/* --- LoRa --- */
	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa-устройство %s не готово", lora_dev->name);
	} else {
		config.frequency = 868000000;
		config.bandwidth = BW_125_KHZ;
		config.datarate = SF_7;
		config.coding_rate = CR_4_5;
		config.preamble_len = 8;
		config.tx_power = 14;
		config.iq_inverted = false;
		config.public_network = false;
		config.packet_crc_disable = false;
		config.tx = false;

		ret = lora_config(lora_dev, &config);
		if (ret < 0) {
			LOG_ERR("lora_config() failed: %d", ret);
		} else {
			LOG_INF("Модем: 868 МГц, BW125, SF7, CR4/5, RX");
			sx1272_dump_registers();

			ret = lora_recv_async(lora_dev, on_lora_packet_recv, NULL);
			if (ret < 0) {
				LOG_ERR("lora_recv_async() failed: %d", ret);
			} else {
				LOG_INF("Непрерывный приём (RXCONTINUOUS) запущен");
			}
		}
	}

	/* Основной поток спит; пакеты LoRa обрабатываются в callback,
	 * нажатия кнопки — в workqueue + HID-notify по Bluetooth.
	 */
	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}