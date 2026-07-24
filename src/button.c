/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Реализация работы с механическими кнопками (sw0/sw1/sw2) и светодиодом (led0).
 *
 * Подробнее см. button.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "button.h"
#include "ble.h"

LOG_MODULE_REGISTER(button, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Devicetree-узлы и константы                                        *
 * ------------------------------------------------------------------ */

/* sw0 = boot_button на PH3 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN). */
#define SW0_NODE DT_ALIAS(sw0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SW0_NODE),
	     "Не найден узел кнопки (alias sw0) в devicetree. "
	     "Проверьте DTS платы: узел gpio-keys с алиасом sw0.");

  /* sw1 = кнопка Copy (Ctrl+C) на PA0. */
  #define SW1_NODE DT_ALIAS(sw1)
  BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SW1_NODE),
  	     "Не найден узел кнопки (alias sw1) в devicetree. "
  	     "Добавьте узел gpio-keys с алиасом sw1 в app.overlay.");
  
  /* sw2 = кнопка Paste (Ctrl+V) на PA1. */
  #define SW2_NODE DT_ALIAS(sw2)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SW2_NODE),
	     "Не найден узел кнопки (alias sw2) в devicetree. "
	     "Добавьте узел gpio-keys с алиасом sw2 в app.overlay.");

/* Светодиод led0 (синий User LED на PE4). */
#define LED0_NODE DT_ALIAS(led0)

#define BUTTON_DEBOUNCE_MS 20

/* ------------------------------------------------------------------ *
 *  Описание кнопки                                                    *
 * ------------------------------------------------------------------ */

/* Конфигурация одной кнопки: GPIO, HID-модификатор и код клавиши.
 * Каждая кнопка имеет собственный callback и debounce work.
 */
struct button_config {
	const struct gpio_dt_spec spec;
	struct gpio_callback cb;
	struct k_work_delayable debounce_work;

	/* HID-отчёт, отправляемый при нажатии. */
	uint8_t modifier;
	uint8_t hid_key;

	/* Текстовое описание для логов. */
	const char *name;
};

#define BUTTON_ENTRY(node_id, mod, key, label)			\
{									\
	.spec    = GPIO_DT_SPEC_GET(node_id, gpios),			\
	.modifier = (mod),						\
	.hid_key = (key),						\
	.name    = (label),						\
}

static struct button_config buttons[] = {
	BUTTON_ENTRY(SW0_NODE, HID_MOD_NONE,      HID_KEY_ENTER, "sw0 Enter"),
	BUTTON_ENTRY(SW1_NODE, HID_MOD_LEFT_GUI,  HID_KEY_C,     "sw1 Cmd+C"),
	BUTTON_ENTRY(SW2_NODE, HID_MOD_LEFT_GUI,  HID_KEY_V,     "sw2 Cmd+V"),
};

#define BUTTONS_COUNT ARRAY_SIZE(buttons)

/* Светодиод led0 — зажигается при нажатии любой кнопки. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Callback активности: вызывается при каждом нажатии (для сброса таймера сна). */
static void (*activity_callback)(void);

/* ------------------------------------------------------------------ *
 *  Обработка нажатий (антидребезг + отправка HID)                     *
 * ------------------------------------------------------------------ */

static void button_debounce_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct button_config *btn =
		CONTAINER_OF(dwork, struct button_config, debounce_work);

	int val = gpio_pin_get_dt(&btn->spec);
	if (val < 0) {
		LOG_ERR("[%s] Не удалось прочитать состояние кнопки: %d",
			btn->name, val);
		return;
	}

	if (val) {
		gpio_pin_set_dt(&led, 1);
		LOG_INF("[%s] нажата -> HID modifier=0x%02x key=0x%02x",
			btn->name, btn->modifier, btn->hid_key);
		hid_send_key(btn->modifier, btn->hid_key);
		/* Уведомляем main() о активности для сброса таймера сна. */
		if (activity_callback) {
			activity_callback();
		}
	} else {
		gpio_pin_set_dt(&led, 0);
		LOG_INF("[%s] отжата -> HID release", btn->name);
		hid_send_key(HID_MOD_NONE, 0);
	}
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pins);

	/* Находим кнопку по указателю callback. */
	struct button_config *btn =
		CONTAINER_OF(cb, struct button_config, cb);

	k_work_reschedule(&btn->debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

/* ------------------------------------------------------------------ *
 *  Публичные функции                                                  *
 * ------------------------------------------------------------------ */

static int button_setup_one(struct button_config *btn)
{
	int ret;

	if (!device_is_ready(btn->spec.port)) {
		LOG_ERR("[%s] GPIO-устройство %s не готово",
			btn->name, btn->spec.port->name);
		return -ENODEV;
	}

	LOG_INF("[%s] настройка: порт=%s pin=%d",
		btn->name, btn->spec.port->name, btn->spec.pin);

	k_work_init_delayable(&btn->debounce_work, button_debounce_handler);

	ret = gpio_pin_configure_dt(&btn->spec, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("[%s] gpio_pin_configure_dt() failed: %d",
			btn->name, ret);
		return ret;
	}

	/* Проверяем текущее состояние пина после настройки. */
	int val = gpio_pin_get_dt(&btn->spec);
	LOG_INF("[%s] состояние после настройки: %d", btn->name, val);

	ret = gpio_pin_interrupt_configure_dt(&btn->spec, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("[%s] gpio_pin_interrupt_configure_dt() failed: %d",
			btn->name, ret);
		return ret;
	}

	gpio_init_callback(&btn->cb, button_pressed, BIT(btn->spec.pin));
	ret = gpio_add_callback(btn->spec.port, &btn->cb);
	if (ret < 0) {
		LOG_ERR("[%s] gpio_add_callback() failed: %d",
			btn->name, ret);
		return ret;
	}

	LOG_INF("[%s] готова: прерывания + антидребезг %d мс",
		btn->name, BUTTON_DEBOUNCE_MS);
	return 0;
}

static int buttons_setup(void)
{
	for (size_t i = 0; i < BUTTONS_COUNT; i++) {
		int ret = button_setup_one(&buttons[i]);
		if (ret < 0) {
			return ret;
		}
	}
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

	LOG_INF("Светодиод готов: зажигается по нажатию любой кнопки");
	return 0;
}

void button_set_activity_callback(void (*callback)(void))
{
	activity_callback = callback;
}

int button_init(void)
{
	int ret;

	ret = led_setup();
	if (ret < 0) {
		return ret;
	}

	ret = buttons_setup();
	if (ret < 0) {
		return ret;
	}

	return 0;
}