#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "io-loader.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000L

/* If the platform doesn't define O_DIRECT (e.g. macOS), provide a
  safe no-op definition so code that ORs flags with O_DIRECT still
  compiles. Prefer platform-specific fallbacks (F_NOCACHE on macOS)
  to actually disable caching when needed. */
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

/* ------------------------------ КОНСТАНТЫ ------------------------------ */
const int FILE_MODE_PERMISSIONS = 0666; /* права при создании файла */
const int MIN_ARG_COUNT = 9; /* минимальное количество аргументов */

/* ------------------------------ ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
 * ------------------------------ */

/* usage() — печатает, как правильно запускать программу */
void usage(const char* program_name) {
  fprintf(
      stderr,
      "Usage: %s --rw read|write --block_size <n> --block_count <n> --file "
      "<path>\n"
      "       [--range A-B] [--direct on|off] [--type sequence|random] "
      "[--repetitions N]\n",
      program_name
  );
}

/* parse_range() — парсит диапазон формата 'A-B' и сохраняет числа в *start_val
 * и *end_val. Возвращает 0 при успехе, -1 при ошибке.
 */
int parse_range(const char* range_str, off_t* start_val, off_t* end_val) {
  if (!range_str) {
    return -1;
  }

  char* dash_position = strchr(range_str, '-');
  if (!dash_position) {
    return -1;
  }

  /* Разделяем строку на две части */
  *dash_position = '\0';
  char* endptr = NULL;

  long long start_long = strtoll(range_str, &endptr, 10);
  if (*endptr != '\0' || start_long < 0) {
    *dash_position = '-';
    return -1;
  }

  long long end_long = strtoll(dash_position + 1, &endptr, 10);
  if (*endptr != '\0' || end_long < 0) {
    *dash_position = '-';
    return -1;
  }

  *start_val = (off_t)start_long;
  *end_val = (off_t)end_long;

  *dash_position = '-';
  return 0;
}

/* ------------------------------ MAIN ------------------------------ */

int main(int argc, char** argv) {
  /** Проверяем, есть ли вообще аргументы **/
  if (argc < MIN_ARG_COUNT) {
    usage(argv[0]);
    return 1;
  }

  /* -------------------- Переменные конфигурации -------------------- */
  const char* rw_mode_str = NULL; /** режим: read или write **/
  size_t block_size_bytes = 0; /** размер одного блока в байтах **/
  size_t block_count_total = 0; /** количество блоков для чтения/записи **/
  const char* file_path_str = NULL; /** путь к файлу **/
  off_t range_start_val = 0;        /** начало диапазона **/
  off_t range_end_val = 0;          /** конец диапазона **/
  int is_range_given_flag = 0;      /** 1 если задан --range **/
  int direct_io_flag = 0;           /** флаг O_DIRECT (0=off, 1=on) **/
  int is_sequence_access =
      1; /** тип доступа: 1=последовательный, 0=случайный **/
  int repetitions_total = 1; /** сколько раз повторить проход **/

  /* -------------------- Парсинг аргументов командной строки
   * -------------------- */
  int arg_index = 0;
  for (arg_index = 1; arg_index < argc; ++arg_index) {
    if (strcmp(argv[arg_index], "--rw") == 0 && arg_index + 1 < argc) {
      rw_mode_str = argv[++arg_index];
      continue;
    }

    if (strcmp(argv[arg_index], "--block_size") == 0 && arg_index + 1 < argc) {
      char* endptr = NULL;
      long long temp_val = strtoll(argv[++arg_index], &endptr, 10);
      if (*endptr != '\0' || temp_val <= 0) {
        fprintf(stderr, "Invalid --block_size value\n");
        return 1;
      }
      block_size_bytes = (size_t)temp_val;
      continue;
    }

    if (strcmp(argv[arg_index], "--block_count") == 0 && arg_index + 1 < argc) {
      char* endptr = NULL;
      long long temp_val = strtoll(argv[++arg_index], &endptr, 10);
      if (*endptr != '\0' || temp_val <= 0) {
        fprintf(stderr, "Invalid --block_count value\n");
        return 1;
      }
      block_count_total = (size_t)temp_val;
      continue;
    }

    if (strcmp(argv[arg_index], "--file") == 0 && arg_index + 1 < argc) {
      file_path_str = argv[++arg_index];
      continue;
    }
        if (strcmp(argv[arg_index], "--range") == 0 && arg_index + 1 < argc) {
      if (parse_range(argv[++arg_index], &range_start_val, &range_end_val) ==
          0) {
        is_range_given_flag = 1;
      }
      continue;
    }

    if (strcmp(argv[arg_index], "--direct") == 0 && arg_index + 1 < argc) {
      direct_io_flag = (strcmp(argv[++arg_index], "on") == 0) ? 1 : 0;
      continue;
    }

    if (strcmp(argv[arg_index], "--type") == 0 && arg_index + 1 < argc) {
      is_sequence_access = (strcmp(argv[++arg_index], "sequence") == 0) ? 1 : 0;
      continue;
    }

    if (strcmp(argv[arg_index], "--repetitions") == 0 && arg_index + 1 < argc) {
      char* endptr = NULL;
      long long temp_val = strtoll(argv[++arg_index], &endptr, 10);
      if (*endptr != '\0' || temp_val <= 0) {
        fprintf(stderr, "Invalid --repetitions value\n");
        return 1;
      }
      repetitions_total = (int)temp_val;
      continue;
    }

    /** Если аргумент непонятный — сообщаем и выходим **/
    fprintf(stderr, "Unknown or malformed arg: %s\n", argv[arg_index]);
    usage(argv[0]);
    return 1;
  }

  /** Проверяем обязательные параметры **/
  if (!rw_mode_str || !block_size_bytes || !block_count_total ||
      !file_path_str) {
    usage(argv[0]);
    return 1;
  }

  /** Определяем, писать или читать **/
  int do_write_flag = (strcmp(rw_mode_str, "write") == 0) ? 1 : 0;

  /* -------------------- Открытие файла -------------------- */
  int open_flags = do_write_flag ? (O_CREAT | O_RDWR) : O_RDONLY;
#ifdef O_DIRECT
  if (direct_io_flag) {
    open_flags |= O_DIRECT;
  }
#else
  /* O_DIRECT not defined on this platform; don't add a flag here. We'll
     attempt a platform-specific fallback (e.g. F_NOCACHE on macOS) after
     opening the file. */
#endif

  int fd_file = open(file_path_str, open_flags, FILE_MODE_PERMISSIONS);
  if (fd_file < 0) {
    perror("open");
    return 1;
  }

#ifndef O_DIRECT
  if (direct_io_flag) {
#if defined(__APPLE__)
    /* On macOS, request no-cache via fcntl(F_NOCACHE). It's a best-effort
       fallback to approximate direct I/O semantics. */
    if (fcntl(fd_file, F_NOCACHE, 1) == -1) {
      perror("fcntl(F_NOCACHE)");
      /* Non-fatal: continue without direct I/O */
    }
#else
    fprintf(stderr,
            "Warning: direct I/O requested but O_DIRECT is not available on this platform; continuing without direct I/O\n");
#endif
  }
#endif

  /** Получаем информацию о файле **/
  struct stat file_stat_struct;
  if (fstat(fd_file, &file_stat_struct) != 0) {
    perror("fstat");
    close(fd_file);
    return 1;
  }
  off_t file_total_size = file_stat_struct.st_size;

  /* -------------------- Настройка диапазона -------------------- */
  off_t io_range_start = 0;
  off_t io_range_end = file_total_size;

  if (is_range_given_flag) {
    if (range_start_val == 0 && range_end_val == 0) {
      io_range_start = 0;
      io_range_end = file_total_size;
    } else {
      io_range_start = range_start_val;
      io_range_end = range_end_val;
      if (io_range_end <= io_range_start) {
        fprintf(stderr, "Bad range\n");
        close(fd_file);
        return 1;
      }
    }
  }

  /* -------------------- Подготовка файла для записи -------------------- */
  if (do_write_flag) {
    off_t needed_size =
        io_range_start + (off_t)block_size_bytes * (off_t)block_count_total;
    if (needed_size > file_total_size) {
      if (ftruncate(fd_file, needed_size) != 0) {
        perror("ftruncate");
        close(fd_file);
        return 1;
      }
      file_total_size = needed_size;
      if (io_range_end < needed_size) {
        io_range_end = needed_size;
      }
    }
  }

  /* -------------------- Выделение буфера -------------------- */
  size_t memory_alignment_bytes =
      (size_t)sysconf(_SC_PAGESIZE); /** системный размер страницы **/
  void* buffer_ptr = NULL;

  /** posix_memalign() выделяет память, выровненную по alignment **/
  if (posix_memalign(&buffer_ptr, memory_alignment_bytes, block_size_bytes) !=
      0) {
    perror("posix_memalign");
    close(fd_file);
    return 1;
  }

  /** Если пишем — заполняем буфер последовательными байтами **/
  if (do_write_flag) {
    size_t byte_index = 0;
    for (byte_index = 0; byte_index < block_size_bytes; ++byte_index) {
      ((unsigned char*)buffer_ptr)[byte_index] =
          (unsigned char)(byte_index & 0xFF);
    }
  }

  /* -------------------- Подготовка цикла IO -------------------- */
  srand((unsigned)time(NULL)); /** инициализация генератора случайных чисел **/

  off_t blocks_in_region =
      (io_range_end - io_range_start) / (off_t)block_size_bytes;
  if (blocks_in_region <= 0) {
    fprintf(stderr, "Range too small for block_size\n");
    free(buffer_ptr);
    close(fd_file);
    return 1;
  }
    /* -------------------- Основной цикл IO -------------------- */
  int repetition_index = 0;
  size_t block_index_iter = 0;
  for (repetition_index = 0; repetition_index < repetitions_total;
       ++repetition_index) {
    struct timespec _start_iter, _end_iter, _diff_iter;
    clock_gettime(CLOCK_MONOTONIC, &_start_iter);
    printf("IO: Iteration %d: ", repetition_index + 1);
    for (block_index_iter = 0; block_index_iter < block_count_total;
         ++block_index_iter) {
      /** Определяем, какой блок читать/писать **/
      off_t current_block_index =
          is_sequence_access ? (off_t)block_index_iter % blocks_in_region
                             : rand() % blocks_in_region;

      /** Вычисляем смещение в байтах **/
      off_t current_offset_bytes =
          io_range_start + current_block_index * (off_t)block_size_bytes;

      /* -------------------- Операция IO -------------------- */
      if (do_write_flag) {
        ssize_t written_bytes =
            pwrite(fd_file, buffer_ptr, block_size_bytes, current_offset_bytes);
        if (written_bytes != (ssize_t)block_size_bytes) {
          if (written_bytes < 0) {
            perror("pwrite");
          } else {
            fprintf(stderr, "Short write %zd\n", written_bytes);
          }
        }
      } else {
        ssize_t read_bytes =
            pread(fd_file, buffer_ptr, block_size_bytes, current_offset_bytes);
        if (read_bytes != (ssize_t)block_size_bytes) {
          if (read_bytes < 0) {
            perror("pread");
          } else {
            fprintf(stderr, "Short read %zd\n", read_bytes);
          }
        }
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &_end_iter);
    _diff_iter.tv_sec = _end_iter.tv_sec - _start_iter.tv_sec;
    _diff_iter.tv_nsec = _end_iter.tv_nsec - _start_iter.tv_nsec;
    if (_diff_iter.tv_nsec < 0) {
      _diff_iter.tv_sec -= 1;
      _diff_iter.tv_nsec += NSEC_PER_SEC;
    }
    double elapsed_iter = _diff_iter.tv_sec + _diff_iter.tv_nsec / (double)NSEC_PER_SEC;
    fprintf(stderr, "elapsed: %.6f s\n", elapsed_iter);
  }

  /* -------------------- Завершение -------------------- */
  free(buffer_ptr);
  close(fd_file);

  printf(
      "IO loader completed (rw=%s, block_size=%zu, block_count=%zu, "
      "repetitions=%d)\n",
      rw_mode_str,
      block_size_bytes,
      block_count_total,
      repetitions_total
  );

  return 0;
}