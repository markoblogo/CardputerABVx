# PR-ready Porting Map: Cast + Persistence Boundary

Scope: подготовка безопасного прогона для PR в `main/main.cpp` и `main/persistence.*` без изменения runtime логики.

## 0) Что уже готово
- `main/main.cpp` собирается.
- В `main/CMakeLists.txt` подключен `esp_http_client` (для cast API).
- Cast/Upload блоки уже живут в одном файле и используют `abvx::loadEventLog` / `abvx::mergeEventLog` для логов инбокса из `main/persistence.cpp`.

## 1) Блоки в `main/main.cpp` (для выноса в `main/persistence.*`)

### A. Cast-конфиг (state + serialization)
- Переносится:
  - `cast_host`, `cast_port`, `cast_trace_enabled` (переменные)
  - `setCastHost`, `setCastPortFromName`, `isCastHostChar`
  - запись/чтение в конфиге (`CAST_HOST=`, `CAST_PORT=`, `CAST_TRACE=`) из `saveConfig`/`loadConfig`
- Эквивалентный новый API в `persistence`:
  - `bool loadCastConfig(std::string* err = nullptr)`
  - `bool saveCastConfig(const char* host, uint16_t port, bool trace, std::string* err = nullptr)`
- Поведение в `main` остается: на `loadConfig` инициализировать значения, на `saveConfig` — persist.

### B. Connection upload state machine
- Оставить в `main` (переноса не делать в этой итерации), но добавить doc-boundary:
  - `connection_upload_*`, `ConnectionUploadOp`, `queueUploadOp`, `processConnectionUploadOps`
  - handlers: `/api/upload-begin`, `/api/upload-chunk`, `/api/upload-finish`, `/api/upload-abort`
- Риск при переносе: зависимость от `screen` состояния и рендеринга статус-панели, поэтому лучше оставить как есть.

### C. Cast trace/debug telemetry
- Переносится только DTO и helper в persistence слой:
  - `setCastTrace`, `cast_last_*`, `cast_last_status_ms`
- Цель: централизовать запись метаданных для debug-экрана, без изменения семантики API.

### D. Message path / settings UI
- Не переносить в эту итерацию (чистый UI).
- Только сделать точечный reference map:
  - `drawSettings()` и режим редактирования хоста/порта
  - `drawSettingsConnection()` / `drawSettings` вызов из `handleInput`.

## 2) Блоки в `main/persistence.h|cpp` (current ownership)

### E. EventLog (уже реализован и используется)
- Текущий код:
  - `loadEventLog`
  - `mergeEventLog`
- Источник: `main/persistence.cpp`
- Использование:
  - Inbox инициализация/flush в `main/main.cpp`

### F. Добавить cast-конфиг helpers (минимум API)
- Добавить новые функции в `main/persistence.h` как read/write тонкий слой.
- Реализация в `main/persistence.cpp` должна:
  - работать поверх существующего `CONFIG.TXT` (без нового формата)
  - менять только строки `CAST_*`
  - возвращать диагностику через `err` по template-модели существующих функций.

## 3) Пошаговый PR-ready pass (без изменения логики)

1. Добавить в `persistence.h/.cpp` функции `loadCastConfig/saveCastConfig`.
2. В `main/main.cpp` заменить только парсинг/сериализацию cast-строк в `loadConfig`/`saveConfig` на вызовы новых функций.
3. Сохранить все текущие endpoint-обработчики cast без логической модификации.
4. Не перемещать `draw`/`handleInput` в этот pass.
5. Проверить: `idf.py build` зелёный.
6. Проверить на устройстве:
   - Settings: изменение host/port
   - reboot: значения выживают
   - cast trace: status path/code/latency обновляются.

## 4) Пределы этого pass (чтобы не поломать текущую логику)

- Не менять существующий HTTP parser/обработчики.
- Не менять порядок рендеров экранов.
- Не менять upload staging/state machine.
- Не выносить `main` UI-логику.

## 5) Критерий готовности

- `idf.py build` успешно.
- Поведение при cold boot и reboot совпадает с текущим.
- Нет изменения контрактов API: `/api/cast/*`, `/api/status`, `/api/write-test`, upload endpoints.
- Никаких новых регрессионных изменений в других модулях.
