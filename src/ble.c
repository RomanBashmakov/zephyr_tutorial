/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Реализация Bluetooth LE HID-клавиатуры.
 *
 * Подробнее см. ble.h.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>

#include "ble.h"

LOG_MODULE_REGISTER(ble, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Типы и константы HIDS                                              *
 * ------------------------------------------------------------------ */

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

/* Флаг режима сна: блокирует перезапуск рекламы в disconnected(). */
static bool is_sleeping;

/* Разрешения: HID требует шифрованного канала, но не обязательно
 * аутентифицированного. Используем READ_ENCRYPT/WRITE_ENCRYPT,
 * чтобы не усложнять спаривание (Just Works).
 */
#define HIDS_PERM_READ  BT_GATT_PERM_READ_ENCRYPT
#define HIDS_PERM_WRITE BT_GATT_PERM_WRITE_ENCRYPT

/* ------------------------------------------------------------------ *
 *  Callback'и чтения/записи атрибутов HIDS                            *
 * ------------------------------------------------------------------ */

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

static uint8_t ctrl_point;

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

/* ------------------------------------------------------------------ *
 *  HID Service Declaration                                            *
 * ------------------------------------------------------------------ */

/* HID Service Declaration.
 * Порядок атрибутов важен: индекс входного отчёта (с notify) используется
 * в hid_send_key() для bt_gatt_notify().
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
#define HIDS_INPUT_REPORT_ATTR_IDX 6

/* ------------------------------------------------------------------ *
 *  Advertising и callbacks подключения                                *
 * ------------------------------------------------------------------ */

/* Медленная реклама для снижения энергопотребления.
 * BT_LE_ADV_CONN_FAST_1 рекламирует каждые 30-60 мс — радио активно часто.
 * Медленный режим: интервал ~1.0-1.2 сек → радио просыпается редко.
 * Для HID-клавиатуры это допустимо: подключение займёт ~1 сек вместо ~30 мс.
 */
#define BT_LE_ADV_CONN_LOW_POWER \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, \
			BT_GAP_ADV_SLOW_INT_MIN, \
			BT_GAP_ADV_SLOW_INT_MAX, \
			NULL)

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

/* Work для отложенного перезапуска рекламы после disconnect.
 * Нельзя вызывать bt_le_adv_start() прямо из callback disconnected(),
 * т.к. он выполняется в RX-потоке HCI до освобождения буферов -> -ENOMEM.
 */
static void adv_restart_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_handler);

static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (default_conn != NULL) {
		/* Уже успели подключиться заново — реклама не нужна. */
		return;
	}

	int err = bt_le_adv_start(BT_LE_ADV_CONN_LOW_POWER,
				  ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Перезапуск рекламы не удался: %d", err);
	} else {
		LOG_INF("Реклама перезапущена");
	}
}

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

	/* Перезапускаем рекламу с задержкой, чтобы выполнить вызов вне
	 * контекста RX-потока HCI (иначе bt_le_adv_start() падает с -ENOMEM).
	 */
	/* Не перезапускаем рекламу, если устройство в режиме сна. */
	if (is_sleeping) {
		return;
	}

	k_work_reschedule(&adv_restart_work, K_MSEC(20));
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

	/* При CONFIG_BT_SETTINGS=y стек не генерирует ID-адрес сам: он ожидает,
	 * что адрес и ключи bonding'а загружены из NVS. Без этого вызова
	 * bt_le_adv_start() падает с -EAGAIN (-11) и в логе видно:
	 *   "No ID address. App must call settings_load()".
	 * Обработчики настроек BT регистрируются внутри bt_enable(), поэтому
	 * settings_load() вызываем здесь, в bt_ready(), ДО старта рекламы.
	 */
	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		int sret = settings_load();
		if (sret) {
			LOG_ERR("settings_load() failed: %d", sret);
		}
	}

	/* Запускаем рекламу: подключаемую (CONNECTABLE) + scan response. */
	err = bt_le_adv_start(BT_LE_ADV_CONN_LOW_POWER,
			      ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Запуск рекламы не удался: %d", err);
		return;
	}

	LOG_INF("Реклама запущена: устройство видно как \"%s\"",
		CONFIG_BT_DEVICE_NAME);
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                  *
 * ------------------------------------------------------------------ */

void hid_send_key(uint8_t modifier, uint8_t hid_key)
{
	if (!default_conn || !hid_notify_enabled) {
		LOG_DBG("HID-отправка пропущена: нет подключения/notify");
		return;
	}

	memset(&keyboard_state, 0, sizeof(keyboard_state));
	if (hid_key != 0) {
		keyboard_state.modifiers = modifier;
		keyboard_state.key[0] = hid_key;
	}

	int err = bt_gatt_notify(default_conn,
				 &hids_svc.attrs[HIDS_INPUT_REPORT_ATTR_IDX],
				 &keyboard_state, sizeof(keyboard_state));
	if (err) {
		LOG_WRN("bt_gatt_notify() failed: %d", err);
	}
}

void ble_sleep(void)
{
	/* Устанавливаем флаг сна ДО disconnect, чтобы disconnected()
	 * callback не запустил рекламу заново.
	 */
	is_sleeping = true;

	/* Отключаем активное подключение, если есть.
	 * Радио CPU2 работает пока есть conn, поэтому рвём его.
	 * disconnected() callback сработает, но не перезапустит рекламу
	 * из-за флага is_sleeping.
	 */
	if (default_conn) {
		bt_conn_disconnect(default_conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	/* Отменяем отложенный перезапуск рекламы (если был запланирован). */
	k_work_cancel_delayable(&adv_restart_work);

	/* Останавливаем рекламу — радио CPU2 прекращает передачу пакетов.
	 * Это основной источник экономии тока в режиме сна.
	 */
	int err = bt_le_adv_stop();
	if (err) {
		LOG_ERR("bt_le_adv_stop() failed: %d", err);
	} else {
		LOG_INF("BLE засыпает: реклама остановлена");
	}
}

void ble_wake(void)
{
	/* Сбрасываем флаг сна. */
	is_sleeping = false;

	if (default_conn != NULL) {
		return;
	}

	/* Запускаем рекламу. Быстрый режим (FAST_1) для быстрого
	 * переподключения. После подключения хост применит медленные
	 * параметры из CONFIG_BT_PERIPHERAL_PREF_*.
	 */
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
				  ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("bt_le_adv_start() failed: %d", err);
	} else {
		LOG_INF("BLE проснулся: реклама запущена");
	}
}

int ble_init(void)
{
	int err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
	}
	return err;
}
