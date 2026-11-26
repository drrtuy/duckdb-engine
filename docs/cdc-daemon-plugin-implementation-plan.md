# План первой реализации batched CDC как MariaDB daemon plugin

## Статус и цель

Первый функциональный MVP реализован в `cdc/` и зарегистрирован как `DUCKDB_CDC` в `ha_duckdb.so`. Разделы ниже сохраняют исходный план; статус и ограничения реализации приведены отдельно. Это не утверждение о готовности всех описанных гарантий или достижении целевых 400 TPS.

### Реализованный объём

- В этой ветке фоновый daemon plugin включён в сборку по умолчанию (`DUCKDB_CDC=ON`, отключение сборки — явным OFF), но runtime enablement по-прежнему OFF; reader и один applier работают отдельными потоками внутри `mariadbd`. Штатный пакетный `duckdb.cnf` сохраняет `plugin-maturity=gamma` и загружает engine и существующие функции по именам, без автозагрузки экспериментального CDC. Для загрузки CDC оператор явно задаёт `plugin-maturity=experimental` и `plugin-load-add=DUCKDB_CDC=ha_duckdb.so`. Доставка CDC в библиотеке не означает включение репликации.
- Connector/C связан приватным PIC static archive с локально скрытыми символами. `dlmopen` не используется: он оказался небезопасен при повторном создании потоков с отдельной libc.
- Reader требует TLS с проверкой сертификата, `mysql_native_password`, ROW/FULL/CRC32. Чтение восстанавливается по file/position, а MariaDB GTID используется для распознавания исходных транзакционных границ. Автоматическое GTID-переключение источника не реализовано.
- Сегментированный spool с CRC32, групповым fsync, exclusive lock, проверкой identity, восстановлением незавершённого хвоста и удалением целиком применённых старых сегментов. `Run`/`flush_group` в `cdc/plugin.cc` накапливают только завершённые source transactions; `Spool::AppendBatch` в `cdc/spool.cc` публикует index/sequence после sync всех затронутых сегментов. Reader reconnect cursor может опережать durable received только в пределах живого RAM-буфера, не сохраняя это опережение как checkpoint.
- Batches из целых исходных транзакций применяются через локальный SQL service и `BINLOG` внутри одного SQL transaction. Данные и checkpoint в `_duckdb_cdc.checkpoints` фиксируются вместе.
- Ограниченные CREATE/DROP/RENAME/TRUNCATE и ADD/DROP COLUMN, принудительный `ENGINE=DuckDB`, durable journal и остановка на неподдерживаемом DDL. `Applier::Impl::Ddl/Recover` в `cdc/applier.cc` записывают снимки обоих каталогов до DDL и completion marker после успешной проверки. При совпадении исходного состояния операция повторяется; выполненные DROP/RENAME распознаются без повтора, TRUNCATE допускает безопасный повтор, а подтверждённый marker позволяет закончить checkpoint без повторного CREATE/ALTER.
- OFF/ON, restart, ротация и выгрузка CDC управляют потоками кооперативно. Ранний engine callback `quack_pre_shutdown` в `ha_duckdb_quack.cc` сначала сохраняет остановку Quack, затем вызывает `cdc::Shutdown` из `cdc/plugin.cc` и join-ит CDC до демонтажа plugin/table infrastructure. Поздние engine/daemon deinit также вызывают идемпотентную остановку как fallback.

### Границы первого MVP

- Bootstrap должен заранее обеспечить совпадающую схему и данные в настроенной базе вместе с исходной file/position. Независимые локальные записи и DDL запрещены эксплуатационным контрактом, но не блокируются отдельным engine-level механизмом; аналитическим пользователям нужны только права чтения.
- Один источник, одна база и один поток. Транзакции за пределами настроенной базы не фильтруются молча, а блокируют применение.
- Максимальная исходная транзакция ограничена 64 MiB. Запланированная потоковая обработка более крупных транзакций пока отсутствует. Начальный лимит spool — 1 GiB; минимальный настраиваемый — 256 MiB.
- Старые записи `_duckdb_cdc.ddl_intents` без снимков требуют ручной сверки. Новый `_duckdb_cdc.ddl_recovery` хранит исходные/итоговые снимки и completion marker. При расхождении каталогов восстановление останавливается. Для CREATE/ALTER остаётся неоднозначное окно после выполнения SQL, но до durable marker: изменившуюся схему без marker нельзя автоматически подтвердить, пока не реализован ожидаемый post-DDL schema fingerprint. Такой случай проверен SIGKILL-тестом и остаётся fail-closed. Снимки включают MySQL SHOW CREATE и DuckDB names/types/nullability/defaults; это обнаружение изменений относительно собственных снимков, не доказательство полной междвижковой эквивалентности типов.
- DDL-parser намеренно ограничен ASCII, не допускает выражения DEFAULT, UNIQUE, generated columns, partitioning и произвольный ALTER. DDL с TIMESTAMP отклоняется до реализации исходного временного контекста и неявных timestamp-defaults. Это ограничение DDL, не общий запрет row events для заранее созданных temporal-колонок.
- Не реализованы XA, MINIMAL/FULL_NODUP, сжатые/зашифрованные event formats, автоматический failover и resnapshot. Source file positions ограничены 32 битами текущего Connector/C dump API.
- Счётчики `received`/`applied` означают последовательности spool records, включая служебные границы rotate; `spool_bytes` — физически удерживаемые байты, включая применённый хвост активного сегмента. Новые `pending_records`/`pending_bytes` считают точную известную неприменённую очередь и payload bytes (включая FDE/rotate), не число пользовательских транзакций. `received_file/position` и `applied_file/position` публикуют source coordinates. `pending_age_seconds` — возраст самого старого известного события очереди, а `apply_lag_seconds` становится UNKNOWN при отсутствии открытого source connection. Это локальные spool snapshots, а не гарантия достижения upstream head. `metrics_ready=0` означает незавершённую инициализацию; timestamp=0 означает неизвестное время. Заголовки имеют секундное разрешение, отрицательные clock-skew интервалы ограничены нулём.
- `last_apply_us`/`apply_us`/`apply_calls` включают завершённые попытки Apply, в том числе ошибочные; `apply_stage` показывает текущую стадию. `last_injection_us` измеряет BINLOG encoding/application, `last_checkpoint_us` — checkpoint UPDATE вместе с вызываемым им engine flush, `last_commit_us` — явные COMMIT. `last_ddl_us` охватывает всю DDL-операцию и пересекается с checkpoint/commit durations. Отдельная чистая DeltaAppender flush duration пока не инструментирована. SHOW-переменные снимаются по отдельности и не образуют атомарный многоколоночный snapshot.
- Connect/read/write timeouts не дают строгой верхней границы времени DNS resolution или завершения DuckDB commit.
- Полный CDC performance gate на 8 × 10 млн строк ещё не выполнен; результаты native replication не являются измерением этого CDC.

### Сборка и проверка

Нужны соответствующий серверу MariaDB plugin SDK, Connector/C headers и пригодный для shared linkage OpenSSL-built `libmariadb.a` (либо `libmariadbclient.a`), OpenSSL и ZLIB development files. Проверка API/link на configure отклоняет несовместимый client archive, включая GnuTLS archive при текущем OpenSSL link configuration. Символы client archive должны оставаться скрытыми в конечном `ha_duckdb.so`; динамическая зависимость от `libmariadb.so` не требуется. Пакетный путь `build.sh -p` явно включает CDC даже при старом cache OFF; прямой CMake использует default ON только для новой/непереопределённой опции.

```bash
cmake -S . -B build -DDUCKDB_CDC=ON
cmake --build build --target duckdb duckdb_cdc_spool_test duckdb_cdc_spool_group_test duckdb_cdc_ddl_test duckdb_cdc_events_test duckdb_cdc_ddl_recovery_test duckdb_cdc_metrics_test -j 5
./build/duckdb_cdc_spool_test
./build/duckdb_cdc_spool_group_test
./build/duckdb_cdc_ddl_test
./build/duckdb_cdc_events_test
./build/duckdb_cdc_ddl_recovery_test
./build/duckdb_cdc_metrics_test
python3 -B mysql-test/duckdb/cdc_binlog_gate.py
python3 -B mysql-test/duckdb/cdc_plugin_integration.py
python3 -B mysql-test/duckdb/cdc_ddl_recovery_integration.py
python3 -B mysql-test/duckdb/cdc_spool_group_integration.py
python3 -B mysql-test/duckdb/cdc_active_shutdown_integration.py
python3 -B mysql-test/duckdb/cdc_shutdown_order_gdb.py
```

Интеграционные scripts используют отдельные datadir в `/tmp`, сохраняют артефакты и останавливают только созданные ими серверы. Их текущие пути source/target binaries и портов заданы в самих scripts.

Проверенный локальный стенд: source 11.4.14, target и SDK 11.4.15. BINLOG gate прошёл 17 проверок атомарности, rollback и отклонения частичных after-images, включая FULL → MINIMAL/FULL_NODUP внутри одного пакета. End-to-end daemon test прошёл 72 проверки (включая новые metrics assertions): объединение 100 исходных транзакций, packet chunking при `max_allowed_packet=16 KiB`, OFF/ON, rotate, SIGKILL/restart, недоступный/restarted source, startup enablement, TLS rejection, DDL и сохранение backlog/intent. Три первоначальных standalone unit executables прошли обычную сборку и ASan/UBSan; добавлены и проверены recovery/metrics unit targets. Отдельный DDL recovery integration прошёл 28 проверок с четырьмя SIGKILL: prepared TRUNCATE, marked ALTER, unmarked RENAME и безопасная остановка на unmarked ALTER. Ранее выбрано и пройдено девять native/security MTR runs. Это не полный MTR suite и не original-scale performance coverage.

Обычная сборка с `-DDUCKDB_CDC=OFF` отдельно проверяется BINLOG gate под неизменённой gamma-политикой: `DUCKDB_TEST_MATURITY=gamma python3 -B mysql-test/duckdb/cdc_binlog_gate.py`.

### Доставка CDC в пакетах ветки repl

Сборка CDC включена по умолчанию и для прямого CMake package path в CI. `build.sh -p deb|rpm|tgz` дополнительно передаёт явный ON, чтобы старый cache OFF не исключил CDC из пакета. Workflow `.github/workflows/build-foundry.yml` включает push ветки `repl`; в build prerequisites добавлены ZLIB development packages. Connector/C headers ищутся в layouts mariadb/mysql, а скрытие client symbols использует basename выбранного static archive. При standalone layout CMake задаёт `etc/my.cnf.d` только если SDK не определил INSTALL_SYSCONF2DIR, чтобы TGZ также содержал конфиг.

Доставка остаётся в существующем `ha_duckdb.so`/пакете движка, без выделения отдельного CDC `.so` и без изменений shutdown lifecycle. Стандартный конфиг содержит gamma и поимённую загрузку engine + 14 существующих функций. Экспериментальный daemon не загружается автоматически. Для него оператор отдельно разрешает experimental и добавляет named load; после загрузки runtime всё равно OFF, до согласованного bootstrap и явного включения.

Новая проверка: `python3 -B mysql-test/duckdb/cdc_package_smoke.py <extracted-package-root>`. Она использует фактические упакованные `.so` и `duckdb.cnf`, создаёт отдельный datadir, проверяет gamma startup без CDC, named experimental CDC startup с runtime OFF, сохранение строки после restart, private client exports и exit=0. `--server`, `--install-db`, повторяемый `--install-db-arg` позволяют использовать отдельную совместимую установку. Неправильный archive отдельно проверен отрицательным configure-тестом: отказ вместо молчаливого отключения CDC.

Локальные проверки выполнены на SDK/server 11.4.15: DEB и standalone TGZ собраны, smoke по 86 checks pass (`/tmp/cdc-package-lyf5qyh8/`, `/tmp/cdc-package-akz1ikc3/`). До включения сборки этот тест обнаруживал отсутствие DUCKDB_CDC; первоначальный TGZ также обнаружил отсутствие конфига, исправленное fallback-ом. CDC integration: 72 pass (`/tmp/cdc-plugin-jh_41q2r/`), deterministic shutdown regression pass (`/tmp/cdc-plugin-5n3v9y33/`). Дополнительно реальный `build.sh -p deb` успешно пересобрал пакет из cache с заранее установленным CDC=OFF; smoke его извлечённого payload также 86 pass (`/tmp/cdc-package-cjovjlwz/`). Полный MTR/RPM и удалённый GitHub CI этим не подтверждаются. Установленный в окружении SDK 13.2.2 выявил существующую несовместимость этой ветки по ha_table_exists в `duckdb_udf_wrappers.cc`, ещё при CDC=OFF; она не исправлялась в данном шаге. Системный MariaDB/datadir не изменялся: недостающие build tools извлечены в изолированный `/tmp/cdc-package-tools.c3fTB0/`.

### CDC workload smoke и открытый performance gate

`mysql-test/duckdb/cdc_benchmark.py` поддерживает `--batch-timeout-ms` и `--batched-only`. По умолчанию сравнивает два настоящих CDC target на одном quiescent snapshot: per-TX (`batch_bytes=1024`, singleton подтверждается по metrics) и batched. Подготовка sysbench выполняется без binlog; snapshot и итоговые ordered-row SHA256 передаются потоково, а не собираются целиком в RAM. Во время нагрузки сохраняются source head и CDC metrics, после неё выполняется ограниченный drain. Watchdog требует минимум 10 GiB свободного диска и 2 GiB MemAvailable. Все datadir/logs сохраняются. Native barrier arm отсутствует на текущем непатченом сервере.

```bash
python3 -B mysql-test/duckdb/cdc_benchmark.py "$(mktemp -d /tmp/cdc-bench.XXXXXX)"
```

Первый smoke: 2 × 1 000 строк, 4 клиента, requested 100 TPS, 10 секунд. Получено 1 011 транзакций / 100.99 TPS, 0 reads и 4 writes на транзакцию. Per-TX сделал 1 011 batches, batched — 79; оба догнали источник, полные ordered-row hashes совпали. Артефакты: `/tmp/cdc-bench.uULJZu/summary.json`.

Расширенный smoke: 8 × 10 000 строк, 16 клиентов, requested 400 TPS, 30 секунд, drain до 60 секунд. Получено 12 082 транзакции / 402.57 TPS. Batched применил все 12 082 в 382 batches и прошёл полное сравнение hashes; per-TX применил 2 475 и не догнал источник. Во время активной нагрузки approximate applied source timestamp age у batched вырос примерно с 1.2 до 13.2 секунды. **Устойчивые 400 TPS этим прогоном не подтверждены**, даже при успешном догонянии после нагрузки. Артефакты: `/tmp/cdc-bench400.JpqvUg/summary.json`. Это 80 тысяч, не 80 миллионов строк; обе реплики делят дисковые/CPU ресурсы одного хоста.

На части load/drain samples расширенного smoke у batched локальная очередь была мала при заметном отставании received position от source head: следующий профиль должен разделить стоимость receive/spool fsync и target apply. Точная причина intake-отставания пока не доказана; нельзя объявлять fsync или сетевой протокол установленной причиной только по этим метрикам. Перед 80M gate нужны устранение роста lag на малом стенде и отдельный capacity preflight; runner пока намеренно ограничен 1M суммарных строк.

### Последовательный tuning batch timeout

Серия 100 → 250 → 500 ms: один batched target, без конкурирующего per-TX target; 8 × 10 000 строк, 16 клиентов, requested 400 TPS, 60 s активной нагрузки, до 120 s drain; лимит batch 4 MiB. Каждый прогон начинает с новой копии данных без backlog. Межпрогонное равенство наполнения/seed не требуется по решению пользователя; source/target hashes внутри каждого прогона проверены и совпали. Это не сравнение с предыдущим dual-target прогоном.

| Timeout, ms | Actual source TPS | Receive records/s, последние 30 s | Apply records/s, последние 30 s | Approx apply age в конце нагрузки, s | Drain после нагрузки, s | Batches во время нагрузки |
|---|---|---|---|---|---|---|
| 100 | 399.23 | 108.42 | 108.69 | 25.39 | 50.76 | 315 |
| 250 | 401.75 | 141.39 | 140.55 | 23.41 | 44.21 | 174 |
| 500 | 401.96 | 173.91 | 175.15 | 20.06 | 34.71 | 100 |

Все три target догнали источник за drain; все серверы завершились с кодом 0. Локальная очередь в конце нагрузки составляла 31 / 77 / 111 records; основное отставание уже находилось перед durable received cursor. Approx apply age во второй половине нагрузки рос на 0.734 / 0.641 / 0.575 s/s соответственно. Увеличение timeout улучшило наблюдаемую сходимость и сократило drain примерно на 32% для 500 ms против 100 ms, но **ни один прогон не удержал 400 TPS без роста lag**. Это один последовательный прогон на настройку, не статистическое доказательство универсального оптимума. Source timestamp resolution — одна секунда; source/target работают на одном хосте.

Артефакты: `/tmp/cdc-timeout-sweep.uF2Oru/{100ms,250ms,500ms}/summary.json`, `samples.jsonl`, logs, datadir. `steady_second_half` содержит расчетную скорость records и линейный slope возраста событий, `load_end_metrics` — снимок после завершения генератора, `catchup_after_load_seconds` — первый обнаруженный catch-up. Совпадение начальных hashes между прогонами не является критерием. Повторная серия через общий SQL fixture не запускалась, код fixture reuse убран. Default плагина остался 100 ms; 500 ms — кандидат для дальнейших нагрузочных проверок, не подтверждение production-готовности.

```bash
python3 -B mysql-test/duckdb/cdc_benchmark.py "$(mktemp -d /tmp/cdc-timeout.XXXXXX)" \
  --batched-only --batch-timeout-ms 500 --tables 8 --rows 10000 \
  --threads 16 --rate 400 --seconds 60 --drain-seconds 120
```

### Проверка timeout 1 → 2 → 5 секунд

Продолжение той же single-target методики: 8 × 10 000 строк, 16 клиентов, requested 400 TPS, 60 s нагрузки и до 120 s drain, batch ≤4 MiB. Плагин и серверные binaries не менялись; начальное наполнение между прогонами может различаться. Startup timeout/bytes проверены через runtime system variables, source/target ordered-row hashes внутри каждого прогона совпали, оба сервера в каждом случае завершились с кодом 0.

| Timeout, s | Actual source TPS | Receive records/s, последние 30 s | Apply records/s, последние 30 s | Approx apply age в конце нагрузки, s | Drain, s | Batches за нагрузку | Рост apply age, s/s |
|---|---|---|---|---|---|---|---|
| 1 | 401.98 | 160.31 | 159.38 | 23.04 | 40.57 | 54 | +0.610 |
| 2 | 401.99 | 162.81 | 160.55 | 31.96 | 62.87 | 28 | +0.579 |
| 5 | 401.99 | 178.24 | 209.91 | 24.06 | 53.79 | 11 | +0.533 |

**5 s не устранило растущий lag.** Во всех случаях slope остаётся положительным, а receive rate существенно ниже source rate. Для 5 s к завершению нагрузки received=15 466, applied=14 871 при 24 123 исходных транзакциях: локальная очередь — 595 records, ещё 8 657 source transactions не дошли до durable received cursor. Последний input batch — 765 records / 1 413 257 payload bytes; до лимита 4 MiB он не дошёл. Суммарное время вызовов Apply за активную нагрузку — около 1.24 s; это не профиль всего applier thread и не включает spool reads/ожидания.

По наблюдённому drain лучший результат среди всех шести настроек по-прежнему у 500 ms (34.71 s), но единичные последовательные прогоны с разным наполнением и общей дисковой системой не доказывают универсальный оптимум. Линейная экстраполяция размера timeout в пропускную способность не подтвердилась. Дальнейшее увеличение timeout без receive/spool профиля не обосновано. Default плагина не менялся.

Артефакты: `/tmp/cdc-timeout-long.yF2NR8/{1000ms,2000ms,5000ms}/summary.json`, `samples.jsonl` и logs/datadir.

### Изоляция receive/spool: применение временно отключено

Диагностический прогон: один target, 8 × 10 000 строк, 16 клиентов, requested 400 TPS, 60 s нагрузки; runtime batch timeout 1000 ms, batch ≤4 MiB. Временная сборка `cdc/plugin.cc` переводила applier loop в `CAPTURE_ONLY` до чтения spool для применения: не выполнялись BINLOG/SQL apply, checkpoint update, чтение очереди applier-ом и GC. Reader, `Spool::Append` и его fsync в `cdc/spool.cc` оставались неизменными. При отключённом applier timeout больше не задаёт период применения; это capture-only измерение с той же конфигурацией, а не batching fsync на одну секунду.

| Метрика | Обычный apply, timeout 1 s | Capture-only, timeout 1 s |
|---|---|---|
| Actual source TPS | 401.98 | 401.96 |
| Durable receive records/s, последние 30 s | 160.31 | 161.58 |
| Source transactions всего | 24 123 | 24 123 |
| Received к концу нагрузки | 15 295 | 16 061 |
| Approx received event age в конце нагрузки | 23.04 s | 20.63 s |
| Applied к концу нагрузки | 15 129 | 0 |
| Apply calls | 54 | 0 |

Capture-only receive age во второй половине нагрузки рос примерно на +0.626 s/s; приём не успевал за источником даже без target apply. После остановки генератора durable received cursor догнал source head за 28.05 s. Это время receive drain, его нельзя напрямую сравнивать с apply drain обычного режима.

После остановки серверов отдельная утилита повторно открыла spool, проверила CRC и непрерывность всех 24 123 records и конечные coordinates `source-bin.000001:51997964`; physical retained bytes=46 134 742. `applied=0`, `apply_calls=0`, checkpoint остался `0 / source-bin.000001:1333`, hashes пользовательских таблиц target не изменились относительно bootstrap. Оба сервера завершились с кодом 0.

**Вывод:** ограничение воспроизводится на участке source dump/TLS/client receive/durable spool и не требует наличия target application. Какой именно этап внутри этого участка доминирует, пока не измерено; считать виновником fsync только по этому прогону нельзя. Разница receive rate 160.31 → 161.58 не показывает существенного ускорения после удаления applier на данном стенде.

Артефакты: `/tmp/cdc-capture-only.cifFtw/run1000ms/summary.json`, `samples.jsonl`, spool/datadir; временный patch, диагностический `plugin/ha_duckdb.so` и `verify_spool.cc` сохранены в `/tmp/cdc-capture-only.cifFtw/`. Runner поддерживает `--plugin /path/to/ha_duckdb.so --capture-only --batched-only`: флаг сам не отключает код, а проверяет sentinel, отсутствие применения, неизменность target и catch-up именно received cursor. Обычный исходник и `build/ha_duckdb.so` восстановлены ещё до измерения; после эксперимента обычная сборка прошла 72 end-to-end проверки. Никакого постоянного capture-only bypass в production code не оставлено.

### Диагностический шаг 1: capture-only без per-transaction fsync

По отдельному подтверждению пользователя выполнен ровно один следующий шаг: тот же capture-only bypass плюс удаление только `Sync(s.active.value)` после header/body writes в `Spool::Append` (`cdc/spool.cc`). Запись файлов, CRC, memory index, metrics и startup/segment/directory sync оставлены. Это **недолговечный buffered capture**: поле с именем received в диагностических samples не означает durable prefix. Плагин и benchmark помечают режим `CAPTURE_ONLY_NO_TX_SYNC` / `received_durability=buffered-no-transaction-fsync`. Checkpoint и applied не продвигаются.

Условия прежние: один target, 8 × 10 000 строк, 16 клиентов, 60 s нагрузки при requested 400 TPS, timeout 1000 ms. Source выдал 24 123 транзакции / 401.97 TPS. В последние 30 s buffered receive составил **400.78 records/s** вместо **161.58 records/s** в durable capture-only baseline. Approx received event age в этом окне держался около 1.01 s (max 1.05 s), slope −0.004 s/s вместо +0.626 s/s. После завершения генератора observed event age был 0.43 s; все 24 123 records уже были приняты, первый catch-up sample зафиксирован через 0.19 s вместо 28.05 s в baseline.

В конце нагрузки `apply_calls=0`, `applied=0`, checkpoint `0 / source-bin.000001:1333`, hashes target равны bootstrap. Оба сервера завершились с кодом 0. Сохранение данных при power loss этим тестом не проверялось и не гарантируется: успешное закрытие и чтение page cache не заменяют fsync.

**Вывод:** исключение per-transaction fsync в этом прогоне сняло ограничение intake при 400 TPS. Это сильное указание на стоимость синхронизации/ожидания хранилища, а не доказательство production-ready durable throughput. Следующий практический кандидат — групповой fsync с публикацией durable cursor только после успешной синхронизации. К нему и к исходному шагу 2 (отключение всего spool) автоматически не переходили: после каждого шага требуется подтверждение пользователя.

Артефакты: `/tmp/cdc-capture-nosync.0Cn0ZR/run1000ms/summary.json`, `samples.jsonl`, logs/datadir; patch и отдельная диагностическая `.so` — `/tmp/cdc-capture-nosync.0Cn0ZR/`. Обычные `cdc/plugin.cc`, `cdc/spool.cc` и `build/ha_duckdb.so` восстановлены до начала нагрузки: все три SHA256 совпали с исходными, `duckdb_cdc_spool_test` прошёл. Никакого fsync bypass в обычной сборке не осталось. В runner добавлен только проверочный флаг `--capture-no-fsync`; он не меняет поведение плагина и требует соответствующий sentinel диагностической сборки.

### Групповой durable fsync: реализация и один нагрузочный прогон

По подтверждённому шагу реализован реальный group fsync, не semisync и не отключение durability. Startup-параметры `duckdb_cdc_spool_group_timeout_ms=1000` (диапазон 1..60000 ms) и `duckdb_cdc_spool_group_bytes=4194304` (1024..64 MiB) независимы от target `batch_timeout_ms`/`batch_bytes`. В RAM накапливаются целые source transactions; byte budget считает framed bytes (48-byte header + filename + payload). Одна source transaction больше group budget не делится и фиксируется отдельной группой. Таймаут — порог начала flush, не realtime-гарантия завершения sync: проверяется между сетевыми операциями, время connect/read/fsync может добавить задержку. Stop/OFF синхронизирует завершённый RAM-хвост; неполная source transaction не публикуется.

`Spool::AppendBatch` в `cdc/spool.cc` держит mutex на записи и sync, синхронизирует старый активный сегмент перед rotation/close и последний сегмент перед публикацией всех новых entries/sequence. При ошибке published bytes/sequence не продвигаются, instance переходит в failed state. При recovery валидный полный префикс синхронизируется до возврата конструктора; неполный последний record обрезается, полная CRC-коррупция остаётся fail-closed. Прерванная группа может восстановиться как префикс целых source transactions, а не обязательно целиком.

Reader сохраняет отдельный volatile reconnect cursor вместе с живым буфером и публикует `received`/pending metrics только после успешного durable append. `buffered_records`/`buffered_bytes` показывают завершённые, ещё не durable RAM records и не входят в `pending_records/bytes`. `spool_groups`, `last_spool_group_records/bytes/us` описывают успешные append-группы; bytes включают framing, us включают mutex/CRC/writes/sync, это не чистая fsync latency. Число групп не равно точному числу syscalls при rotation. В hot path используется консервативный cached upper bound физического spool size; GC может только уменьшать его, а append увеличивает и обновляет, так что проверка backpressure не берёт spool mutex на каждую исходную транзакцию.

Проверки: существующий spool test и новый wrapped-fsync test прошли, последний также под ASan/UBSan. Проверены отказ sync/dirsync, неизменность published prefix, блокировка читателя до sync, multi-segment groups, SIGKILL перед финальным sync, recovery sync failure. Новый `cdc_spool_group_integration.py` прошёл 33 проверки: RAM-группа не видна applier/checkpoint, SIGKILL до flush и повтор из durable source cursor, byte/timer thresholds, oversized whole transaction, flush на Stop и resume. DDL recovery integration — 28 pass, основной CDC — 72 pass. Test helper `drain` теперь проверяет source head и отсутствие buffered records, а не ошибочно приравнивает received==applied к полному catch-up.

Выполнен **один** workload: normal apply включён, 8 × 10 000 строк, 16 клиентов, requested 400 TPS, 60 s нагрузки; group fsync 1 s/4 MiB и target batch 1 s/4 MiB. Источник выдал **24 123 транзакции / 401.98 TPS**. Во второй половине прогона durable receive = **403.62 records/s**, observed apply = **388.59 records/s**. Approx applied event age колебался от примерно 1.04 до 3.08 s; линейный slope +0.0075 s/s близок к нулю на этом коротком окне, но не является гарантией долгосрочной стабильности. В конце нагрузки applied age=2.51 s: 406 records ещё в RAM, 451 в durable queue. После завершения генератора target догнал источник за **1.36 s**. Итог: received=applied=24 123, buffered/pending=0, **56 durable spool groups**, **46 target batches**, source/target полные ordered hashes совпали. Оба сервера завершились с кодом 0.

Для сравнения, прежний normal apply с per-TX fsync и target batch 1 s показывал receive/apply около 160/159 records/s, applied event age 23.04 s и drain 40.57 s. Новый результат поддерживает решение о group fsync, но это один 60-секундный smoke на 80 тысячах строк, не полный 80M performance gate и не power-loss certification.

Артефакты: `/tmp/cdc-group-fsync.VvxtyM/summary.json`, samples/logs/datadir. Следующий нагрузочный эксперимент автоматически не запускался: требуется подтверждение пользователя.

```bash
python3 -B mysql-test/duckdb/cdc_benchmark.py "$(mktemp -d /tmp/cdc-group.XXXXXX)" \
  --batched-only --batch-timeout-ms 1000 --spool-group-timeout-ms 1000 \
  --spool-group-bytes 4194304 --tables 8 --rows 10000 --threads 16 \
  --rate 400 --seconds 60 --drain-seconds 120
```

### Подтверждённый прогон: 32 клиента / 800 TPS / 600 секунд

По отдельному запросу пользователя выполнен один более длительный single-target прогон. Данные — прежний малый масштаб 8 × 10 000 строк, normal apply и durable group fsync включены; group/apply timeout 1 s, оба byte budget 4 MiB. Requested rate=800 TPS, threads=32, active load=600 s, drain limit=120 s. В runner расширены только явно запрошенные пределы клиентов/rate до 32/800; ограничения 1M total rows, 600 s и watchdog 10 GiB disk / 2 GiB MemAvailable сохранены. Binaries плагина/сервера не пересобирались для этого шага.

Источник выполнил **480 924 транзакции**, actual rate **801.52 TPS**: 0 reads, 1 923 696 writes, 961 848 BEGIN/COMMIT statements. Во второй половине нагрузки (301 samples за последние 300 s) durable receive = **802.80 records/s**, observed apply = **805.49 records/s**. Approx received event age: начало/конец окна 1.01/1.04 s, max 2.07 s, slope −0.000336 s/s. Approx applied event age: начало/конец 3.01/2.04 s, max **3.13 s**, slope **−0.000210 s/s**. Измеримого накопления lag на этом интервале не обнаружено; колебания соответствуют двум независимым batch timers и секундному разрешению source timestamp.

Снимок после остановки генератора: applied age=2.82 s, buffered=846 records, durable pending=958, received=480 078, applied=479 120. Через **1.42 s** target достиг final source coordinates. Итог: received=applied=480 924, buffered/pending=0, **560 durable spool groups**, **390 target batches**, ошибок CDC нет. Полные ordered-row hashes source/target совпали для всех восьми таблиц; оба сервера завершились с кодом 0.

Вывод ограничен этим стендом: 32 клиента и около 800 TPS в течение 10 минут на 80 тысячах строк не вызвали растущего lag. Это не 80M-row gate, не тест максимальной мощности и не гарантия 800 TPS при сканировании больших DuckDB таблиц. Source и target делят CPU/диск одного хоста; новые эксперименты без подтверждения не запускались.

Артефакты: `/tmp/cdc-group-32x800.38dHNW/summary.json`, `samples.jsonl`, logs и datadir.

```bash
python3 -B mysql-test/duckdb/cdc_benchmark.py "$(mktemp -d /tmp/cdc-32x800.XXXXXX)" \
  --batched-only --batch-timeout-ms 1000 --spool-group-timeout-ms 1000 \
  --spool-group-bytes 4194304 --tables 8 --rows 10000 --threads 32 \
  --rate 800 --seconds 600 --drain-seconds 120
```

### Прогон 64 клиента / 1600 TPS: lag стабилен, полная проверка не завершена

По отдельному запросу выполнен один нагрузочный интервал: 64 клиента, requested 1600 TPS, 600 s, 8 × 10 000 строк, прежние group/apply 1 s/4 MiB. Изменены только разрешённые пользователем пределы runner до 64/1600, resource watchdog и row/time caps сохранены; binaries и настройки CDC не менялись.

Нагрузочная часть sysbench завершилась за **600.0202 s**: **961 387 transactions / 1602.26 TPS**, read=0, write=3 845 550, other=1 922 776, ignored errors=2, reconnects=0. После этого строгий assertion runner `write==4*transactions && other==2*transactions` завершил программу ошибкой до drain/hash verification. Поэтому исходный `/tmp/cdc-group-64x1600.mgYf8P/summary.json` оставлен со статусом **fail**, не переписан в pass. Коды двух ignored errors в сохранённом отчёте отсутствуют; считать их доказанными deadlocks нельзя.

Offline analysis тех же 602 load samples (без нового запуска) дал для последних ~300 s:

| Метрика | Receive | Apply |
|---|---|---|
| Observed records/s | 1605.40 | 1598.20 |
| Event age в начале окна | 1.32 s | 1.32 s |
| Event age в конце окна | 0.32 s | 2.32 s |
| Max event age, последние 300 s | 1.43 s | 3.37 s |
| Max event age, все load samples | 1.47 s | 3.42 s |
| Linear slope age | +0.000021 s/s | −0.000210 s/s |

По нагрузочным samples накопления lag не обнаружено; source и target перешли на `source-bin.000002` без CDC ERROR. Чтение headers исходных binlog после bootstrap position подтвердило **961 387 GTID/XID**, 1 922 774 UPDATE events и по 961 387 DELETE/INSERT events: committed workload сохранил ровно 2 UPDATE + DELETE + INSERT, несмотря на два дополнительных учитываемых sysbench failed/ignored attempts. Это проверка source event counts, не доказательство финального совпадения target.

**Выявлен отдельный shutdown defect.** Во время cleanup target завершился SIGSEGV (exit −11), source — exit 0. `batched.err` показывает начало normal shutdown и завершение InnoDB shutdown, затем `plugin_lock` → открытие optimizer statistics tables → local SQL service → `Applier::Impl::Apply/Sql` при checkpoint UPDATE. Стек указывает на продолжающий SQL applier во время демонтажа server subsystems; точная цепочка lifecycle и исправление ещё не проверены. На последнем load sample были received=960 285 и applied=958 103 (spool records включают один rotate boundary). Последний source XID заканчивается на `source-bin.000002:999084143`.

**Итог:** нагрузочную способность без роста lag на малом стенде наблюдали, но весь прогон не считается успешным: catch-up time и final source/target hashes **не проверены**, корректный shutdown не пройден. Автоматического исправления или повторного workload не выполнялось. Перед следующей нагрузкой нужно отдельно подтвердить устранение shutdown lifecycle и корректный учёт ignored attempts в runner.

Артефакты: `/tmp/cdc-group-64x1600.mgYf8P/summary.json` (original fail), `samples.jsonl`, `workload-sysbench.out`, `batched.err`; `/tmp/cdc-group-64x1600.mgYf8P/load-analysis.json` и `analyze_load.py` сохраняют воспроизводимый offline разбор, не подменяя исходный статус.

### Исправление shutdown crash (отдельно подтверждённый шаг)

Причина порядка lifecycle подтверждена по серверному коду: `mysql_real_connect_local` в `sql/sql_prepare.cc` создаёт private THD и учитывает его в local_connection_thread_count, но не как обычного клиента server_threads. `close_connections` в `sql/mysqld.cc` намеренно не ждёт эти local connections: их должны завершить владельцы. Остановка CDC только из поздних deinit позволяла checkpoint SQL обращаться к optimizer statistics/table plugins после начала teardown.

Исправление минимально и только в plugin repo: в существующем `quack_pre_shutdown` (`ha_duckdb_quack.cc`) после неизменённого `quack_stop()` добавлен `myduck::cdc::Shutdown()`. `quack_init` уже устанавливал этот callback в handlerton, поэтому отдельное назначение нового callback могло бы перезаписать Quack hook или быть перезаписано им. `ha_pre_shutdown` в `sql/handler.cc` вызывает callback без LOCK_plugin до `clean_up`/`plugin_shutdown` в `mysqld.cc`; worker может закончить текущий SQL/rollback, закрыть local connection и завершиться до уничтожения этих подсистем. Код ядра не изменялся. Повторные поздние stop остаются идемпотентны; это не обещание мгновенной отмены произвольного зависшего SQL/I/O.

Обычный малый regression с пятью checkpoint-table-lock shutdown/restart циклами проходил даже до исправления и не воспроизводил гонку надёжно. Поэтому добавлен `cdc_shutdown_order_gdb.py`: GDB non-stop приостанавливает только CDC thread на checkpoint UPDATE, остальные потоки продолжают работать. На собственный изолированный target отправляется SIGTERM. До исправления полный InnoDB teardown завершался при ещё приостановленном worker, а его продолжение воспроизвело SIGSEGV в `plugin_lock`. После исправления teardown остаётся заблокирован join-ом CDC; после продолжения worker server выходит с кодом 0, а restart/resume даёт точные строки source и checkpoint. В production code не добавлены fault-injection переменные или тестовые паузы. GDB-тест требует `/usr/bin/gdb`, текущих libstdc++ symbols и RelWithDebInfo/Debug build.

Артефакты red: `/tmp/cdc-plugin-am3kgvxe/shutdown-order-gdb-summary.json`; green: `/tmp/cdc-plugin-hncvk9e6/` и повтор `/tmp/cdc-plugin-63wrjcfj/`. `full_teardown_while_paused`: true → false, inferior exit после исправления — 0. Обычный active shutdown/restart test: 22 checks pass, пять циклов, `/tmp/cdc-plugin-tng1qzi_/active-shutdown-summary.json`. Последние regression runs: основной CDC 72 pass, group spool 33 pass, DDL recovery 28 pass. Сборка CDC=OFF также прошла 17 BINLOG gate checks с gamma maturity; финальная `build/ha_duckdb.so` оставлена с CDC=ON.

Дополнительные Quack MTR assertions (`quack_server`, `quack_server_off`, `quack_bridge`) прошли; internal state checker для `quack_bridge` отдельно сообщил изменение checksum mysql.global_priv. Поэтому чистый полный MTR gate этим запуском не заявляется, и cleanup/grant-сценарий не изменялся в данном шаге. Логи — `build/cdc_shutdown_quack_regression/`.

Повтор 64/1600 и исправление учёта ignored attempts в runner **не выполнялись**: это отдельные шаги после подтверждения. Исходный status=fail и отсутствие финальных hashes в `/tmp/cdc-group-64x1600.mgYf8P/summary.json` остаются неизменными.

### Отложенное разделение lifecycle DuckDB и CDC

По решению пользователя текущий deinit и ранний shutdown пока оставлены без изменений. `duckdb_deinit_func` в `ha_duckdb.cc` вызывает `cdc::Shutdown()`, `quack_stop()` и `DuckdbManager::Cleanup()`; собственный `cdc::Deinit` в `cdc/plugin.cc` также останавливает worker и освобождает plugin reference на DuckDB. Ранний вызов CDC из `quack_pre_shutdown` в `ha_duckdb_quack.cc` сохраняется, чтобы не вернуть воспроизведённую shutdown race.

DuckDB и CDC планируются к раздельной поставке. Прямые вызовы CDC из lifecycle движка/Quack — временная связь, которую предстоит убрать отдельным согласованным шагом. Обсуждён вариант интерфейса регистрации необязательного callback: DuckDB хранит указатель, CDC регистрирует свою остановку при init и снимает регистрацию при deinit. Это предложение, а не реализованный механизм. Регистрацию/снятие необходимо синхронизировать с вызовом, чтобы CDC module не выгружался при выполнении callback; одного atomic-указателя недостаточно.

Альтернатива явной регистрации callback — обнаружение CDC через plugin manager в раннем DuckDB pre-shutdown. Предлагается получить временный plugin reference на `DUCKDB_CDC`, через handle его загруженного модуля найти версионированный экспортированный интерфейс Stop и вызвать кооперативный stop/join, удерживая reference до возврата. Постоянный указатель на CDC callback в DuckDB тогда не нужен; окончательный deinit остаётся ответственностью самого CDC и plugin manager. Это проектный вариант, не реализованный путь. В текущем сервере поиск по имени выполняет `plugin_lock_by_name` в `sql/sql_plugin.cc`; он учитывает состояние плагина, а не гарантирует доступность CDC в любой фазе shutdown. При реализации необходимо проверить допустимые состояния, отсутствие CDC, гонку с UNINSTALL/deinit и порядок ссылок, не создавая постоянной взаимной зависимости DuckDB↔CDC. Неуспешный поиск нельзя считать доказательством, что CDC worker уже остановлен. Plugin manager сам по себе не предоставляет стандартного daemon Stop; интерфейс остановки ещё предстоит определить. Вызов чужого deinit напрямую недопустим: он обходит управление состояниями и очистку plugin manager. Получение module handle использует private SDK; динамический поиск интерфейса и его ABI требуют отдельной проверки при выделении CDC в `.so`.

Вызов такого callback только из позднего DuckDB deinit не заменяет раннюю остановку: к plugin deinit MariaDB уже освобождает часть общей SQL-инфраструктуры. При последующем разделении нужно сохранить ранний stop/join CDC, идемпотентный собственный CDC deinit и корректное время жизни callback/ссылки на engine. Окончательный механизм раннего уведомления и выделение отдельного `.so` в этом шаге не реализуются; патч сервера также не согласован.

### Начальная конфигурация

Параметры подключения и spool задаются при старте сервера. Пароль не публикуется как SQL system variable; хранить его следует в защищённом конфигурационном файле, а не в командной строке. Пример — дополнительный защищённый конфиг, читаемый после штатного пакетного `duckdb.cnf`, который уже загружает DuckDB engine и функции. Он содержит только placeholders и явное согласие оператора на экспериментальный CDC:

```ini
[mariadbd]
plugin-maturity=experimental
plugin-load-add=DUCKDB_CDC=ha_duckdb.so
duckdb-cdc-enabled=OFF
duckdb-cdc-source-host=oltp-primary
duckdb-cdc-source-port=3306
duckdb-cdc-source-user=cdc
duckdb-cdc-server-id=112405
duckdb-cdc-source-password=REPLACE_IN_PROTECTED_CONFIG
duckdb-cdc-source-tls-ca=/path/to/ca.pem
duckdb-cdc-source-file=SOURCE_SNAPSHOT_BINLOG_FILE
duckdb-cdc-source-position=SOURCE_SNAPSHOT_POSITION
duckdb-cdc-database=app
duckdb-cdc-spool-group-timeout-ms=1000
duckdb-cdc-spool-group-bytes=4194304
```

Hostname должен соответствовать source TLS certificate. Source account должен иметь `REPLICATION SLAVE`; bootstrap выполняется отдельно. `duckdb-cdc-server-id` обязателен при включении и должен быть уникален среди подключённых к источнику реплик и CDC readers. Исходные `source-file`/`source-position` остаются привязкой bootstrap: при рестарте продолжение идёт из checkpoint, менять начальные координаты для обычного resume не нужно.

После bootstrap:

```sql
SET GLOBAL duckdb_cdc_enabled=ON;
SHOW GLOBAL STATUS LIKE 'Duckdb_cdc_%';
SELECT * FROM _duckdb_cdc.checkpoints;
```

Для остановки установить `duckdb_cdc_enabled=OFF` и дождаться `Duckdb_cdc_state=STOPPED`. После ERROR переключение OFF/ON повторяет попытку с durable checkpoint; неподтверждённый DDL intent таким способом не обходится. Перед выгрузкой движка сначала остановить и выгрузить `DUCKDB_CDC`.

Цель — получать MariaDB binlog по async replication protocol, надёжно сохранять неприменённые события и применять несколько исходных транзакций одним batch через существующий DeltaAppender на целевой MariaDB с `ENGINE=DuckDB`.

Не изменять семантику `BEGIN`/`COMMIT` штатного replication applier. Серверный патч группового барьера для native replication — отдельный механизм, не архитектурная зависимость CDC.

## 1. Упаковка и границы MVP

Зарегистрировать отдельный `MYSQL_DAEMON_PLUGIN` с рабочим названием `duckdb_cdc` в существующей библиотеке `ha_duckdb.so`. Логику разместить отдельно, например в `cdc/`, не смешивая её с методами handler.

Для первой версии не создавать отдельный executable, отдельный сетевой endpoint или внешние staging-таблицы. Физическое разделение `.so` можно рассмотреть позже при необходимости независимого распространения.

```text
mariadbd
├── DuckDB storage engine
└── duckdb_cdc — daemon plugin
    ├── upstream reader
    ├── durable spool
    └── batch applier
         ↓ local SQL service
       row injection → DeltaAppender
```

Daemon plugin запускает потоки внутри `mariadbd`, а не отдельный ОС-процесс. Ошибка плагина потенциально затрагивает весь сервер.

Ограничения MVP:

- Один upstream, один упорядоченный поток и один active writer на назначении.
- `binlog_format=ROW`, `binlog_row_image=FULL`.
- Пользовательские таблицы назначения — DuckDB, с PK.
- На целевых таблицах нет других писателей и независимых DDL.
- Ограниченный whitelist DDL; неподдерживаемые события блокируют применение без удаления очереди.
- Без XA, автоматического failover источника и автоматического resnapshot.
- Начальная копия данных и соответствующая source position предоставляются отдельной bootstrap-процедурой.
- По умолчанию CDC выключен.

## 2. Первый этап: проверить техническую связку внутри сервера

До реализации полного reader/spool сделать минимальный plugin-прототип.

### 2.1. Upstream reader

Проверить использование MariaDB Connector/C replication API внутри `mariadbd`:

- Получение raw events через доступные `mariadb_rpl_open()` и `mariadb_rpl_fetch()`.
- Reconnect и прерывание блокирующего чтения.
- Отсутствие конфликтов клиентских символов с сервером и SQL service.
- Поддерживаемые версии и форматы исходного binlog.

Ограниченный SQL service не считать заменой полноценному replication API. Совместимость библиотек — отдельный обязательный результат этапа.

### 2.2. Локальное применение

Открыть выделенную локальную SQL-сессию через SQL service и проверить схему:

```text
START TRANSACTION
→ несколько BINLOG-команд с row events
→ обновление checkpoint
→ COMMIT
```

Исходные `BEGIN`, XID и GTID не исполняются как целевые границы транзакций: ими управляет CDC. DDL передаётся отдельно обычным SQL.

Нужно доказать:

- Несколько исходных транзакций остаются одной целевой транзакцией.
- Промежуточные события и `STMT_END` не вызывают целевой commit.
- UPDATE/DELETE попадают в read-free DeltaAppender без поиска строк.
- Несколько BINLOG-команд, повторное использование table-id и жизненный цикл TABLE_MAP-контекста обрабатываются корректно.
- Rollback отменяет и данные, и checkpoint.
- Обычные клиентские SELECT/UPDATE не получают возможность фиктивного read-free поиска.

### 2.3. Минимальная адаптация engine

Если необходимость подтвердится, разрешить read-free путь только для корректного авторизованного BINLOG row-injection context. Не расширять его на произвольные клиентские запросы.

Проверить переходы insert-only/mixed DeltaAppender и отсутствие лишних flush при применении batch. Не вводить новый API до доказательства недостаточности существующего входа.

**Критерий завершения этапа:** интеграционный тест INSERT/UPDATE/DELETE, смены PK и двух таблиц проходит commit и rollback внутри daemon plugin. Если путь не подходит, сначала уточнить минимальное расширение engine, а не строить вокруг непроверенного предположения полный агент.

## 3. Жизненный цикл плагина

`init` проверяет конфигурацию, создаёт состояние и запускает управляемые потоки. Он не должен надолго блокировать старт MariaDB подключением к источнику.

Применение начинается только после готовности DuckDB и служебных таблиц.

При STOP, unload и shutdown:

1. Прекратить формирование новой работы.
2. Прервать upstream read.
3. Закончить либо откатить текущий batch.
4. Закрыть локальную SQL-сессию.
5. Завершить и присоединить потоки.
6. Освободить зависимости.

Не использовать `pthread_cancel()` для транзакционного applier. Не оставлять detached-потоки после выгрузки кода.

Нельзя деинициализировать DuckDB, пока CDC его использует. Нужны явное удержание зависимости или согласованная остановка. Проверить реальный порядок shutdown; размещение двух плагинов в одном `.so` само по себе не гарантирует корректный порядок.

**Проверки:** запуск без доступного upstream, остановка во время сетевого ожидания, остановка во время batch, unload/reload CDC и попытка выгрузки используемого движка.

## 4. Reader и durable spool

Reader подключается к OLTP как async-реплика с отдельными replication credentials и уникальным `server_id`. Проверяет checksum и поддерживаемые типы событий, отслеживает транзакционные границы, GTID, rotate и format description.

Одно append-only хранилище исходных событий содержит:

- Идентификатор источника/потока и source coordinates.
- Необходимый format context.
- События и контроль целостности.
- Восстанавливаемый индекс завершённых транзакций.

Отдельные постоянные файлы декодированных effects не создавать. Batch — диапазон spool из целых исходных транзакций. Значения строк агенту не требуется декодировать, если этап 2 подтвердит передачу raw row events штатному decoder-у MariaDB.

Курсоры:

- `received` — надёжно сохранённый завершённый исходный transaction.
- `applied` — checkpoint, подтверждённый целевым хранилищем.

Порядок курсоров определяется исходным потоком, а не произвольным численным сравнением GTID из разных domains.

При рестарте:

1. Проверить сегменты и распознать незавершённый хвост.
2. Получить checkpoint назначения.
3. Сверить его с имеющимся spool.
4. Продолжить чтение и применение без пропусков.

Удалять сегмент можно только когда всё его содержимое применено и он больше не нужен для восстановления контекста.

При достижении лимита spool приостанавливать чтение. Async CDC не останавливает OLTP автоматически: retention binlog источника должен покрывать простой. Утрата необходимой истории — явная ошибка с необходимостью нового snapshot, не повод молча продолжать.

**Проверки:** обрыв записи, повреждённый хвост, reconnect, заполнение диска, сохранение и восстановление format context, рестарт всего сервера.

## 5. Batch applier

Один writer, два основных ограничения накопления:

- Бюджет объёма.
- Максимальное время накопления.

Граница batch проходит между исходными транзакциями. Одна большая исходная транзакция может передаваться частями внутри одного целевого SQL transaction, но не разбивается на несколько целевых коммитов.

Передача выполняется через выделенную локальную SQL-сессию с отключённым binlog в MVP.

Внутри одной DuckDB-транзакции:

1. Row effects поступают в DeltaAppender в исходном порядке.
2. Выполняется flush.
3. Сохраняется checkpoint.
4. Выполняется commit.

Не создавать вторую обязательную дедупликацию в агенте. Использовать PK и существующую свёртку DeltaAppender. Полезность дополнительной предварительной свёртки горячих ключей оценить отдельно измерением.

Учитывать:

- Суммарный бюджет всех appender-ов транзакции, а не одной таблицы.
- UPDATE может создавать две записи эффектов.
- Внутренний Appender flush не является commit и не задаёт жёсткий общий предел RAM.
- Размер SQL-команд должен учитывать лимит пакета и расширение base64.
- Неподдерживаемые сжатые форматы явно отклоняются до появления проверенного пути их обработки.

**Проверки:** повторные изменения одного PK, DELETE → INSERT, смена PK, несколько таблиц, большие значения, крупная исходная транзакция и корректное направление эффектов в mixed batch.

## 6. Checkpoint и повторная доставка

Одна служебная checkpoint-таблица в DuckDB содержит:

- Идентификатор исходного потока.
- Применённую source position и необходимое GTID-состояние.
- Версию формата и схемы.

Данные и checkpoint фиксируются одной DuckDB-транзакцией. Не хранить авторитетный applied checkpoint отдельно в spool или InnoDB.

После ошибки откатывать весь batch. После потери подтверждения перечитывать checkpoint и перестраивать оставшийся диапазон из spool. Отдельная постоянная история receipts для каждого batch в MVP не требуется.

Crash-тесты:

- До commit.
- Во время commit.
- После commit до получения подтверждения.
- После подтверждения до удаления spool.
- Рестарт CDC и рестарт целевого сервера.

**Критерий:** нет потерянных эффектов и повторного изменения уже применённого префикса при заданных настройках durability.

## 7. DDL coordinator

DDL — отдельный упорядоченный барьер:

```text
завершить предыдущие DML
→ сохранить DDL intent
→ выполнить DDL через MariaDB
→ проверить оба каталога
→ подтвердить DDL checkpoint
→ продолжить DML
```

Начальный whitelist:

- CREATE/DROP TABLE.
- RENAME TABLE.
- TRUNCATE.
- Ограниченные ADD/DROP COLUMN.

Для каждого оператора отдельно определить поддерживаемые параметры, правила повторного запуска и восстановления. Произвольный ALTER, generated columns, partitioning и другие неподдерживаемые конструкции блокируют поток.

Проверять оба слоя метаданных — MariaDB и DuckDB. Не считать DDL и checkpoint атомарными только потому, что DML checkpoint атомарен. При неоднозначном состоянии после сбоя применение остаётся остановленным, следующие события сохраняются в spool.

Не строить полноценный независимый каталог будущих схем: обработка следует за DDL-барьером, затем обновляется текущая схема. Нельзя подменять историческую схему текущим `SHOW CREATE TABLE` источника при наличии отставания.

Локальная SQL-сессия плагина доверенная. Поэтому явно ограничить разрешённые базы и операции; не передавать произвольный upstream QUERY без проверки. Обработку выполняет SQL-слой MariaDB, но политика выбора DuckDB и допустимости DDL остаётся ответственностью CDC.

**Проверки:** данные до/после DDL, рестарт между выполнением и подтверждением, повторная доставка, смена схемы между batches, неподдерживаемый DDL с сохранением backlog.

## 8. Bootstrap и управление

Использовать отдельную проверенную процедуру snapshot → source position. На время bootstrap запретить DDL. Возможность продолжать DML и сохранять хвост в spool зависит от корректной согласованной snapshot-процедуры.

Предоставить собственные plugin system/status variables:

- Enabled и текущее состояние.
- Параметры upstream и spool.
- Received/applied positions.
- Receive/apply lag.
- Неприменённые байты и транзакции.
- Размер batch и время flush/commit.
- Последняя блокирующая ошибка.

В MVP параметры подключения менять только в остановленном состоянии. Credentials не выводить в логи.

CDC не является штатным replication channel. Интеграцию с `SHOW SLAVE STATUS` не планировать.

## 9. Проверка корректности и производительности

Специфичные для plugin проверки:

- MariaDB стартует при недоступном upstream.
- Блокирующее чтение и текущий batch корректно останавливаются.
- DuckDB не выгружается во время использования CDC.
- Unload/reload CDC не оставляет потоков, соединений и открытых транзакций.
- Штатный shutdown и crash recovery сохраняют необходимые данные spool/checkpoint.
- Обрабатываемая ошибка CDC не прерывает остальные SQL-сессии.

Сравнить на одинаковых данных:

1. Native replication с групповым барьером.
2. CDC с одной исходной транзакцией на целевой commit.
3. CDC с batching.

Сначала выполнить небольшие тесты с полной сверкой данных. Затем использовать исходный масштаб MDEV-42299: восемь таблиц по десять миллионов строк, шестнадцать sysbench клиентов, две UPDATE, одна DELETE и одна INSERT на транзакцию.

Провести ступени TPS с пустым backlog перед каждой ступенью. Измерять устойчивое изменение lag во время работающего OLTP, а не только догоняние после остановки. Отдельно фиксировать размер исходных group-commit-групп, фактический source TPS, apply throughput, backlog и длительности стадий.

Цель — проверить достижимость 400 TPS. Это целевой результат измерения, а не обещание производительности до реализации.

## Итоговый объём изменений

- **CDC daemon plugin:** upstream reader, spool, batch applier, checkpoint, DDL coordinator и управление жизненным циклом.
- **DuckDB engine:** минимальная адаптация безопасного row-injection пути при подтверждённой необходимости и регрессионные тесты.
- **Ядро MariaDB:** изменений для CDC-пути заранее не планировать. Сначала доказать достаточность существующих daemon plugin, SQL service и BINLOG-входа.

Первый результат реализации — доказанный атомарный путь «несколько исходных row-транзакций → один DeltaAppender batch → данные + checkpoint» внутри daemon plugin. От него зависит реализация остальных компонентов.
