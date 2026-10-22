# BOX Node: обратный BLE-канал, интеграционный тест

## Назначение

Проверить цикл:

1. Node как Peripheral передаёт Android уже проверенный тестовый BOX.
2. Node останавливает своё advertising на время RX-окна, сканирует и подключается к Android как Central.
3. Согласует MTU, обнаруживает сервис и notification-характеристику по UUID-префиксам, находит CCCD и подписывается.
4. Принимает CBB/CDB, содержащий только известный тестовый box_id.
5. Сохраняет ID самого CBB/CDB в RAM на 60 секунд.
6. В следующем исходящем сеансе передаёт SID → ACB → BOX → TRM → TRM. При нескольких ожидающих callback каждый ACB отправляется отдельно.

ACB содержит callback_id, НЕ box_id. Успешное помещение notification в очередь NimBLE не считается подтверждением обработки Android.

## Установка

Распакуйте этот каталог рядом с проектом `tx`, например:

    bm-node/
      tx/
      box-reverse/

Из корня `tx`:

```bash
cd /Users/nickrusfort/Desktop/dev/bm-node/tx
python3 ../box-reverse/apply_reverse.py --dry-run
python3 ../box-reverse/apply_reverse.py
idf.py build
idf.py -p /dev/cu.usbmodem5CCD0111651 flash monitor
```

Если каталог распакован иначе, укажите фактический путь к apply_reverse.py. Не выполняйте команды сборки, если установщик выдал NOT APPLIED.

Установщик рассчитан на текущий BOX-enabled main.c, а не на исходный пример bleprph. Он проверяет все точки замены до записи, отказывается от повторной установки, создаёт backup-before-reverse-ДАТА-ВРЕМЯ и добавляет исходники в main/CMakeLists.txt.

Меняются `main/main.c`, `main/CMakeLists.txt`. Добавляются `main/box_reverse.c`, `main/box_reverse.h`, `main/box_protocol.c`, `main/box_protocol.h`. `gatt_svr.c`, `bleprph.h`, `idf_component.yml`, sdkconfig не изменяются.

Необходимые настройки sdkconfig: NimBLE Central, Peripheral, Observer, Broadcaster, GATT Client и GATT Server включены; preferred MTU рекомендуется 517 (проверка допускает минимум 483). Extended advertising в этой реализации не поддерживается. Если проверка отклоняет конфигурацию, измените соответствующие параметры через `idf.py menuconfig` и повторите dry-run.

## Что искать в логах

Ниже образцы формата, а НЕ результаты запуска на плате:

```text
BOX_RX: Reverse channel enabled: TX windows alternate with RX scans
BOX_RX: RX scan started (6500 ms)
BOX_RX: Android candidate found; connecting
BOX_RX: RX connected: conn=...
BOX_RX: MTU exchange status=0 mtu=517
BOX_RX: BOX service handles=.....
BOX_RX: Subscribed: conn=... characteristic=... cccd=...
BOX_RX: RX SID=...
BOX_RX: RX CBB id=<callback_uuid> boxes=1 match_test_box=1
BOX_RX: ACB pending: callback_id=<callback_uuid> (RAM, retry 60s)
BOX_RX: RX TRM sid=...
BOX_RX: RX closing: matching termination received
BOX_RX: RX disconnected: reason=...
NimBLE_BLE_PRPH: BOX notify queued: conn=... type=ACB len=43 mtu=517
NimBLE_BLE_PRPH: ACB queued: callback_id=<callback_uuid> (not Android confirmation)
```

Вместо CBB может прийти CDB. Android не обязан оказаться в advertising-фазе с первого RX-окна: Node чередует advertising и scan, не сканирует во время собственного TX-соединения. При несовпадении фаз возможны пустые окна и повторные попытки.

На Android основной критерий: `A` увеличился после получения ACB, соответствующего ещё ожидающему CBB/CDB. В FileSyncWorker `A` увеличивается на число реально удалённых callback-объектов, поэтому повторный ACB для уже удалённого ID не обязан менять счётчик. Точную итоговую строку счётчиков не фиксируем: SID/TRM и другие сеансы могут её менять.

Не очищайте данные приложения и не перезапускайте его перед первой попыткой: очередь CBB/CDB из предыдущего теста находится в RAM. Если её уже нет, потребуется снова создать новое тестовое сообщение/BOX; этот пакет намеренно не меняет прежние box_id и message_id. Одинаковый BOX может быть отфильтрован Android как уже известный, и это само по себе не ошибка обратного BLE-канала.

Если сканирование работает, но callback не пришёл, сначала пришлите полный лог от `RX scan started` до `RX disconnected`; не считайте отсутствие CBB доказательством ошибки GATT discovery.

## Ограничения теста

- Это НЕ полноценный relay: входящие чужие BOX не сохраняются; нет постоянного хранилища сообщений.
- Очередь ACB: 10 callback ID в RAM, срок 60 секунд; перезагрузка теряет очередь.
- Подтверждаются только CBB/CDB, в которых все box_id равны тестовому `10000000-0000-4000-8000-aabbccddeeff`. Смешанные callbacks не подтверждаются, чтобы не погасить уведомления для других отправителей.
- Готовая очередь ACB повторяется при новых TX-сессиях до истечения TTL. Доставка Android не подтверждается автоматически по локальному notify rc=0.
- Проверка MTU требует минимум 483, так как Android формирует пакеты до 480 байт; фрагментация не реализована.
- Не меняет Android-код, UUID существующего Node GATT-сервиса, фиксированный test message_id и TTL сообщения.
- На радиоканале и плате эта версия ещё НЕ проверена. Полная сборка и линковка проекта этой версии также ещё не подтверждены; после установки обязательно `idf.py build`.

## Выполненные проверки

- 19 unit-тестов протокольного парсера, с AddressSanitizer и UndefinedBehaviorSanitizer.
- Установщик: dry-run, применение к fixture с точками интеграции, отказ от повторного применения, откат, отказ от неподдерживаемых исходников без частичной записи.
- Проверка наличия точек замены в текущем main.c; синтаксическая проверка используемых NimBLE API реальным ESP32-S3 компилятором и заголовками установленного ESP-IDF v5.5.5.
- Эти проверки не заменяют компиляцию всего нового модуля и аппаратный тест.

Для повторения parser-тестов на компьютере с C-компилятором, из этого каталога:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main tests/test_protocol.c main/box_protocol.c -o /tmp/box-protocol-test
/tmp/box-protocol-test
```

## Откат

Из `tx`, заменив путь на созданный установщиком backup:

```bash
python3 ../box-reverse/restore_reverse.py --backup ./backup-before-reverse-ДАТА-ВРЕМЯ --dry-run
python3 ../box-reverse/restore_reverse.py --backup ./backup-before-reverse-ДАТА-ВРЕМЯ
idf.py build
```

Откат возвращает исходные main.c и CMakeLists.txt и удаляет четыре добавленных файла. Если после установки вы вручную меняли эти шесть файлов, сначала сохраните свои изменения: восстановление их заменит.
