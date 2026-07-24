/*
 * Copyright (c) 2016 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Демонстрация Bluetooth LE HID-клавиатуры "stm32_button".
 *
 * По нажатию механической кнопки (sw0 / BOOT на PH3):
 *   - зажигается светодиод led0;
 *   - по BLE HID отправляется нажатие клавиши Enter,
 *     а при отжатии — отпускание (нуль-отчёт).
 *
 * Энергосбережение:
 *   1) CONFIG_PM + CONFIG_TICKLESS_KERNEL: CPU1 (M4) входит в STOP2
 *      при idle. Пробуждение — по IPCC (BLE от CPU2) и EXTI (кнопки).
 *   2) Софтверный «сон BLE»: через SLEEP_TIMEOUT_SEC (10 сек) без
 *      нажатий останавливается BLE-реклама (радио CPU2 полностью
 *      выключается). Любое нажатие кнопки будит устройство.
 *   3) Основной поток после init уходит в k_sleep(K_FOREVER), чтобы
 *      не будить CPU периодическими таймерами.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble.h"
#include "button.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* Таймаут сна: 10 секунд без активности → засыпаем. */
#define SLEEP_TIMEOUT_SEC 10

/* Work для отложенного засыпания.
 * Сбрасывается при каждом нажатии кнопки (activity_callback).
 */
static void sleep_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_work_handler);

/* Флаг состояния сна. */
static bool is_sleeping;

/* Вызывается при каждом нажатии кнопки (из button.c).
 * Сбрасывает таймер сна и будит BLE, если спит.
 */
static void on_button_activity(void)
{
	/* Сбрасываем таймер засыпания. */
	k_work_reschedule(&sleep_work, K_SECONDS(SLEEP_TIMEOUT_SEC));

	/* Если спали — просыпаемся. */
	if (is_sleeping) {
		is_sleeping = false;
		ble_wake();
	}
}

/* Вызывается по истечению SLEEP_TIMEOUT_SEC без активности. */
static void sleep_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!is_sleeping) {
		is_sleeping = true;
		ble_sleep();
	}
}

int main(void)
{
	int ret;

	LOG_INF("Старт: BLE HID \"%s\"", CONFIG_BT_DEVICE_NAME);

	/* --- Кнопка + светодиод --- */
	ret = button_init();
	if (ret < 0) {
		LOG_ERR("Инициализация кнопки/светодиода не удалась: %d", ret);
	}

	/* Регистрируем callback активности для сброса таймера сна. */
	button_set_activity_callback(on_button_activity);

	/* Даём RTT-вьюеру время вывести логи инициализации кнопок,
	 * прежде чем BLE начнёт спамить HCI-сообщениями.
	 */
	LOG_INF("Жду 3 сек перед BLE init...");
	k_sleep(K_SECONDS(3));
	LOG_INF("Запускаю BLE init");

	/* --- Bluetooth LE --- */
	ret = ble_init();
	if (ret < 0) {
		LOG_ERR("Инициализация Bluetooth не удалась: %d", ret);
	}

	/* Запускаем таймер сна: через 10 сек без нажатий → засыпаем. */
	k_work_reschedule(&sleep_work, K_SECONDS(SLEEP_TIMEOUT_SEC));

	/* Основной поток больше не нужен: вся логика (кнопки, BLE) живёт
	 * в прерываниях и системной workqueue. Уходим в вечный сон:
	 * при CONFIG_PM + TICKLESS_KERNEL CPU1 войдёт в STOP2 и будет
	 * пробуждаться только прерываниями IPCC (BLE от CPU2) или EXTI (кнопки).
	 *
	 * Раньше тут был цикл с k_sleep(K_SECONDS(10)) — он каждые 10 секунд
	 * будил CPU по программному таймеру, не давая достичь минимального тока.
	 */
	k_sleep(K_FOREVER);

	return 0;
}
