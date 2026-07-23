/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth LE HID-клавиатура "stm32_button".
 *
 * Модуль реализует:
 *   - HIDS (Human Interface Device Service) поверх GATT;
 *   - рекламу (advertising) с UUID HID-сервиса и именем устройства;
 *   - обработку подключений/безопасности;
 *   - отправку HID-отчётов нажатий/отпусканий клавиш.
 *
 * Имя устройства задаётся в prj.conf: CONFIG_BT_DEVICE_NAME="stm32_button".
 */

#ifndef BLE_H
#define BLE_H

#include <stdint.h>

/* HID Usage ID клавиш (Keyboard/Keypad Page). */
#define HID_KEY_ENTER 0x28
#define HID_KEY_C     0x06   /* клавиша 'c' */
#define HID_KEY_V     0x19   /* клавиша 'v' */

/* Биты модификаторов в byte 0 HID-отчёта. */
#define HID_MOD_NONE       0x00
#define HID_MOD_LEFT_CTRL  0x01
#define HID_MOD_LEFT_SHIFT 0x02
#define HID_MOD_LEFT_ALT   0x04
#define HID_MOD_LEFT_GUI   0x08

/*
 * Инициализация Bluetooth LE:
 *   - bt_enable() с callback bt_ready;
 *   - запуск рекламы в bt_ready().
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче
 * (аналогично bt_enable()).
 */
int ble_init(void);

/*
 * Отправка HID-отчёта хосту.
 *   modifier — биты модификаторов (HID_MOD_*) или HID_MOD_NONE;
 *   hid_key  — код клавиши; hid_key == 0 — отпускание всех клавиш.
 *
 * Ничего не делает, если нет подключения или хост не включил notify (CCC).
 */
void hid_send_key(uint8_t modifier, uint8_t hid_key);

#endif /* BLE_H */