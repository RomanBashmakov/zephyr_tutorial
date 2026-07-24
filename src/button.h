/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Механическая кнопка (alias sw0) + светодиод led0.
 *
 * По нажатию/отжатию кнопки:
 *   - зажигается/гаснет светодиод led0;
 *   - вызывается hid_send_key() из модуля BLE (нажатие Enter / отпускание).
 *
 * sw0 = boot_button на PH3 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN) —
 * алиас уже определён в базовом DTS платы WeAct STM32WB55 Core.
 */

#ifndef BUTTON_H
#define BUTTON_H

/*
 * Инициализация кнопки и светодиода:
 *   - настраивает GPIO кнопки на вход с прерыванием по обоим фронтам;
 *   - включает антидребезг (BUTTON_DEBOUNCE_MS);
 *   - настраивает светодиод led0 на выход.
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче.
 */
int button_init(void);

void button_set_activity_callback(void (*callback)(void));
#endif /* BUTTON_H */