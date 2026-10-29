/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <charconv>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace nixlbench {

// Zero means the environment variable is absent: retain the original benchmark.
inline size_t
parseMemosKeyPoolSize(const char *value) {
    if (value == nullptr) {
        return 0;
    }
    const std::string_view text(value);
    size_t count = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        count == 0 || count > 1000000) {
        throw std::invalid_argument("expected an integer in [1, 1000000]");
    }
    return count;
}

inline void
validateMemosKeyPoolSize(size_t keys,
                         size_t batch,
                         size_t max_block_size,
                         size_t total_buffer_size,
                         size_t threads) {
    if (batch == 0 || keys < batch || keys % batch != 0) {
        throw std::invalid_argument("key count must be a positive multiple of batch size");
    }
    // Division avoids overflowing either the working-set size or descriptor IDs.
    if (threads == 0 || keys > static_cast<size_t>(std::numeric_limits<int>::max()) / threads) {
        throw std::invalid_argument("total key count exceeds the descriptor ID range");
    }
    if (max_block_size == 0 || keys > (total_buffer_size / threads) / max_block_size) {
        throw std::invalid_argument("key pool exceeds the per-thread buffer at max_block_size");
    }
}

} // namespace nixlbench
