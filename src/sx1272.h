/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa-трансивер SX1272 по SPI (Zephyr LoRa subsystem).
 *
 * Модуль отвечает за:
 *   - конфигурирование модема (частота, BW, SF, CR, мощность, режим RX);
 *   - чтение/дамп регистров SX1272 через низкоуровневый API loramac-node;
 *   - запуск непрерывного приёма (RXCONTINUOUS) с callback on_lora_packet_recv.
 *
 * Узел devicetree задаётся алиасом lora0 (см. app.overlay: sx1272@0 на spi1).
 */

#ifndef SX1272_H
#define SX1272_H

/*
 * Инициализация LoRa-модема SX1272:
 *   - получает устройство по DT_ALIAS(lora0);
 *   - конфигурирует модем (868 МГц, BW125, SF7, CR4/5, RX);
 *   - выводит дамп регистров;
 *   - запускает непрерывный приём (lora_recv_async).
 *
 * Возвращает 0 при успехе, отрицательный код ошибки при неудаче.
 */
int sx1272_init(void);

#endif /* SX1272_H */