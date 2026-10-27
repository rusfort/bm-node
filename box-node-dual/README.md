# Box Node Dual — совместный тест на двух платах (dual-0.4)

Box Node на двух платах ESP32-S3 (YD-ESP32-S3, N8R2), связанных UART:

| Плата | Что делает |
|---|---|
| **CORE** | BLE central: находит телефон, подключается к его GATT server и принимает сессии. С dual-0.4 — **ретранслятор**: складывает принятые сообщения во flash, отвечает CBB/CDB/ACB как Android и собирает сессии раздачи для TX. |
| **TX** | BLE peripheral: держит GATT-сервис Box. Когда телефон подписывается, просит у CORE сессию и отправляет её notifications. |

## Что нового в dual-0.4

Узел впервые работает как настоящий узел сети: телефон A отдаёт ему сообщения, уходит, а
телефон B позже получает их **от узла**, хотя A и B не встречались.

- **Хранилище транзитных сообщений** (`main/store.c`) в разделе `transit` (5 МБ flash):
  журнал «только дописываем» с CRC у каждой записи, индекс в PSRAM (до 16 000 сообщений).
  Переживает перезагрузку и обрыв питания в любой момент.
- **Срок хранения — 7 суток по часам узла**, TTL сообщения не учитывается (поле хранится и
  передаётся дальше как есть). Часы узла — секунды, сохраняются в NVS раз в 10 минут; после
  обрыва питания отстают максимум на 10 минут (сообщения живут чуть дольше 7 суток).
- **Переполнение**: вытесняются самые старые сообщения. Один телефон занимает не больше 20%
  хранилища (защита от «заливки»). Недавно удалённые ID (4096) помнятся, чтобы не принимать их снова.
- **Логика ретранслятора** (`main/relay_node.c`) повторяет `FileSyncWorker` / `Queue`:
  - BOX от телефона: у каждого сообщения `unit_count + 1` (как `Box.getBoxContent`); новое →
    CBB на эту коробку, уже известное → CDB (бывают оба сразу);
  - CBB/CDB от телефона про коробки узла → узел отмечает, что у этого телефона сообщение есть,
    и отвечает ACB. На чужие колбэки ACB не шлёт — как Android (иначе отберёт колбэк у адресата);
  - ACB от телефона снимает CBB/CDB узла; не подтверждённые повторяются в каждой сессии,
    через 15 минут или 30 отправок узел перестаёт;
  - сессия раздачи: `SID`, ACB (по 10 ID), CBB, CDB, затем BOX — **по одному сообщению в
    коробке** (как у Android при `GATT_BATCH_VOLUME 480`), `TRM` ×2. До 40 пакетов в сессии,
    первыми — сообщения, которые узел отдавал реже, при равенстве — более свежие;
  - `box_id` у сообщения на узле стабильный (вычисляется из `message_id`), поэтому CBB/CDB,
    пришедший от телефона позже или после перезагрузки узла, всё равно сопоставляется;
  - телефону, от которого сообщение пришло или который подтвердил его CBB/CDB, оно больше не
    отдаётся. Если телефон в раздаче неизвестен (текущая версия Android без HELLO и адрес не
    совпал), узел отдаёт всё — лишнее телефон отсеет как дубликаты (CDB).
  - повтор сессии с тем же SID (телефон раздаёт её нескольким соединениям) узел закрывает сразу.
- **Прежний тест e2e сохранён**: `-D NODE_MODE=E2E` при сборке CORE (см. ниже).
- **Своя таблица разделов** (`partitions.csv`): `factory` 3 МБ, `transit` 5 МБ; включён PSRAM.
  Нужна **одна полная очистка flash** на обеих платах (см. «Сборка и прошивка»).
- Сессия может быть длиннее: до 40 пакетов (было 8). В логе TX тип `BOX`.

## Что проверено до передачи

- Все три варианта (CORE-RELAY, CORE-E2E, TX) собираются на ESP-IDF v5.5.5 без ошибок и
  предупреждений.
- `host_test/test_store.c` (ASan/UBSan, модель NOR-flash: запись только в стёртое, стирание
  секторами): сохранность всех полей после перезагрузки, проверка полей, истечение 7 суток,
  повторный приём истёкшего, выбор для телефона, квота 20%, заполнение и сборка мусора,
  23 000 сообщений по кругу журнала, 300 прогонов со случайным мусором во flash,
  **170 обрывов питания без вытеснения — все подтверждённые сообщения на месте**,
  3000 обрывов питания на случайном байте записи или стирания — ни одной битой записи.
- `host_test/test_relay_node.c` (ASan/UBSan) — модельные телефоны по правилам FileSyncWorker:
  A → узел → B (у B `unit_count = 2`), CBB/ACB в обе стороны, дубликат → CDB → ACB,
  перезагрузка узла (C получает всё, A своё не получает), неизвестный телефон и частичная
  доставка, 100 сообщений за 3 сессии, истечение 7 суток, отказ от неподтверждённого CBB.
- Прежние `test_relay.c`, `test_adv.c`, `test_link.c` — проходят.
- **На платах dual-0.4 не запускалась.** Не проверено на железе: работа с PSRAM и разделом
  `transit`, время монтирования, поведение Android на длинной сессии (до 40 пакетов),
  что Android принимает коробки узла и отдаёт сообщение получателю.

## Сборка и прошивка

Распакуй в `~/Desktop/dev/bm-node/`, чтобы получилось `bm-node/box-node-dual/`.

**Важно: удали старые каталоги сборки** — в них лежит прежний `sdkconfig`, и новые настройки
(PSRAM, таблица разделов) не применятся:

```bash
cd ~/Desktop/dev/bm-node/box-node-dual
rm -rf build_core build_tx build_core_e2e
```

Сборка и **первая** прошивка (с полной очисткой flash — таблица разделов изменилась):

```bash
source ~/esp/esp-idf-v5.5.5/export.sh

idf.py -B build_core -D NODE_ROLE=CORE set-target esp32s3
idf.py -B build_core -D NODE_ROLE=CORE -p /dev/cu.usbmodemXXXX erase-flash flash monitor

idf.py -B build_tx -D NODE_ROLE=TX set-target esp32s3
idf.py -B build_tx -D NODE_ROLE=TX -p /dev/cu.usbmodemYYYY erase-flash flash monitor
```

Дальше — без `erase-flash` (иначе сотрутся накопленные сообщения).

Прежний тест e2e на CORE (если нужно сравнить):

```bash
idf.py -B build_core_e2e -D NODE_ROLE=CORE -D NODE_MODE=E2E set-target esp32s3
idf.py -B build_core_e2e -D NODE_ROLE=CORE -D NODE_MODE=E2E -p /dev/cu.usbmodemXXXX flash monitor
```

Готовые бинарники (CORE — ретранслятор):

```bash
cd prebuilt/core   # или prebuilt/tx
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX erase_flash      # только первый раз
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
shasum -a 256 -c SHA256SUMS
```

В начале лога: `Box Node Dual role=CORE fw=dual-0.4`, затем на CORE:

```
RELAY: node mode: RELAY (transit store, messages kept up to 7 days)
STORE: partition transit at 0x310000, 5056 KB, 1264 sectors; index 795 KB in PSRAM
STORE: mounted 1264 sectors, 0 messages (0 bytes live), records=0 bad=0 expired=0 cap=16000
STORE: mounted in ... ms, node clock ... s
```

Если вместо `PSRAM` написано `internal RAM (no PSRAM)` — PSRAM не поднялся, узел работает,
но помнит только 400 сообщений. Пришли этот лог.

## Тест на железе: A → узел → B

Телефоны A и B (в примерах ниже у A ID `72389525976d`, у B — `70ce4cf097ea`), обе платы с dual-0.4.

1. **Только A рядом с узлом** (на B Bluetooth выключен). На A отправь 1–2 коротких сообщения
   контакту B. Ждём на CORE:
   ```
   NODE: rx BOX 1a2b3c4d from 72389525976d msgs=1 new=1 dup=0
   ```
   затем сессию через TX с `CBB` для A и `NODE: session to ... delivered N/N packets`,
   а в следующем приёме от A — `NODE: rx ACB, 1 of our callbacks confirmed`.
2. **Выключи Bluetooth на A** (или унеси его). Перезагрузи CORE кнопкой `RST` — проверка, что
   сообщение пережило перезагрузку: `STORE: mounted ..., 1 messages`.
3. **Включи Bluetooth на B.** Ждём на TX `notify BOX ... ok`, на CORE
   `NODE: session to ... delivered N/N packets, 1 messages`. На B сообщение от A должно
   появиться в чате (B — получатель), счётчик входящих коробок вырастет.
4. Через минуту на CORE `NODE: rx CBB ... from 70ce4cf097ea boxes=1 ours=1` — B подтвердил,
   узел отметил, что сообщение у B есть.
5. Включи A снова и посмотри строку `RELAY: session -> TX ... phone=...`:
   - `phone=<ID>` (адрес телефона совпал с адресом на CORE) — в сессиях для этого телефона нет
     `BOX` с сообщениями, которые у него уже есть;
   - `phone=?` (текущая версия Android, адрес не совпал) — узел не знает, кто подключился, и
     отдаёт все сообщения подряд (до 35 за сессию). Телефон ответит CDB, сообщения у него
     не задвоятся. Это ожидаемо до появления HELLO в Android.

Каждые 30 с на CORE:

```
NODE: node: rx sess=.. known=.. boxes=.. msgs new=.. dup=.. rej=.. cbb=.. cdb=.. acb=.. | tx sess=.. msgs=.. pkts=.. | wait cb=.. acb=.. acked=.. gaveup=..
STORE: store: msgs=../16000 live=..KB sectors used=.. free=.. | put new=.. dup=.. rej=.. | gc=.. copied=.. evicted=.. expired=.. io=0 clock=..s
```

Критерии: сообщение A дошло до B без встречи A и B; после перезагрузки CORE сообщения на
месте (`mounted ..., N messages`, `bad=0`); `io=0`; на TX все `notify ... ok`;
`RELAY: ... aborted timeout=0 send=0`.

## Что прислать

1. Лог CORE от `Box Node Dual role=CORE` до конца теста (с перезагрузкой на шаге 2).
2. Лог TX за то же время.
3. Дошло ли сообщение до B (скриншот чата) и счётчики телефонов до/после.

## Проводка

Та же, что для `box-node-link` (провода уже стоят):

```
CORE GPIO17 ──► TX GPIO18
CORE GPIO18 ◄── TX GPIO17
CORE GND    ─── TX GND
```

Каждая плата — от своего USB (`COM`), 5V/3V3 не соединять. Желательно резисторы 1 кОм
последовательно в сигнальные провода. Платы лучше разнести на 20–30 см: обе работают с BLE.

## Состав

```
box-node-dual/
├── CMakeLists.txt              роль: -D NODE_ROLE=CORE|TX, логика CORE: -D NODE_MODE=RELAY|E2E
├── partitions.csv              factory 3 МБ + transit 5 МБ
├── sdkconfig.defaults          esp32s3, 8 MB, NimBLE, MTU 517, PSRAM, своя таблица разделов
├── components/board_link/      надёжный канал UART (из box-node-link)
├── main/
│   ├── main.c                  общий старт, boot_id, NimBLE
│   ├── link_glue.c/.h          UART1 + задачи канала + очередь доставки
│   ├── relay.c/.h              протокол CORE<->TX (чистый C)
│   ├── core_main.c             CORE: логика узла, хранилище во flash, часы узла, менеджер сессий
│   ├── core_central.c          CORE: BLE central
│   ├── store.c/.h              хранилище транзитных сообщений (чистый C)
│   ├── relay_node.c/.h         логика ретранслятора (чистый C)
│   ├── tx_periph.c             TX: GATT server, scan response, HELLO, отправка сессий
│   ├── box_adv.c/.h            scan response, HELLO, выбор подключений (чистый C)
│   └── e2e_test.c/.h, box_proto.c/.h
├── host_test/test_store.c, test_relay_node.c, test_relay.c, test_link.c, test_adv.c
└── prebuilt/{core,tx}/         CORE — ретранслятор
```

Хост-тесты на Mac:

```bash
cd host_test
F="-std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I ../main"
cc $F test_store.c ../main/store.c ../main/box_proto.c -o test_store && ./test_store
cc $F test_relay_node.c ../main/relay_node.c ../main/store.c ../main/box_proto.c -o test_relay_node && ./test_relay_node
cc $F test_relay.c ../main/relay.c ../main/e2e_test.c ../main/box_proto.c -o test_relay && ./test_relay
cc $F test_adv.c ../main/box_adv.c ../main/box_proto.c -o test_adv && ./test_adv
cc -std=c11 -Wall -Wextra -I ../components/board_link/include test_link.c ../components/board_link/board_link.c -o test_link && ./test_link
```

## Ранее

Версия `dual-0.1` прошла совместный тест с одним телефоном: `PASS` за 95 с, счётчики
`B1 C1 D1 A2`; `dual-0.2` — два телефона, `PASS` за 405 с. dual-0.3 (scan response, HELLO)
на платах не запускалась; её поведение сохранено.

### Изменения в dual-0.2

- **CORE не подключается к своей TX.** Адрес TX не забывается при её перезагрузке; пока TX
  на связи, но адрес ещё не пришёл, CORE не подключается ни к кому
  (`waiting TX address from TX board`). Если всё же попала на TX (`self loop`), адрес
  запоминается, и повторов не будет.
- **Очерёдность телефонов на CORE.** Телефон, обслуженный за последние 20 с, CORE берёт
  только если за 1,5 с скана не нашлось другого (`(recent, no other phone found)`).
- **Тест различает телефоны** по ID из callback (последние 12 символов). Ошибка дедупликации —
  только если CBB на X2 прислал тот телефон, который уже подтвердил M1 через CBB(X1).
  Другой телефон мог впервые увидеть M1 именно в X2, и его CBB — правильный ответ.
  В статусе — строки `phone <ID>: CBB=.. CDB=.. | X1 new=.. X2 new=.. X2 dup=..`.
- **Диагностика адресов.** TX пишет адрес телефона при подключении и передаёт его CORE в
  `SUBSCRIBED`. CORE сопоставляет его с адресами, по которым сама подключалась к телефонам:
  `phone=<ID>` или `phone=? (address not seen by CORE)`. Раз в 30 с CORE печатает
  `phone <ID> addr=.. connects=.. sessions=..`.

### Изменения в dual-0.3 (сторона узла по спецификации «кто раздаёт первым» v2)

- **TX объявляет себя в scan response**: Service Data 128-bit с `SERVICE_UUID`, `ver=1`,
  флаги `NODE|SERVING`, ID узла `aabbccddeeff`. Имя `BOX-NODE-01` в эфире больше не
  объявляется: вместе с Service Data оно не помещается в 31 байт (имя GAP после
  подключения осталось).
- **TX принимает HELLO** (`V1\nHLO\n<id>\n<flags>[\n<время>]`) и передаёт ID телефона на CORE
  в `SUBSCRIBED`: строка `RELAY: ... phone=<ID> (HELLO)`. Без HELLO ждёт 300 мс после
  подписки (было 150 мс) и работает как раньше.
- **CORE выбирает, к кому подключаться**, по scan response:
  - телефон новой версии без `SERVING` (фаза приёма) — не подключается:
    `phone <ID> is in scan phase (not SERVING) -> wait`. Это убирает `CCCD write failed rc=7`
    и 20-секундные `hold timeout`;
  - телефон с `SERVING` или другой Box Node — подключается;
  - старая версия (scan response без данных или его нет 0,8 с) — подключается как раньше:
    `(old app, no scan data)`;
  - своя TX узнаётся по `NODE` + свой ID ещё до подключения: `own TX found by scan response`.
- **CORE пишет HELLO** телефону новой версии перед подпиской на CCCD. Старым версиям не пишет.
- Скан CORE без фильтра повторов: иначе смена `SERVING` у телефона была бы не видна.

Логика вынесена в `main/box_adv.c` (чистый C) и проверена `host_test/test_adv.c`.

#### Что можно проверить уже сейчас, с текущей версией Android

Текущий Box Messenger не объявляет scan response и не пишет HELLO, поэтому для него узел
должен работать **как dual-0.2**: тот же `PASS`, в логе CORE `(old app, no scan data)`, на
TX `phone=? (no HELLO)`. Это и есть проверка совместимости.

Если хочется проверить новую ветку без Android, можно изобразить «телефон новой версии»
рекламой из nRF Connect (Advertiser), если он позволит задать 128-битный Service Data:
UUID `12345678-1234-5678-9012-123456789abc`, данные `01 00 72 38 95 25 97 6d` — CORE
должна писать `not SERVING -> wait` и не подключаться; с `01 02 ...` — подключиться
(и отключиться с `Box service not found`, потому что сервиса у nRF Connect нет).

