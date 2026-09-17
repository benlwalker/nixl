# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

gtest_dep = dependency('gtest', version: '>=1.11.0', required: false)

if not gtest_dep.found()
    message('GTest not found, skipping NIXLBench unit tests')
else
    doca_memos_key_generator_test = executable(
        'doca_memos_key_generator_test',
        'doca_memos_key_generator_test.cpp',
        include_directories: inc_dir,
        dependencies: gtest_dep,
    )

    test('doca_memos_key_generator_test', doca_memos_key_generator_test,
         suite: ['unit', 'doca_memos'])
endif
