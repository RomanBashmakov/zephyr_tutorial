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

6. **⚠️ Bus Fault с CONFIG_SETTINGS / CONFIG_NVS**: При включении
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

- **`prj.conf`**: BLE HID + RTT + **энергосбережение CPU1 (PM, Tickless)**
- **`src/main.c`**: BLE HID клавиатура
  - `button_init()` → кнопки + светодиод
  - `ble_init()` → Bluetooth LE HID
  - Кнопка → HID Enter: `button_debounce_handler()`
  - `k_sleep(K_FOREVER)` в конце main — не будит CPU периодически
- **`app.overlay`**: кнопки sw1/sw2 + led1, освобождение PA0/PA1,
  отключение i2c1/usart1/spi1/spi2 для снижения утечки тока
  (BT-узел добавлять НЕ нужно — он в SoC dtsi)

---

## 15. Энергосбережение CPU1 (PM + STOP2)

### 15.1. Что было изменено для ухода CPU1 в сон

Ранее `CONFIG_PM` был отключён с комментарием «ломает BLE».
Проблема была в том, что PM без `CONFIG_TICKLESS_KERNEL=y` не давал
глубокого сна: SysTick будил CPU каждые ~1 мс. Теперь PM и Tickless
включены вместе, что позволяет CPU1 (Cortex-M4) входить в **STOP2**.

| Настройка | Где | Эффект |
|-----------|-----|--------|
| `CONFIG_PM=y` | prj.conf | Power Management: idle → STOP2 |
| `CONFIG_PM_DEVICE=y` | prj.conf | Сон периферии при PM |
| `CONFIG_TICKLESS_KERNEL=y` | prj.conf | Нет системного тика → STOP2 достижим |
| `k_sleep(K_FOREVER)` | main.c | Main-поток не будит CPU |
| `CONFIG_LOG_DEFAULT_LEVEL=1` | prj.conf | Только ERR — меньше пробуждений от логов |
| `i2c1/usart1/spi* disabled` | app.overlay | Меньше тактируемой периферии (usbus отсутствует в DTS WeAct) |

### 15.2. Почему STOP2 безопасен для BLE на STM32WB55

- Связь CPU1↔CPU2 идёт через **IPCC** (Inter-Processor Communication).
- IPCC-прерывание (`IPCC_C1_RX_IRQn`) подключается через **прямой NVIC**
  в `ipm_stm32wb.c` — оно **будит CPU1 из STOP2** без EXTI.
- Радио на CPU2 продолжает работать автономно (connection events).
- Кнопки используют **EXTI** — тоже будят CPU1 из STOP2.

### 15.3. BLE connection parameters (радио CPU2)

Дополнительно к сну CPU1 минимизирована активность радио CPU2:

```
interval = 400 мс (MIN=MAX=320 × 1.25 мс)
latency  = 9      → пропуск 9 events → радио спит ~4 сек
timeout  = 10 сек (1000 × 10 мс)
```

Формула BLE: `timeout > (1 + latency) × interval × 2`
→ `10 > (1+9) × 0.4 × 2 = 8` ✓

### 15.4. Ожидаемое потребление

| Режим | Документация STM32WB55 | Замечание |
|-------|------------------------|-----------|
| STOP2 (CPU1+CPU2) | ~2–5 мкА | Радио выключено, реклама остановлена |
| Реклама (ADV) | ~1–5 мА | Зависит от интервала рекламы |
| Connected (400мс, latency 9) | ~десятки мкА | Радио просыпается раз в ~4 сек |

> ⚠️ Реальное потребление зависит от: внешних подтяжек, активности RTT,
> состояния пинов и утечек на плате. RTT (SEGGER) пишет в SRAM и **не**
> будит CPU, но держит область памяти — ток стремится к нулю при закрытом
> J-Link/вьюере.

### 15.5. Отладка энергопотребления

1. Проверить, что CPU1 реально входит в STOP2: включить
   `CONFIG_PM_DEVICE_LOG_LEVEL_DBG` — увидите сообщения о transition.
2. Убедиться, что нет активных таймеров: `CONFIG_KERNEL_SHELL` (временно)
   → команда `kernel timers` покажит список.
3. Измерить ток внешним амперметром: питание через 3V3/GND (не USB!).
4. Если потребление > 1 мА в STOP2 — проверить незадействованные пины
   (аналоговый вход, без подтяжки = утечка через входной буфер).

---

## 14. ⚠️ BLE-устройство не определяется (диагностика)

Если `bt_enable()` отрабатывает (лог `Bluetooth инициализирован`),
реклама стартует (`Реклама запущена`), но устройство **не видно** в сканере
 телефона/ПК — причина почти всегда на стороне **CPU2 (Cortex-M0+)**.

### 14.1. Главная причина: прошивка CPU2 (Wireless Stack)

STM32WB55 — двухъядерный: BLE-контроллер физически работает на **CPU2 (M0+)**,
а Zephyr — на CPU1 (M4). Связь между ними — через IPCC/shared RAM
(драйвер `ipm_stm32wb.c`).

**WeAct STM32WB55 Core поставляется с «Full Stack» прошивкой CPU2**
(полный стек ST, не-HCI), которая **НЕ совместима с Zephyr**.
Её нужно заменить на **HCI Layer** прошивку.

Симптомы «не той» прошивки CPU2:
- `bt_enable()` не возвращает управление (висит) или
  логирует таймаут `C2 unlocked` / `STM32WB_C2_LOCK_TIMEOUT`;
- реклама не стартует;
- устройство вообще не появляется в эфире.

### 14.2. Какую прошивку заливать (зависит от версии Zephyr!)

Сначала узнайте версию Zephyr: `west topdir` → в `zephyr/VERSION`.
В этом проекте — **Zephyr 4.4.99** → нужен **STM32CubeWB 1.24.0**.

| Версия Zephyr | STM32CubeWB | HCI-бинник                       |
|---------------|-------------|----------------------------------|
| 3.7           | 1.19.1      | `stm32wb5x_BLE_HCILayer*_fw.bin` |
| 4.2           | 1.23.0      | `stm32wb5x_BLE_HCILayer*_fw.bin` |
| 4.3+          | 1.24.0      | `stm32wb5x_BLE_HCILayer*_fw.bin` |

> ⚠️ **Несовпадение версий** Zephyr-модуля `hal_stm32` и прошивки CPU2 —
> одна из частых причин «не работает».
> Проверьте: `modules/hal/stm32/lib/stm32wb/README.rst` → `Status: version vX.Y.Z`.

### 14.3. Где взять бинник

ST GitHub: `STM32CubeWB/Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x/`
- `stm32wb5x_BLE_HCILayer_extended_fw.bin` — расширенная (рекомендуется)
- `stm32wb5x_BLE_HCILayer_fw.bin` — базовая

### 14.4. Как прошить CPU2

**DFU НЕ ПОДХОДИТ для CPU2** — только внешний отладчик
(ST-LINK/V2 или J-Link) на 4-пиновый SWD-разъём (P3).

Через **STM32CubeProgrammer** (внешний SWD):
```
# Для STM32WB5x (1 MB), адрес зависит от типа бинника:
BLE_HCILayer_extended → 0x080DB000
BLE_HCILayer          → 0x080E1000
FUS v2.1.0            → 0x080EE000
```

### 14.5. Включение отладочного лога HCI-драйвера

В `prj.conf` (уже добавлено):
```kconfig
CONFIG_BT_HCI_DRIVER_LOG_LEVEL_DBG=y
```
После сборки в RTT Channel 1 появятся сообщения от `hci_ipm`:
- `BleCmdBuffer: 0x...` / `EvtPool: ...` — размещение mailbox-буферов
- `C2 unlocked` — CPU2 ответил (если этого нет → проблема с CPU2)
- `Could not enable IPCC clock` / `Could not configure RF Wake up clock`
  — проблема с тактированием (HSI48/LSE в DTS)
- `-ETIMEDOUT` — таймаут ожидания готовности CPU2

### 14.5.1. Расшифровка лога (подтверждённый случай)

Реальный лог RTT при Full Stack на CPU2:
```
<dbg> hci_ipm.c2_reset: C2 unlocked                              ← CPU2 ответил ✅
<dbg> hci_ipm.bt_ipm_setup: IPM Channel Setup Completed          ← ACI setup прошёл ✅
<dbg> hci_ipm.bt_ipm_send: CMD: buf 0x20008500 type 1 len 4      ← Read Local Features (0x1003)
<dbg> hci_ipm.bt_ipm_rx_thread: EVT: evtcode: 0x0f               ← Command Status (НЕ Complete!)
<wrn> bt_hci_core: opcode 0x1003 status 0x01                      ← Unknown HCI Command!
<err> main: Bluetooth init failed: -5                             ← -EIO
```

**Расшифровка**:
- `0x1003` = `BT_HCI_OP_READ_LOCAL_FEATURES` (`hci_types.h:1107`)
- `status 0x01` = `BT_HCI_ERR_UNKNOWN_CMD` (Unknown HCI Command)
- `evtcode 0x0f` = Command Status (нужно было `0x0e` Command Complete)

**Почему ACI проходит, а HCI — нет**: ST Full Stack понимает
проприетарные **ACI-команды** (vendor-specific, `bt_ipm_setup` использует их
для задания адреса/мощности), но **не понимает стандартные HCI-команды**
Bluetooth. HCI Layer прошивка понимает и то, и другое.

**Итог**: этот лог = 100% подтверждение, что на CPU2 залит
**Full Stack** (или иная не-HCI прошивка), а не `BLE_HCILayer_extended`.

### 14.6. Порядок диагностики

1. **Включить** `CONFIG_BT_HCI_DRIVER_LOG_LEVEL_DBG=y` и пересобрать.
2. Прошить CPU1, смотреть лог в RTT (Channel 1).
3. Если **нет** `C2 unlocked` / есть `-ETIMEDOUT` → **проблема с CPU2**
   (не прошит / не та версия / не тот тип — Full Stack вместо HCI).
4. Проверить тактирование: `clk_hsi48` и `clk_lse` должны быть `okay`
   (в `weact_stm32wb55_core.dts` они уже включены).
5. Если `C2 unlocked` есть, реклама идёт, но не видно телефоном →
   проверить антенну/распайку RF, либо конфликт пинов (см. 14.7).
6. Прошить правильный `stm32wb5x_BLE_HCILayer_extended_fw.bin`
   по адресу `0x080DB000` внешним SWD.

### 14.7. Конфликт пинов (побочный)

В `app.overlay` `led1` разведён на **PB8** (`gpios = <&gpiob 8 ...>`),
а в базовом DTS платы PB8 занят `i2c1_scl_pb8` (I2C1 SCL).
Это не ломает BLE напрямую, но: (а) I2C1 и led1 нельзя использовать
одновременно; (б) при инициализации I2C пин перехватится.
Если I2C не нужен — отключите `&i2c1 { status = "disabled"; }` в overlay.
