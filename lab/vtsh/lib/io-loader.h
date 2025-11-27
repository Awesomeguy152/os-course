#ifndef IO_LOADER_H
#define IO_LOADER_H

#include <stddef.h>

/* io_task_t — структура для передачи данных IO потоку */
typedef struct {
    const char* file;      /* путь к файлу */
    size_t block_size;     /* размер блока в байтах */
    size_t block_count;    /* количество блоков для чтения/записи */
    int do_write;          /* 1 = писать, 0 = читать */
    int repetitions;       /* количество повторов */
} io_task_t;

/* io_worker(void* arg)
 * Потоковая функция для нагрузки ввода/вывода.
 * Принимает указатель на io_task_t.
 */
void* io_worker(void* arg);

#endif