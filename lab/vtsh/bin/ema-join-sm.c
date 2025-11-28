/* ema_join_sm.c
 *
 * Использование:
 *   ./ema_join_sm left.txt right.txt out.txt
 *
 * Формат входных файлов:
 *   первая строка — количество записей
 *   далее каждая строка: <id> <key>
 *   key не длиннее 8 символов
 */

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ema-join-sm.h"
#include <time.h>

#define NSEC_PER_SEC 1000000000L

/* =======================================================================
 *                          КОНСТАНТЫ
 * ======================================================================= */
#define MAX_KEY_LENGTH 8 /** максимальная длина ключа из файла */
#define MAX_BUFFER_SIZE 64 /** размер буфера для чтения строк файла */

/* =======================================================================
 *                        ФУНКЦИИ ДЛЯ СОРТИРОВКИ
 * ======================================================================= */

/* cmp_row — сравнение двух записей по id для qsort */
int cmp_row(const void* first_ptr, const void* second_ptr) {
  const row_t* left_row_ptr = (const row_t*)first_ptr;
  const row_t* right_row_ptr = (const row_t*)second_ptr;

  if (left_row_ptr->id < right_row_ptr->id) {
    return -1;
  }
  if (left_row_ptr->id > right_row_ptr->id) {
    return 1;
  }
  return 0;
}

/* =======================================================================
 *                          ЧТЕНИЕ ТАБЛИЦЫ
 * ======================================================================= */

/* read_table(path, out_number_of_rows)
 * Читает таблицу из текстового файла.
 * Возвращает массив row_t, количество записей через out_number_of_rows.
 */
row_t* read_table(const char* file_path, size_t* out_number_of_rows) {
  /** открываем файл для чтения */
  FILE* file_handle = fopen(file_path, "r");
  if (!file_handle) {
    perror("fopen");
    return NULL;
  }

  /** читаем число строк из первой строки */
  size_t number_of_rows;
  if (fscanf(file_handle, "%zu", &number_of_rows) != 1) {
    fprintf(stderr, "Bad header in %s\n", file_path);
    fclose(file_handle);
    return NULL;
  }

  /** выделяем память под строки */
  row_t* table_rows = malloc(sizeof(row_t) * number_of_rows);
  if (!table_rows) {
    perror("malloc");
    fclose(file_handle);
    return NULL;
  }

  /** читаем каждую запись */
  size_t row_index = 0;
  for (row_index = 0; row_index < number_of_rows; ++row_index) {
    uint64_t current_id;
    char buffer[MAX_BUFFER_SIZE];

    if (fscanf(file_handle, "%" SCNu64 " %63s", &current_id, buffer) != 2) {
      fprintf(stderr, "Bad line %zu in %s\n", row_index + 1, file_path);
      free(table_rows);
      fclose(file_handle);
      return NULL;
    }

    table_rows[row_index].id = current_id;
    memset(table_rows[row_index].key, 0, sizeof(table_rows[row_index].key));
    strncpy(
        table_rows[row_index].key, buffer, MAX_KEY_LENGTH
    ); /** копируем первые MAX_KEY_LENGTH символов */
  }

  fclose(file_handle);
  *out_number_of_rows = number_of_rows;
  return table_rows;
}

/* =======================================================================
 *                                MAIN
 * ======================================================================= */

int main(int argc, char** argv) {
  /** проверка аргументов командной строки */
  if (argc < 4) {
    fprintf(stderr, "Usage: %s left_file right_file out_file\n", argv[0]);
    return 2;
  }

  const char* left_file_path = argv[1];
  const char* right_file_path = argv[2];
  const char* output_file_path = argv[3];

  long num_rep_long = 1;
  if (argc > 4) {
    char* str_end2 = NULL;
    num_rep_long = strtol(argv[4], &str_end2, 10);
  }

  int num_repetitions =
      (num_rep_long > INT32_MAX) ? INT32_MAX : (int)num_rep_long;

  int idx_iter = 0;
  for (idx_iter = 0; idx_iter < num_repetitions; idx_iter++) {
    struct timespec _start_iter, _end_iter, _diff_iter;
    clock_gettime(CLOCK_MONOTONIC, &_start_iter);
    printf("EMA: Iteration %d: ", idx_iter + 1);

    size_t left_table_size = 0;
    size_t right_table_size = 0;
        /** читаем левую таблицу */
    row_t* left_table_rows = read_table(left_file_path, &left_table_size);
    if (!left_table_rows) {
      return 1;
    }

    /** читаем правую таблицу */
    row_t* right_table_rows = read_table(right_file_path, &right_table_size);
    if (!right_table_rows) {
      free(left_table_rows);
      return 1;
    }

    /** сортируем таблицы по id для merge join */
    qsort(left_table_rows, left_table_size, sizeof(row_t), cmp_row);
    qsort(right_table_rows, right_table_size, sizeof(row_t), cmp_row);

    /** открываем файл для записи результата */
    FILE* output_file_handle = fopen(output_file_path, "w");
    if (!output_file_handle) {
      perror("fopen out");
      free(left_table_rows);
      free(right_table_rows);
      return 1;
    }

    /* ===================================================================
     *                     MERGE JOIN
     * =================================================================== */

    size_t left_table_index = 0;
    size_t right_table_index = 0;
    size_t result_rows_count = 0;

    /** выделяем память для результата (максимальный размер = left_table_size +
     * right_table_size) */
    out_row_t* result_rows_array =
        malloc(sizeof(out_row_t) * (left_table_size + right_table_size));
    if (!result_rows_array) {
      perror("malloc result_array");
      fclose(output_file_handle);
      free(left_table_rows);
      free(right_table_rows);
      return 1;
    }

    /** основной цикл merge join */
    while (left_table_index < left_table_size &&
           right_table_index < right_table_size) {
      if (left_table_rows[left_table_index].id <
          right_table_rows[right_table_index].id) {
        left_table_index++;
        continue;
      }

      if (left_table_rows[left_table_index].id >
          right_table_rows[right_table_index].id) {
        right_table_index++;
        continue;
      }

      /** найдено совпадение id */
      uint64_t current_matching_id = left_table_rows[left_table_index].id;

      /** находим диапазон одинаковых id в левой таблице */
      size_t left_range_start_index = left_table_index;
      while (left_table_index < left_table_size &&
             left_table_rows[left_table_index].id == current_matching_id) {
        left_table_index++;
      }
      size_t left_range_end_index =
          left_table_index; /** [left_range_start_index, left_range_end_index)
                             */

      /** находим диапазон одинаковых id в правой таблице */
      size_t right_range_start_index = right_table_index;
      while (right_table_index < right_table_size &&
             right_table_rows[right_table_index].id == current_matching_id) {
        right_table_index++;
      }
      size_t right_range_end_index =
          right_table_index; /** [right_range_start_index,
                              * right_range_end_index)
                              */

      /** создаём декартово произведение для всех совпадений */
      size_t left_inner_index = 0;
      size_t right_inner_index = 0;
      for (left_inner_index = left_range_start_index;
           left_inner_index < left_range_end_index;
           ++left_inner_index) {
        for (right_inner_index = right_range_start_index;
             right_inner_index < right_range_end_index;
             ++right_inner_index) {
          result_rows_array[result_rows_count].id = current_matching_id;
          strncpy(
              result_rows_array[result_rows_count].table_a,
              left_table_rows[left_inner_index].key,
              MAX_KEY_LENGTH
          );
          strncpy(
              result_rows_array[result_rows_count].table_b,
              right_table_rows[right_inner_index].key,
              MAX_KEY_LENGTH
          );
          result_rows_count++;
        }
      }
    }

    /* ===================================================================
     *                     ЗАПИСЬ РЕЗУЛЬТАТА
     * =================================================================== */
         /** пишем заголовок (количество строк) */
    fprintf(output_file_handle, "%zu\n", result_rows_count);

    /** пишем каждую запись */
    size_t result_index = 0;
    for (result_index = 0; result_index < result_rows_count; ++result_index) {
      fprintf(
          output_file_handle,
          "%" PRIu64 " %8s %8s\n",
          result_rows_array[result_index].id,
          result_rows_array[result_index].table_a,
          result_rows_array[result_index].table_b
      );
    }

    printf("Join produced %zu rows\n", result_rows_count);

    clock_gettime(CLOCK_MONOTONIC, &_end_iter);
    _diff_iter.tv_sec = _end_iter.tv_sec - _start_iter.tv_sec;
    _diff_iter.tv_nsec = _end_iter.tv_nsec - _start_iter.tv_nsec;
    if (_diff_iter.tv_nsec < 0) {
      _diff_iter.tv_sec -= 1;
      _diff_iter.tv_nsec += NSEC_PER_SEC;
    }
    double elapsed_iter = _diff_iter.tv_sec + _diff_iter.tv_nsec / (double)NSEC_PER_SEC;
    printf("elapsed: %.6f s\n", elapsed_iter);

    /** освобождаем ресурсы */
    fclose(output_file_handle);
    free(left_table_rows);
    free(right_table_rows);
    free(result_rows_array);
  }
  return 0;
}