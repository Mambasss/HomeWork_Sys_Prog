# runtime — минимальная утилита контейнеризации с PTY

---

## Сборка

```bash
gcc -O2 -Wall -Wextra -o runtime main.c
```

Требования: Linux с `devpts`, `gcc` (или `clang`), `make` (опционально).
Проект полностью **rootless** — `sudo` не нужен.

Проверено на: Ubuntu 20.04, x86_64.

---

**Ключевые системные вызовы:**

| Назначение | Вызов |
|---|---|
| Создание PTY | `posix_openpt` / `grantpt` / `unlockpt` / `ptsname` / `open` |
| Изоляция PID | `clone(CLONE_NEWPID \| ...)` |
| Изоляция mount | `clone(CLONE_NEWNS)` + `mount(MS_PRIVATE)` |
| Изоляция user | `clone(CLONE_NEWUSER)` + `write uid_map/gid_map` |
| Изоляция UTS | `clone(CLONE_NEWUTS)` + `sethostname` |
| Изоляция IPC | `clone(CLONE_NEWIPC)` |
| Изоляция network | `clone(CLONE_NEWNET)` |
| Смена корня | `chroot` |
| Управляющий терминал | `setsid` + `ioctl(TIOCSCTTY)` |
| Перенаправление stdio | `dup2` |
| Raw-режим | `tcgetattr` + `cfmakeraw` + `tcsetattr` |
| Relay | `poll` + `read`/`write` |
| Восстановление терминала | `atexit` + `tcsetattr(ORIG)` + обработчики сигналов |

---

## Использование

### Домашнее задание №1 — изолированный запуск (без PTY)

```bash
./runtime run --rootfs /tmp/alpine /bin/ls
./runtime run --rootfs /tmp/alpine /bin/ps
```

Проверяет: `CLONE_NEW*`, `chroot`, `mount /proc`, `mount /dev`, PID 1 внутри
контейнера. stdio наследуется от хоста (без PTY).

### Домашнее задание №2 — интерактивный режим (с PTY)

```bash
./runtime run --rootfs /tmp/alpine -it /bin/sh
```

Флаг `-it` включает:
- создание пары PTY **до** `clone()`;
- `setsid` + `ioctl(TIOCSCTTY)` + `dup2(slave, 0/1/2)` в ребёнке перед `exec`;
- `cfmakeraw` на терминале хоста;
- асинхронный `poll`-relay между терминалом хоста и PTY master;
- гарантированное восстановление терминала через `atexit` и обработчики
  `SIGINT`/`SIGTERM`/`SIGHUP`.

---

## Результаты проверки

### Домашнее задание №1 — изоляция

```
$ ./runtime run --rootfs /tmp/alpine /bin/ls
bin    dev    etc    home   lib    media  mnt    opt    proc   root
run    sbin   sys    srv    tmp    usr    var
```
→ вывод содержит содержимое rootfs, а не хоста. `chroot` + `CLONE_NEWNS` работают.

```
$ ./runtime run --rootfs /tmp/alpine /bin/ps
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/ps
```
→ один процесс, PID 1. `CLONE_NEWPID` работает.

### Домашнее задание №2 — интерактивный режим

```
$ ./runtime run --rootfs /tmp/alpine -it /bin/sh
Note: could not change / propagation
/ # hostname
container
/ # id
uid=0(root) gid=0(root) groups=65534(nobody),...,0(root)
/ # ps
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/sh
    4 root      0:00 ps
/ # /bin/<Tab><Tab>       # список applets busybox — Tab работает
arch  ash  base64  ...  zcat
/ # /bin/^C               # Ctrl+C: SIGINT обработан внутри, runtime жив
/ #
/ # vi /etc/hostname      # полноэкранный vi: открывается, стрелки, :q! — работает
/ #                       # Ctrl+D -> выход в training@xilinx$
$ stty -a | grep -Eo '[-]?(icanon|echo|isig|opost)'
opost
isig
icanon
echo
```
→ после выхода терминал хоста восстановлен (все ключевые флаги положительные).

### Сводка проверок

| Проверка | Результат |
|---|---|
| `hostname` → `container` | `CLONE_NEWUTS` + `sethostname` |
| `id` → `uid=0(root) gid=0(root)` | `CLONE_NEWUSER` + `uid_map`/`gid_map` |
| `ps` → PID 1 `/bin/sh` | `CLONE_NEWPID` |
| Tab-автодополнение | `\t` проходит relay, shell обрабатывает |
| Ctrl+C | `SIGINT` внутри контейнера, `runtime` не умер |
| Ctrl+D | выход из `sh`, возврат в хост |
| `vi /etc/hostname` | полноэкранный TUI, стрелки, `:q!` |
| `stty` после выхода | `icanon`/`isig`/`opost`/`echo` восстановлены |

---

## Tеоретический вопрос

> **Объясните роль управляющих байт (escape-последовательностей) и почему канонический
> режим ввода хоста ломает работу интерактивных консольных приложений
> внутри контейнера.**

**Escape-последовательности** - это «мини-язык» управления терминалом,
начинающийся с `ESC` (0x1B): перемещение курсора, очистка экрана, смена цвета,
альтернативный экранный буфер. Полноэкранные приложения (`vim`, `htop`, `less`,
readline) генерируют их в выводе и ожидают их во вводе (стрелки, `F1`-`F12`,
`Home`, `End` - тоже escape-последовательности).

**Канонический режим ломает интерактив по трём причинам:**

1. **Буферизация по строкам.** Драйвер терминала (`n_tty`) накапливает ввод до
   `\n`. Нажатие `Esc` или `Ctrl+C` в `vim` не доходит до приложения, пока
   пользователь не нажмёт Enter.
2. **Искажение управляющих байт.** Драйвер сам обрабатывает `Ctrl+C` (0x03) как
   `SIGINT`, `Ctrl+D` (0x04) как EOF, `Backspace` как `ERASE` — байты не доходят
   до приложения в исходном виде.
3. **Двойное эхо.** Драйвер эхо-печатает символы, а приложение (например, `vim`)
   рисует их ещё раз - «каша» на экране.

**Raw-режим** (`cfmakeraw`) отключает `ICANON`, `ECHO`, `ISIG`, `IEXTEN`,
`ICRNL`, `OPOST`. Каждый байт от клавиатуры уходит в relay и далее в PTY
master **немедленно и без изменений**. Драйвер PTY на slave-конце (настроенный
самим приложением) уже сам решает, как трактовать `Ctrl+C` - `vim` получит
байт `0x03` и обработает его как команду, а не как сигнал. Эхо-печать полностью
ложится на приложение, что и нужно для TUI.

Именно поэтому родитель переводит **хост** в raw-режим, а **slave** оставляет
каноническим: сигналы (`SIGINT` от `Ctrl+C`, `SIGTSTP` от `Ctrl+Z`) генерирует
драйвер PTY **внутри** контейнера, а не хоста.
