#include "../lib/cpu-sort.h"
#include <stdio.h>

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
    return 0;
}
