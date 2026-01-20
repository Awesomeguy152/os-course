#define _POSIX_C_SOURCE 200809L  // активируем современные POSIX-функции (getline, clock_gettime и т.д.) до подключения системных заголовков

#include <inttypes.h>  // предоставляет макросы формата (SCNu64, PRIu64) для безопасной работы с uint64_t в IO
#include <stdint.h>  // даёт точные целочисленные типы фиксированной ширины, такие как uint64_t
#include <stdio.h>  // стандартный ввод/вывод: fopen, fscanf, fprintf, printf
#include <stdlib.h>  // функции управления памятью и преобразования строк в числа (malloc, free, strtol)
#include <string.h>  // строковые утилиты, включая memset и strncpy для работы с ключами
#include <time.h>  // clock_gettime и структура timespec для измерения времени выполнения

#include "ema-join-sm.h"  // заголовок библиотеки с определениями row_t и out_row_t

#define NSEC_PER_SEC 1000000000L  // число наносекунд в секунде, применяемое при нормализации временных интервалов

/* --------------------------------------------------------------------------
 * Общие сведения
 * --------------------------------------------------------------------------
 * Файл реализует упрощённое соединение двух таблиц (аналог merge-join) по
 * идентификатору записи. Каждая таблица хранится в текстовом файле: первая
 * строка содержит количество строк, далее следуют пары <id> <key>. Программа
 * читает обе таблицы, сортирует их по id, а затем формирует результирующий
 * набор, составляя декартово произведение совпадающих ключей.
 */

/* --------------------------------------------------------------------------
 * Константы конфигурации
 * -------------------------------------------------------------------------- */
#define MAX_KEY_LENGTH 8  // максимальная длина ключа, считываемого из файла; лишние символы отбрасываются
#define MAX_BUFFER_SIZE 64  // размер временного буфера для хранения считанной строки (с запасом относительно MAX_KEY_LENGTH)

/* --------------------------------------------------------------------------
 * Служебные функции: сравнение строк таблицы
 * -------------------------------------------------------------------------- */
/*
 * cmp_row — функция-компаратор для qsort. Она сравнивает две строки таблицы по
 * идентификатору id и возвращает отрицательное значение, если первая строка
 * меньше второй, положительное, если больше, и ноль при равенстве. Такая
 * функция необходима, чтобы qsort мог упорядочить таблицы подготовительно для
 * merge join.
 */
int cmp_row(const void* first_ptr, const void* second_ptr) {  // принимает указатели на элементы массива row_t, представленные как void*
  const row_t* left_row_ptr = (const row_t*)first_ptr;  // приводим первый указатель к типу row_t, чтобы получить доступ к полям структуры
  const row_t* right_row_ptr = (const row_t*)second_ptr;  // аналогично приводим второй указатель

  if (left_row_ptr->id < right_row_ptr->id) {  // если id первой строки меньше, возвращаем отрицательное значение
    return -1;  // сигнализируем qsort о том, что первый элемент должен располагаться раньше
  }
  if (left_row_ptr->id > right_row_ptr->id) {  // если id первой строки больше, возвращаем положительное значение
    return 1;  // qsort разместит второй элемент раньше первого
  }
  return 0;  // в остальных случаях id равны и порядок не важен
}

/* --------------------------------------------------------------------------
 * Чтение таблицы из текстового файла
 * -------------------------------------------------------------------------- */
/*
 * read_table(file_path, out_number_of_rows)
 * ---------------------------------------
 * Открывает текстовый файл, читает из него количество строк и сами строки,
 * после чего формирует массив структур row_t. Количество прочитанных записей
 * возвращается через указатель out_number_of_rows. В случае ошибки функция
 * печатает диагностическое сообщение и возвращает NULL.
 */
row_t* read_table(const char* file_path, size_t* out_number_of_rows) {  // принимает путь к файлу и указатель на переменную, куда будет записано количество прочитанных строк
  FILE* file_handle = fopen(file_path, "r");  // открываем файл в текстовом режиме для чтения
  if (!file_handle) {  // если открыть файл не удалось (например, отсутствует или нет прав)
    perror("fopen");  // выводим ошибку с описанием от системы
    return NULL;  // сигнализируем вызывающему коду о сбое
  }

  size_t number_of_rows;  // переменная для хранения количества записей, указанного в заголовке файла
  if (fscanf(file_handle, "%zu", &number_of_rows) != 1) {  // считываем первую строку и проверяем, что получили одно целое число
    fprintf(stderr, "Bad header in %s\n", file_path);  // информируем пользователя о некорректном формате заголовка
    fclose(file_handle);  // закрываем файл перед возвратом
    return NULL;  // возвращаем NULL, так как данные считать не удалось
  }

  row_t* table_rows = malloc(sizeof(row_t) * number_of_rows);  // выделяем память под массив строк таблицы
  if (!table_rows) {  // проверяем, успешно ли выполнено выделение памяти
    perror("malloc");  // сообщаем об ошибке выделения памяти
    fclose(file_handle);  // закрываем файл, чтобы не терять дескриптор
    return NULL;  // возвращаем NULL, поскольку без памяти продолжить нельзя
  }

  size_t row_index = 0;  // индекс текущей строки, считываемой из файла
  for (row_index = 0; row_index < number_of_rows; ++row_index) {  // повторяем чтение для каждой строки
    uint64_t current_id;  // переменная для хранения считанного идентификатора
    char buffer[MAX_BUFFER_SIZE];  // временный буфер для хранения ключа в текстовом виде

    if (fscanf(file_handle, "%" SCNu64 " %63s", &current_id, buffer) != 2) {  // пытаемся прочитать пару "id key" и проверяем, что оба значения присутствуют
      fprintf(stderr, "Bad line %zu in %s\n", row_index + 1, file_path);  // выводим сообщение об ошибке, указывая номер строки
      free(table_rows);  // освобождаем уже выделенную память, чтобы не допустить утечки
      fclose(file_handle);  // закрываем файл, поскольку дальнейшее чтение невозможно
      return NULL;  // прекращаем работу функции с признаком ошибки
    }

    table_rows[row_index].id = current_id;  // сохраняем идентификатор строки в результирующую структуру
    memset(table_rows[row_index].key, 0, sizeof(table_rows[row_index].key));  // обнуляем массив символов, чтобы удалить прошлое содержимое и гарантировать завершающий ноль
    strncpy(table_rows[row_index].key, buffer, MAX_KEY_LENGTH);  // копируем не более MAX_KEY_LENGTH символов ключа, оставляя лишние символы вне диапазона
  }

  fclose(file_handle);  // закрываем файл после успешного чтения всех строк
  *out_number_of_rows = number_of_rows;  // передаём количество строк через выходной параметр
  return table_rows;  // возвращаем указатель на заполненный массив строк
}

/* --------------------------------------------------------------------------
 * Точка входа программы
 * -------------------------------------------------------------------------- */
int main(int argc, char** argv) {  // точка входа: управляет чтением таблиц, соединением и измерением времени
  if (argc < 4) {  // проверяем, переданы ли обязательные аргументы командной строки
    fprintf(stderr, "Usage: %s left_file right_file out_file\n", argv[0]);  // выводим подсказку о корректном синтаксисе
    return 2;  // возвращаем код ошибки, подчёркивая, что аргументов недостаточно
  }

  const char* left_file_path = argv[1];  // путь к файлу с левой таблицей
  const char* right_file_path = argv[2];  // путь к файлу с правой таблицей
  const char* output_file_path = argv[3];  // путь к файлу, в который будет записан результат соединения

  long num_rep_long = 1;  // по умолчанию выполняем ровно одно повторение merge join
  if (argc > 4) {  // если передан дополнительный аргумент, интерпретируем его как количество повторений
    char* str_end2 = NULL;  // указатель на позицию, где strtol остановится; позволяет отлавливать некорректные символы
    num_rep_long = strtol(argv[4], &str_end2, 10);  // преобразуем строку в целое число, работая в десятичной системе
  }

  int num_repetitions = (num_rep_long > INT32_MAX) ? INT32_MAX : (int)num_rep_long;  // ограничиваем количество повторов значением INT32_MAX, чтобы защититься от переполнения

  int idx_iter = 0;  // счётчик выполненных повторов merge join
  for (idx_iter = 0; idx_iter < num_repetitions; idx_iter++) {  // запускаем серию повторений для накопления статистики времени
    struct timespec _start_iter, _end_iter, _diff_iter;  // структуры для измерения времени начала, конца и длительности итерации
    clock_gettime(CLOCK_MONOTONIC, &_start_iter);  // фиксируем момент старта, используя монотонные часы
    printf("EMA: Iteration %d: ", idx_iter + 1);  // выводим порядковый номер повторения, чтобы отслеживать прогресс

    size_t left_table_size = 0;  // переменная для хранения количества строк в левой таблице
    size_t right_table_size = 0;  // переменная для количества строк в правой таблице

    row_t* left_table_rows = read_table(left_file_path, &left_table_size);  // читаем левую таблицу в память
    if (!left_table_rows) {  // проверяем, успешно ли завершилось чтение
      return 1;  // при ошибке завершаем работу программы, так как продолжить невозможно
    }

    row_t* right_table_rows = read_table(right_file_path, &right_table_size);  // читаем правую таблицу
    if (!right_table_rows) {  // если чтение правой таблицы не удалось
      free(left_table_rows);  // освобождаем память левой таблицы, чтобы избежать утечки
      return 1;  // завершаем работу с кодом ошибки
    }

    qsort(left_table_rows, left_table_size, sizeof(row_t), cmp_row);  // сортируем левую таблицу по id, подготавливая её к merge join
    qsort(right_table_rows, right_table_size, sizeof(row_t), cmp_row);  // сортируем правую таблицу тем же образом

    FILE* output_file_handle = fopen(output_file_path, "w");  // открываем файл для записи результата соединения
    if (!output_file_handle) {  // если файл открыть не удалось (например, нет прав на запись)
      perror("fopen out");  // выводим системное сообщение об ошибке
      free(left_table_rows);  // освобождаем память обоих массивов
      free(right_table_rows);
      return 1;  // завершаем выполнение
    }

    size_t left_table_index = 0;  // текущая позиция в левой таблице при merge join
    size_t right_table_index = 0;  // текущая позиция в правой таблице
    size_t result_rows_count = 0;  // счётчик записей, сформированных в результате соединения

    out_row_t* result_rows_array = malloc(sizeof(out_row_t) * (left_table_size + right_table_size));  // выделяем память под результирующие строки (верхняя оценка размера)
    if (!result_rows_array) {  // проверяем успешность выделения
      perror("malloc result_array");  // сообщаем о нехватке памяти
      fclose(output_file_handle);  // закрываем файл результата
      free(left_table_rows);  // освобождаем ранее выделенные массивы
      free(right_table_rows);
      return 1;  // прекращаем выполнение итерации
    }

    while (left_table_index < left_table_size && right_table_index < right_table_size) {  // основной цикл merge join до тех пор, пока обе таблицы не исчерпаны
      if (left_table_rows[left_table_index].id < right_table_rows[right_table_index].id) {  // если текущий id левой таблицы меньше, смещаем левый индекс
        left_table_index++;  // увеличиваем индекс, продвигаясь к более крупным идентификаторам
        continue;  // переходим к следующей итерации цикла
      }

      if (left_table_rows[left_table_index].id > right_table_rows[right_table_index].id) {  // аналогично, если id правой таблицы меньше, продвигаем правый индекс
        right_table_index++;  // перемещаемся к следующему элементу правой таблицы
        continue;  // итерацию завершили, повторяем сравнение
      }

      uint64_t current_matching_id = left_table_rows[left_table_index].id;  // оба id равны, запоминаем совпадающее значение

      size_t left_range_start_index = left_table_index;  // фиксируем начало диапазона совпадающих id в левой таблице
      while (left_table_index < left_table_size && left_table_rows[left_table_index].id == current_matching_id) {  // продвигаемся по левой таблице, пока встречаем тот же id
        left_table_index++;  // увеличиваем индекс, захватывая весь поддиапазон совпадающих строк
      }
      size_t left_range_end_index = left_table_index;  // теперь left_table_index указывает на первый элемент после диапазона; диапазон полуоткрытый [start, end)

      size_t right_range_start_index = right_table_index;  // аналогично фиксируем начало диапазона совпадающих id в правой таблице
      while (right_table_index < right_table_size && right_table_rows[right_table_index].id == current_matching_id) {  // продвигаемся по правой таблице, пока id совпадает
        right_table_index++;  // увеличиваем индекс, включая все совпадающие записи
      }
      size_t right_range_end_index = right_table_index;  // правый диапазон также полуоткрытый [start, end)

      size_t left_inner_index = 0;  // вспомогательный индекс для прохода по диапазону левой таблицы
      size_t right_inner_index = 0;  // вспомогательный индекс для диапазона правой таблицы
      for (left_inner_index = left_range_start_index; left_inner_index < left_range_end_index; ++left_inner_index) {  // перебираем все строки с совпадающим id в левой таблице
        for (right_inner_index = right_range_start_index; right_inner_index < right_range_end_index; ++right_inner_index) {  // для каждой строки левой таблицы перебираем все совпадения в правой таблице
          result_rows_array[result_rows_count].id = current_matching_id;  // записываем общий id в результирующую строку
          strncpy(result_rows_array[result_rows_count].table_a, left_table_rows[left_inner_index].key, MAX_KEY_LENGTH);  // копируем ключ из левой таблицы, обрезая до MAX_KEY_LENGTH
          strncpy(result_rows_array[result_rows_count].table_b, right_table_rows[right_inner_index].key, MAX_KEY_LENGTH);  // копируем ключ из правой таблицы
          result_rows_count++;  // увеличиваем счётчик результирующих строк
        }
      }
    }

    fprintf(output_file_handle, "%zu\n", result_rows_count);  // записываем количество строк результата в качестве заголовка

    size_t result_index = 0;  // индекс для прохода по массиву result_rows_array при записи в файл
    for (result_index = 0; result_index < result_rows_count; ++result_index) {  // перебираем все строки результата
      fprintf(output_file_handle, "%" PRIu64 " %8s %8s\n", result_rows_array[result_index].id, result_rows_array[result_index].table_a, result_rows_array[result_index].table_b);  // записываем id и оба ключа, выравнивая их по ширине 8 символов
    }

    printf("Join produced %zu rows\n", result_rows_count);  // выводим количество строк, чтобы пользователь видел объём результата

    clock_gettime(CLOCK_MONOTONIC, &_end_iter);  // фиксируем окончание итерации
    _diff_iter.tv_sec = _end_iter.tv_sec - _start_iter.tv_sec;  // вычисляем разницу в секундах между началом и концом
    _diff_iter.tv_nsec = _end_iter.tv_nsec - _start_iter.tv_nsec;  // вычисляем разницу в наносекундах
    if (_diff_iter.tv_nsec < 0) {  // нормализуем значение, если наносекундная часть стала отрицательной
      _diff_iter.tv_sec -= 1;  // уменьшаем количество секунд на единицу
      _diff_iter.tv_nsec += NSEC_PER_SEC;  // добавляем NSEC_PER_SEC, чтобы компенсировать заимствованную секунду
    }
    double elapsed_iter = _diff_iter.tv_sec + _diff_iter.tv_nsec / (double)NSEC_PER_SEC;  // переводим длительность в секунды с дробной частью
    fprintf(stderr, "elapsed: %.6f s\n", elapsed_iter);  // выводим время итерации в поток ошибок, отделяя его от основного вывода

    fclose(output_file_handle);  // закрываем файл результатов, так как запись завершена
    free(left_table_rows);  // освобождаем память, занятую левой таблицей
    free(right_table_rows);  // освобождаем память правой таблицы
    free(result_rows_array);  // освобождаем память, выделенную под результат соединения
  }

  return 0;  // возвращаем код успеха после завершения всех повторений
}