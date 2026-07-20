# Bluetooth (BLE HID) — шпаргалка для проекта MY_PRJ

> Цель: при следующей работе над Bluetooth в этом проекте быстро вспомнить
> архитектуру, расположение ключевых файлов и «подводные камни» STM32WB55.

---

## 1. Плата и чип

- **Плата**: `weact_stm32wb55_core` (WeAct Studio STM32WB55 Core Board)
- **Чип**: STM32WB55CGU6 (Cortex-M4 + Cortex-M0+)
- **DTS платы**: `zephyr/boards/weact/stm32wb55_core/weact_stm32wb55_core.dts`
- **defconfig**: `.../weact_stm32wb55_core_defconfig`
- **YAML**: `.../weact_stm32wb55_core.yaml` (Bluetooth НЕ указан в `supported`)

---

## 2. Как работает Bluetooth на STM32WB55

Радиомодуль физически находится на **CPU2 (Cortex-M0+)**.
Связь CPU1 (где работает Zephyr) ↔ CPU2 идёт через **IPCC** (Inter-Processor Communication Controller).

### Дерево зависимостей конфигов:
```
CONFIG_BT=y  (в prj.conf)
    └──> активируется CONFIG_BT_STM32_IPM (в Kconfig.drivers)
            └──> зависит от DT_HAS_ST_STM32WB_RF_ENABLED
                    └──> узел ble_rf (compatible = "st,stm32wb-rf")
                         уже включён в stm32wb.dtsi (стр. 650-654, без status="disabled")
                         и выбран: zephyr,bt-hci = &ble_rf (стр. 27)
```

**Важно**: узел `ble_rf` и `chosen` уже определены в SoC dtsi.
**НЕ нужно** добавлять их в `app.overlay` — только сломает сборку.

---

## 3. Ключевые файлы Zephyr (пути от корня zephyr/)

| Что | Путь |
|-----|------|
| DTS платы | `boards/weact/stm32wb55_core/weact_stm32wb55_core.dts` |
| SoC DTSI (узел ble_rf!) | `dts/arm/st/wb/stm32wb.dtsi:650` |
| Binding для st,stm32wb-rf | `dts/bindings/bluetooth/st,stm32wb-rf.yaml` |
| HCI-драйвер (IPM) | `drivers/bluetooth/hci/ipm_stm32wb.c` |
| Kconfig BT-драйверов | `drivers/bluetooth/hci/Kconfig` |
| UUID (HIDS и др.) | `include/zephyr/bluetooth/uuid.h` |
| Пример HID (образец) | `samples/bluetooth/peripheral_hids/` |
| Базовые Kconfig BT | `subsys/bluetooth/Kconfig.*` |

---

## 4. Конфигурация в `prj.conf` (минимум для BLE HID)

```kconfig
# Базовый стек
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_SMP=y          # HID требует шифрования

# Хранение ключей (для переподключения)
CONFIG_BT_SETTINGS=y
CONFIG_SETTINGS=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y

# Имя и внешний вид
CONFIG_BT_DEVICE_NAME="stm32_button"
CONFIG_BT_DEVICE_APPEARANCE=961   # HID Generic Keyboard

# Стек (settings/BT требуют больше места)
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
```

**Не нужно** вручную включать `CONFIG_BT_STM32_IPM` — оно активируется
автоматически при наличии узла `ble_rf` в DTS.

---

## 5. HID UUID (шпаргалка по uuid.h)

| UUID | Hex | Макрос |
|------|-----|--------|
| HID Service | 0x1812 | `BT_UUID_HIDS` |
| HID Info | 0x2A4A | `BT_UUID_HIDS_INFO` |
| Report Map | 0x2A4B | `BT_UUID_HIDS_REPORT_MAP` |
| Report | 0x2A4D | `BT_UUID_HIDS_REPORT` |
| Report Reference | 0x2908 | `BT_UUID_HIDS_REPORT_REF` |
| Control Point | 0x2A4C | `BT_UUID_HIDS_CTRL_POINT` |
| Protocol Mode | 0x2A4E | `BT_UUID_HIDS_PROTOCOL_MODE` |
| Boot Keyboard Input | 0x2A22 | `BT_UUID_HIDS_BOOT_KB_IN_REPORT` |

---

## 6. HID Report Map для клавиатуры (готовый шаблон)

```c
static const uint8_t report_map[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop) */
    0x09, 0x06, /* Usage (Keyboard) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x01, /*   Report Id (1) */
    0x05, 0x07, /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0, /*   Usage Minimum (Left Control) */
    0x29, 0xE7, /*   Usage Maximum (Right GUI) */
    0x15, 0x00, /*   Logical Minimum (0) */
    0x25, 0x01, /*   Logical Maximum (1) */
    0x75, 0x01, /*   Report Size (1) */
    0x95, 0x08, /*   Report Count (8) */
    0x81, 0x02, /*   Input (Data,Var,Abs) — модификаторы */
    0x95, 0x01, /*   Report Count (1) */
    0x75, 0x08, /*   Report Size (8) */
    0x81, 0x01, /*   Input (Const) — reserved */
    0x95, 0x05, /*   Report Count (5) — LEDs */
    0x75, 0x01, /*   Report Size (1) */
    0x05, 0x08, /*   Usage Page (LEDs) */
    0x19, 0x01, /*   Usage Minimum (Num Lock) */
    0x29, 0x05, /*   Usage Maximum (Kana) */
    0x91, 0x02, /*   Output (Data,Var,Abs) */
    0x95, 0x01, /*   Report Count (1) */
    0x75, 0x03, /*   Report Size (3) */
    0x91, 0x01, /*   Output (Const) — padding */
    0x95, 0x06, /*   Report Count (6) */
    0x75, 0x08, /*   Report Size (8) */
    0x15, 0x00, /*   Logical Minimum (0) */
    0x25, 0x73, /*   Logical Maximum (0x73) */
    0x05, 0x07, /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00, /*   Usage Minimum (0) */
    0x29, 0x73, /*   Usage Maximum (0x73) */
    0x81, 0x00, /*   Input (Data,Array) — ключи */
    0xC0,       /* End Collection */
};
```

### Коды клавиш (HID Usage 0x07 page):
| Клавиша | HID Code |
|---------|----------|
| Enter | 0x28 |
| Space | 0x2C |
| A | 0x04 |
| Z | 0x1D |
| 1 | 0x1E |
| Esc | 0x29 |
| Backspace | 0x2A |
| Tab | 0x2B |
| Left Ctrl (modifier) | 0xE0 |
| Left Shift (modifier) | 0xE1 |

---

## 7. Структура GATT-сервиса HIDS (порядок атрибутов!)

```
attrs[0]  Primary Service (0x1812)
attrs[1]  Characteristic decl — Info
attrs[2]  Info value        ← read_hids_info
attrs[3]  Characteristic decl — Report Map
attrs[4]  Report Map value  ← read_report_map
attrs[5]  Characteristic decl — Report (read+notify)
attrs[6]  Report value      ← bt_gatt_notify() цель!
attrs[7]  CCC descriptor    ← input_ccc_changed
attrs[8]  Report Reference descriptor
attrs[9]  Characteristic decl — Control Point
attrs[10] Control Point value
```

**Важно**: `BT_GATT_CHARACTERISTIC` разворачивается в 2 атрибута (decl + value).
Индекс `6` для notify — это отсчёт от начала сервиса.

---

## 8. Отправка HID-отчёта (шаблон)

```c
struct hid_keyboard_report {
    uint8_t modifiers;  // биты: Ctrl/Shift/Alt/GUI
    uint8_t reserved;   // всегда 0
    uint8_t key[6];     // до 6 одновременных клавиш
} __packed;

static void hid_send_key(uint8_t key) {
    if (!default_conn || !hid_notify_enabled) return;

    struct hid_keyboard_report report = {0};
    report.key[0] = key;  // 0 = отпускание

    bt_gatt_notify(default_conn,
                   &hids_svc.attrs[6],  // индекс Report value
                   &report, sizeof(report));
}
```

---

## 9. Advertising (шаблон)

```c
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS,
                  (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL,
                  BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

// Запуск:
bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                ad, ARRAY_SIZE(ad),
                sd, ARRAY_SIZE(sd));
```

---

## 10. Безопасность и спаривание

- HID **требует** шифрованный канал → `CONFIG_BT_SMP=y` обязательно
- Уровень безопасности: `bt_conn_set_security(conn, BT_SECURITY_L2)`
- Разрешения атрибутов: `BT_GATT_PERM_READ_ENCRYPT` / `WRITE_ENCRYPT`
  (не AUTHEN — чтобы работало Just Works без PIN)
- Ключи сохраняются через `CONFIG_BT_SETTINGS` + `CONFIG_NVS`
- В `bt_ready()` после init вызывать `settings_load()`

---

## 11. Подводные камни

1. **IPCC IRQ**: `ipm_stm32wb.c` сам подключает `IPCC_C1_RX_IRQn`/`IPCC_C1_TX_IRQn`
   через `IRQ_CONNECT` — не нужно делать это вручную.

2. **CPU2 firmware**: На чипе должен быть прошит BLE-стек на CPU2
   (см. комментарии в DTS платы про partition layout).
   По умолчанию у WeAct уже прошит `BLE_HCILayer_extended @ 0x080DB000`.

3. **Клок HSI48**: Драйвер IPM требует `clk_hsi48` (для RF).
   В DTS платы он уже включён (`status = "okay"`).

4. **SRAM2**: CPU2 использует SRAM2 для mailbox. Не отключать.

5. **`CONFIG_BT_DEVICE_NAME`**: Если менять имя, могут быть проблемы
   с кэшированием у хоста. Можно включить `CONFIG_BT_DEVICE_NAME_GATT_WRITABLE`.

6. **IntelliSense VS Code**: Появляются ошибки про `radio.h` —
   это нормально, путь настраивается в `CMakeLists.txt`, а не в `.vscode/`.
   Реальная сборка через `west build` работает корректно.

7. **⚠️ Bus Fault с CONFIG_SETTINGS / CONFIG_NVS**: При включении
   `CONFIG_BT_SETTINGS` + `CONFIG_SETTINGS` + `CONFIG_NVS` во время
   `bt_enable()` возникает **Bus Fault** (Precise data bus error, BFAR=0x080D8FF8).
   Причина: `storage_partition` в DTS платы использует
   `compatible = "zephyr,mapped-partition"`, и NVS не может корректно
   с ней работать на этой плате.

   **Решение**: НЕ включать эти конфиги. Устройство работает, но
   не сохраняет ключи спаривания (каждое подключение = новое спаривание).
   Для постоянного bonding нужно либо починить partition layout,
   либо использовать другой backend для settings.

---

## 12. Сборка (для справки)

```bash
# В Docker-контейнере zephyrprojectrtos/zephyr-build:
cd /workdir/MY_PRJ
west build -b weact_stm32wb55_core --pristine
```

---

## 13. Текущее состояние проекта

- **`prj.conf`**: LoRa + BLE HID (BT-секция в конце)
- **`src/main.c`**: LoRa + BLE HID клавиатура
  - BT-код: строки 89–300 (примерно)
  - LoRa-код: `sx1272_dump_registers()`, `on_lora_packet_recv()`
  - Кнопка → HID Enter: `button_debounce_handler()`
- **`app.overlay`**: только LoRa (SX1272 на SPI1) + led1
  (BT-узел добавлять НЕ нужно — он в SoC dtsi)