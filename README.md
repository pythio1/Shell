# README

# myshell — Простая реализация командной оболочки на C

Небольшой, но полнофункциональный интерпретатор командной строки, написанный на языке C. Поддерживает конвейеры, перенаправления ввода/вывода, встроенные команды и корректную обработку ошибок.

---

## Содержание

- [Возможности](#возможности)
- [Сборка](#сборка)
- [Запуск](#запуск)
- [Использование](#использование)
  - [Встроенные команды](#встроенные-команды)
  - [Конвейеры](#конвейеры)
  - [Перенаправления](#перенаправления)
  - [Кавычки](#кавычки)
- [Обработка ошибок](#обработка-ошибок)
- [Архитектура](#архитектура)
- [Ограничения](#ограничения)
- [Примеры](#примеры)


---

## Возможности

- ✅ **Интерактивный ввод команд** со стандартного ввода
- ✅ **Встроенные команды**: `cd`, `pwd`, `exit`
- ✅ **Конвейеры** (`|`) — неограниченное число команд в цепочке
- ✅ **Перенаправления**:
  - `<` — ввод из файла
  - `>` — вывод в файл (с перезаписью)
  - `>>` — вывод в файл (с добавлением)
  - `2>` — перенаправление `stderr`
- ✅ **Кавычки** — одинарные (`'...'`) и двойные (`"..."`) для аргументов с пробелами
- ✅ **Автоматический поиск команд** в `PATH`
- ✅ **Проверка синтаксиса** с выводом `Syntax error`
- ✅ **Проверка существования файлов** перед запуском
- ✅ **Корректная работа с дочерними процессами** через `fork()` и `execvp()`
- ✅ **Освобождение памяти** во всех ветвях выполнения

---

## Сборка

### Требования

- Компилятор C с поддержкой POSIX (`gcc` или `clang`)
- ОС: Linux, macOS, *BSD (должна работать и в WSL)
- Заголовочные файлы POSIX: `unistd.h`, `sys/wait.h`, `fcntl.h`

### Команда сборки

```bash
gcc -Wall -Wextra -O2 -o myshell myshell.c
```

Для отладочной версии:

```bash
gcc -Wall -Wextra -g -fsanitize=address -o myshell myshell.c
```

Рекомендуется использовать флаг `-Wall -Wextra`, чтобы поймать предупреждения. Код собирается без единого warning'а.

---

## Запуск

```bash
./myshell
```

После запуска оболочка читает строки со стандартного ввода. Выход:

- По нажатию `Ctrl+D` (EOF)
- По команде `exit`

---

## Использование

### Встроенные команды

#### `cd [directory]`

Смена текущего каталога.

```bash
cd /tmp           # перейти в /tmp
cd ..             # перейти в родительский каталог
cd                # перейти в $HOME
cd "Мои файлы"    # путь с пробелами
```

Если каталог не существует, выводится:

```
Directory <path> not found
```

#### `pwd`

Вывод текущего рабочего каталога.

```bash
pwd
# /home/user/projects
```

#### `exit`

Завершение работы оболочки.

```bash
exit
# Shell is closing, bye
```

> ⚠️ `exit` завершает shell **только как одиночная команда**. Внутри конвейера (`echo | exit`) он не завершает сессию — это сделано намеренно, чтобы не допускать неожиданных выходов.

---

### Конвейеры

Символ `|` соединяет вывод одной команды со входом следующей.

```bash
ls -la | grep ".c" | wc -l
cat /etc/passwd | sort | uniq
echo "hello world" | tr 'a-z' 'A-Z'
```

Количество команд в конвейере ограничено только доступной памятью.

---

### Перенаправления

#### Ввод из файла: `<`

```bash
sort < input.txt
grep "pattern" < log.txt
```

#### Вывод в файл: `>`

```bash
echo "Hello" > output.txt      # перезапись
ls -la > listing.txt
```

#### Добавление в файл: `>>`

```bash
echo "New line" >> log.txt
date >> log.txt
```

#### Перенаправление stderr: `2>`

```bash
ls /nonexistent 2> errors.txt
gcc bad.c 2> compile_errors.txt
```

#### Комбинирование

```bash
prog < input.txt > output.txt 2> errors.txt
cat data.txt | sort | uniq > result.txt
```

#### Порядок токенов

Перенаправления должны идти **после** аргументов команды:

```bash
# Правильно:
grep "pattern" file.txt > out.txt

# Неправильно (Syntax error):
grep > out.txt "pattern" file.txt
```

---

### Кавычки

Аргументы с пробелами и специальными символами можно заключать в одинарные или двойные кавычки. Внутри кавычек специальные символы (`|`, `<`, `>`) считаются частью строки.

```bash
echo "Hello, World!"
mkdir "My Documents"
cat 'file with spaces.txt'
echo "a|b" > "out|file.txt"
```

> ⚠️ Экранирование внутри кавычек (`\"`, `\\`) **не поддерживается**. Незакрытая кавычка приводит к `Syntax error`.

---

## Обработка ошибок

Оболочка сообщает об ошибках в `stderr` и продолжает работу.

| Сообщение | Причина |
|-----------|---------|
| `Syntax error` | Нечётное число кавычек, пустая команда, `\|\|`, двойное перенаправление, `>` без файла, слово после перенаправления |
| `Command <name> not found` | Команда не найдена в `PATH` |
| `Directory <path> not found` | `cd` в несуществующий каталог |
| `File <name> not found` | Файл для перенаправления не может быть открыт |

Пример:

```bash
$ echo "unclosed
Syntax error
$ nonexistent_command
Command nonexistent_command not found
$ cd /no/such/dir
Directory /no/such/dir not found
$ cat < missing.txt
File missing.txt not found
```

---

## Архитектура

Программа разделена на логические этапы:

```
┌────────────────────┐
│  fgets(stdin)      │  читаем строку
└─────────┬──────────┘
          ▼
┌────────────────────┐
│  tokenize_line()   │  разбиваем на токены (учёт кавычек и операторов)
└─────────┬──────────┘
          ▼
┌────────────────────┐
│  parse_pipeline()  │  группируем токены в структуры Command
└─────────┬──────────┘
          ▼
┌────────────────────┐
│  проверки          │  команда существует? файлы открываются?
└─────────┬──────────┘
          ▼
┌────────────────────┐
│  fork() + pipe()   │  создаём процессы и конвейеры
└─────────┬──────────┘
          ▼
┌────────────────────┐
│  execvp()          │  выполняем команды
└─────────┬──────────┘
          ▼
┌────────────────────┐
│  waitpid()         │  ждём завершения
└────────────────────┘
```

### Основные структуры данных

```c
typedef enum {
    TOKEN_WORD, TOKEN_PIPE, TOKEN_REDIR_IN,
    TOKEN_REDIR_OUT, TOKEN_REDIR_OUT_APPEND, TOKEN_REDIR_ERR
} TokenType;

typedef struct {
    TokenType type;
    char *value;
} Token;

typedef struct {
    char *cmd;         // имя команды
    char **args;       // argv (NULL-терминированный)
    int argc;
    char *in_file;     // < file
    char *out_file;    // > file или >> file
    int append_out;    // 0 = ">", 1 = ">>"
    char *err_file;    // 2> file
} Command;
```

### Ключевые функции

| Функция | Назначение |
|---------|-----------|
| `tokenize_line()` | Разбивает строку на токены с учётом кавычек |
| `parse_pipeline()` | Группирует токены в структуры `Command` |
| `command_exists()` | Ищет исполняемый файл в `PATH` |
| `is_builtin()` | Проверяет, встроенная ли команда |
| `run_builtin()` | Выполняет `cd`/`pwd` |
| `cleanup_on_error()` | Освобождает память при ошибке парсинга |
| `free_tokens()`, `free_commands()` | Освобождение памяти |

---

## Ограничения

Текущая версия **не поддерживает**:

- ❌ Фоновый режим (`&`)
- ❌ Раскрытие переменных окружения (`$VAR`, `${VAR}`)
- ❌ Glob-паттерны (`*`, `?`, `[abc]`)
- ❌ Подстановку команд (`$(...)` или `` `...` ``)
- ❌ Логические операторы `&&`, `||`
- ❌ Группировку команд (`(cmd1; cmd2)`)
- ❌ Историю команд, автодополнение, редактирование строки
- ❌ Экранирование внутри кавычек (`\"`, `\\`)
- ❌ Управление заданиями (`jobs`, `fg`, `bg`)
- ❌ Сигналы (Ctrl+C завершает всю сессию, а не текущую команду)

Также стоит помнить:

- `2>` распознаётся только как **отдельный токен** (после пробела). Выражение вида `echo 2>file` без пробела перед `2` будет работать, но `echo x2>file` воспримет `x2` как слово.
- Команды ищутся в `PATH` на момент запуска; изменения `PATH` в самой оболочке не поддерживаются.
- Оболочка не является login-shell и не читает `.bashrc`/`.profile`.

---

## Примеры

### Базовые команды

```bash
ls -la
pwd
cd /tmp
echo "Hello, World!"
```

### Конвейеры

```bash
ls -la | grep ".c" | wc -l
cat access.log | awk '{print $1}' | sort -u
ps aux | grep "my_prog" | awk '{print $2}'
```

### Перенаправления

```bash
echo "log entry" >> /var/log/myapp.log
sort < unsorted.txt > sorted.txt
gcc program.c 2> compile_errors.txt
find / -name "*.conf" 2> /dev/null | head
```

### Комбинирование

```bash
cat input.txt | tr 'a-z' 'A-Z' | sort | uniq > output.txt
grep "ERROR" app.log 2> error_log.txt | wc -l
```

### Работа с кавычками

```bash
mkdir "New Folder"
cd "New Folder"
echo "line with | pipe" > "special|name.txt"
touch 'file with spaces.txt'
```

### Обработка ошибок

```bash
$ echo "unclosed
Syntax error

$ cat < nonexistent.txt
File nonexistent.txt not found

$ some_fake_command
Command some_fake_command not found

$ ls | | grep
Syntax error
```




## Полезные ссылки

- [POSIX `fork()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fork.html)
- [POSIX `execvp()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/exec.html)
- [POSIX `pipe()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pipe.html)
- [POSIX `dup2()`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/dup.html)
- [Linux `man` pages](https://man7.org/linux/man-pages/)
