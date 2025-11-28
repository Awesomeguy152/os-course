#include "../lib/cpu-sort.h"
#include <stdio.h>
#include <time.h>

#define NSEC_PER_SEC 1000000000L

/* Быстрая сортировка (N*logN) */
void quick_sort(int *arr, int left, int right) {
    if (left >= right) return;
    int pivot = arr[(left + right) / 2];
    int i = left, j = right;
    while (i <= j) {
        while (arr[i] < pivot) i++;
        while (arr[j] > pivot) j--;
        if (i <= j) {
            int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
            i++; j--;
        }
    }
    quick_sort(arr, left, j);
    quick_sort(arr, i, right);
}

/* Пузырьковая сортировка (N^2) */
void bubble_sort(int *arr, int n) {
    for (int i = 0; i < n - 1; ++i) {
        for (int j = 0; j < n - i - 1; ++j) {
            if (arr[j] > arr[j + 1]) {
                int tmp = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = tmp;
            }
        }
    }
}


int main() {
    struct timespec _start, _end, _diff;
    clock_gettime(CLOCK_MONOTONIC, &_start);
    int arr1[] = {5, 2, 9, 1, 5, 6};
    int n = sizeof(arr1)/sizeof(arr1[0]);
    quick_sort(arr1, 0, n-1);
    printf("Quick sort: ");
    for (int i = 0; i < n; ++i) printf("%d ", arr1[i]);
    printf("\n");

    int arr2[] = {3, 7, 4, 9, 5, 2};
    bubble_sort(arr2, n);
    printf("Bubble sort: ");
    for (int i = 0; i < n; ++i) printf("%d ", arr2[i]);
    printf("\n");
    clock_gettime(CLOCK_MONOTONIC, &_end);
    _diff.tv_sec = _end.tv_sec - _start.tv_sec;
    _diff.tv_nsec = _end.tv_nsec - _start.tv_nsec;
    if (_diff.tv_nsec < 0) {
        _diff.tv_sec -= 1;
        _diff.tv_nsec += NSEC_PER_SEC;
    }
    double elapsed = _diff.tv_sec + _diff.tv_nsec / (double)NSEC_PER_SEC;
    printf("elapsed: %.6f s\n", elapsed);
    return 0;
}
