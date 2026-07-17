/*
 * Copyright (c) 2016 Intel Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Демонстрация работы с LoRa-трансивером SX1272 по SPI через штатную
 * подсистему Zephyr (CONFIG_LORA + backend loramac-node).
 *
 * Что делает:
 *   1. Получает LoRa-устройство из devicetree (alias lora0 -> &sx1272).
 *   2. Конфигурирует модем на 868 МГц, BW 125 кГц, SF 7, CR 4/5, приём.
 *   3. Считывает ключевые регистры SX1272 напрямую через SPI с помощью
 *      низкоуровневого API модуля loramac-node (struct Radio_s).
 *   4. Запускает непрерывный асинхронный приём (RXCONTINUOUS).
 *   5. Обрабатывает нажатие/отжатие механической кнопки (alias sw0)
 *      через GPIO-прерывания по обоим фронтам с программным
 *      антидребезгом (debounce) на базе k_work_delayable.
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

/* Низкоуровневый API модуля loramac-node: Radio.Read()/Write() поверх SPI.
 * Путь к radio.h добавлен в CMakeLists.txt (ZEPHYR_BASE/../modules/...).
 */
#include <radio.h>

/* Адреса регистров SX1272 (LoRa-режим).
 * Источник: modules/lib/loramac-node/src/radio/sx1272/sx1272Regs-LoRa.h
 */
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

/* Ожидаемое значение REG_VERSION: 0x22 для SX1272 */
#define SX1272_VERSION_ID            0x22

#define LORA_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE),
	     "Не найден узел LoRa (alias lora0) в devicetree. "
	     "Проверьте app.overlay: узел sx1272 с compatible = \"semtech,sx1272\".");

/* ------------------------------------------------------------------ *
 *  Механическая кнопка (alias sw0) + антидребезг                       *
 *                                                                     *
 *  sw0 берётся из базового DTS платы WeAct STM32WB55 Core:            *
 *  кнопка BOOT на PH3 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN).            *
 * ------------------------------------------------------------------ */
#define SW0_NODE DT_ALIAS(sw0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(SW0_NODE),
	     "Не найден узел кнопки (alias sw0) в devicetree. "
	     "Проверьте DTS платы: узел gpio-keys с алиасом sw0.");

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* Период антидребезга (мс): после последнего прерывания ждём это время,
 * прежде чем зафиксировать стабильное состояние. Дребезг механических
 * контактов обычно утихает за 5–20 мс.
 */
#define BUTTON_DEBOUNCE_MS  20

/* Отложенная работа для антидребезга. ISR только перепланирует таймер,
 * а само чтение пина и логирование выполняются в контексте workqueue.
 * k_work_reschedule() сдвигает таймер при каждом новом импульсе, поэтому
 * серия дребезга схлопывается в одно итоговое событие.
 */
static struct k_work_delayable button_debounce_work;

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* ------------------------------------------------------------------ *
 *  Вывод значений ключевых регистров SX1272 по SPI                    *
 * ------------------------------------------------------------------ */
static void sx1272_dump_registers(void)
{
	/* Radio глобально определена в zephyr/drivers/lora/loramac-node/sx127x.c */
	const uint8_t version = Radio.Read(SX1272_REG_LR_VERSION);

	printk("=== Регистры SX1272 (чтение по SPI) ===\n");
	printk("REG_VERSION       (0x42) = 0x%02x  (%s)\n", version,
		version == SX1272_VERSION_ID ? "SX1272 OK" : "НЕВЕРНЫЙ ID!");

	printk("REG_OPMODE        (0x01) = 0x%02x\n", Radio.Read(SX1272_REG_LR_OPMODE));

	/* Частота: frf = MSB<<16 | MID<<8 | LSB; F_rf = frf * 32MHz / 2^19 */
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

/* ------------------------------------------------------------------ *
 *  Callback асинхронного приёма пакетов                               *
 *  (имя не должно совпадать с typedef lora_recv_cb из lora.h)         *
 * ------------------------------------------------------------------ */
static void on_lora_packet_recv(const struct device *dev, uint8_t *data,
				uint16_t size, int16_t rssi, int8_t snr,
				void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_INF("LoRa RX: RSSI=%d dBm, SNR=%d dB, %u байт", rssi, snr, size);
	LOG_HEXDUMP_INF(data, size, "payload");
}

/* ------------------------------------------------------------------ *
 *  Обработчик отложенной работы (антидребезг)                         *
 *                                                                     *
 *  Вызывается из workqueue спустя BUTTON_DEBOUNCE_MS после последнего  *
 *  прерывания GPIO. К этому моменту дребезг уже утих, и можно         *
 *  достоверно прочитать состояние пина.                               *
 * ------------------------------------------------------------------ */
static void button_debounce_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int val = gpio_pin_get_dt(&button);
	if (val < 0) {
		LOG_ERR("Не удалось прочитать состояние кнопки: %d", val);
		return;
	}

	if (val) {
		LOG_INF("Кнопка нажата");
	} else {
		LOG_INF("Кнопка отжата");
	}
}

/* ------------------------------------------------------------------ *
 *  Callback прерывания GPIO кнопки (ISR)                              *
 *                                                                     *
 *  Срабатывает по обоим фронтам (GPIO_INT_EDGE_BOTH). Не читает пин   *
 *  и не логирует напрямую — только перепланирует отложенную работу.   *
 *  Каждый новый импульс дребезга сдвигает таймер, поэтому итоговое    *
 *  чтение выполнится один раз после стабилизации контакта.            *
 * ------------------------------------------------------------------ */
static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Переносим тяжесть в контекст потока: чтение GPIO + логирование. */
	k_work_reschedule(&button_debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

/* ------------------------------------------------------------------ *
 *  Инициализация кнопки: вход + прерывания по обоим фронтам           *
 * ------------------------------------------------------------------ */
static int button_setup(void)
{
	int ret;

	if (!device_is_ready(button.port)) {
		LOG_ERR("GPIO-устройство кнопки %s не готово", button.port->name);
		return -ENODEV;
	}

	/* Инициализация отложенной работы для антидребезга */
	k_work_init_delayable(&button_debounce_work, button_debounce_handler);

	/* Настройка пина как входа (pull-down задан в DTS платы) */
	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("gpio_pin_configure_dt() failed: %d", ret);
		return ret;
	}

	/* Прерывания по обоим фронтам: нажатие и отжатие */
	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("gpio_pin_interrupt_configure_dt() failed: %d", ret);
		return ret;
	}

	/* Регистрация callback */
	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	ret = gpio_add_callback(button.port, &button_cb_data);
	if (ret < 0) {
		LOG_ERR("gpio_add_callback() failed: %d", ret);
		return ret;
	}

	LOG_INF("Кнопка готова: GPIO-прерывания по обоим фронтам + антидребезг %d мс",
		BUTTON_DEBOUNCE_MS);
	return 0;
}

int main(void)
{
	const struct device *const lora_dev = DEVICE_DT_GET(LORA_NODE);
	struct lora_modem_config config = {0};
	int ret;

	LOG_INF("Старт демо SX1272 по SPI (Zephyr LoRa subsystem)");

	/* --- Инициализация механической кнопки (GPIO-прерывания + debounce) --- */
	ret = button_setup();
	if (ret < 0) {
		LOG_ERR("Инициализация кнопки не удалась: %d", ret);
	}

	if (!device_is_ready(lora_dev)) {
		LOG_ERR("LoRa-устройство %s не готово", lora_dev->name);
		return 0;
	}

	/* --- Конфигурация модема: 868 МГц, приёмник --- */
	config.frequency = 868000000;          /* Европа: 863–870 МГц */
	config.bandwidth = BW_125_KHZ;
	config.datarate = SF_7;
	config.coding_rate = CR_4_5;
	config.preamble_len = 8;
	config.tx_power = 14;                  /* не используется в режиме RX */
	config.iq_inverted = false;
	config.public_network = false;         /* приватный sync word (0x12) */
	config.packet_crc_disable = false;     /* CRC включён */
	config.tx = false;                     /* режим приёма */

	ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("lora_config() failed: %d", ret);
		return 0;
	}
	LOG_INF("Модем сконфигурирован: 868 МГц, BW125, SF7, CR4/5, RX");

	/* --- Чтение регистров SX1272 после конфигурации --- */
	sx1272_dump_registers();

	/* --- Запуск непрерывного асинхронного приёма --- */
	ret = lora_recv_async(lora_dev, on_lora_packet_recv, NULL);
	if (ret < 0) {
		LOG_ERR("lora_recv_async() failed: %d", ret);
		return 0;
	}
	LOG_INF("Непрерывный приём (RXCONTINUOUS) запущен");

	/* Основной поток спит; пакеты обрабатываются в callback через DIO0,
	 * нажатия кнопки — в workqueue после антидребезга.
	 */
	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}