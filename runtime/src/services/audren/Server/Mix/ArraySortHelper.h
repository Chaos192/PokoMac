// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of the .NET ArraySortHelper<T> introspective sort with a Comparison<T> comparer (dotnet/runtime, System.Private.CoreLib, MIT).
// Ryujinx.Audio/Renderer/Server/Mix/MixContext.cs calls Array.Sort(int[], Comparison<int>). The order of equal keys depends on this exact
// algorithm (it is not stable for more than 3 elements), so it is ported literally instead of using std::sort or std::stable_sort.
// Comparison convention as in C#: cmp(a, b) < 0 means a sorts before b.
#pragma once
#include <bit>
#include <cstdint>
#include <utility>

#include "../../audren_common.h"

namespace nxrt::audren {

class ArraySortHelper {
  public:
    // C# Array.Sort(keys, comparison): sorts keys in place.
    template <class T, class TCmp>
    static void IntrospectiveSort(Span<T> keys, TCmp comparer) {
        if (keys.Length > 1) {
            // C#: 2 * (BitOperations.Log2((uint)Length) + 1), and Log2(n) + 1 == bit_width(n) for n > 0.
            int32_t depthLimit = 2 * static_cast<int32_t>(std::bit_width(static_cast<uint32_t>(keys.Length)));

            IntroSort(keys.Data, static_cast<int32_t>(keys.Length), depthLimit, comparer);
        }
    }

  private:
    static constexpr int32_t kIntrosortSizeThreshold = 16;

    template <class T>
    static void Swap(T* keys, int32_t i, int32_t j) {
        T t = keys[i];
        keys[i] = keys[j];
        keys[j] = t;
    }

    template <class T, class TCmp>
    static void SwapIfGreater(T* keys, TCmp& comparer, int32_t i, int32_t j) {
        if (comparer(keys[i], keys[j]) > 0) {
            Swap(keys, i, j);
        }
    }

    template <class T, class TCmp>
    static void IntroSort(T* keys, int32_t length, int32_t depthLimit, TCmp& comparer) {
        int32_t partitionSize = length;

        while (partitionSize > 1) {
            if (partitionSize <= kIntrosortSizeThreshold) {
                if (partitionSize == 2) {
                    SwapIfGreater(keys, comparer, 0, 1);
                    return;
                }

                if (partitionSize == 3) {
                    SwapIfGreater(keys, comparer, 0, 1);
                    SwapIfGreater(keys, comparer, 0, 2);
                    SwapIfGreater(keys, comparer, 1, 2);
                    return;
                }

                InsertionSort(keys, partitionSize, comparer);
                return;
            }

            if (depthLimit == 0) {
                HeapSort(keys, partitionSize, comparer);
                return;
            }
            depthLimit--;

            int32_t p = PickPivotAndPartition(keys, partitionSize, comparer);

            // The pivot is already in place: recurse on the right part and loop on the left part.
            IntroSort(keys + (p + 1), partitionSize - (p + 1), depthLimit, comparer);
            partitionSize = p;
        }
    }

    template <class T, class TCmp>
    static int32_t PickPivotAndPartition(T* keys, int32_t length, TCmp& comparer) {
        int32_t hi = length - 1;

        // Median of three. Also partitions them, since the comparisons are done.
        int32_t middle = hi >> 1;

        SwapIfGreater(keys, comparer, 0, middle);
        SwapIfGreater(keys, comparer, 0, hi);
        SwapIfGreater(keys, comparer, middle, hi);

        T pivot = keys[middle];
        Swap(keys, middle, hi - 1);
        int32_t left = 0;
        int32_t right = hi - 1;

        while (left < right) {
            while (comparer(keys[++left], pivot) < 0) {
            }
            while (comparer(pivot, keys[--right]) < 0) {
            }

            if (left >= right) {
                break;
            }

            Swap(keys, left, right);
        }

        // Put the pivot in the right location.
        if (left != hi - 1) {
            Swap(keys, left, hi - 1);
        }

        return left;
    }

    template <class T, class TCmp>
    static void HeapSort(T* keys, int32_t length, TCmp& comparer) {
        int32_t n = length;

        for (int32_t i = n >> 1; i >= 1; i--) {
            DownHeap(keys, i, n, comparer);
        }

        for (int32_t i = n; i > 1; i--) {
            Swap(keys, 0, i - 1);
            DownHeap(keys, 1, i - 1, comparer);
        }
    }

    template <class T, class TCmp>
    static void DownHeap(T* keys, int32_t i, int32_t n, TCmp& comparer) {
        T d = keys[i - 1];

        while (i <= n >> 1) {
            int32_t child = 2 * i;

            if (child < n && comparer(keys[child - 1], keys[child]) < 0) {
                child++;
            }

            if (!(comparer(d, keys[child - 1]) < 0)) {
                break;
            }

            keys[i - 1] = keys[child - 1];
            i = child;
        }

        keys[i - 1] = d;
    }

    template <class T, class TCmp>
    static void InsertionSort(T* keys, int32_t length, TCmp& comparer) {
        for (int32_t i = 0; i < length - 1; i++) {
            T t = keys[i + 1];

            int32_t j = i;
            while (j >= 0 && comparer(t, keys[j]) < 0) {
                keys[j + 1] = keys[j];
                j--;
            }

            keys[j + 1] = t;
        }
    }
};

}  // namespace nxrt::audren
