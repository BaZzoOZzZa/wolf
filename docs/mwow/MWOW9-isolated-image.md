# MWOW-9 — source review / later provisioning checklist

**Только исходники, не разрешение сборки/запуска.** Родитель владеет MWOW/9,
lease и provisioning. Эта ветка основана на native `0e3710b`: её native-код
не собирать/развёртывать. Сверены read-only принятые native
`e1fdfd2e12b79bfd30e6a776478ead29ce958e67` и Rust
`a8d3a8033e1b0b51efeee9b9c2b1ff43a2d1b1c1`.

1. **Образ.** `tools/wolf-isolated/image/Dockerfile` закрепляет linux/amd64
   child digest `sha256:fcee73ea1b96e236248f53a213f71276cc9072d9025b58bce9915e32f19963ae`.
   [Официальный registry](https://hub.docker.com/_/archlinux),
   [Docker official-images](https://github.com/docker-library/official-images/blob/master/library/archlinux):
   base-20261108.0.623074, upstream Git `457808cdaf8ca50c14692f1099c2efd63049bd00`.
   OCI bytes сохранены в `image/evidence`; слои/config blob не скачивались.
   [Arch snapshot 2026/11/12](https://archive.archlinux.org/repos/2026/11/12/)
   закреплён хешами трёх repository DB и версиями в `packages.lock.json`.
   Gamescope **3.17.0-1**, Xwayland **24.1.15-1**, glibc **2.44+r50+g1848099f063e-1**.
   Сначала проверяются DB, затем полный upgrade/установка с package signatures.
   Системный Gamescope 3.16.31 не копируется. Это metadata evidence, не доказательство ABI.

2. **Build input — отдельное разрешение.** После разрешённого инвентаря точного
   Lutris runner создать проверенный `approved-wine.tar` с `bin/`, `lib/` и
   остальным содержимым runner в корне архива; без prefix, аккаунтов, устройств,
   абсолютных/traversal entries, внешних symlink/hardlink. Записать SHA256 архива,
   всех файлов/ссылок и ELF dependency closure. Имя `11.19-pinned` не provenance.
   Другой Wine не устанавливать. Контекст готовить только в
   `/run/media/bazzoozza/Games/bb-targets/wolf/isolated-image-source/context`.
   После отдельного допуска daemon/storage/build:

   ```bash
   : "${APPROVED_WINE_SHA256:?reviewed archive hash required}"
   docker build --platform linux/amd64 --build-arg "WINE_ARCHIVE_SHA256=$APPROVED_WINE_SHA256" \
     --tag mwow9-candidate /run/media/bazzoozza/Games/bb-targets/wolf/isolated-image-source/context
   ```

   В контекст включить Dockerfile, scripts, package-db.sha256, packages.requested,
   .dockerignore и только одобренный архив. Зафиксировать installed-packages,
   окончательный image ID/digest, ELF closure и hashes всех трёх runtime scripts.
   Entrypoint artifact hash относится к `approved-entrypoint`; остальные scripts
   покрываются неизменным image digest. Контракты root создаются позднее,
   этот source их не удостоверяет.

3. **Runtime review.** Единственный creator — принятый RunDocker. `USER 2001`,
   entrypoint `/opt/wolf/approved-entrypoint`, Python как PID 1, без CMD/передачи
   произвольных команд. Проверяются реальные UID/GID, E/P/I/A=0, NNP=1;
   bounding bits не обязаны быть нулевыми. Rust требует CapDrop ALL **с CapAdd
   MKNOD/NET_RAW/NET_ADMIN** для отдельного root fake-udev exec: не заменять
   пустым CapAdd. Минимальность/kernel-совместимость helper ещё не доказана.
   Mounts: prefix RW `/home/retro/prefix`, game RO `/games/wow`, точные native
   auto-mounts. Только динамическое WAYLAND_DISPLAY от native, private Pulse,
   валидированные размеры и PULSE_SINK. Host DISPLAY/home/runtime/input-directory/
   Docker socket отсутствуют. Gamescope использует private Wayland → nested Xwayland;
   Wine запускает фиксированный Battle.net Launcher.exe. Alias прежнего
   `Z:\run\media\bazzoozza\Games\World of Warcraft` ведёт в `/games/wow`.
   Prefix registry не редактируется; сохранение входа не гарантируется.

4. **Lifetime.** Гостевой `.mwow9-wine-session.lock` внутри рабочей копии отличается
   от outer `launcher/prefix-<dev>-<ino>.lock`. PID 1 держит flock до исчезновения
   всех потомков, включая orphan/double-fork; session ждёт `wineserver -w` того же
   prefix. Сигналы направляются только собственному private PID tree. Завершение
   launcher не считается завершением игры. R41-1: до успешного Popen ошибка
   setup освобождает lease; после Popen wait/handler exception записывает sticky
   Unknown, сохраняет настоящий PID1/fd и повторяет drain с backoff 0.1s.
   Обработчик сигналов не выбрасывает исключения. Освобождение разрешает только
   нормальный возврат drain после kernel ECHILD; неизвестность даёт ненулевой
   итог даже после позднего drain. Нет retry-budget/forced-exit/detach.
   Тестовый watchdog ограничивает только synthetic fixture, timeout всегда failure.
   Нужна поздняя реальная проверка
   Gamescope/Wine/signals; тесты используют только синтетические факты/процессы.

5. **Блокирующие gates.** RO game не обеспечивает WTF/Logs/Cache/settings/updates;
   playable-ready не заявлен. Минимальное последующее изменение: root-pinned
   GameContract с различными source/working dev+ino, проверенной полной private
   копией без hardlinks, UID/правами и migration receipt; затем изменить **только
   этот** game bind на RW одновременно в runtime_spec и inspection. Число mounts
   сохранить; generic RW/mount flags не добавлять. Предложение: game clone и backup
   prefix на Work, одна working-prefix на Games, после clean-stop/копирования
   с отдельным разрешением и проверки места. Нынешние оригиналы не тронуты.
   Дополнительно обязательны native compositor/interpipe/GStreamer ABI,
   private Pulse path/auth, Wine ELF closure, GPU/device ACL и crossover,
   migration/auth и root-helper proofs. Сборка образа ничего из этого не заменяет.
# Existing Lutris DLL runtime overlay

The prefix's DXVK/VKD3D/compiler DLL links can point at versioned software paths
under `/home/bazzoozza/.local/share/lutris/runtime`. The Wine runner archive does
not supply these separate Lutris runtimes. The recipe in
`tools/wolf-isolated/image/lutris-runtime/` adds only `dxvk/v2.6.2`,
`vkd3d/v2.14`, and `d3d_extras/v2` at their original absolute guest paths.
It does not modify a prefix, copy game data, mount HOME, or execute Wine.

`bundle.py --context <fresh-approved-Games-context>` inventories every source
file before copying, rejects escaping symlinks and external hardlinks, and emits
a deterministic root-owned archive plus the exact source/content manifest.
Source files remain untouched. Image directories are normalized to 0555 and
regular files to 0444 (0555 for source executables). Internal symlink aliases
remain links; their targets must stay inside the same versioned software tree.
`verify.py` runs inside the build as inherited UID2001 and checks the complete
path set, hashes, ownership, modes, traversal, and readability. It has no Wine,
game, prefix, device, compositor, or service operations.

The parent is the existing **local** image ID
`sha256:84719e0af05e1ee4720e1d6605aed0bf9c3d55d7ebf24e159aee355580632505`.
The Dockerfile uses its existing local tag `mwow9-candidate:latest`, not an
invented registry manifest pin. A build owner must verify that tag's exact ID
before and after the build, use the default Docker driver with `--pull=false`
and `--network=none`, and require the derived image's initial layers to equal
the parent's complete layer list. Keep the old tag/image; tag the derived image
separately and deploy only its resulting immutable ID. Record source revision,
parent ID/source/context, software manifest/archive hashes, and derived context
hash as image labels and in the build packet. No downloads or global installs.

Deployment is a separate Root operation: update the configured guest image,
profile image, and image-contract image/image_id; recompute the config, profile,
and image-contract hashes in runtime.json. Rebind prefix-contract's profile hash
to the actual new profile without inventing a new clean-stop/migration receipt;
recompute runtime.json's prefix-contract hash. Preserve prefix identities,
capabilities, Wine/entrypoint hashes, touch/input, and silent audio provenance.
Archive/retire the exact old guest and epoch only after positive exit evidence;
do not bypass the one-CID-per-epoch guard or rename arbitrary containers.
The image's content checks do not prove a successful D3D11/BNet/WoW launch.
