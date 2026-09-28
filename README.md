# gpufan

**Управление вентиляторами материнской платы на macOS: обороты следуют за температурой
процессора и видеокарты.** Для хакинтошей на SuperIO-чипах Nuvoton NCT679x — то же самое,
что `pwmconfig` и `fancontrol` из lm-sensors для Linux.

![license](https://img.shields.io/badge/license-BSD--3--Clause-blue)
![platform](https://img.shields.io/badge/platform-macOS-lightgrey)
![chips](https://img.shields.io/badge/Nuvoton-NCT6779D%20%2F%20NCT679xD-success)

Проект состоит из двух частей.

**Кекст** делает каналы вентиляторов видимыми для системы. Плагин VirtualSMC публикует
вентиляторы SuperIO-чипа как обычные SMC-вентиляторы Apple (`F<n>Ac`, `F<n>Md`, `F<n>Tg`…),
поэтому управлять ими может любая программа, понимающая SMC, — не только эта.

**Утилита `gpufan`** их настраивает: показывает каналы и датчики, связывает одно с другим,
задаёт пороги температур и ставит фоновую службу в автозапуск. Настройка одноразовая.

## Как это выглядит

```console
$ gpufan
Каналы вентиляторов
  №   разъём        об/мин    PWM  режим  управление
  0   SYSFAN             0     80   BIOS  есть
  1   CPUFAN           492     69   duty  есть
  2   AUXFAN0            0    255   BIOS  есть
  3   AUXFAN1            0    255   BIOS  есть
  4   AUXFAN2            0    255   BIOS  есть
  5   AUXFAN3            0    127   BIOS  есть
  7   Vega20 GPU         0      —      —  нет, только тахометр

Датчики температуры
   1) TC0P     55 °C  CPU, рядом с сокетом
   2) TC0D     55 °C  CPU, кристалл
   3) TG0D     43 °C  GPU, кристалл
   4) TGDD     43 °C  GPU, кристалл (Vega20)
   5) TG0P     43 °C  GPU, рядом с чипом
  ...

Служба gpufand: работает (pid 3059)
```

## Установка

```sh
zsh kext/install.sh          # собрать кекст и положить в EFI/OC/Kexts
# перезагрузиться
./tools/gpufan doctor        # проверить, что кекст загрузился и управление включилось
sudo ./tools/gpufan setup    # мастер настройки, в конце поднимает службу
```

Мастер спрашивает три вещи: какие каналы крутить, к каким датчикам их привязать и при каких
температурах держать минимальные и максимальные обороты. Групп датчиков может быть несколько —
итоговые обороты берутся по максимуму, поэтому вентиляторы раскрутятся и когда греется
процессор, и когда греется видеокарта.

## Команды

| Команда | Что делает |
|---|---|
| `gpufan` | каналы, датчики, состояние службы |
| `gpufan doctor` | что загружено и работает ли управление |
| `gpufan sensors [-a]` | датчики; `-a` — все ключи, без свёртки дубликатов |
| `gpufan log [n]` | последние строки журнала службы |
| `sudo gpufan setup` | мастер настройки и установка службы |
| `sudo gpufan scan [сек]` | прогон всех каналов: какой из них что крутит |
| `sudo gpufan test <n>` | один канал на слух, обороты только вверх |
| `sudo gpufan calibrate <n>` | обороты по ступеням скважности |
| `sudo gpufan install` | поставить и запустить службу по текущему конфигу |
| `sudo gpufan start` `stop` `restart` | управление службой; `stop` возвращает вентиляторы BIOS |
| `sudo gpufan uninstall` | снять службу |

Запись в SMC возможна только от root — без `sudo` AppleSMC отвечает `kIOReturnNotPrivileged`.
Чтение привилегий не требует, поэтому `gpufan`, `doctor` и `sensors` работают от пользователя.

## Конфигурация

Мастер пишет `/usr/local/etc/gpufand.conf`; формат простой, править руками можно и нужно.
Образец с описанием всех параметров — [`daemon/gpufand.conf.example`](daemon/gpufand.conf.example).

```ini
fans = 1

source = TC0P : 55:30 85:100
source = TG0D,TGDD : 45:30 80:100

interval   = 2
min_duty   = 30
fail_duty  = 100
hysteresis = 3
```

Каждая строка `source` — своя кривая: по нескольким ключам в строке берётся самый горячий
датчик, по нескольким строкам — наибольшая скважность. Применить правки без перезапуска:

```sh
sudo launchctl kill HUP system/com.shohart.gpufand
```

Служба ведёт журнал в `/var/log/gpufand.log` и выкладывает состояние в `/var/run/gpufand.json`.
Если датчики перестают читаться, обороты уходят на максимум; при остановке службы вентиляторы
возвращаются под управление BIOS.

## Устройство

```
kext/     плагин VirtualSMC: вентиляторы как SMC-ключи Apple
          README с описанием ключей, диф к апстриму, сборка, установка в EFI
daemon/   gpufand: кривая «температура → скважность», LaunchDaemon
tools/    gpufan — утилита настройки; smc.c — низкоуровневый доступ к SMC-ключам
```

Описание SMC-ключей, режимов управления и карты регистров — в [`kext/README.md`](kext/README.md).
Управление включается только на NCT6779D и NCT679xD; на остальных чипах кекст ведёт себя как
апстримный SMCSuperIO. Собирается из исходников: `kext/build.sh` сам подтягивает VirtualSMC 1.3.8,
Lilu и MacKernelSDK.

## Про разводку платы

Каналов у чипа обычно шесть, а разъёмов на плате два-три, и соответствие между ними не всегда
очевидное. На X99-QD3, например, оба четырёхконтактных разъёма (CPU_FAN и SYS_FAN) сидят на одном
PWM-канале, а тахометр заведён только с CPU_FAN: `gpufan` показывает шесть каналов, но обороты
меняет только канал 1, зато сразу у обоих вентиляторов. Это не ошибка определения — так разведена
плата.

Разобраться на своей помогают `sudo gpufan scan` (прогоняет все каналы по очереди) и
`sudo gpufan test <n>` (один канал, на слух). Нулевые обороты при работающем управлении означают
только отсутствие тахометра на этом канале — скважностью он всё равно управляется, и службе
этого достаточно.

## Благодарности

Кекст — форк [SMCSuperIO](https://github.com/acidanthera/VirtualSMC) из VirtualSMC
(vit9696, joedm); изменён единственный файл, `NuvotonDevice`. Карта регистров NCT679x взята
из драйвера `nct6775` ядра Linux. Подробности — в [`NOTICE`](NOTICE).

## English summary

`gpufan` makes motherboard fans follow CPU and GPU temperature on macOS — the equivalent of
lm-sensors' `pwmconfig` and `fancontrol`, for hackintoshes with Nuvoton NCT679x SuperIO chips.

A patched VirtualSMC plugin exposes the chip's fans as ordinary Apple SMC fans (`F<n>Ac`,
`F<n>Md`, `F<n>Tg`…), so any SMC-aware program can drive them. The `gpufan` utility lists
channels and sensors, walks you through binding them and setting temperature thresholds, and
installs a launchd service. Fan curves take the maximum across several sensor groups, so the
fans spin up whether the CPU or the GPU is the one getting hot. Documentation is in Russian.

Licensed BSD-3-Clause, matching upstream VirtualSMC; see [`NOTICE`](NOTICE) for attribution
of derived code.
