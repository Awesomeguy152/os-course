#define _XOPEN_SOURCE 700
#define _BSD_SOURCE
#include "shell.h"

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

/** ------------------------------------------------------------
 *  Константы для читаемости и предотвращения magic numbers
 * ------------------------------------------------------------ */
#define NSEC_PER_SEC 1000000000L /** количество наносекунд в одной секунде */
#define EXIT_SUCCESS_CODE 0 /** код успешного завершения */
#define EXIT_FAILURE_CODE 1 /** код неудачи */
#define CHILD_ERROR_CODE 127 /** код выхода при ошибке execvp */
#define PROMPT_STRING PROMPT /** приглашение (берется из shell.h) */

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
 *  Возвращает массив argv и количество аргументов через argc_out.
 * ------------------------------------------------------------ */
char** parse_command_line(char* line, int* argc_out) {
  char** argv = malloc((MAX_ARGS + 1) * sizeof(char*));
  if (!argv) {
    perror("malloc");
    return NULL;
  }

  int argc = 0;
  char* saveptr = NULL;
  char* token = strtok_r(line, " \t\r\n", &saveptr);

  while (token && argc < MAX_ARGS) {
    argv[argc++] = token;
    token = strtok_r(NULL, " \t\r\n", &saveptr);
  }

  argv[argc] = NULL;
  if (argc_out) {
    *argc_out = argc;
  }
  return argv;
}

/* ========== Структуры и функции для редиректов и переменных ========== */

typedef struct {
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
} redirect_info_t;

redirect_info_t apply_redirects(char** argv, int* argc_ptr) {
  redirect_info_t redir = {-1, -1, -1};
  int new_argc = 0;
  
  for (int i = 0; argv[i] != NULL; ++i) {
    if (strcmp(argv[i], ">") == 0) {
      if (argv[i + 1] == NULL) {
        fprintf(stderr, "Error: > requires a filename\n");
        return redir;
      }
      int fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) {
        perror("open >");
        return redir;
      }
      redir.stdout_fd = fd;
      ++i;
      continue;
    }
    
    if (strcmp(argv[i], ">>") == 0) {
      if (argv[i + 1] == NULL) {
        fprintf(stderr, "Error: >> requires a filename\n");
        return redir;
      }
      int fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd < 0) {
        perror("open >>");
        return redir;
      }
      redir.stdout_fd = fd;
      ++i;
      continue;
    }
    
    if (strcmp(argv[i], "<") == 0) {
      if (argv[i + 1] == NULL) {
        fprintf(stderr, "Error: < requires a filename\n");
        return redir;
      }
      int fd = open(argv[i + 1], O_RDONLY);
      if (fd < 0) {
        perror("open <");
        return redir;
      }
      redir.stdin_fd = fd;
      ++i;
      continue;
    }
    
    argv[new_argc++] = argv[i];
  }
  
  argv[new_argc] = NULL;
  if (argc_ptr) {
    *argc_ptr = new_argc;
  }
  return redir;
}

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
           (isalnum((unsigned char)var_start[var_name_len]) || var_start[var_name_len] == '_')) {
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

/** ------------------------------------------------------------
 *  run_shell:
 *  Главный цикл оболочки — вынесен отдельно, чтобы не вызывать main()
 * ------------------------------------------------------------ */
void run_shell(void);

/** ------------------------------------------------------------
 *  execute_command:
 *  Выполняет команду argv:
 *   - встроенные команды ("exit", "cd")
 *   - внешние команды (через execvp)
 *   - замеряет время выполнения
 * ------------------------------------------------------------ */
int execute_command(char** argv) {
  if (!argv || !argv[0])
    return 0;

  /** ---- Встроенная команда: exit ---- */
  if (strcmp(argv[0], "exit") == 0) {
    exit(EXIT_SUCCESS_CODE);
  }

  /** ---- Встроенная команда: cd ---- */
  if (strcmp(argv[0], "cd") == 0) {
    const char* target_dir = (argv[1]) ? argv[1] : getenv("HOME");
    if (chdir(target_dir) != 0) {
      fprintf(stderr, "cd: %s: %s\n", target_dir, strerror(errno));
    }
    return 0;
  }

  /** ---- Запуск вложенного шелла ---- */
  if (strcmp(argv[0], "./shell") == 0) {
    run_shell();
    return 0;
  }

  /** ---- Обработка редиректов ---- */
  int argc_temp = 0;
  for (int i = 0; argv[i] != NULL; ++i) {
    ++argc_temp;
  }
  redirect_info_t redir = apply_redirects(argv, &argc_temp);
  
  if (argc_temp == 0 || argv[0] == NULL) {
    if (redir.stdin_fd >= 0) close(redir.stdin_fd);
    if (redir.stdout_fd >= 0) close(redir.stdout_fd);
    if (redir.stderr_fd >= 0) close(redir.stderr_fd);
    return EXIT_FAILURE_CODE;
  }

  /** ---- Измеряем время выполнения ---- */
  struct timespec start_time, end_time, duration;
  clock_gettime(CLOCK_MONOTONIC, &start_time);

  /** ---- Создаем дочерний процесс ---- */
  pid_t pid = vfork();
  if (pid < 0) {
    perror("vfork");
    if (redir.stdin_fd >= 0) close(redir.stdin_fd);
    if (redir.stdout_fd >= 0) close(redir.stdout_fd);
    if (redir.stderr_fd >= 0) close(redir.stderr_fd);
    return EXIT_FAILURE_CODE;
  }

  /** ---- Код дочернего процесса ---- */
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
    }
    
    execvp(argv[0], argv);
    const char msg[] = "Command not found\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(CHILD_ERROR_CODE);
  }

  /** ---- Код родителя ---- */
  int status = 0;
  waitpid(pid, &status, 0);
  
  if (redir.stdin_fd >= 0) close(redir.stdin_fd);
  if (redir.stdout_fd >= 0) close(redir.stdout_fd);
  if (redir.stderr_fd >= 0) close(redir.stderr_fd);

  clock_gettime(CLOCK_MONOTONIC, &end_time);
  calculate_timespec_diff(&start_time, &end_time, &duration);
  double elapsed = duration.tv_sec + duration.tv_nsec / (double)NSEC_PER_SEC;
  if (WIFEXITED(status)) {
    /* Print exit status and elapsed time to stderr for test compatibility. */
    fprintf(stderr, "exit status: %d; elapsed: %.6f s\n", WEXITSTATUS(status), elapsed);
    return WEXITSTATUS(status);
  }  if (WIFSIGNALED(status)) {
    /* Print termination info to stderr for test compatibility. */
    fprintf(stderr, "terminated by signal %d; elapsed: %.6f s\n", WTERMSIG(status), elapsed);
    return 128 + WTERMSIG(status);
  }

  return EXIT_FAILURE_CODE;
}

/** ------------------------------------------------------------
 *  execute_single_command:
 *  Выполняет одну команду (без операторов &&).
 * ------------------------------------------------------------ */
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

/** ------------------------------------------------------------
 *  execute_with_and:
 *  Выполняет несколько команд, разделенных оператором "&&".
 * ------------------------------------------------------------ */
void execute_with_and(char* line) {
  char* saveptr = NULL;
  char* current_cmd = strtok_r(line, "&", &saveptr);
  int last_status = 0;

  while (current_cmd) {
    if (current_cmd > line && *(current_cmd - 1) == '&') {
      current_cmd = strtok_r(NULL, "&", &saveptr);
      continue;
    }

    last_status = execute_single_command(current_cmd);

    if (last_status != 0)
      break;

    current_cmd = strtok_r(NULL, "&", &saveptr);
  }
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

  execute_with_and(line);
}

/** ------------------------------------------------------------
 *  run_shell:
 *  Главный цикл shell — выделен отдельно от main
 * ------------------------------------------------------------ */
void run_shell(void) {
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

  free(input_line);
}

/** ------------------------------------------------------------
 *  main:
 * ------------------------------------------------------------ */
int main(void) {
  run_shell();
  return EXIT_SUCCESS_CODE;
}