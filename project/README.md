# Minidocker - простая система контейнеризации на C

Лёгкий аналог Docker, написанный с нуля на C.
Программа умеет запускать процессы в изолированном окружении,
управлять ими через демон и подключаться к их терминалу.

Проект сделан в рамках домашнего задания по системному
программированию. Все обязательные требования выполнены. Три
дополнительных задания (OverlayFS, изоляция сети с loopback,
подключение к сети другого контейнера) тоже реализованы.

## Содержание

- [Что умеет программа](#что-умеет-программа)
- [Требования к системе](#требования-к-системе)
- [Сборка](#сборка)
- [Скачивание тестовой файловой системы](#скачивание-тестовой-файловой-системы)
- [Запуск](#запуск)
- [Команды и флаги](#команды-и-флаги)
- [Примеры](#примеры)
- [Как это устроено](#как-это-устроено)
- [Интересные решения](#интересные-решения)
- [Ограничения](#ограничения)
- [Результаты тестирования](#результаты-тестирования)
- [Структура проекта](#структура-проекта)
- [Что можно было бы улучшить](#что-можно-было-бы-улучшить)
- [Итог](#итог)

## Что умеет программа

**Обязательные требования (все выполнены):**

- **Изолировать процессы** - запускать программу так, чтобы она
  не видела другие процессы на хосте, имела свой hostname, свою
  файловую систему, свою сеть и свой IPC.
- **Подменять корневую файловую систему** - контейнер видит только
  то, что лежит в переданной папке `rootfs`, а не весь хост.
- **Монтировать `/proc` и `/dev`** - стандартные вещи, нужные
  обычным программам.
- **Интерактивный режим** - запустить shell внутри контейнера и
  работать с ним как с обычным терминалом (Ctrl+C, Ctrl+D и т.д.).
- **Запуск в фоне** - контейнер работает, а терминал свободен.
- **Управляющий демон** - отдельная программа `minidockerd`, которая
  держит состояние всех контейнеров и общается с клиентом по
  Unix-сокету.
- **Логи** - каждый контейнер пишет свой вывод в файл, можно смотреть
  его целиком или «в реальном времени» (`--follow`).
- **Список контейнеров** - какие запущены, какие завершились, с
  каким кодом.
- **Остановка** - корректная: сначала вежливо (SIGTERM), потом
  жёстко (SIGKILL), если не помогло.
- **Подключение к работающему контейнеру** (`attach`) - сесть за
  его терминал, как будто ты изначально за ним работал.

**Дополнительные задания (все выполнены):**

- **Loopback в контейнере** - `127.0.0.1` работает, ping проходит.
- **OverlayFS** - слоистая файловая система: rootfs остаётся
  неизменной, а все изменения контейнера складываются в отдельный
  «верхний» слой.
- **Подключение к сети другого контейнера** - можно запустить
  контейнер «внутри сети» уже работающего, чтобы они видели друг
  друга по `127.0.0.1`.
- **Работа без root** (rootless) - всё запускается от обычного
  пользователя.

## Требования к системе

- Linux с ядром **≥ 3.10** (рекомендую ≥ 5.11 для rootless overlay)
- `gcc`, `make`, `curl`, `tar`
- Для rootless-режима - включённые user namespaces
  (на Ubuntu 20.04+ по умолчанию включены)

Проверял на **Ubuntu 20.04.6 LTS + HWE-ядро 5.15**.

## Сборка

```bash
git clone https://github.com/Mambasss/HomeWork_Sys_Prog.git
cd minidocker
chmod +x scripts/*.sh tests/*.sh
make
```

После сборки появятся два бинарника в `bin/`:

- `bin/minidockerd` - демон
- `bin/minidocker` - клиент

## Скачивание тестовой файловой системы

В контейнере должна быть своя файловая система. Для тестов удобно
использовать минимальный Alpine Linux (~3 МБ):

```bash
scripts/download-alpine-rootfs.sh /tmp/minidocker/rootfs
```

Скрипт скачивает официальный архив с сайта Alpine и распаковывает
его в указанную папку.

## Запуск

### Обычный режим (без root)

```bash
./bin/minidockerd > /tmp/minidockerd.log 2>&1 &
```

В логе должно быть:

```
minidockerd listening on /tmp/minidocker.sock (uid=1000)
```

`uid=1000` означает, что демон работает от обычного пользователя.
Это и есть rootless-режим.

### Режим с root (нужен для подключения к сети другого контейнера)

```bash
sudo ./bin/minidockerd > /tmp/minidockerd-root.log 2>&1 &
```

В логе:

```
minidockerd listening on /tmp/minidocker.sock (uid=0)
```

### Остановка демона

```bash
pkill -f bin/minidockerd           # для rootless
sudo pkill -f bin/minidockerd      # для root
```

## Команды и флаги

```
minidocker run [флаги] <команда> [аргументы...]
minidocker list
minidocker logs <id> [--follow]
minidocker kill <id>
minidocker attach <id>
```

### Флаги для `run`

| Флаг | Что делает |
|---|---|
| `-d`, `--detach` | Запустить в фоне. Клиент сразу вернёт управление. |
| `-t`, `--tty` | Выделить контейнеру псевдотерминал. Нужен, если хотим интерактивный shell. |
| `--name ИМЯ` | Задать контейнеру имя. По умолчанию - `c-XXXXXXXX` (случайный). |
| `--rootfs ПУТЬ` | Путь к корневой файловой системе. По умолчанию `/tmp/minidocker/rootfs`. |
| `--overlay` | Включить OverlayFS (слоистую ФС). |
| `--network container:ID` | Запустить контейнер в сети другого контейнера (по его id). |

## Примеры

### Простой запуск - вывести hello

```bash
./bin/minidocker run --rootfs /tmp/minidocker/rootfs /bin/echo hello
# hello
```

### Проверка кода возврата

```bash
./bin/minidocker run --rootfs /tmp/minidocker/rootfs /bin/sh -c 'exit 42'
echo $?
# 42
```

### Проверка, что PID внутри = 1

```bash
./bin/minidocker run --rootfs /tmp/minidocker/rootfs /bin/sh -c 'echo $$'
# 1
```

### Проверка hostname (UTS-namespace)

```bash
hostname
# xilinx   - это хост

./bin/minidocker run --rootfs /tmp/minidocker/rootfs /bin/hostname
# minidocker   - это уже контейнер
```

### Фоновый контейнер + список

```bash
id=$(./bin/minidocker run -d --rootfs /tmp/minidocker/rootfs /bin/sleep 60)
echo "id=$id"
# id=c-6af2c51b

./bin/minidocker list
# ID          PID     STATUS    COMMAND
# c-6af2c51b  14385   running   /bin/sleep 60
```

### Логи

```bash
id=$(./bin/minidocker run -d --rootfs /tmp/minidocker/rootfs \
       /bin/sh -c 'echo line1; echo line2; sleep 3')
sleep 0.5
./bin/minidocker logs "$id"
# line1
# line2
```

### Логи в реальном времени

```bash
id=$(./bin/minidocker run -d --rootfs /tmp/minidocker/rootfs \
       /bin/sh -c 'for i in 1 2 3; do echo tick $i; sleep 1; done')
./bin/minidocker logs "$id" --follow
# tick 1
# tick 2
# tick 3
```

### Остановка

```bash
./bin/minidocker kill "$id"
echo $?
# 0
./bin/minidocker list | grep "$id"
# c-6af2c51b  14385  exited  /bin/sleep 60
```

### Интерактивный shell

```bash
id=$(./bin/minidocker run -d -t --rootfs /tmp/minidocker/rootfs /bin/sh)
sleep 0.5
./bin/minidocker attach "$id"
```

Внутри:

```
/ # echo attached
attached
/ # exit
```

После `exit` клиент `attach` сам вернётся в обычный терминал.

### OverlayFS

```bash
./bin/minidocker run --overlay --rootfs /tmp/minidocker/rootfs \
    /bin/sh -c 'echo marker > /root/probe.txt && cat /root/probe.txt'
# marker
```

Проверяем, что rootfs не тронута:

```bash
ls /tmp/minidocker/rootfs/root/probe.txt
# No such file or directory

ls /tmp/minidocker/overlay/*/upper/root/probe.txt
# /tmp/minidocker/overlay/c-XXXXXXXX/upper/root/probe.txt
```

### Loopback в контейнере

```bash
./bin/minidocker run --rootfs /tmp/minidocker/rootfs /bin/ping -c1 127.0.0.1
# 1 packets transmitted, 1 packets received, 0% packet loss
```

### Подключение к сети другого контейнера

**Важно: для этого шага нужен демон, запущенный через `sudo`.**

```bash
sudo ./bin/minidockerd &

id1=$(sudo ./bin/minidocker run -d --rootfs /tmp/minidocker/rootfs \
       /bin/sh -c 'while true; do echo HELLO_FROM_PARENT | nc -l -p 8080; done')

sudo ./bin/minidocker run --network "container:$id1" \
    --rootfs /tmp/minidocker/rootfs \
    /bin/sh -c 'echo PING | nc -w 2 127.0.0.1 8080'
# HELLO_FROM_PARENT
```

## Как это устроено

### Два бинарника

- **`minidockerd`** - демон. Фоновая программа, которая держит
  состояние всех контейнеров. Создаёт контейнеры, слушает Unix-сокет
  `/tmp/minidocker.sock`, отвечает на команды клиента.
- **`minidocker`** - клиент. Отправляет команды демону по сокету и
  показывает результат. Сам ничего не изолирует - вся работа с ядром
  Linux происходит в демоне.

### Протокол между клиентом и демоном

Клиент пишет в сокет:

```
[длина сообщения][данные]
```

Данные - набор строк, разделённых нулевыми байтами. Первая строка -
команда (`RUN`, `LIST`, `KILL`, `ATTACH`, `LOGS`). Остальные - параметры.

Демон отвечает так же, но начинка начинается с `OK` или `ERR`.

Для `RUN` в интерактивном режиме и для `ATTACH` после ответа `OK`
демон **передаёт клиенту файловый дескриптор** псевдотерминала
контейнера. Дальше клиент общается с терминалом напрямую.

### Как создаются контейнеры

Системный вызов `clone()` с шестью флагами:

```c
CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER
```

Каждый флаг - «новая копия» подсистемы ядра: свой список процессов,
свои точки монтирования, свой hostname, свой IPC, своя сеть, своя
таблица пользователей. Дочерний процесс попадает в новый «мир» и
ничего не знает про хостовый.

### После pivot_root - монтирую /proc и /dev

```c
mount("proc", "/proc", "proc", 0, "subset=pid");
```

Опция `subset=pid` важна: она говорит ядру «покажи только информацию
о процессах». Без неё в rootless ядро отказывает с EPERM.

`/dev` делаю через bind-mount нужных устройств (`null`, `zero`,
`random`, `urandom`, `tty`).

### Сеть: изоляция + loopback

`CLONE_NEWNET` создаёт новый сетевой namespace. В нём сразу есть
`lo`, но он **выключен**. Поднимаю через `ioctl(SIOCSIFFLAGS)` с
флагом `IFF_UP`. После этого `127.0.0.1` работает, ping проходит.

### Псевдотерминал (PTY)

При `-t` контейнер получает настоящий терминал через
`posix_openpt()` + `grantpt()` + `unlockpt()`. Slave-часть отдаю
дочернему процессу как stdin/stdout/stderr, master-часть держит
демон. Это нужно, чтобы работали интерактивные программы и
корректно обрабатывались Ctrl+C, Ctrl+D, backspace.

### OverlayFS

```c
mount("overlay", merged, "overlay", 0,
      "lowerdir=rootfs,upperdir=upper,workdir=work");
```

`merged` - что видит контейнер. Читает из `lowerdir`, пишет в
`upperdir`. `lowerdir` (rootfs) **никогда не меняется**.

### Связь сетей двух контейнеров

С `--network container:<id>` демон открывает
`/proc/<pid_родителя>/ns/net` и в дочернем процессе вызывает:

```c
setns(fd, CLONE_NEWNET);
```

Новый контейнер «переезжает» в сеть родителя. У них общий
`127.0.0.1`.

## Интересные решения

### Как работает attach

1. Клиент просит демон «подключить меня к контейнеру X».
2. Демон создаёт пару соединённых сокетов (`socketpair`).
3. Один конец оставляет себе, другой **передаёт клиенту через
   `SCM_RIGHTS`** - специальный механизм Unix-сокетов для передачи
   файловых дескрипторов между процессами.
4. Клиент работает с этим сокетом напрямую.

Демон при этом остаётся единственным читателем PTY-мастера и
раскладывает вывод сразу в три места: файл логов, attach-клиентам,
follow-клиентам (`logs --follow`). Даже если подключились два
человека и один смотрит логи - никто никому не мешает.

### Почему attach закрывается сам

Когда контейнер завершается, демон вызывает `container_close_fds()`
- закрывает PTY-мастер и все attach-сокеты. Клиент на той стороне
получает EOF и корректно выходит. Поэтому после `exit` в контейнере
клиент `attach` не висит. Раньше был баг - клиент висел, потому что
сокеты не закрывались. Я его починил.

### Как работает kill

1. Отправляю `SIGTERM` - «пожалуйста, завершись».
2. Жду до 3 секунд, проверяя каждые 100 мс.
3. Если не завершился - `SIGKILL`.

Код возврата сохраняется: 143 = 128 + 15 (SIGTERM).

## Ограничения

### Состояние в памяти

Демон хранит список контейнеров в памяти. Перезапуск `minidockerd`
теряет список. Сами процессы остаются жить, но управлять ими через
`minidocker` уже не получится. Для учебного проекта это нормально.

### Размер терминала не обновляется

Если во время `attach` поменять размер окна, контейнер об этом не
узнает.

### Cross-container network требует root

В rootless у каждого контейнера свой user namespace, а `setns()` в
чужой netns требует прав в том user namespace. У контейнеров они
разные - `setns` возвращает `EPERM`. Когда демон с `sudo`,
контейнеры создаются без `CLONE_NEWUSER`, все в root userns, и
`setns` работает. Так же устроены podman и buildah.

### OverlayFS в rootless и ext4

Для OverlayFS важно, чтобы `lowerdir`, `upperdir` и `workdir` были
на одной ФС, и это не tmpfs. На моей машине `/tmp` на ext4 - всё
заработало. На tmpfs - `Operation not permitted`.

## Результаты тестирования

```bash
ROOTFS=/tmp/minidocker/rootfs tests/test.sh
```

| Тест | Что проверяем |
|---|---|
| echo output | Контейнер запускается и возвращает вывод |
| exit code | Код возврата передаётся клиенту (`exit 42` → 42) |
| PID 1 inside | Внутри контейнера процесс думает, что он первый |
| UTS isolation | Hostname внутри - `minidocker`, снаружи - `xilinx` |
| detached + list | Фоновый контейнер виден в `list` как `running` |
| logs | Вывод контейнера попадает в лог-файл |
| kill | Остановка переводит контейнер в `exited` |
| attach | Можно подключиться к шеллу и выйти через `exit` |
| overlayfs | Запись попадает в верхний слой, rootfs не тронута |
| loopback | Интерфейс `lo` внутри поднят (`UP`) |

Результат:

```
=== 10 passed, 0 failed ===
```

Cross-container network проверял отдельно (с `sudo`):

```bash
sudo ./bin/minidocker run --network "container:$id1" ... nc 127.0.0.1 8080
# HELLO_FROM_PARENT
```

Два независимых контейнера общаются через `127.0.0.1:8080` - сеть
работает.

## Структура проекта

```
minidocker/
├── Makefile
├── README.md
├── include/
│   ├── common.h        - общие include и константы
│   └── container.h     - структура для запуска контейнера
├── src/
│   ├── container.c     - clone, namespaces, pivot_root, PTY
│   ├── daemon.c        - демон, Unix-сокет, обработка команд
│   ├── client.c        - CLI, attach, интерактивный ввод-вывод
│   └── util.c          - вспомогательные read_full/write_full
├── scripts/
│   └── download-alpine-rootfs.sh    - скачивание Alpine rootfs
└── tests/
    └── test.sh         - автотесты
```

## Итог

**Обязательные требования - все выполнены:**

- ✅ Изоляция во всех 6 пространствах имён
- ✅ `pivot_root` в переданную rootfs
- ✅ Монтирование `/proc` и `/dev`
- ✅ Интерактивный режим через PTY
- ✅ Фоновый запуск
- ✅ Управляющий демон на Unix-сокете
- ✅ Логи с возможностью просмотра и follow
- ✅ Список контейнеров
- ✅ Остановка по id
- ✅ Подключение к работающему контейнеру

**Дополнительные задания - все выполнены:**

- ✅ OverlayFS (слоистая файловая система)
- ✅ Изоляция сети с поднятым loopback
- ✅ Участие в сети другого контейнера

Проект собирается и работает как с root, так и без root
(rootless), тесты проходят (10 из 10).
