# MWOW-7 — Docker и lobby lifecycle

Статус: **review, не принят и не интегрирован**. База: `0e3710be36a6a7fa130f633b358d1b520a3cee9c`.
Роль: game-code-sol. Работа выполнена в `feature/MWOW-7-lifecycle-fix`, исходники на утверждённом ext4 worktree.
Никаких запусков Wolf, Docker workloads, игр, prefix, устройств, deployment, merge или push.

## Что меняется

| Риск | Поведение |
|---|---|
| R2, повтор create после timeout | Один gate на экземпляр lobby handlers резервирует canonical host runner-state directory **до** создания lobby/producer. Новый UUID не обходит эту резервацию. Timeout HTTP не освобождает её. |
| R3, конфликт имени | `DockerAPI::create` по умолчанию не пересоздаёт контейнер. Runner явно выбирает безопасный режим. Конфликт не вызывает remove/adoption. Старый административный opt-in `true` сохранён; его повторы ограничены. |
| R4, inspect | `present`, подтверждённый HTTP 404 `missing` и `unknown` различаются. Ошибка транспорта, 5xx, некорректный JSON и незнакомый status не означают EXITED. |
| R5, зависший HTTP | Connect deadline 2 s, каждый обычный transfer 10 s, image pull 300 s; `NOSIGNAL` для многопоточного использования curl. Create делает не более трёх попыток: максимум один pull и одно явно разрешённое recreation. |
| Конкурентный join | Допуск и ownership session резервируются под коротким mutex до device migration. Второй join в single-user lobby не проходит. Повтор уже завершённого join не повторяет side effects. Одна session не присоединяется одновременно к двум lobby. |

Mutex защищает только внутреннее состояние gate. Под ним нет event bus callbacks, promise completion,
Docker I/O, миграции устройств, filesystem или ожидания другого потока. Во время join/leave второй
переход получает отказ либо deferred leave/stop; вызовы event bus выполняются после освобождения mutex.
Это не делает весь input path или Unix HTTP server последовательным.

## Контракт create и совместимость

Маршруты и JSON схемы legacy create/join сохраняются. Старый клиент без нового заголовка получает UUID,
но конкурирующий create в тот же canonical state directory возвращает 409. Ответ первоначального create
по-прежнему означает готовность lobby/producer и допуск runner, **не** подтверждение здоровья приложения.

Клиент может прислать ровно один непустой `Idempotency-Key`, не более 128 bytes. Из profile ID и ключа
получается стабильный UUID v5 ([Boost.UUID](https://www.boost.org/doc/libs/1_87_0/libs/uuid/doc/html/uuid.html)).
Это адресация операции, не authentication или криптографическая гарантия. Gate отдельно сверяет canonical
resource и сериализованный launch fingerprint (runner, profile, имя, icon, PIN, настройки multi-user,
stop-on-empty, видео, аудио и client settings).

* Пока исходный create ещё Starting, retry того же ключа получает 409; он не создаёт второй worker.
* После готовности совпадающий retry возвращает 200 и **тот же lobby ID**.
* Изменённый payload, Stopping, Unknown или завершённый ключ возвращают 409.
* Ожидание HTTP ограничено 20 s; timeout возвращает 504 с lobby ID и указанием, что launch остаётся reserved.
  Он не является подтверждением отмены. Legacy клиент должен проверить `/lobbies`; keyed клиент может повторять тот же ключ.
* Ключ завершённой операции нельзя использовать для нового запуска. Нужен новый ключ после подтверждённого завершения.

Stop до допуска runner блокирует поздний ready callback. Stop, соревнующийся с уже допущенным запуском,
может пересечь начало create/start; резервация остаётся занятой, а `DockerContainerCreated` повторно
публикует stop для уже зарегистрированного runner handler. Stop handlers только выставляют atomic request;
синхронный Docker stop выполняет supervision worker. Неуспешная попытка остаётся состоянием failed,
а запись lobby с прежними ID/PIN сохраняется в `/lobbies` до подтверждения terminal всех actual workers.
Поэтому `/lobbies/stop` можно вызвать ещё раз явно. Producer/deferred completion не публикует второй stop:
он выполняет только membership/finalization bookkeeping. Это не connection epoch и не защита от stale disconnect.

Ресурс освобождается после membership cleanup и завершения всех стартовавших audio/video producers,
а если runner был допущен — ещё и после его подтверждённого завершения. Публичная запись удаляется только
на этой terminal границе, а не после одного лишь membership cleanup. Позднее завершение старого ticket
не освобождает слот нового. Закрытый ticket хранит только ключ/малую метаинформацию; fingerprint/resource
освобождаются. История ограничена 1024 ключами на экземпляр handlers, ID <=128, resource <=4096,
fingerprint <=65536 bytes. При исчерпании — fail-closed отказ; автоматического eviction нет.
`members/session_owners_` не имеют собственного численного cap и очищаются только при завершённом leave.
Если session удалена раньше leave, stale memberships могут удерживаться и накапливаться; они **не** строго
ограничены текущим числом server sessions. Перенос этого ownership/retention в согласованный connection lifecycle
остаётся работой MWOW-9; лимит 1024 launch entries не доказывает bounded всех maps.

## Неопределённый исход и recovery

`create_with_outcome` различает created, rejected, conflict и unknown; legacy optional API остаётся обёрткой.
После create 201 полученный immutable ID сохраняется даже при failed inspect. После потерянного ответа
create нельзя считать последующий 404 имени доказательством отсутствия ещё выполняющегося запроса.
Поэтому unknown/conflict передаётся как конкретный `DockerOutcomeUnknown(identity)` в общий
`RunnerOutcome`. `start_runner` ловит именно этот тип: не удаляет audio sink и возвращает Unknown.
Lobby и Moonlight callers при этом не публикуют ложные StopLobby/StopStream. Lobby gate остаётся занят.
Worker завершается, бесконечного quarantine thread нет. Имя/ID остаются в диагностическом логе.

Для известного ID supervision обрабатывает Running/Paused/Restarting как активный экземпляр,
Removing как ещё занятый. После неуспешного start ранний Exited/Dead не считается барьером:
задержавшийся start мог ещё не вступить в силу. Нужен наблюдённый активный state, затем terminal,
либо 404 самого immutable ID. Неизвестные inspect и Created сверяются до 30 s непрерывной неопределённости,
после чего worker возвращает Unknown с ID. Это не жёсткая общая latency: текущий transfer может занять
ещё до 10 s. Обычная длительность Running supervision не ограничивается.

Unknown не публикует DockerContainerStopped, не запускает destructive cleanup и не открывает новый launch.
После подтверждённого terminal cleanup не вызывает force stop; opt-in `WOLF_STOP_CONTAINER_ON_EXIT=TRUE`
делает только non-force remove без volume deletion. При 404 не запрашиваются бессмысленные logs.

Автоматическое reconciliation/recovery Unknown **не реализовано**. Оператор должен сохранить identity,
проверить реальные container/process состояния и незавершённые daemon операции, и только затем решать
вопрос восстановления. Слепой restart Wolf не является recovery: gate живёт в памяти и после restart
теряет историю, тогда как старый контейнер может оставаться живым. Административный reset route здесь не добавлен.

## Граница гарантий и оставшаяся работа

* Это защита **lobby launch ресурса**, а не полный singleton WoW. Нет устойчивого GameSession ID,
  общего prefix writer lock, обнаружения внешнего Battle.net/Wine и координации всех legacy launch/runner routes.
  Разные runner-state folders могут монтировать один prefix через custom binds; такой alias gate не обнаруживает.
  Несколько setup_lobbies_handlers или процессов Wolf не разделяют эту память. Нужен отдельный этап durable game admission.
* При частично выполненном join/leave исключение сохраняет admission и пишет диагностику. Автоматического
  отката устройств/neutral pad нет: нельзя освобождать ownership, притворяясь, что все side effects отменены.
  Если Moonlight session уже удалена до leave, membership остаётся для reconciliation.
* Connection epochs, stale Pause/Stop/disconnect, consumer completion barrier, neutral Xbox state и один pad slot
  относятся к R1/R6 и не исправлены этим patch set. Существующие input emplace/read races не объявляются решёнными.
* Hotplug unplug всё ещё синхронно вызывает `exec` на event publishing thread. Один exec состоит из нескольких
  transfer по 10 s; это **не** общий input/handoff deadline. Worker обрабатывает один queued device event за итерацию,
  но в event может быть несколько udev операций. Для полной R5 нужен отдельный ограниченный executor/cancellation.
* Producer teardown не имеет нового bounded completion barrier. Видео с поздним ready получает EOS напрямую;
  audio/producer, не завершившийся после stop (включая существующую race регистрации EOS handler), удерживает ресурс.
  Это безопасный отказ в новом launch, но не обещание отсутствия утечек media workers или автоматического recovery.
* Changed default recreation может потребовать явного административного восстановления stale sidecar/test containers.
  Fixtures реального Docker suite обновлены для explicit opt-in; сам этот suite не запускался.

## Доработка после независимого MWOW-8 review

F1 (failed stop при продолжающемся Running) исправляется совместно в runner и lobby handler:
`ContainerStopRequests` отделяет одноразовую notification от сохраняющегося failed/awaiting-terminal intent;
`ContainerSupervision::poll` исполняет одну мутацию на новую notification и сохраняет её результат.
`handle_lobby_stop` оставляет ID/PIN в публичном состоянии до terminal acknowledgement, поэтому повторный
публичный stop может сформировать новую notification. `notify_lobby_completion` не превращает окончание
producer в автоматическую повторную мутацию. Failed stop + Running не создаёт ложный ContainerStopped.

F2 (определённый отказ std::thread constructor) исправляется production seam
`run_lobby_startup` / `ProducerHandoffs`, через который реально идёт create handler. Reservation получает
handoff только после возврата actual worker от factory; при исключении отменяются только ещё не созданные
audio/video workers. Completion уже созданного worker исполняет общий wrapper, а не тестовая копия счётчиков.
Setup completion barrier не позволяет callback освободить ресурс, пока setup ещё конструирует workers.

Lobby теперь владеет final sink cleanup: `RunnerArgs::cleanup_audio_sink=false` для lobby, normal Moonlight
caller сохраняет прежний default true. После закрытия всех actual workers и membership finalizer однократно
забирает sink из lobby и удаляет его, включая audio-thread failure после создания sink и video-thread failure
после audio handoff. Sink живого/Unknown runner не удаляется. Изменение не перекладывает F1/F2 в residuals.

Deterministic fixtures используют те же production seams: отказ factory при audio и video handoff,
реально исполненный completion wrapper handed-off audio, sink cleanup ровно один раз, новый ID после recovery,
старый key как tombstone; и реальный DockerAPI/mock HTTP stop500 → Running → completion без повтора → Running
→ явный retry stop204 → Exited. Проверяются доступность записи, reservation и terminal boundary.
Это callback/launch-policy harness без реального GStreamer/RunDocker environment, не live system test.

## Проверки и план сборки

Выполнены `git diff --check`, targeted clang-format, C++20 syntax checks state-only тестов и production
`core/.../docker.cpp` с настоящими logger/Boost/range headers. ASan/UBSan: standalone gate/monitor scenarios
и mock Docker HTTP scenarios: **10 state-only scenarios + 8 mock HTTP scenarios PASS** на исходном candidate; доработка добавляет
2 deterministic startup-failure fixtures, 1 stop-intent/Unknown fixture и 1 stop/retry HTTP fixture;
финальный suite содержит 13 state-only + 9 mock HTTP scenarios, результаты приведены в raw evidence. SHA фиксируется в итоговом отчёте.
Mock tests используют только временный Unix socket, не Docker daemon. Проверяются 404/500/malformed responses,
неизвестный status, конфликт без recreation, потерянный create response, сохранение ID после 201,
ограниченный pull retry, reuse curl handle после POST и реальный 10 s timeout медленного mock engine.
State-only tests включают конкуренцию 16 create/16 join, late callback после stop, repeat completion,
stop/launch interleaving, cross-lobby session ownership, deferred leave/stop, лимиты истории и всех producers.

Изолированная сборка использует `/run/media/bazzoozza/Games/bb-targets/wolf/MWOW-7`; headers Boost/range
переиспользованы read-only из MWOW-6. В mock **link harness** заменён только logger на fmt-validating no-op:
host Boost.Log 1.92 не соответствует доступным headers 1.87. HTTP, curl, JSON, parser, create/inspect и monitor
исполняются из production кода; Boost.JSON собран из тех же headers. Production Docker syntax check использует
настоящий logger. Полный линк/ABI этим не проверен.

Новые штатные CMake targets: `wolf-lifecycle-tests`, `wolf-docker-lifecycle-tests` (Linux). Они не зависят от
`TEST_DOCKER` и не запускают live suite. После выделения heavy-слота: configure в approved build-root, C++20,
`TEST_DOCKER/NVIDIA/VIRTUAL_INPUT/UHID/RUST_WAYLAND/SDL=OFF`, собрать эти targets и changed server TUs,
проверить static/shared. Root configure тянет остальные Wolf dependencies даже для этих targets;
он не запускался этим исполнителем. Полный wolftests по умолчанию не запускать: он включает runtime/hardware.

Отдельный adapter commit `0794803960db47d0e730f4e3a43da6296a157e1d` содержит только адаптацию API input к
length-bearing MWOW-6 handler и SSE subscribe на MWOW-6 strand. Он требует security patch MWOW-6
(`api.hpp`, input handler и сопутствующие изменения). Lifecycle commit строится поверх adapter, поэтому
полная проверка endpoints требует combined candidate. Этот worktree сам не содержит security-owned файлов.
Доработка проверена последовательным production `-fsyntax-only` для core Docker, runner Docker и
sessions/common.cpp, sessions/lobbies.cpp, sessions/moonlight.cpp с настоящими dependency headers из
подготовленного MWOW-6 compile_commands; это не полный link. Combined endpoints/полный server build,
static/shared link и независимое повторное review/приёмка ещё требуются; задача остаётся review.

Tracker lease/status ведёт Manager `thr_yw564ieyms`, project MWOW, task MWOW-7. Точные usage token counts
неизвестны; report_usage с выдуманными числами не отправлялся.

Сырые свидетельства доработки сохраняются в
`/home/bazzoozza/.bb/thread-storage/thr_7rjh7u5mnu/MWOW-7-review-fixes/`:
`verify.py`, `*/manifest.json`, `*.command.json`, отдельные raw `*.stdout` / `*.stderr`.
Manifest фиксирует точные argv/cwd, revision/status, SHA256 исходников и бинарников, exit codes и duration;
кандидат с dirty source hashes и финальный clean commit различаются label. Список непроверенных/непрошедших
production TUs определяется этими логами, а не успешностью state-only suite.
