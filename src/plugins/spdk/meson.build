# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# ---------------------------------------------------------------------------
# SPDK plugin: a clean-sheet, transport-agnostic, backend-agnostic NIXL
# backend that speaks the ratified NVMe Key-Value command set over the SPDK
# NVMe driver (lib/nvme). WRITE -> KV Store, READ -> KV Retrieve. It ships its
# OWN generic host shim (spdk_shim.{h,cpp}); it does NOT depend on any external
# host shim.
# ---------------------------------------------------------------------------

# Location of the (already-built) SPDK tree we link against.
spdk_root = get_option('spdk_root')

# Where the SPDK .pc files live. An in-tree SPDK build writes them under
# build/lib/pkgconfig; leaving spdk_root unset falls back to whatever pkg-config
# already finds, so a system or otherwise-installed SPDK needs no option at all.
spdk_pc_env = {}
if spdk_root != ''
    spdk_pc_env = {'PKG_CONFIG_PATH': spdk_root / 'build' / 'lib' / 'pkgconfig'}
endif

# The components this plugin links against. spdk_nvme pulls the rest of the
# datapath closure (sock, dma, json, util, log, ...) through its Requires, and
# spdk_vfio_user adds the VFIOUSER transport. spdk_env_dpdk is the env
# implementation; it drops out when the plugin brings its own.
spdk_pc_components = ['spdk_nvme', 'spdk_vfio_user', 'spdk_env_dpdk']

# The plugin is gated on that closure resolving and on the KV public header.
# Skip cleanly when either is absent. These checks need no C compiler, so they
# run before acquiring one: a default `meson setup` reaches this file (SPDK is
# in all_plugins), and on a host with no SPDK build the plugin must skip rather
# than fail configuration.
pkgconfig_prog = find_program('pkg-config', required: false)
if not pkgconfig_prog.found()
    warning('SPDK plugin disabled: pkg-config not found')
    subdir_done()
endif

# spdk_syslibs is queried separately rather than as part of the closure: it
# names the system and ISA-L libraries the SPDK archives depend on, which have
# to land outside the --whole-archive group. It is still required, so it belongs
# in this gate.
spdk_pc_ok = true
foreach spdk_pc_component : spdk_pc_components + ['spdk_syslibs']
    if spdk_pc_ok
        spdk_pc_probe = run_command(pkgconfig_prog, '--exists', spdk_pc_component,
                                    env: spdk_pc_env, check: false)
        spdk_pc_ok = spdk_pc_probe.returncode() == 0
    endif
endforeach
if not spdk_pc_ok
    warning('SPDK plugin disabled: SPDK pkg-config components not found' +
            (spdk_root != '' ? ' under ' + spdk_root : ''))
    subdir_done()
endif

# Include directories come from the same pkg-config query, so an SPDK laid out
# any other way than an in-tree build still works.
spdk_pc_cflags = run_command(pkgconfig_prog, '--cflags-only-I', spdk_pc_components,
                             env: spdk_pc_env, check: true)
spdk_inc_paths = []
foreach spdk_pc_flag : spdk_pc_cflags.stdout().split()
    if spdk_pc_flag.startswith('-I') and not spdk_inc_paths.contains(spdk_pc_flag.substring(2))
        spdk_inc_paths += spdk_pc_flag.substring(2)
    endif
endforeach

# The KV command set is what this plugin speaks, so its public header has to be
# present in whichever include tree pkg-config just named.
spdk_have_kv = false
foreach spdk_inc_path : spdk_inc_paths
    if fs.exists(spdk_inc_path / 'spdk' / 'nvme_kv.h')
        spdk_have_kv = true
    endif
endforeach
if not spdk_have_kv
    warning('SPDK plugin disabled: spdk/nvme_kv.h not found in the SPDK include path')
    subdir_done()
endif

# Include dirs: the plugin's own headers, plus the SPDK headers.
spdk_local_inc = include_directories('.')
spdk_spdk_inc = include_directories(spdk_inc_paths, is_system: true)

# Resolve the SPDK link closure with pkg-config rather than naming the archives.
# Which libraries an SPDK build produces depends on how it was configured and on
# its DPDK version: a build without VMD has no libspdk_vmd.a, and a DPDK older
# than the argparse split has no librte_argparse.a. A fixed list fails to link
# against either, and both are ordinary SPDK builds.
spdk_pc_libs = run_command(pkgconfig_prog, '--libs', spdk_pc_components,
                           env: spdk_pc_env, check: true)
spdk_pc_libdirs = run_command(pkgconfig_prog, '--libs-only-L', spdk_pc_components,
                              env: spdk_pc_env, check: true)

# The archives live in more than one directory (SPDK's own build/lib and DPDK's
# build/lib), so each -l is matched against every -L pkg-config reported.
spdk_search_dirs = []
foreach spdk_pc_flag : spdk_pc_libdirs.stdout().split()
    if spdk_pc_flag.startswith('-L') and not spdk_search_dirs.contains(spdk_pc_flag.substring(2))
        spdk_search_dirs += spdk_pc_flag.substring(2)
    endif
endforeach

# Resolve each -lspdk_*/-lrte_* to its full .a path instead of passing the flag
# through. Two reasons: a build dir that also ships .so files would otherwise be
# linked dynamically and leave an unsatisfiable runtime dependency, and meson
# reorders bare -l flags out of a raw -Wl,--whole-archive group, which silently
# drops the constructor-only registrations the env and transports rely on.
# Anything that does not resolve to an archive (system -l, -pthread, ...) is
# kept in order and lands outside the group.
spdk_archives = []
spdk_tail_args = []
foreach spdk_pc_flag : spdk_pc_libs.stdout().split()
    if spdk_pc_flag.startswith('-L')
        # Folded into the resolved archive paths.
    elif spdk_pc_flag.startswith('-l')
        spdk_archive_name = 'lib' + spdk_pc_flag.substring(2) + '.a'
        spdk_archive_path = ''
        foreach spdk_search_dir : spdk_search_dirs
            if spdk_archive_path == '' and fs.exists(spdk_search_dir / spdk_archive_name)
                spdk_archive_path = spdk_search_dir / spdk_archive_name
            endif
        endforeach
        if spdk_archive_path != ''
            spdk_archives += spdk_archive_path
        else
            spdk_tail_args += spdk_pc_flag
        endif
    else
        spdk_tail_args += spdk_pc_flag
    endif
endforeach

# Whole-archive the SPDK and DPDK static libs so their constructor-based
# self-registration (env, transports, bus drivers) survives the link.
spdk_link_args = ['-Wl,--no-as-needed', '-Wl,--whole-archive']
spdk_link_args += spdk_archives
spdk_link_args += ['-Wl,--no-whole-archive']
spdk_link_args += spdk_tail_args

# The system and ISA-L libraries the SPDK archives depend on. Taken from
# pkg-config rather than hardcoded, because which ones are needed depends on how
# SPDK was configured. These belong after --no-whole-archive: they carry no
# constructor registrations and whole-archiving them would drag in every object.
spdk_syslibs_pc = run_command(pkgconfig_prog, '--libs', 'spdk_syslibs',
                              env: spdk_pc_env, check: true)
spdk_link_args += spdk_syslibs_pc.stdout().split()

spdk_sources = [
    'spdk_backend.cpp',
    'spdk_backend.h',
    'spdk_key.cpp',
    'spdk_key.h',
    'spdk_plugin.cpp',
    'spdk_shim.cpp',
    'spdk_shim.h',
    'spdk_fence.cpp',
    'spdk_fence.h',
]

plugin_deps = [nixl_infra, nixl_common_dep]
spdk_inc = [nixl_inc_dirs, utils_inc_dirs, spdk_local_inc, spdk_spdk_inc]

if 'SPDK' in static_plugins
    spdk_backend_lib = static_library(
        'SPDK',
        spdk_sources,
        dependencies: plugin_deps,
        link_args: spdk_link_args,
        include_directories: spdk_inc,
        install: false,
        name_prefix: 'libplugin_')
else
    spdk_backend_lib = shared_library(
        'SPDK',
        spdk_sources,
        dependencies: plugin_deps,
        link_args: spdk_link_args,
        cpp_args: ['-fPIC'],
        include_directories: spdk_inc,
        install: true,
        name_prefix: 'libplugin_',
        install_dir: plugin_install_dir,
        install_rpath: '$ORIGIN/..')
    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
            'echo "SPDK=' + spdk_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
            check: true
        )
    endif
endif

if 'SPDK' in static_plugins
    spdk_backend_interface = declare_dependency(link_whole: spdk_backend_lib)
else
    spdk_backend_interface = declare_dependency(link_with: spdk_backend_lib)
endif

# Direct-engine round-trip tests (link the engine TUs + shim against a live SPDK
# target). Enabled by -Dspdk_build_test=true. The KV test drives the OBJ_SEG
# Store/Retrieve path; the block test drives the BLK_SEG LBA read/write path.
if get_option('spdk_build_test')
    # Pure-logic unit test for the timeout/orphan/quarantine fence. spdk_fence
    # includes no SPDK headers, so this links only itself: no lib/nvme, no live
    # target, no hugepages. It exercises the stale-orphan discard, the poison
    # latch and the staging-buffer quarantine accounting standalone.
    executable(
        'spdk_fence_test',
        ['spdk_fence_test.cpp', 'spdk_fence.cpp'],
        include_directories: [spdk_local_inc],
        install: false,
    )
    executable(
        'spdk_roundtrip_test',
        ['spdk_roundtrip_test.cpp', 'spdk_backend.cpp', 'spdk_backend.h',
         'spdk_key.cpp', 'spdk_key.h', 'spdk_shim.cpp', 'spdk_shim.h',
         'spdk_fence.cpp', 'spdk_fence.h'],
        dependencies: plugin_deps,
        link_args: spdk_link_args,
        include_directories: spdk_inc,
        install: false,
    )
    executable(
        'spdk_block_roundtrip_test',
        ['spdk_block_roundtrip_test.cpp', 'spdk_backend.cpp', 'spdk_backend.h',
         'spdk_key.cpp', 'spdk_key.h', 'spdk_shim.cpp', 'spdk_shim.h',
         'spdk_fence.cpp', 'spdk_fence.h'],
        dependencies: plugin_deps,
        link_args: spdk_link_args,
        include_directories: spdk_inc,
        install: false,
    )
endif
