# Домашнее задание: Изолированный запуск процесса

## Цель

Написать минимальную утилиту контейнеризации на C, которая запускает
процесс в изолированных пространствах имён Linux: PID, mount, user,
UTS, IPC и network. Запуск должен быть полностью rootless, без
дополнительной настройки окружения.

## Как это работает

Утилита состоит из родительского и дочернего процессов.

1. Родитель вызывает `clone()` с флагами `CLONE_NEWPID | CLONE_NEWNS |
   CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET`.
2. Через pipe родитель и ребёнок синхронизируются: ребёнок ждёт,
   пока родитель настроит отображение UID/GID в
   `/proc/<pid>/uid_map` и `/proc/<pid>/gid_map`.
3. После сигнала ребёнок:
   - меняет hostname внутри UTS-namespace (`sethostname`);
   - делает `chroot` в указанный rootfs и `chdir("/")`;
   - монтирует `/proc` и `/dev` (tmpfs + базовые device-ноды);
   - запускает целевую команду через `execvp`.

Сборка:

    gcc -o runtime main.c -Wall -Wextra

Запуск:

    ./runtime run --rootfs /tmp/alpine /bin/ls

## Результаты

Использовался Alpine minirootfs, распакованный в `/tmp/alpine`.
Все команды запускались от обычного пользователя, без `sudo`.

### Хост-система (родитель)

Корень файловой системы:

    $ ls /
    bin   cdrom  etc   lib    lib64   lost+found  mnt  proc  run   snap
    swapfile  tftpboot  usr
    boot  dev    home  lib32  libx32  media       opt  root  sbin  srv
    sys       tmp       var

Процессы:

    $ ps
        PID TTY          TIME CMD
       2883 pts/1    00:00:00 bash
       3892 pts/1    00:00:00 ps

Пользователь:

    $ id
    uid=1000(xilinx) gid=1000(xilinx)
    groups=1000(xilinx),4(adm),20(dialout),24(cdrom),27(sudo),
           30(dip),46(plugdev),120(lpadmin),131(lxd),
           132(sambashare),998(vboxsf)

Сеть:

    $ ip a
    1: lo: ... UP,LOWER_UP ...
        inet 127.0.0.1/8 scope host lo
    2: eth0: ... UP,LOWER_UP ...
        inet 10.0.2.15/24 ...
    3: eth1: ... UP,LOWER_UP ...
        inet 10.10.10.15/24 ...
        inet 172.16.21.202/24 ...

### Внутри контейнера

Список файлов:

    $ ./runtime run --rootfs /tmp/alpine /bin/ls
    bin    dev    etc    home   lib    media  mnt    opt    proc
    root   run    sbin   srv    sys    tmp    usr    var

PID-пространство:

    $ ./runtime run --rootfs /tmp/alpine /bin/ps
    PID   USER     TIME  COMMAND
        1 root      0:00 /bin/ps

User-namespace:

    $ ./runtime run --rootfs /tmp/alpine /usr/bin/id
    uid=0(root) gid=0(root)
    groups=65534(nobody),...,0(root)

UTS-namespace:

    $ ./runtime run --rootfs /tmp/alpine /bin/hostname
    container

Network-namespace:

    $ ./runtime run --rootfs /tmp/alpine /bin/cat /proc/net/dev

Виден только loopback `lo` без трафика.

### Сводная таблица

| Проверка            | Хост                                  | Контейнер                |
|---------------------|---------------------------------------|--------------------------|
| Пользователь (`id`) | `uid=1000(xilinx)`                    | `uid=0(root)`            |
| Hostname            | `xilinx`                              | `container`              |
| Процессы (`ps`)     | `bash` (2883), `ps` (3892)            | один, PID 1              |
| Корень (`ls /`)     | `boot`, `snap`, `tftpboot`, ...       | `bin`, `dev`, `etc`, ... |
| Сеть                | `lo`, `eth0`, `eth1` с адресами       | только `lo`              |

Все различия в столбцах подтверждают, что изоляция работает:
пользователь, хост, набор процессов, корневая файловая система
и сетевые интерфейсы внутри контейнера отличаются от хоста.

## Ответ на вопрос

**Что будет, если смонтировать `/proc` без изоляции mount-пространства?**

Если монтировать `/proc` без `CLONE_NEWNS`, то таблица монтирований
общая для всей системы, и монтирование повлияет на хост.

Последствия:

1. Вместо процессов контейнера `/proc` покажет **все процессы хоста**,
   их командные строки и переменные окружения. Это утечка информации.
2. Через `/proc/<pid>/root/` можно попытаться добраться до корня
   хост-системы и выйти за пределы контейнера.
3. Изоляция перестаёт работать, потому что mount namespace —
   это базовый механизм, на котором держится вся остальная
   изоляция файловой системы.

Поэтому mount namespace обязателен: он гарантирует, что все
монтирования внутри контейнера остаются локальными.
