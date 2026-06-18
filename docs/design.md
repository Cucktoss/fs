# path: docs/design.md
# u235fs — техническое описание (design)

Документ описывает внутреннюю архитектуру модуля `u235fs`.

## 1. Архитектура модуля

`u235fs` — это in-memory pseudo-файловая система, реализованная поверх библиотеки
VFS-помощников ядра (`libfs`/simple_*). Модуль:

* регистрирует `struct file_system_type` через `register_filesystem()`;
* монтируется без устройства через `mount_nodev()` + `u235fs_fill_super()`;
* очищается через `kill_litter_super()` (с предварительной остановкой работы);
* хранит всё состояние симуляции в одной структуре `struct u235_sim`,
  закреплённой за суперблоком (`sb->s_fs_info`);
* отображает виртуальные файлы, содержимое которых генерируется при чтении из
  текущего состояния симуляции;
* обновляет состояние раз в секунду через `delayed_work`;
* защищает общее состояние через `struct mutex`.

Карта файлов задаётся `enum u235_file`, а конкретный тип файла хранится в
`inode->i_private`. Обработчик чтения по этому типу формирует нужный текст.

## 2. Структуры данных

```c
struct u235_sim {
    struct mutex        lock;        /* защита всего состояния      */
    struct delayed_work dwork;       /* тик симуляции (раз в сек)   */
    struct super_block *sb;

    long mass, volume, freq_milli,
         impurities, external_neutrons, neutron_speed;   /* PARAMS */

    bool params_locked, started, running, ram_present;   /* флаги  */
    u64  real_seconds;                                   /* время  */

    double N0, remaining, decayed, decay_rate, temperature; /* модель */
    char   s_remaining[32], s_decayed[32], s_rate[32], s_temp[24];
};
```

`initial_frequency` хранится как целое `freq_milli` (= значение × 1000), чтобы не
держать в структуре `double`-поля, к которым обращаются вне FPU-региона. Поля
`double` (`N0`, `remaining`, …) читаются и пишутся **только** внутри
`u235fs_recompute()`, то есть только между `kernel_fpu_begin()/kernel_fpu_end()`.

## 3. Жизненный цикл mount / unmount

**mount:**
1. `u235fs_mount()` → `mount_nodev(..., u235fs_fill_super)`.
2. `u235fs_fill_super()` задаёт `s_magic`, `s_op`, выделяет `struct u235_sim`
   (`kzalloc`), инициализирует mutex и `delayed_work`, заполняет параметры по
   умолчанию, создаёт корневой inode (`d_make_root`) и файл `PARAMS`.

**unmount / rmmod:**
* `kill_sb` (`u235fs_kill_sb`) сначала вызывает `cancel_delayed_work_sync()`,
  затем `kill_litter_super()` (освобождает все закреплённые dentry/inode) и
  `kfree(sim)`.
* Пока ФС смонтирована, счётчик ссылок модуля удерживается
  (`file_system_type.owner = THIS_MODULE`), поэтому `rmmod` при смонтированной ФС
  невозможен — это исключает гонку выгрузки.

## 4. Как создаются файлы

Файлы создаются как пары inode+dentry в dcache:
* `u235fs_make_inode()` — `new_inode()`, режим, владелец, времена
  (`simple_inode_init_ts` на 6.6+, иначе прямое присваивание), тип в `i_private`,
  назначение `i_op`/`i_fop`.
* `u235fs_add_file()` — `d_alloc_name()` + `d_add()`. Удерживаемая ссылка
  закрепляет файл на время жизни ФС; `kill_litter_super()` корректно их освободит.

`PARAMS` создаётся при `fill_super`. Файлы состояния (`TEMPERATURE`,
`DECAYED_NUCLEI`, `DECAY_RATE`, `ELAPSED_TIME`, `STATE`) создаются один раз — при
первом старте симуляции (внутри `create(RAM)`), когда VFS уже держит блокировку
каталога. Поэтому они остаются видимыми и после паузы.

`readdir` (`ls`) обслуживается `simple_dir_operations` (dcache_readdir): он
перечисляет дочерние dentry каталога. Содержимое файлов генерируется на лету в
`u235fs_read()` через `simple_read_from_buffer()`.

## 5. Запуск/пауза через `RAM`

* **create(`RAM`)** — `u235fs_create()`. Разрешено только имя `RAM`; иначе
  `-EPERM`. Инстанцируется inode RAM (`d_instantiate` + `dget` для закрепления).
  При первом запуске: `started=true`, `params_locked=true`, `real_seconds=0`,
  вычисляется снимок t=0 (`u235fs_recompute`), создаются файлы состояния. Затем
  `running=ram_present=true` и `schedule_delayed_work(&dwork, HZ)`.
* **unlink(`RAM`)** — `u235fs_unlink()`. Разрешено только `RAM`. Под mutex
  выставляются `running=false`, `ram_present=false`; затем (без удержания mutex)
  `cancel_delayed_work_sync()` и `simple_unlink()` (снимает закрепление,
  удаляет dentry → файл `RAM` исчезает). Значения остаются в `struct u235_sim`.
* **повторный create(`RAM`)** — так как `started==true`, файлы состояния уже
  существуют; просто `running=ram_present=true` и перезапуск `delayed_work`.
  Симуляция продолжается с сохранённого `real_seconds` и `remaining`.

## 6. Почему `PARAMS` блокируется после первого старта

Параметры влияют на `N0` и `lambda_eff`. Изменение их в середине эксперимента
сделало бы кривую распада противоречивой. Поэтому при первом `create(RAM)`
выставляется `params_locked=true`, и `u235fs_write()` для `PARAMS` возвращает
`-EPERM`, пока ФС смонтирована. До первого `RAM` запись разрешена и парсит строки
`key=value` (`strsep`/`strim`/`kstrtol`; частота — через `parse_double` внутри
FPU-региона с конвертацией в `freq_milli`).

## 7. Периодическое обновление (`delayed_work`)

Используется `struct delayed_work` (контекст процесса — это удобно и безопасно для
работы с FPU), запланированный на `HZ` (1 секунда). Обработчик `u235fs_tick()`:

1. берёт `mutex`;
2. если `running` — увеличивает `real_seconds`, вызывает `u235fs_recompute()`;
3. отпускает `mutex`;
4. если тикали — перепланирует себя на `+HZ`.

Так как `running` проверяется под mutex, а в `unlink` он сбрасывается перед
`cancel_delayed_work_sync()`, после паузы новых тиков не возникает (любой уже
запущенный обработчик увидит `running==false` и не перепланируется; уже
поставленная в очередь работа будет снята синхронным cancel).

## 8. Синхронизация

Всё разделяемое состояние защищено одним `mutex` (`sim->lock`). Путь чтения, путь
записи, тик и create/unlink берут этот mutex. `cancel_delayed_work_sync()`
вызывается **без** удержания mutex, чтобы не было взаимной блокировки с
обработчиком, который этот mutex берёт.

## 9. Математическая модель в коде

`u235fs_recompute()` — единственная точка вычислений:

* конвертирует параметры в `double`;
* считает `N0 = (mass/235) * Avogadro`;
* `lambda = ln(2)/T_half`;
* поправки: `impurity_factor`, `neutron_factor` (с верхним clamp),
  `density_factor`, множитель `initial_frequency`;
* `lambda_eff = lambda * freq * impurity_factor * neutron_factor`;
* `model_years = real_seconds * MODEL_YEARS_PER_SECOND`;
* `remaining = N0 * exp(-lambda_eff * model_years)` (`k_exp` — собственный exp);
* `decayed = N0 - remaining`; `decay_rate = remaining_prev - remaining` за 1 с;
* `temperature = base + decay_rate * TEMP_FACTOR * density_factor`;
* применяет все clamp и форматирует значения в строки.

### Плавающая точка в ядре

В ядре нет `libm`, поэтому реализованы вручную:
* `k_exp()` — `exp()` через редукцию аргумента и ряд Тейлора;
* `fmt_sci()` — научная нотация `m.mmme±NN` (kernel `printf` не умеет `%e`/`%f`);
* `fmt_fixed2()` — формат с двумя знаками после запятой;
* `parse_double()` — разбор строки в `double`.

Все эти функции вызываются строго внутри `kernel_fpu_begin()/kernel_fpu_end()`.
Объект собирается с `-msse -msse2 -mpreferred-stack-boundary=4
-fno-tree-vectorize` (см. Makefile), так как ядро отключает SSE глобально. Модуль
ориентирован на x86_64.

## 10. demo.sh и test.sh

* `demo.sh` — последовательная демонстрация всех шагов жизненного цикла
  (`set -euo pipefail`, `trap cleanup EXIT`): сборка, загрузка, монтирование,
  показ/изменение `PARAMS`, запуск, наблюдение за обновлением, пауза,
  продолжение, проверка блокировки `PARAMS`, размонтирование, выгрузка.
* `test.sh` — автоматические проверки с выводом `[OK]/[FAIL]`; каждая проверка —
  отдельное утверждение (наличие файлов, рост `ELAPSED_TIME`/`DECAYED_NUCLEI`,
  положительные `DECAY_RATE`/`TEMPERATURE`, пауза/продолжение, блокировка
  `PARAMS`). Числа в научной нотации сравниваются через `awk`. Любая ошибка →
  выход с ненулевым кодом.
* `clean.sh` — идемпотентная очистка (размонтирование, выгрузка, удаление
  артефактов), не падает при уже выполненной очистке.
