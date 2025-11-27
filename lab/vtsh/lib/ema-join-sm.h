#ifndef EMA_JOIN_SM_H
#define EMA_JOIN_SM_H

#include <stdint.h>
#include <stddef.h>

#define MAX_KEY_LENGTH 8 

/* row_t — одна запись таблицы */
typedef struct {
    uint64_t id;  /* идентификатор */
    char key[MAX_KEY_LENGTH];  /* ключ (8 байт + NUL) */
} row_t;

/* out_row_t — запись результата объединения */
typedef struct {
    uint64_t id;
    char table_a[MAX_KEY_LENGTH];
    char table_b[MAX_KEY_LENGTH];
} out_row_t;

/* cmp_row(a, b)
 * Функция сравнения двух row_t по id для qsort.
 */
int cmp_row(const void* first_ptr, const void* second_ptr);

/* read_table(path, out_n)
 * Читает таблицу из текстового файла.
 * Возвращает массив row_t и количество записей через out_n.
 */
row_t* read_table(const char* path, size_t* out_n);

#endif