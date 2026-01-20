#include <string.h>
#define _XOPEN_SOURCE 700
#define _BSD_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

/** ------------------------------------------------------------
 *  Константы для читаемости и предотвращения magic numbers
 * ------------------------------------------------------------
 *  Вместо "магических" чисел в коде мы даём имена, отражающие смысл значения:
 *  - NSEC_PER_SEC: перевод наносекунд в секунды при вычислении длительности.
 *  - EXIT_*: единая политика кодов возврата для успешного и аварийного выхода.
 *  - CHILD_ERROR_CODE: значение, которым завершает работу дочерний процесс
 *    при невозможности запустить execvp.
 *  - PROMPT_STRING: строка приглашения, определённая в заголовке.
 */
#define NSEC_PER_SEC 1000000000L

#define EXIT_SUCCESS_CODE 0
#define EXIT_FAILURE_CODE 1
#define CHILD_ERROR_CODE 127

#define PROMPT_STRING PROMPT

/* Прототипы функций, используемых в разных частях файла */
int is_redir(const char* t);
int execute_single_command(char* command);
void execute_with_and(char* line);
void process_input_line(char* line);

/** ------------------------------------------------------------
 *  calculate_timespec_diff:
 *  Вычисляет разницу между двумя временными метками start и end.
 *  Результат сохраняется в result.
 * ------------------------------------------------------------ */
void calculate_timespec_diff(
    const struct timespec* start,
    const struct timespec* end,
    struct timespec* result
) {
  result->tv_sec = end->tv_sec - start->tv_sec;
  result->tv_nsec = end->tv_nsec - start->tv_nsec;

  /** Корректируем разницу, если наносекунды стали отрицательными */
  if (result->tv_nsec < 0) {
    result->tv_sec -= 1;
    result->tv_nsec += NSEC_PER_SEC;
  }
}

/** ------------------------------------------------------------
 *  parse_command_line:
 *  Разбивает строку line на аргументы по пробелам, табам и переводам строк.
 *  Обрабатывает редиректы в начале токена: >file → > + file.
 *  Токен >>file остаётся как есть (для проверки синтаксической ошибки).
 *  Токены вида bar>bbb остаются как есть (обычный аргумент).
 *  Возвращает массив argv и количество аргументов через argc_out.
 * ------------------------------------------------------------ */
char** parse_command_line(char* line, int* argc_out) {
  char** argv = malloc((MAX_ARGS + 1) * sizeof(char*));
  if (!argv) {
    perror("malloc");
    return NULL;
  }

  int argc = 0;
  char* p = line;

  while (*p && argc < MAX_ARGS) {
    /* Пропускаем пробельные символы */
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
      ++p;
    }
    if (*p == '\0')
      break;

    /* Обрабатываем 2>&1 как единый токен */
    if (strncmp(p, "2>&1", 4) == 0) {
      argv[argc++] = "2>&1";
      p += 4;
      continue;
    }

    /* Обрабатываем >> - если за ним пробел/конец, это оператор; иначе читаем
     * всё до пробела как токен */
    if (strncmp(p, ">>", 2) == 0) {
      if (p[2] == '\0' || p[2] == ' ' || p[2] == '\t' || p[2] == '\r' ||
          p[2] == '\n') {
        /* >> отдельно стоящий оператор */
        argv[argc++] = ">>";
        p += 2;
        continue;
      }
      /* >>filename без пробела - читаем как один токен для последующей проверки
       * ошибки */
      char* token_start = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        ++p;
      size_t len = p - token_start;
      char* token = malloc(len + 1);
      if (token) {
        strncpy(token, token_start, len);
        token[len] = '\0';
        argv[argc++] = token;
      }
      continue;
    }

    /* Обрабатываем > или < в начале токена */
    if (*p == '>' || *p == '<') {
      char op = *p;
      if (op == '>') {
        argv[argc++] = ">";
      } else {
        argv[argc++] = "<";
      }
      ++p;
      /* После оператора может сразу идти имя файла без пробела */
      continue;
    }

    /* Обрабатываем | и & */
    if (*p == '|') {
      argv[argc++] = "|";
      ++p;
      continue;
    }
    if (*p == '&') {
      argv[argc++] = "&";
      ++p;
      continue;
    }

    /* Обычный аргумент: читаем до пробела или оператора в начале следующего
     * токена */
    char* token_start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      /* Если встретили оператор, проверяем, не начало ли это нового токена */
      if (*p == '>' || *p == '<' || *p == '|' || *p == '&') {
        /* Если мы ещё ничего не прочитали (len == 0), это оператор в начале */
        if (p == token_start) {
          break; /* Это будет обработано на следующей итерации */
        }
        /* Иначе это символ внутри аргумента (например bar>bbb), продолжаем */
      }
      ++p;
    }

    size_t len = p - token_start;
    if (len == 0)
      continue; /* Пустой токен — пропускаем */

    char* token = malloc(len + 1);
    if (token) {
      strncpy(token, token_start, len);
      token[len] = '\0';
      argv[argc++] = token;
    }
  }

  argv[argc] = NULL;
  if (argc_out) {
    *argc_out = argc;
  }
  return argv;
}

/* ========== Структуры и функции для редиректов и переменных ========== */

/** ------------------------------------------------------------
 *  redirect_info_t — агрегирует параметры перенаправления потоков
 * ------------------------------------------------------------
 *  В процессе разбора аргументов мы собираем все сведения о перенаправлениях
 *  в одну структуру, чтобы затем компактно применить их перед execvp.
 */
typedef struct {
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
  int redirect_stderr_to_stdout;

} redirect_info_t;

/** ------------------------------------------------------------
 *  apply_redirects — вырезает операторы перенаправления из argv и
 *  подготавливает файловые дескрипторы для dup2
 * ------------------------------------------------------------ */
redirect_info_t apply_redirects(char** argv, int* argc_ptr) {
  redirect_info_t redir = {-1, -1, -1, 0};
  int new_argc = 0;

  for (int i = 0; argv[i] != NULL; ++i) {
    if (strcmp(argv[i], ">") == 0) {
      if (argv[i + 1] == NULL) {
        /* Синтаксическая ошибка: отсутствует имя файла */
        /* Оставляем распечатанную ошибку на уровне вызывающего кода:
         * execute_single_command проверяет синтаксис заранее, но на всякий
         * случай возвращаем структуру */
        return redir;
      }
      int fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) {
        /* Ошибка ввода-вывода — пометим это специальным значением и вернём */
        redir.stdout_fd = -2;
        return redir;
      }
      redir.stdout_fd = fd;
      ++i;
      continue;
    }

    if (strcmp(argv[i], ">>") == 0) {
      if (argv[i + 1] == NULL) {
        return redir;
      }
      int fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd < 0) {
        redir.stdout_fd = -2;
        return redir;
      }
      redir.stdout_fd = fd;
      ++i;
      continue;
    }

    if (strcmp(argv[i], "<") == 0) {
      if (argv[i + 1] == NULL) {
        return redir;
      }
      int fd = open(argv[i + 1], O_RDONLY);
      if (fd < 0) {
        redir.stdin_fd = -2;
        return redir;
      }
      redir.stdin_fd = fd;
      ++i;
      continue;
    }

    if (strcmp(argv[i], "2>&1") == 0) {
      redir.redirect_stderr_to_stdout = 1;
      continue; /* Пропускаем этот аргумент (не добавляем в argv) */
    }

    argv[new_argc++] = argv[i];
  }

  argv[new_argc] = NULL;
  if (argc_ptr) {
    *argc_ptr = new_argc;
  }
  return redir;
}

/** ------------------------------------------------------------
 *  expand_env_vars — подставляет значения из окружения в аргументы
 * ------------------------------------------------------------ */
char** expand_env_vars(char** argv, int argc) {
  char** expanded_argv = malloc((argc + 1) * sizeof(char*));
  if (!expanded_argv) {
    perror("malloc");
    return argv;
  }

  for (int i = 0; i < argc; ++i) {
    const char* arg = argv[i];
    const char* dollar_pos = strchr(arg, '$');

    if (!dollar_pos) {
      expanded_argv[i] = argv[i];
      continue;
    }

    size_t var_name_len = 0;
    const char* var_start = dollar_pos + 1;
    while (var_start[var_name_len] &&
           (isalnum((unsigned char)var_start[var_name_len]) ||
            var_start[var_name_len] == '_')) {
      ++var_name_len;
    }

    if (var_name_len == 0) {
      expanded_argv[i] = argv[i];
      continue;
    }

    char var_name[256];
    if (var_name_len >= sizeof(var_name)) {
      var_name_len = sizeof(var_name) - 1;
    }
    strncpy(var_name, var_start, var_name_len);
    var_name[var_name_len] = '\0';

    const char* var_value = getenv(var_name);
    if (!var_value) {
      var_value = "";
    }

    size_t prefix_len = dollar_pos - arg;
    const char* suffix = var_start + var_name_len;
    size_t suffix_len = strlen(suffix);
    size_t total_len = prefix_len + strlen(var_value) + suffix_len + 1;

    char* expanded = malloc(total_len);
    if (!expanded) {
      perror("malloc");
      expanded_argv[i] = argv[i];
      continue;
    }

    strncpy(expanded, arg, prefix_len);
    strcpy(expanded + prefix_len, var_value);
    strcpy(expanded + prefix_len + strlen(var_value), suffix);

    expanded_argv[i] = expanded;
  }

  expanded_argv[argc] = NULL;
  return expanded_argv;
}

/* ========== Функция для обработки конвейера (pipe) ========== */

int has_pipe(char* line) {
  return strchr(line, '|') != NULL;
}

int has_background(char* line) {
  const char* ptr = line;
  while (*ptr) {
    if (*ptr == '&') {
      return 1;
    }
    ++ptr;
  }
  return 0;
}

void execute_pipeline(char* line) {
  /* Копируем строку, так как стrtok модифицирует её */
  char* line_copy = malloc(strlen(line) + 1);
  if (!line_copy) {
    perror("malloc");
    return;
  }
  strcpy(line_copy, line);

  /* Подсчитываем количество команд (разделены |), чтобы заранее выделить массив
   * указателей */
  int cmd_count = 1;
  for (int i = 0; line_copy[i]; ++i) {
    if (line_copy[i] == '|') {
      ++cmd_count;
    }
  }

  /* Выделяем память для команд */
  char** commands = malloc(cmd_count * sizeof(char*));
  if (!commands) {
    perror("malloc");
    free(line_copy);
    return;
  }

  /* Разбиваем на команды, сохраняя указатели на начало каждого фрагмента */
  char* saveptr = NULL;
  char* cmd = strtok_r(line_copy, "|", &saveptr);
  int idx = 0;
  while (cmd && idx < cmd_count) {
    /* Пропускаем пробелы в начале */
    while (*cmd && (*cmd == ' ' || *cmd == '\t')) {
      ++cmd;
    }
    commands[idx++] = cmd;
    cmd = strtok_r(NULL, "|", &saveptr);
  }

  /* Массив для хранения PID дочерних процессов */
  pid_t* pids = malloc(cmd_count * sizeof(pid_t));
  if (!pids) {
    perror("malloc");
    free(commands);
    free(line_copy);
    return;
  }

  int prev_pipe_read = -1;

  for (int i = 0; i < cmd_count; ++i) {
    int pipe_fds[2];

    /* Создаём трубу для всех команд кроме последней */
    if (i < cmd_count - 1) {
      if (pipe(pipe_fds) < 0) {
        perror("pipe");
        free(pids);
        free(commands);
        free(line_copy);
        return;
      }
    }

    /* Парсим текущую команду */
    int argc = 0;
    char** argv = parse_command_line(commands[i], &argc);
    if (!argv || argc == 0) {
      free(argv);
      if (i < cmd_count - 1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
      }
      continue;
    }

    /* Расширяем переменные окружения */
    char** expanded_argv = expand_env_vars(argv, argc);

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      free(expanded_argv);
      if (expanded_argv != argv)
        free(argv);

      if (i < cmd_count - 1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
      }
      continue;
    }

    if (pid == 0) {
      /* Дочерний процесс */

      /* Перенаправляем stdin из предыдущей трубы (если не первая команда) */
      if (prev_pipe_read != -1) {
        dup2(prev_pipe_read, STDIN_FILENO);
        close(prev_pipe_read);
      }

      /* Перенаправляем stdout в трубу (если не последняя команда) */
      if (i < cmd_count - 1) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
      }

      execvp(expanded_argv[0], expanded_argv);
      perror(expanded_argv[0]);
      _exit(EXIT_FAILURE_CODE);
    }

    /* Родительский процесс */
    pids[i] = pid;

    /* Закрываем дескрипторы в родителе */
    if (prev_pipe_read != -1) {
      close(prev_pipe_read);
    }
    if (i < cmd_count - 1) {
      close(pipe_fds[1]);
      prev_pipe_read = pipe_fds[0];
    }

    if (expanded_argv != argv) {
      for (int j = 0; j < argc; ++j) {
        if (expanded_argv[j] != argv[j]) {
          free(expanded_argv[j]);
        }
      }
      free(expanded_argv);
    }
    free(argv);
  }

  /* Ждём завершения всех процессов в конвейере, чтобы не оставить зомби */
  for (int i = 0; i < cmd_count; ++i) {
    int status;
    waitpid(pids[i], &status, 0);
  }

  free(pids);
  free(commands);
  free(line_copy);
}

/** ------------------------------------------------------------
 *  process_input_line:
 *  Обрабатывает одну строку ввода.
 * ------------------------------------------------------------ */
void process_input_line(char* line) {
  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\n')
    line[len - 1] = '\0';
  while (*line == ' ' || *line == '\t')
    line++;
  if (*line == '\0')
    return;

  if (strncmp(line, "cat", 3) == 0) {
    char* after_cat = line + 3;
    while (*after_cat == ' ' || *after_cat == '\t')
      after_cat++;
    if (*after_cat == '\0') {
      char* buffer = NULL;
      size_t buf_size = 0;
      while (1) {
        ssize_t read_len = getline(&buffer, &buf_size, stdin);
        if (read_len == -1)
          break;
        fwrite(buffer, 1, (size_t)read_len, stdout);
        fflush(stdout);
      }
      free(buffer);
      return;
    }
  }

  /* Проверяем, есть ли конвейер (|) */
  if (has_pipe(line)) {
    execute_pipeline(line);
    return;
  }

  execute_with_and(line);
}

/** ------------------------------------------------------------
 *  run_shell:
 *  Главный цикл shell — выделен отдельно от main
 * ------------------------------------------------------------ */
void run_shell(void) {
  /* Отключаем буферизацию stdin чтобы дочерние процессы могли читать оставшиеся
   * данные */
  setvbuf(stdin, NULL, _IONBF, 0);

  char* input_line = NULL;

  size_t buffer_size = 0;

  while (1) {
    if (isatty(STDIN_FILENO)) {
      fputs(PROMPT_STRING, stdout);
      fflush(stdout);
    }

    ssize_t line_length = getline(&input_line, &buffer_size, stdin);
    if (line_length == -1) {
      if (isatty(STDIN_FILENO))

        putchar('\n');

      break;
    }

    process_input_line(input_line);
  }
}

void execute_with_and(char* line) {
  /* Проверяем, есть ли & в конце (фоновое выполнение) */
  int run_background = 0;
  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '&') {
    /* Проверяем, это не && */
    if (len == 1 || line[len - 2] != '&') {
      run_background = 1;
      line[len - 1] = '\0'; /* Удаляем & */
      /* Пропускаем пробелы перед & */
      len--;
      while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
      }
    }
  }

  /* Разбиваем на команды по && */
  int cmd_count = 1;
  for (size_t i = 0; i < len; ++i) {
    if (i < len - 1 && line[i] == '&' && line[i + 1] == '&') {
      ++cmd_count;
    }
  }

  char** commands = malloc((cmd_count + 1) * sizeof(char*));
  if (!commands) {
    perror("malloc");
    return;
  }

  /* Извлекаем команды */
  int cmd_idx = 0;
  char* cmd_start = line;
  for (size_t i = 0; i <= len; ++i) {
    if (i < len && i < len - 1 && line[i] == '&' && line[i + 1] == '&') {
      /* Найден && */
      line[i] = '\0';
      commands[cmd_idx++] = cmd_start;
      i += 1; /* Пропускаем второй & */
      cmd_start = &line[i + 1];
      i++;

    } else if (i == len) {
      /* Конец строки */
      commands[cmd_idx++] = cmd_start;
    }
  }
  commands[cmd_count] = NULL;

  int last_status = 0;
  pid_t bg_pid = -1;

  for (int i = 0; i < cmd_count; ++i) {
    char* current_cmd = commands[i];

    /* Пропускаем пробелы в начале */
    while (*current_cmd && (*current_cmd == ' ' || *current_cmd == '\t')) {
      ++current_cmd;
    }

    if (*current_cmd == '\0') {
      continue;
    }

    if (run_background && i == cmd_count - 1) {
      /* Запускаем в фоне - выполняем через fork без waitpid */
      int argc = 0;
      char** argv = parse_command_line(current_cmd, &argc);
      if (argv && argc > 0) {
        char** expanded_argv = expand_env_vars(argv, argc);
        bg_pid = fork();
        if (bg_pid == 0) {
          /* Дочерний процесс */
          redirect_info_t redir = apply_redirects(expanded_argv, &argc);

          if (redir.stdin_fd >= 0) {
            dup2(redir.stdin_fd, STDIN_FILENO);
            close(redir.stdin_fd);
          }
          if (redir.stdout_fd >= 0) {
            dup2(redir.stdout_fd, STDOUT_FILENO);
            close(redir.stdout_fd);
          }
          if (redir.stderr_fd >= 0) {
            dup2(redir.stderr_fd, STDERR_FILENO);
            close(redir.stderr_fd);
          } else if (redir.redirect_stderr_to_stdout) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
          }

          execvp(expanded_argv[0], expanded_argv);
          perror(expanded_argv[0]);
          _exit(EXIT_FAILURE_CODE);
        } else if (bg_pid > 0) {
          /* Родительский процесс - выводим PID */
          fprintf(stderr, "[%d]\n", (int)bg_pid);
        }

        if (expanded_argv != argv) {
          for (int j = 0; j < argc; ++j) {
            if (expanded_argv[j] != argv[j]) {
              free(expanded_argv[j]);
            }
          }
          free(expanded_argv);
        }
        free(argv);
      }
    } else {
      last_status = execute_single_command(current_cmd);
      if (last_status != 0)
        break;
    }
  }

  free(commands);
}

int execute_command(char** argv) {
  if (!argv || !argv[0])
    return 0;

  if (strcmp(argv[0], "exit") == 0) {
    exit(EXIT_SUCCESS_CODE);
  }
  if (strcmp(argv[0], "cd") == 0) {
    const char* target = argv[1] ? argv[1] : getenv("HOME");
    if (chdir(target) != 0)
      fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
    return 0;
  }

  /* Обработка редиректов */
  int argc_temp = 0;
  for (int i = 0; argv[i] != NULL; ++i)
    ++argc_temp;
  redirect_info_t redir = apply_redirects(argv, &argc_temp);
  if (redir.stdin_fd == -2 || redir.stdout_fd == -2 || redir.stderr_fd == -2) {
    puts("I/O error");
    if (redir.stdin_fd >= 0)
      close(redir.stdin_fd);
    if (redir.stdout_fd >= 0)
      close(redir.stdout_fd);
    if (redir.stderr_fd >= 0)
      close(redir.stderr_fd);
    return 0;
  }

  if (argc_temp == 0 || argv[0] == NULL) {
    if (redir.stdin_fd >= 0)
      close(redir.stdin_fd);
    if (redir.stdout_fd >= 0)
      close(redir.stdout_fd);
    if (redir.stderr_fd >= 0)
      close(redir.stderr_fd);
    return EXIT_FAILURE_CODE;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    if (redir.stdin_fd >= 0)
      close(redir.stdin_fd);
    if (redir.stdout_fd >= 0)
      close(redir.stdout_fd);
    if (redir.stderr_fd >= 0)
      close(redir.stderr_fd);
    return EXIT_FAILURE_CODE;
  }
  if (pid == 0) {
    if (redir.stdin_fd >= 0) {
      dup2(redir.stdin_fd, STDIN_FILENO);
      close(redir.stdin_fd);
    }
    if (redir.stdout_fd >= 0) {
      dup2(redir.stdout_fd, STDOUT_FILENO);
      close(redir.stdout_fd);
    }
    if (redir.stderr_fd >= 0) {
      dup2(redir.stderr_fd, STDERR_FILENO);
      close(redir.stderr_fd);
    } else if (redir.redirect_stderr_to_stdout) {
      dup2(STDOUT_FILENO, STDERR_FILENO);
    }
    execvp(argv[0], argv);
    /* execvp failed - check if command not found */
    if (errno == ENOENT) {
      printf("Command not found\n");
      fflush(stdout);
    }
    _exit(CHILD_ERROR_CODE);
  }
  int status = 0;
  waitpid(pid, &status, 0);

  if (redir.stdin_fd >= 0)
    close(redir.stdin_fd);
  if (redir.stdout_fd >= 0)
    close(redir.stdout_fd);
  if (redir.stderr_fd >= 0)
    close(redir.stderr_fd);

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return EXIT_FAILURE_CODE;
}

int execute_single_command(char* command) {
  while (*command == ' ' || *command == '\t' || *command == '\n')
    command++;
  if (*command == '\0')
    return 0;

  int argc = 0;
  char** argv = parse_command_line(command, &argc);
  if (!argv || argc == 0) {
    free(argv);
    return 0;
  }

  /* Валидация синтаксиса редиректов:
   * 1. Редирект в конце без файла (например "wc -l <")
   * 2. Два редиректа подряд (например "< >hello")
   * 3. Два редиректа одного типа (например ">foo > bar" или "<one ... <bar")
   * 4. Токен >>filename (слитно) — синтаксическая ошибка
   */

  int stdout_redir_count = 0; /* Счётчик > и >> */
  int stdin_redir_count = 0;  /* Счётчик < */

  for (int i = 0; i < argc; ++i) {
    const char* t = argv[i];

    /* Токен >>filename (слитно без пробела) — синтаксическая ошибка */
    if (strncmp(t, ">>", 2) == 0 && strlen(t) > 2) {
      puts("Syntax error");
      for (int j = 0; j < argc; ++j) {
        if (argv[j] && strcmp(argv[j], ">") != 0 &&
            strcmp(argv[j], ">>") != 0 && strcmp(argv[j], "<") != 0 &&
            strcmp(argv[j], "|") != 0 && strcmp(argv[j], "&") != 0 &&
            strcmp(argv[j], "2>&1") != 0) {
          free(argv[j]);
        }
      }
      free(argv);
      return 0;
    }

    /* Редирект в конце без файла */
    if (is_redir(t) && i + 1 >= argc) {
      puts("Syntax error");
      for (int j = 0; j < argc; ++j) {
        if (argv[j] && strcmp(argv[j], ">") != 0 &&
            strcmp(argv[j], ">>") != 0 && strcmp(argv[j], "<") != 0 &&
            strcmp(argv[j], "|") != 0 && strcmp(argv[j], "&") != 0 &&
            strcmp(argv[j], "2>&1") != 0) {
          free(argv[j]);
        }
      }
      free(argv);
      return 0;
    }

    /* Два редиректа подряд */
    if (is_redir(t) && i + 1 < argc && is_redir(argv[i + 1])) {
      puts("Syntax error");
      for (int j = 0; j < argc; ++j) {
        if (argv[j] && strcmp(argv[j], ">") != 0 &&
            strcmp(argv[j], ">>") != 0 && strcmp(argv[j], "<") != 0 &&
            strcmp(argv[j], "|") != 0 && strcmp(argv[j], "&") != 0 &&
            strcmp(argv[j], "2>&1") != 0) {
          free(argv[j]);
        }
      }
      free(argv);
      return 0;
    }

    /* Подсчитываем редиректы */
    if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {
      stdout_redir_count++;
    }
    if (strcmp(t, "<") == 0) {
      stdin_redir_count++;
    }
  }

  /* Два редиректа stdout (">foo > bar") или два stdin ("<one ... <bar") */
  if (stdout_redir_count > 1 || stdin_redir_count > 1) {
    puts("Syntax error");
    for (int j = 0; j < argc; ++j) {
      if (argv[j] && strcmp(argv[j], ">") != 0 && strcmp(argv[j], ">>") != 0 &&
          strcmp(argv[j], "<") != 0 && strcmp(argv[j], "|") != 0 &&
          strcmp(argv[j], "&") != 0 && strcmp(argv[j], "2>&1") != 0) {
        free(argv[j]);
      }
    }
    free(argv);
    return 0;
  }

  char** expanded_argv = expand_env_vars(argv, argc);
  int status = execute_command(expanded_argv);

  if (expanded_argv != argv) {
    for (int i = 0; i < argc; ++i) {
      if (expanded_argv[i] != argv[i]) {
        free(expanded_argv[i]);
      }
    }
    free(expanded_argv);
  }

  free(argv);
  return status;
}

// Вспомогательная функция для проверки, является ли токен оператором редиректа
int is_redir(const char* t) {
  const char* redirs[] = {">", ">>", "<"};
  for (int i = 0; i < 3; ++i)
    if (strcmp(t, redirs[i]) == 0)
      return 1;
  return 0;
}

int main(void) {
  run_shell();
  return EXIT_SUCCESS_CODE;
}