/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_BENCHMARK_NIXLBENCH_SRC_WORKER_NIXL_DOCA_MEMOS_KEY_GENERATOR_H
#define NIXL_BENCHMARK_NIXLBENCH_SRC_WORKER_NIXL_DOCA_MEMOS_KEY_GENERATOR_H

#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>

namespace nixlbench {

template<typename RandomEngine>
std::string
makeDocaMemosObjectKey(bool random_keys,
                       RandomEngine &rng,
                       std::unordered_set<std::string> &generated_keys) {
    if (!random_keys) {
        return {};
    }

    std::string key;
    do {
        std::ostringstream key_stream;
        key_stream << std::hex << std::setfill('0') << std::setw(16) << rng() << std::setw(16)
                   << rng();
        key = key_stream.str();
    } while (!generated_keys.insert(key).second);

    return key;
}

} // namespace nixlbench

#endif // NIXL_BENCHMARK_NIXLBENCH_SRC_WORKER_NIXL_DOCA_MEMOS_KEY_GENERATOR_H
