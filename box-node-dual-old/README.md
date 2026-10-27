# Box Node Dual — совместный тест на двух платах

Box Node на двух платах ESP32-S3 (YD-ESP32-S3, N8R2), связанных UART:

| Плата | Что делает |
|---|---|
| **CORE** | BLE central: находит телефон, подключается к его GATT server и принимает сессии. Ведёт автомат теста (тот же `e2e_test.c`, что в `box-node-e2e`), строит передающие сессии. Сервис Box **не** публикует и не объявляется. |
| **TX** | BLE peripheral: объявляется как `BOX-NODE-01`, держит GATT-сервис Box. Когда телефон подписывается, просит у CORE сессию и отправляет её notifications. |

Радио на каждой плате делает одну работу: CORE больше не нужно делать паузы, чтобы
телефон успел подключиться к узлу, а TX всегда свободен для входящих подключений.

## Обмен между платами

Поверх канала `board_link` (проверен в `box-node-link`: COBS + CRC16, ACK, повторы):

```
телефон подписался на TX  ->  TX:   SUBSCRIBED(conn, mtu)        -> CORE
                              CORE: строит сессию SID..TRM
                              CORE: SESS_PKT x N (token, idx)    -> TX
                              TX:   notifications телефону
                              TX:   SESS_DONE(token, delivered)  -> CORE
                              CORE: e2e_commit_session(delivered)
```

- CORE учитывает доставку **только по `SESS_DONE`** от TX: учтены лишь пакеты, которые
  стек TX подтвердил (как раньше на одной плате).
- Нет `SESS_DONE` за 20 с, TX перезагрузился или канал не принял пакет — сессия
  считается недоставленной (`delivered=0`), шаги теста повторятся в следующей сессии.
- TX сообщает CORE свой BLE-адрес (`TX_INFO`), чтобы CORE не подключался к своей же TX.
- После перезагрузки любой платы CORE запрашивает у TX состояние (`SYNC_REQ`), TX
  повторяет подписки, которым ещё не отдана сессия.

## Что проверено до передачи

- Обе роли собираются на ESP-IDF v5.5.5 (`esp32s3`) без ошибок и предупреждений.
- `host_test/test_relay.c` (ASan/UBSan) — обмен CORE↔TX вместе с настоящим `e2e_test.c`:
  сессия на TX побайтно совпадает с построенной на CORE (MTU 517 и 185), два телефона
  (CBB на X2 от второго телефона — не ошибка, от первого — ошибка), адрес в SUBSCRIBED, частичная
  доставка, устаревший и чужой `SESS_DONE`, перезагрузка TX посреди сессии, тайм-аут,
  обрыв передачи и повтор, очередь подписок, 200 000 случайных кадров в сборщик.
- `host_test/test_link.c` — канал `board_link` (как в `box-node-link`).

Версия `dual-0.1` прошла совместный тест с одним телефоном: `PASS` за 95 с
(на одной плате — 486 с), счётчики телефона `B1 C1 D1 A2`.

## Изменения в dual-0.2

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

## Проводка

Та же, что для `box-node-link` (провода уже стоят):

```
CORE GPIO17 ──► TX GPIO18
CORE GPIO18 ◄── TX GPIO17
CORE GND    ─── TX GND
```

Каждая плата — от своего USB (`COM`), 5V/3V3 не соединять. Желательно резисторы 1 кОм
последовательно в сигнальные провода. Платы лучше разнести на 20–30 см: обе работают с BLE.

## Сборка и прошивка

Распакуй в `~/Desktop/dev/bm-node/`, чтобы получилось `bm-node/box-node-dual/`.

```bash
source ~/esp/esp-idf-v5.5.5/export.sh
cd ~/Desktop/dev/bm-node/box-node-dual

idf.py -B build_core -D NODE_ROLE=CORE set-target esp32s3
idf.py -B build_core -D NODE_ROLE=CORE -p /dev/cu.usbmodemXXXX flash monitor

idf.py -B build_tx -D NODE_ROLE=TX set-target esp32s3
idf.py -B build_tx -D NODE_ROLE=TX -p /dev/cu.usbmodemYYYY flash monitor
```

Порты — те же, что в проверке канала (CORE — `XXXX`, TX — `YYYY`). В начале лога каждой
платы: `Box Node Dual role=CORE fw=dual-0.2` / `role=TX`.

Готовые бинарники:

```bash
cd prebuilt/core   # или prebuilt/tx
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
```

## Два телефона

Сессии от TX не адресованы: TX не знает, какой телефон к ней подключился (телефон на этом
соединении — GATT client и ничего не пишет). Поэтому X1, X2, ACB и CBB/CDB узла получает тот
телефон, который подключился первым. Протокол к этому готов: телефон считает ACB только для
своих CBB/CDB, а не получив ACB, повторяет CBB — узел снова ставит ACB в очередь.

Следствие: **счётчики каждого телефона по отдельности заранее не известны**. Например, CBB узла
для бокса телефона A может получить телефон B (у него вырастет `C`). Кроме того, телефоны
обмениваются между собой напрямую, и это тоже меняет счётчики.

Критерии для двух телефонов:

- CORE: `result=PASS`, нет `!! ... duplicate NOT detected`;
- обе строки `phone ...` в статусе CORE, у обоих телефонов `sessions` растёт;
- TX: подписки обоих телефонов, все `notify ... ok`; `RELAY`: `aborted timeout=0 send=0`;
- главное, что нужно для следующего шага: совпадает ли адрес телефона на TX с адресом на
  CORE (`phone=<ID>` в строке `RELAY: Android subscribed`). Если совпадает — TX сможет
  адресовать сессии конкретному телефону.

## Подготовка стенда

Как в `box-node-e2e`:

1. Для теста с одним телефоном Bluetooth на втором — **выключен**. Для двух телефонов — включён на обоих.
2. Перезапусти Box Messenger на телефонах, которые участвуют: обнулятся debug-счётчики и очереди.
3. Отправь одно короткое сообщение любому контакту (для шага 5); при двух телефонах — с каждого.
4. До итога приложение **не перезапускай**.
5. Плату с прошивкой `box-node-e2e` (если третья) отключи: она объявляет тот же узел.

## Что должно быть в логах

TX:

```
BOX_TX: advertising Box UUID as BOX-NODE-01
BOX_TX: subscription conn=1 notify=1
BOX_TX: subscribed conn=1 mtu=517 -> CORE
BOX_TX: session start token=xxxx conn=1 mtu=517 packets=5
BOX_TX:   notify SID ... -> ok
BOX_TX:   notify BOX-X1(new) ... -> ok
```

CORE:

```
RELAY: TX BLE address xx:xx:xx:xx:xx:xx
RELAY: Android subscribed on TX conn=1 mtu=517
RELAY: session -> TX token=xxxx conn=1 mtu=517 sid=... packets=5
RELAY: session token=xxxx conn=1 done rc=0 delivered=5/5
E2E:   STEP 1: BOX X1 with new M1 delivered -> Android B+1 expected
BOX_RX: connected to Android GATT server ...
...
E2E:   ==== E2E run ........  result=PASS ====
```

Каждые 30 с: `LINK: link=UP ...` (обе платы) и `RELAY: relay: subs=... done=... aborted ...` (CORE).

## Критерии

- CORE: `result=PASS`, все шесть шагов `[OK]`.
- Телефон: прирост **`B+1 C+1 D+1 A+2`**.
- `RELAY`: `aborted timeout=0`, `send=0`; LINK: `failed=0`.
- Время до `PASS` — ориентир для сравнения с одной платой (там было около 8 минут).

Дополнительно, после `PASS` или в отдельном прогоне:

| Действие | Ожидание |
|---|---|
| `RST` на TX во время теста | CORE: `aborted (TX restarted)` (если сессия шла), затем снова `TX BLE address`; тест продолжается |
| `RST` на CORE | новый прогон теста с новыми ID; TX повторяет неотданные подписки (`resync`) |

## Что прислать

1. Лог CORE от `Box Node Dual role=CORE` до `PASS`/`FAIL` и ещё 1–2 минуты после.
2. Лог TX за то же время.
3. Счётчики каждого телефона до и после.

Отдельно интересно: нет ли в логе телефона ошибок «service not found» по адресу CORE.
CORE подключается к телефону как central и сервиса Box у себя не держит; на одной плате
телефон иногда подписывался поверх такого соединения.

## Состав

```
box-node-dual/
├── CMakeLists.txt              роль: -D NODE_ROLE=CORE|TX
├── sdkconfig.defaults          esp32s3, 8 MB, NimBLE, MTU 517
├── components/board_link/      надёжный канал UART (из box-node-link)
├── main/
│   ├── main.c                  общий старт, boot_id, NimBLE
│   ├── link_glue.c/.h          UART1 + задачи канала + очередь доставки
│   ├── relay.c/.h              протокол CORE<->TX (чистый C)
│   ├── core_main.c             CORE: автомат e2e + менеджер сессий
│   ├── core_central.c          CORE: BLE central (из box-node-e2e)
│   ├── tx_periph.c             TX: GATT server и отправка сессий
│   └── e2e_test.c/.h, box_proto.c/.h   без изменений из box-node-e2e
├── host_test/test_relay.c, test_link.c
└── prebuilt/{core,tx}/
```

Хост-тесты на Mac:

```bash
cd host_test
cc -std=c11 -Wall -Wextra -I ../main test_relay.c ../main/relay.c ../main/e2e_test.c ../main/box_proto.c -o test_relay && ./test_relay
cc -std=c11 -Wall -Wextra -I ../components/board_link/include test_link.c ../components/board_link/board_link.c -o test_link && ./test_link
```
