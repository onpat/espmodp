#pragma once

static inline unsigned int libxm_stdc_count_ones_ull(unsigned long long value) {
    return (unsigned int)__builtin_popcountll(value);
}

#define stdc_count_ones(value) libxm_stdc_count_ones_ull((unsigned long long)(value))
