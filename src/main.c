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
 * Логика разнесена по модулям:
 *   - ble.c    — Bluetooth LE HID-клавиатура;
 *   - button.c — кнопка (sw0) + светодиод (led0) + антидребезг.
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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble.h"
#include "button.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

int main(void)
{
	int ret;

	LOG_INF("Старт: BLE HID \"%s\"", CONFIG_BT_DEVICE_NAME);

	/* --- Кнопка + светодиод (вызывает hid_send_key из BLE) --- */
	ret = button_init();
	if (ret < 0) {
		LOG_ERR("Инициализация кнопки/светодиода не удалась: %d", ret);
	}

	/* Даём RTT-вьюеру время вывести логи инициализации кнопок,
	 * прежде чем BLE начнёт спамить HCI-сообщениями.
	 */
	LOG_INF("Жду 3 сек перед BLE init...");
	k_sleep(K_SECONDS(3));
	LOG_INF("Запускаю BLE init");

	/* --- Bluetooth LE (асинхронная инициализация, callback bt_ready) --- */
	ret = ble_init();
	if (ret < 0) {
		LOG_ERR("Инициализация Bluetooth не удалась: %d", ret);
	}

	/* Основной поток спит; нажатия кнопки обрабатываются в workqueue
	 * и отправляются как HID-notify по Bluetooth.
	 */
	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}