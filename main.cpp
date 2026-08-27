#include <iostream>
#include <cstdint>
#include <vector>
#include <random>

uint64_t l1_norm_stride(const uint64_t* matrix, size_t n) {
    uint64_t max_sum = 0;
    for (size_t col = 0; col < n; ++col) {
        uint64_t col_sum = 0;
        for (size_t row = 0; row < n; ++row) {
            col_sum += matrix[row * n + col];
        }
        if (col_sum > max_sum) max_sum = col_sum;
    }
    return max_sum;
}

uint64_t l1_norm_sequential(const uint64_t* matrix, size_t n) {
    std::vector<uint64_t> col_sums(n, 0);
    for (size_t row = 0; row < n; ++row) {
        for (size_t col = 0; col < n; ++col) {
            col_sums[col] += matrix[row * n + col];
        }
    }
    uint64_t max_sum = 0;
    for (size_t col = 0; col < n; ++col) {
        if (col_sums[col] > max_sum) max_sum = col_sums[col];
    }
    return max_sum;
}

int main() {
    const size_t N = 1000;
    const size_t REPEAT = 10000;

    std::vector<uint64_t> matrix(N * N);
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(1, 100);
    for (auto& val : matrix) val = dist(gen);

    uint64_t total = 0;
    for (size_t i = 0; i < REPEAT; ++i) {
        total += l1_norm_stride(matrix.data(), N);
    }
    std::cout << "Checksum = " << total << '\n';

    //for (size_t i = 0; i < REPEAT; ++i) {
    //    total += l1_norm_sequential(matrix.data(), N);
    //}
    //std::cout << "Checksum = " << total << '\n';

    return 0;
}