#ifndef CPU_SORT_H
#define CPU_SORT_H

/* Быстрая сортировка массива целых чисел (N*logN)
   arr — массив, left/right — границы сортировки (обычно 0, n-1) */
void quick_sort(int *arr, int left, int right);

/* Пузырьковая сортировка массива целых чисел (N^2)
   arr — массив, n — размер массива */
void bubble_sort(int *arr, int n);

#endif // CPU_SORT_H
