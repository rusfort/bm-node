# Box Node E2E — совместный тест Node ↔ Box Messenger

Самостоятельный проект ESP-IDF для одной платы ESP32-S3 (YD-ESP32-S3, N8R2).
Плата одновременно работает как GATT server (Android подключается к ней) и как
GATT client (плата подключается к Android). Проект **не зависит** от прежних
патчей в `tx/` и не изменяет его.

Проверено перед передачей:

- `idf.py build` на ESP-IDF v5.5.5, target `esp32s3`: без ошибок и предупреждений в коде проекта;
- `host_test/test_proto`: юнит-тесты и 200 000 итераций фаззинга парсера (ASan/UBSan);
- `host_test/android_sim.py`: 80 прогонов автомата против эмулятора правил
  `FileSyncWorker.java`, включая обрывы соединений — итог везде `B1|C1|D1|A2`.

На реальных устройствах комплект ещё не запускался — это первый прогон.

## Что проверяет тест

| Шаг | Обмен | Ожидание на Android |
|---|---|---|
| 1 | Node → BOX X1 с новым сообщением M1 | `B+1` |
| 2 | Android → CBB(X1); Node → ACB с ID этого CBB | `A+1` |
| 3 | Node → BOX X2 с тем же M1 | `B` не меняется |
| 4 | Android → CDB(X2); Node → ACB с ID этого CDB | `A+1` |
| 5 | Android → BOX с сообщением, неизвестным Node; Node → CBB | `C+1` |
| 6 | Android → BOX с сообщением, которое Node уже знает (M1); Node → CDB | `D+1` |
| инфо | Android → ACB на CBB/CDB узла | — |

Итоговый прирост: **`B+1 C+1 D+1 A+2`**. Node отправляет ровно один CBB и один CDB,
поэтому `C` и `D` не «накручиваются» при повторных сессиях.

На каждый запуск платы генерируются новые `message_id` и `box_id`, поэтому повторный
прогон после перезагрузки платы — снова «новое сообщение» для телефона.

## Подготовка стенда

1. Bluetooth на втором телефоне — **выключен**.
2. На тестовом телефоне перезапусти Box Messenger: в памяти очистятся старые очереди
   CBB/CDB и обнулятся debug-счётчики. Данные приложения **не** удаляй.
3. Отправь в Box Messenger одно короткое текстовое сообщение любому контакту.
   Это гарантирует шаг 5: у телефона будет сообщение, которого Node ещё не знает.
4. После старта теста приложение **не перезапускай** до итога.

## Сборка и прошивка

Распакуй архив в `~/Desktop/dev/bm-node/`, чтобы получилось `bm-node/box-node-e2e/`.

```bash
source ~/esp/esp-idf-v5.5.5/export.sh
cd ~/Desktop/dev/bm-node/box-node-e2e

idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem5CCD0111651 flash monitor
```

`set-target` создаст `sdkconfig` из `sdkconfig.defaults`: flash 8 MB, NimBLE,
роли central+peripheral, до 4 соединений, MTU 517. В `menuconfig` ничего менять не нужно.

Если собрать не получается, можно прошить готовые бинарники из `prebuilt/`:

```bash
cd ~/Desktop/dev/bm-node/box-node-e2e/prebuilt
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem5CCD0111651 -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
idf.py -p /dev/cu.usbmodem5CCD0111651 monitor   # из папки проекта
```

## Порядок теста

1. Прошей плату и открой монитор. В начале лога будут ID этого прогона:
   ```
   E2E: E2E init: run=xxxxxxxx node_suffix=aabbccddeeff ...
   E2E: E2E ids: M1=... X1=... X2=...
   ```
2. Запусти обычный обмен в Box Messenger и положи телефон рядом с платой.
3. Каждые 20 секунд плата печатает сводку шагов `[OK]`/`[..]`. Обычно тест занимает
   1–3 цикла смены фаз Android (примерно 30–90 секунд).
4. Успех на стороне Node:
   ```
   E2E: ################ E2E NODE-SIDE PASS ################
   E2E: Check Android debug counters now: expected delta B+1 C+1 D+1 A+2
   ```
5. Сверь debug-счётчики Box Messenger: после перезапуска приложения ожидается
   `B1|C1|D1|A2`. `S`, `T` и общие sent/received — не критерии (TRM шлётся дважды,
   SID создаётся на каждую сессию).

Тайм-аут теста — 15 минут; по нему печатается `E2E FAIL: timeout` и сводка с незакрытыми шагами.

## Ключевые строки лога

| Строка | Значение |
|---|---|
| `BOX_TX: subscription conn=N notify=1` | Android подписался на notifications Node |
| `BOX_TX: notify BOX-X1(new) ... ok` | Новое сообщение передано стеку BLE |
| `BOX_RX: subscribed to Android <id>` | Node подписался на GATT server Android |
| `E2E: STEP 2: Android stored M1 as new ...` | Получен CBB(X1) |
| `E2E: STEP 4: Android detected duplicate M1 ...` | Получен CDB(X2) |
| `E2E: RX BOX id=... new=1 dup=1` | Node принял BOX телефона и классифицировал сообщения |
| `E2E: STEP 5 done / STEP 6 done` | CBB/CDB узла доставлены → `C+1` / `D+1` |
| `E2E: INFO Android confirmed Node CBB` | Android вернул ACB на CBB узла (информационно) |
| `foreign callback ... no ACB` | CBB/CDB, не относящийся к этому прогону: намеренно не подтверждается, чтобы не менять `A` |

## Если шаг не закрывается

- **Шаг 1–2 стоит:** нет `subscription notify=1` — Android не подключается к Node.
  Проверь, что в логе есть `advertising Box UUID as BOX-NODE-01`.
- **Нет `BOX_RX: subscribed to Android`:** Node не нашёл сервис телефона. Пришли строки
  `BOX_RX` — там будет причина (`Box service not found`, `connect failed` и т.п.).
- **Шаг 5 ждёт `send any chat msg on phone`:** у телефона нет сообщений, неизвестных
  Node. Отправь любое текстовое сообщение в Box Messenger.
- **`Android answered CBB to duplicate X2`:** телефон сохранил M1 повторно —
  дедупликация по `message_id` не сработала. Это реальная находка, а не сбой теста.

Пришли полный лог от `E2E init` до `PASS`/`FAIL` и строку счётчиков Android.

## Состав

```
box-node-e2e/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── main.c            инициализация NVS, NimBLE, автомата
│   ├── node.h            общие константы: ID узла, префиксы UUID
│   ├── node_periph.c     advertising, GATT server, передающие сессии
│   ├── node_central.c    сканирование Android, discovery, подписка, приём
│   ├── e2e_test.c/.h     автомат теста (чистый C)
│   └── box_proto.c/.h    разбор и сборка пакетов V1 (чистый C)
├── host_test/
│   ├── test_proto.c      юнит-тесты и фаззинг парсера
│   └── android_sim.py    эмулятор правил FileSyncWorker.java
└── prebuilt/             готовые бинарники для esptool
```

Хост-тесты (по желанию, на Mac):

```bash
cd host_test
cc -std=c11 -Wall -Wextra -I ../main test_proto.c ../main/box_proto.c -o test_proto && ./test_proto
python3 android_sim.py
```

## Ограничения этой версии

- Тестовая прошивка: сообщения хранятся только в RAM, ретрансляции нет.
- TTL тестового сообщения — `2026-12-31 23:59:59` (`E2E_TTL_DEFAULT` в `e2e_test.h`),
  чтобы тестовые записи не остались на телефоне навсегда.
- Одна плата совмещает обе роли; разнесение на две платы по UART — следующий этап.
