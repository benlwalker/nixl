/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "worker/nixl/doca_memos_key_generator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool
isLowercaseHexKey(const std::string &key) {
    return key.size() == 32 &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) {
               return std::isdigit(c) || (c >= 'a' && c <= 'f');
           });
}

class ScriptedRandomEngine {
public:
    using result_type = uint64_t;

    explicit ScriptedRandomEngine(std::vector<result_type> values)
        : values_(std::move(values)) {}

    result_type
    operator()() {
        return values_.at(calls_++);
    }

    static constexpr result_type
    min() {
        return std::numeric_limits<result_type>::min();
    }

    static constexpr result_type
    max() {
        return std::numeric_limits<result_type>::max();
    }

    size_t
    calls() const {
        return calls_;
    }

private:
    std::vector<result_type> values_;
    size_t calls_ = 0;
};

TEST(DocaMemosKeyGeneratorTest, SequentialModeReturnsEmptyKeyWithoutConsumingRandomness) {
    std::mt19937_64 rng(17);
    const auto initial_rng = rng;
    std::unordered_set<std::string> generated_keys;

    EXPECT_TRUE(nixlbench::makeDocaMemosObjectKey(false, rng, generated_keys).empty());
    EXPECT_EQ(rng, initial_rng);
    EXPECT_TRUE(generated_keys.empty());
}

TEST(DocaMemosKeyGeneratorTest, SameSeedProducesSameKeySet) {
    constexpr size_t num_keys = 256;
    std::mt19937_64 rng_a(23);
    std::mt19937_64 rng_b(23);
    std::unordered_set<std::string> generated_a;
    std::unordered_set<std::string> generated_b;
    std::vector<std::string> keys_a;
    std::vector<std::string> keys_b;

    for (size_t i = 0; i < num_keys; i++) {
        keys_a.push_back(nixlbench::makeDocaMemosObjectKey(true, rng_a, generated_a));
        keys_b.push_back(nixlbench::makeDocaMemosObjectKey(true, rng_b, generated_b));
    }

    EXPECT_EQ(keys_a, keys_b);
}

TEST(DocaMemosKeyGeneratorTest, AllGeneratedKeysAreUnique128BitLowercaseHex) {
    constexpr size_t num_keys = 4096;
    std::mt19937_64 rng(29);
    std::unordered_set<std::string> generated_keys;
    std::vector<std::string> keys;
    keys.reserve(num_keys);

    for (size_t i = 0; i < num_keys; i++) {
        keys.push_back(nixlbench::makeDocaMemosObjectKey(true, rng, generated_keys));
    }

    ASSERT_EQ(keys.size(), num_keys);
    EXPECT_EQ(generated_keys.size(), num_keys);
    for (const auto &key : keys) {
        EXPECT_TRUE(isLowercaseHexKey(key)) << key;
    }
}

TEST(DocaMemosKeyGeneratorTest, CollisionIsRetried) {
    ScriptedRandomEngine rng({1, 2, 1, 2, 3, 4});
    std::unordered_set<std::string> generated_keys;

    EXPECT_EQ(nixlbench::makeDocaMemosObjectKey(true, rng, generated_keys),
              "00000000000000010000000000000002");
    EXPECT_EQ(nixlbench::makeDocaMemosObjectKey(true, rng, generated_keys),
              "00000000000000030000000000000004");
    EXPECT_EQ(rng.calls(), 6u);
    EXPECT_EQ(generated_keys.size(), 2u);
}

} // namespace

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
