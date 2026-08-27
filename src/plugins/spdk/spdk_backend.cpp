/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "spdk_backend.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>

#include "common/nixl_log.h"

#include "spdk_shim.h"

namespace {

// Per-descriptor metadata for an OBJ_SEG (remote KV-key) registration: the
// verbatim inline NVMe-KV key derived from the descriptor's metaInfo.
class nixlSpdkMetadata : public nixlBackendMD {
public:
    explicit nixlSpdkMetadata(std::vector<uint8_t> key)
        : nixlBackendMD(true),
          key(std::move(key)) {}

    ~nixlSpdkMetadata() override = default;

    std::vector<uint8_t> key;
};

// Per-descriptor metadata for a DRAM_SEG (local host buffer) registration. When
// the region is DMA-reachable for the shim's transport -- already (SPDK-DMA
// memory) or after we register it (spdk_shim_mem_register) -- postXfer DMAs
// store/retrieve/read/write straight into the caller's buffer with NO staging
// copy (zero-copy). Otherwise dma_registered is false and postXfer falls back to
// the staged-copy path -- always correct, just a copy. owns_registration is true
// only when WE registered the region (return 0), so deregisterMem releases it;
// it stays false when the region was already reachable (return 1) and thus must
// not be unregistered here. base/len record the registered span.
class nixlSpdkDramMD : public nixlBackendMD {
public:
    nixlSpdkDramMD(void *base, size_t len, bool dma_registered, bool owns_registration)
        : nixlBackendMD(true),
          base(base),
          len(len),
          dma_registered(dma_registered),
          owns_registration(owns_registration) {}

    ~nixlSpdkDramMD() override = default;

    void *base = nullptr;
    size_t len = 0;
    bool dma_registered = false;
    bool owns_registration = false;
};

// Synchronous request handle: the shim ops complete inline, so we just stash
// Request handle. postXfer submits every descriptor's op and returns
// NIXL_IN_PROG; checkXfer polls the shim until they have all been reaped. The
// ops live here because a submitted op stays a live DMA target until it is
// reaped or abandoned, which outlasts the postXfer call that made it.
class nixlSpdkBackendReqH : public nixlBackendReqH {
public:
    nixlSpdkBackendReqH() = default;
    ~nixlSpdkBackendReqH() override = default;

    // A validated block (BLK_SEG) LBA range: one per descriptor, computed and
    // range-checked in prepXfer and consumed verbatim by postXferBlock.
    struct BlockRange {
        uint64_t lba = 0;
        uint32_t nlba = 0;
    };

    // One shim op per non-empty descriptor, with the descriptor index it came
    // from (zero-length descriptors are skipped, so the indices have gaps).
    struct Op {
        // Shim-owned: allocated by spdk_shim_op_alloc() at submit and handed
        // back with spdk_shim_op_release(), which quarantines it if the op was
        // abandoned with a live tracker. Null once released.
        spdk_shim_op *op = nullptr;
        int desc = -1;
        void *user_buf = nullptr; // caller memory to copy a staged READ back into
        size_t len = 0;
        bool staged = false;
        bool reaped = false;
    };

    nixl_status_t status = NIXL_IN_PROG;
    std::vector<Op> ops;
    // Ops submitted and not yet reaped. checkXfer polls while this is non-zero.
    size_t outstanding = 0;
    // True for a READ, which is the only direction that copies a staged buffer
    // back and the only one with value auto-sizing.
    bool is_read = false;
    // Value auto-sizing: the device's TRUE value length recorded when a READ's
    // host buffer was too small (status == NIXL_ERR_MISMATCH). postXfer returns
    // at the FIRST too-small descriptor, so at most one is ever recorded -- a
    // scalar, not a per-descriptor vector. true_len_desc is that descriptor's
    // index; -1 means "no too-small result recorded" (the READ fit, or none was
    // reached). Read by getReqTrueLen, which returns true_len iff idx matches.
    size_t true_len = 0;
    int true_len_desc = -1;
    // Block path: the per-descriptor LBA ranges validated in prepXfer. Filled for
    // a BLK_SEG transfer and consumed by postXferBlock so it issues the pre-
    // validated IO instead of recomputing computeBlockRange per descriptor
    // (mirrors gusli, which builds the block IO at prep). Empty for the KV path.
    std::vector<BlockRange> block_ranges;
};

// ASCII lower-case a string (locale-independent). Shared by the init-param
// parsing below so the boolean and namespace-kind sites normalize identically.
std::string
toLower(const std::string &v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return s;
}

// Parse "true"/"1"/"yes"/"on" (case-insensitive) as boolean true.
bool
parseBool(const std::string &v) {
    const std::string s = toLower(v);
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

} // namespace

// -----------------------------------------------------------------------------
// nixlSpdkEngine
// -----------------------------------------------------------------------------

nixlSpdkEngine::nixlSpdkEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params),
      shim_lock_(init_params->syncMode) {
    // Preferred config is a full SPDK transport ID string,
    // e.g. "trtype:VFIOUSER traddr:<socket-dir>". For ergonomics also accept a
    // bare socket directory and wrap it into a VFIOUSER transport ID.
    std::string transport_id;
    if (getInitParam("transport_id", transport_id) != NIXL_SUCCESS || transport_id.empty()) {
        std::string vfu_addr;
        for (const char *k : {"vfu_addr", "socket", "vfio_user_path"}) {
            if (getInitParam(k, vfu_addr) == NIXL_SUCCESS && !vfu_addr.empty()) {
                break;
            }
            vfu_addr.clear();
        }
        if (!vfu_addr.empty()) {
            transport_id = "trtype:VFIOUSER traddr:" + vfu_addr;
        }
    }

    if (transport_id.empty()) {
        NIXL_ERROR << "SPDK: missing required custom param 'transport_id' "
                      "(a SPDK transport ID, e.g. 'trtype:VFIOUSER traddr:<socket>')";
        initErr = true;
        return;
    }

    std::string nsid_str;
    uint32_t nsid = 0; // 0 selects the first CSI==KV namespace
    if (getInitParam("nsid", nsid_str) == NIXL_SUCCESS && !nsid_str.empty()) {
        try {
            nsid = static_cast<uint32_t>(std::stoul(nsid_str));
        }
        catch (const std::exception &e) {
            NIXL_WARN << "SPDK: bad nsid '" << nsid_str << "', using auto-select";
            nsid = 0;
        }
    }

    // init_env defaults to false: in production the host/agent owns the SPDK
    // env (lets multiple engines coexist in one process). Standalone tests with
    // no host env pass init_env=true so the shim brings up its own (no-hugepage,
    // single-instance) SPDK env.
    bool init_env = false;
    std::string init_env_str;
    if (getInitParam("init_env", init_env_str) == NIXL_SUCCESS && !init_env_str.empty()) {
        init_env = parseBool(init_env_str);
    }

    // Namespace kind (option (b), one kind per engine). Default KV preserves the
    // historical behavior; "csi=block" (aliases: ns_kind/mode = block|blk|nvm)
    // binds a CSI==NVM block namespace and enables BLK_SEG LBA read/write.
    spdk_shim_ns_kind ns_kind = SPDK_SHIM_NS_KIND_KV;
    std::string ns_kind_str;
    for (const char *k : {"csi", "ns_kind", "mode"}) {
        if (getInitParam(k, ns_kind_str) == NIXL_SUCCESS && !ns_kind_str.empty()) {
            break;
        }
        ns_kind_str.clear();
    }
    if (!ns_kind_str.empty()) {
        const std::string s = toLower(ns_kind_str);
        if (s == "block" || s == "blk" || s == "nvm") {
            ns_kind = SPDK_SHIM_NS_KIND_BLOCK;
        } else if (s == "kv") {
            ns_kind = SPDK_SHIM_NS_KIND_KV;
        } else {
            NIXL_WARN << "SPDK: unknown namespace kind '" << ns_kind_str << "', defaulting to kv";
        }
    }
    blockMode_ = (ns_kind == SPDK_SHIM_NS_KIND_BLOCK);

    struct spdk_shim_opts opts = {};
    opts.opts_size = sizeof(opts);
    opts.name = "nixl_spdk";
    opts.transport_id = transport_id.c_str();
    opts.nsid = nsid;
    opts.init_env = init_env;
    opts.ns_kind = ns_kind;

    int rc = spdk_shim_open(&opts, &shim_);
    if (rc != 0 || shim_ == nullptr) {
        if (rc == -ENOTSUP) {
            // The shim refused the bound block namespace because it carries
            // per-LBA metadata (interleaved/extended LBA, or separate DIF/DIX).
            // The block datapath sizes transfers from the data-only sector size,
            // so a metadata-formatted namespace would fault at SGL build; fail
            // cleanly here instead. Full metadata/PI support is out of scope.
            NIXL_ERROR << "SPDK: namespace on '" << transport_id
                       << "' carries per-LBA metadata (extended-LBA/DIF/DIX), which the "
                          "block datapath does not support; refusing (rc="
                       << rc << ")";
        } else {
            NIXL_ERROR << "SPDK: spdk_shim_open(" << transport_id << ") failed: rc=" << rc;
        }
        shim_ = nullptr;
        initErr = true;
        return;
    }

    if (blockMode_) {
        // Block namespace: no key space; the datapath uses the sector geometry.
        NIXL_INFO << "SPDK: opened block namespace on '" << transport_id << "'"
                  << " (init_env=" << (init_env ? "true" : "false")
                  << ", sector_size=" << spdk_shim_sector_size(shim_)
                  << ", num_sectors=" << spdk_shim_num_sectors(shim_) << ")";
        return;
    }

    // Clamp the effective key length to the namespace-advertised kvkml so a key
    // always fits the device key space. kvkml==0 means the namespace reports no
    // limit; fall back to kMaxKeyLen rather than clamping to 0 (which would
    // reject every key).
    uint32_t shim_kvkml = spdk_shim_max_key_len(shim_);
    if (shim_kvkml == 0) {
        NIXL_WARN << "SPDK: namespace advertised kvkml=0 (no key-length limit); "
                     "using default max key length "
                  << static_cast<unsigned>(kMaxKeyLen);
        shim_kvkml = kMaxKeyLen;
    }
    maxKeyLen_ = static_cast<uint8_t>(std::min<uint32_t>(kMaxKeyLen, shim_kvkml));

    NIXL_INFO << "SPDK: opened SPDK KV shim on '" << transport_id << "'"
              << " (init_env=" << (init_env ? "true" : "false") << ", max_key=" << shim_kvkml
              << ", effective_max_key=" << static_cast<unsigned>(maxKeyLen_)
              << ", max_value=" << spdk_shim_max_value_len(shim_) << ")";
}

nixlSpdkEngine::~nixlSpdkEngine() {
    // Staging buffers belong to their ops, and those belong to request handles
    // NIXL has already released. spdk_shim_close() frees whatever an abandoned
    // op left behind.
    if (shim_) {
        spdk_shim_close(shim_);
        shim_ = nullptr;
    }
}

nixl_mem_list_t
nixlSpdkEngine::getSupportedMems() const {
    // The remote op-set is fixed by the bound namespace kind (option (b)): a
    // KV-bound engine exposes the key-addressed KV blob (OBJ_SEG), a block-bound
    // engine exposes the NVMe LBA range (BLK_SEG). Deriving the list from the
    // mode keeps advertisement and enforcement in lockstep: advertising both
    // regardless of mode would let a caller register (and transfer) a cross-mode
    // remote, which -- because the KV opcodes alias NVM WRITE/READ -- would run a
    // KV op as a wild-LBA block op. Local host DRAM (DRAM_SEG) is the
    // source/sink in either mode.
    if (blockMode_) {
        return {DRAM_SEG, BLK_SEG};
    }
    return {DRAM_SEG, OBJ_SEG};
}

nixl_status_t
nixlSpdkEngine::registerMem(const nixlBlobDesc &mem,
                            const nixl_mem_t &nixl_mem,
                            nixlBackendMD *&out) {
    // Serialize the shim's DMA-registration (spdk_shim_mem_register on the
    // DRAM_SEG path, which mutates the shared SPDK memory map and probes the
    // qpair's DMA reachability) against a concurrent postXfer/queryMem.
    NIXL_LOCK_GUARD(shim_lock_);
    if (nixl_mem != DRAM_SEG && nixl_mem != OBJ_SEG && nixl_mem != BLK_SEG) {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    // Cross-mode guard: the remote op-set must match the bound namespace kind. A
    // block-bound engine must refuse OBJ_SEG (KV) and a KV-bound engine must
    // refuse BLK_SEG, before any per-descriptor metadata is built -- otherwise a
    // KV op could later be routed to a block namespace (the KV opcodes alias NVM
    // WRITE/READ -> wild-LBA corruption). DRAM_SEG (the local buffer) is valid in
    // either mode.
    if ((nixl_mem == OBJ_SEG && blockMode_) || (nixl_mem == BLK_SEG && !blockMode_)) {
        NIXL_ERROR << "SPDK: memory type " << nixl_mem
                   << " does not match the engine's namespace kind; rejecting";
        return NIXL_ERR_NOT_SUPPORTED;
    }

    if (nixl_mem == OBJ_SEG) {
        // The remote descriptor carries the NIXL block identifier in metaInfo.
        // Take it VERBATIM as the opaque inline NVMe-KV key. The key lives in
        // the per-desc metadata and is read back at transfer time.
        std::vector<uint8_t> key;
        if (!spdkKvKeyFromBlobId(mem.metaInfo, maxKeyLen_, key)) {
            NIXL_ERROR << "SPDK: invalid KV key in metaInfo (empty or > "
                       << static_cast<unsigned>(maxKeyLen_) << " bytes); rejecting";
            return NIXL_ERR_INVALID_PARAM;
        }
        out = new nixlSpdkMetadata(std::move(key));
    } else if (nixl_mem == DRAM_SEG) {
        // Make the caller's host buffer DMA-usable so postXfer can transfer
        // directly into it (zero-copy), skipping the per-transfer staging copy.
        // The shim decides per transport whether the region is reachable and
        // registers it if needed: return 1 = already reachable (SPDK-DMA memory),
        // 0 = we registered it (must release in deregisterMem), <0 = not usable
        // directly (unaligned, or non-fd-backed DRAM over vfio-user) so we fall
        // back to a staged copy. A <0 is NOT a registerMem error -- zero-copy is
        // an optimization; the staged path is always correct.
        //
        // Registered regions are expected to be DISJOINT: over a PCIE/IOMMU
        // transport an owned registration covers exactly [base, len), and
        // reachability is probed at the base. Registering OVERLAPPING DRAM
        // regions is unsupported for the direct path -- a sub-range whose base
        // falls inside another region's mapping may be reported reachable, then
        // fail cleanly (NIXL_ERR_BACKEND, never corruption) once that other
        // region is deregistered. NIXL callers register disjoint regions, so this
        // is a documented constraint, not a live hazard.
        //
        // Construct the MD BEFORE registering so an owned registration always has
        // its releasing MD (no leak if the allocation throws).
        void *base = reinterpret_cast<void *>(mem.addr);
        const size_t len = mem.len;
        auto *md = new nixlSpdkDramMD(base,
                                      len,
                                      /*dma_registered=*/false,
                                      /*owns_registration=*/false);
        if (shim_ != nullptr && base != nullptr && len != 0) {
            int rc = spdk_shim_mem_register(shim_, base, len);
            if (rc >= 0) {
                md->dma_registered = true;
                md->owns_registration = (rc == 0);
            } else {
                NIXL_DEBUG << "SPDK: DRAM region [" << base << ", +" << len
                           << ") not DMA-reachable directly (rc=" << rc
                           << "); will stage-copy this buffer";
            }
        }
        out = md;
    } else {
        // BLK_SEG (remote LBA range) needs no per-descriptor metadata: the
        // descriptor's addr carries the starting LBA (read at transfer time, NO
        // key derivation), mirroring gusli's near-no-op block registration.
        out = nullptr;
    }
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkEngine::deregisterMem(nixlBackendMD *meta) {
    // Serialize the shim's DMA-deregistration (spdk_shim_mem_unregister
    // mutates the shared SPDK memory map the datapath translates against)
    // against a concurrent postXfer/queryMem/registerMem.
    NIXL_LOCK_GUARD(shim_lock_);
    // A DRAM registration that took the zero-copy path holds an SPDK memory
    // registration; release it before freeing the MD. NIXL deregisters only
    // after all transfers to the region have completed, so no DMA is in flight.
    // (nixlBackendMD has a virtual dtor, so the base-pointer delete below runs
    // the correct derived destructor for either MD type.)
    if (auto *dram = dynamic_cast<nixlSpdkDramMD *>(meta); dram != nullptr) {
        if (dram->dma_registered && dram->owns_registration) {
            spdk_shim_mem_unregister(dram->base, dram->len);
        }
    }
    delete meta;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkEngine::prepXfer(const nixl_xfer_op_t &operation,
                         const nixl_meta_dlist_t &local,
                         const nixl_meta_dlist_t &remote,
                         const std::string &remote_agent,
                         nixlBackendReqH *&handle,
                         const nixl_opt_b_args_t *opt_args) const {
    if (operation != NIXL_WRITE && operation != NIXL_READ) {
        NIXL_ERROR << "SPDK: invalid operation " << operation;
        return NIXL_ERR_INVALID_PARAM;
    }
    if (local.getType() != DRAM_SEG) {
        NIXL_ERROR << "SPDK: local memory type must be DRAM_SEG, got " << local.getType();
        return NIXL_ERR_INVALID_PARAM;
    }
    // The remote memory type is the LBA/KV knob: OBJ_SEG -> KV, BLK_SEG -> block.
    const nixl_mem_t remote_type = remote.getType();
    if (remote_type != OBJ_SEG && remote_type != BLK_SEG) {
        NIXL_ERROR << "SPDK: remote memory type must be OBJ_SEG or BLK_SEG, got " << remote_type;
        return NIXL_ERR_INVALID_PARAM;
    }
    // Cross-mode guard at prep: the remote op-set must match the bound namespace
    // kind, refused BEFORE any DMA (defense-in-depth alongside the postXfer and
    // registerMem guards). OBJ_SEG on a block-bound engine would alias a KV Store
    // to an NVM WRITE at a wild LBA; BLK_SEG on a KV-bound engine is equally
    // invalid. Refusing malformed-mode lists here keeps prep and post in lockstep.
    if ((remote_type == OBJ_SEG && blockMode_) || (remote_type == BLK_SEG && !blockMode_)) {
        NIXL_ERROR << "SPDK: remote memory type " << remote_type
                   << " does not match the engine's namespace kind; rejecting";
        return NIXL_ERR_NOT_SUPPORTED;
    }
    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "SPDK: local/remote descriptor count mismatch";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (!shim_) {
        NIXL_ERROR << "SPDK: shim not initialized";
        return NIXL_ERR_BACKEND;
    }

    // Validate EVERY descriptor up front, before any device op, so a multi-
    // descriptor list with one malformed descriptor is rejected atomically at
    // prep -- postXfer then issues ZERO device ops (no mid-list partial mutation
    // where an earlier descriptor is durably Stored/written and a later one fails
    // with no rollback).
    if (remote_type == BLK_SEG) {
        // Block: derive and range-check (length match, sector alignment, single-op
        // bound, capacity) every LBA range, then STASH it so postXferBlock issues
        // the validated IO rather than recomputing (mirrors gusli's prep-built IO).
        std::vector<nixlSpdkBackendReqH::BlockRange> ranges(local.descCount());
        for (int i = 0; i < local.descCount(); ++i) {
            uint64_t lba = 0;
            uint32_t nlba = 0;
            nixl_status_t vs = computeBlockRange(local[i], remote[i], lba, nlba);
            if (vs != NIXL_SUCCESS) {
                return vs;
            }
            ranges[i].lba = lba;
            ranges[i].nlba = nlba;
        }
        auto *req_h = new nixlSpdkBackendReqH();
        req_h->block_ranges = std::move(ranges);
        handle = req_h;
        return NIXL_SUCCESS;
    }

    // KV (OBJ_SEG): validate length match, the single-op bound (no striping), and
    // the key-metadata presence/length for every descriptor. These checks used to
    // live inside the postXfer loop, which validated-and-issued per iteration --
    // the source of the mid-list partial Store. Run them all here so a bad
    // descriptor is caught before the first Store/Retrieve.
    const uint32_t max_op = spdk_shim_max_value_len_op(shim_);
    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        // A local/remote length mismatch means the caller's view of the value
        // size disagrees; fail rather than Store/Retrieve a different byte count.
        if (local_desc.len != remote_desc.len) {
            NIXL_ERROR << "SPDK: descriptor " << i << " length mismatch: local=" << local_desc.len
                       << " remote=" << remote_desc.len;
            return NIXL_ERR_INVALID_PARAM;
        }
        // NO striping: a value past the single-op region-bounded SGL bound is
        // rejected here, never split across ops.
        if (local_desc.len > max_op) {
            NIXL_ERROR << "SPDK: descriptor " << i << " length " << local_desc.len
                       << " exceeds the single-op bound " << max_op
                       << " bytes; rejecting (no striping)";
            return NIXL_ERR_INVALID_PARAM;
        }
        // The verbatim inline key lives in the descriptor's registration metadata
        // (built and length-checked in registerMem). Require it to be present and
        // still within [1, maxKeyLen_] so postXfer can take it as-is.
        auto *md = static_cast<nixlSpdkMetadata *>(remote_desc.metadataP);
        if (!md) {
            NIXL_ERROR << "SPDK: remote descriptor " << i << " has no registered KV-key metadata";
            return NIXL_ERR_INVALID_PARAM;
        }
        if (md->key.empty() || md->key.size() > maxKeyLen_) {
            NIXL_ERROR << "SPDK: remote descriptor " << i << " has invalid key length "
                       << md->key.size();
            return NIXL_ERR_INVALID_PARAM;
        }
    }

    handle = new nixlSpdkBackendReqH();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkEngine::postXfer(const nixl_xfer_op_t &operation,
                         const nixl_meta_dlist_t &local,
                         const nixl_meta_dlist_t &remote,
                         const std::string &remote_agent,
                         nixlBackendReqH *&handle,
                         const nixl_opt_b_args_t *opt_args) const {
    if (!handle) {
        NIXL_ERROR << "SPDK: transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    // Serialize the shim's single qpair + shared SGL/completion state for the
    // whole synchronous op (submit+poll). Held here rather than inside
    // postXferBlock so the block delegation below runs UNDER this same lock
    // without re-locking (absl::Mutex is non-recursive -> a second guard would
    // self-deadlock).
    NIXL_LOCK_GUARD(shim_lock_);
    if (!shim_) {
        NIXL_ERROR << "SPDK: shim not initialized";
        return NIXL_ERR_BACKEND;
    }
    // Cross-mode guard: the remote op-set must match the bound namespace kind,
    // enforced before any device op. A KV (OBJ_SEG) transfer on a block-bound
    // engine would alias a KV Store to an NVM WRITE at a wild LBA (silent
    // corruption); a block (BLK_SEG) transfer on a KV-bound engine is equally
    // invalid. Mirrors the shim's is_block guards and registerMem.
    const nixl_mem_t remote_type = remote.getType();
    if ((remote_type == OBJ_SEG && blockMode_) || (remote_type == BLK_SEG && !blockMode_)) {
        NIXL_ERROR << "SPDK: remote memory type " << remote_type
                   << " does not match the engine's namespace kind; rejecting";
        static_cast<nixlSpdkBackendReqH *>(handle)->status = NIXL_ERR_NOT_SUPPORTED;
        return NIXL_ERR_NOT_SUPPORTED;
    }
    // LBA/KV knob: a BLK_SEG remote drives the NVMe block path; OBJ_SEG falls
    // through to the unchanged KV (Store/Retrieve) path below.
    if (remote_type == BLK_SEG) {
        return postXferBlock(operation, local, remote, handle);
    }
    auto *req_h = static_cast<nixlSpdkBackendReqH *>(handle);
    // Value auto-sizing state: no too-small result yet (getReqTrueLen -> 0). Set
    // to (true_len, i) only if a READ reports the host buffer was too small.
    req_h->true_len = 0;
    req_h->true_len_desc = -1;
    req_h->is_read = (operation == NIXL_READ);
    req_h->ops.clear();
    req_h->outstanding = 0;
    // Reserved once and never grown: the shim holds each op by address and the
    // device writes into it, so a reallocation would leave the qpair pointing at
    // freed memory.
    req_h->ops.reserve(static_cast<size_t>(local.descCount()));

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        // prepXfer already validated EVERY descriptor (length match, single-op
        // bound, key-metadata presence/length) BEFORE this loop, so a malformed
        // mid-list descriptor was rejected up front and NO Store/Retrieve has run
        // for this list -- there is no mid-list partial mutation. The read below
        // takes the pre-validated key from the descriptor's registration
        // metadata; the nullptr check is a defensive deref guard (prep guarantees
        // it holds for every descriptor), not a mid-list validation reject.
        auto *md = static_cast<nixlSpdkMetadata *>(remote_desc.metadataP);
        if (!md) {
            NIXL_ERROR << "SPDK: remote descriptor " << i << " has no registered KV-key metadata";
            req_h->status = NIXL_ERR_INVALID_PARAM;
            return NIXL_ERR_INVALID_PARAM;
        }
        const std::vector<uint8_t> &key = md->key;

        const auto data_ptr = reinterpret_cast<void *>(local_desc.addr);
        const size_t data_len = local_desc.len;

        // A zero-length descriptor carries no value bytes. Treat it as a
        // successful no-op rather than routing spdk_dma_zmalloc(0) (which may
        // return NULL) through the alloc-failure path and misreporting it as a
        // backend error. (Skeleton: zero-length KV value semantics are out of
        // scope here.)
        if (data_len == 0) {
            continue;
        }

        // Zero-copy when registerMem made the caller's DRAM reachable, staged
        // otherwise (unaligned, unregisterable, or only partially reachable).
        // Staging is always correct, just a copy. A submit that fails on the
        // direct path is not fatal, since nothing reached the qpair yet; demote
        // that descriptor to a staged copy and retry. A failure reported at
        // completion has no such retry and is surfaced as-is. prepXfer
        // guarantees DRAM_SEG here and registerMem always builds a
        // nixlSpdkDramMD for it, so the cast is safe; the nullptr guard covers a
        // descriptor registered with no MD.
        auto *dram = static_cast<nixlSpdkDramMD *>(local_desc.metadataP);
        const bool direct = dram != nullptr && dram->dma_registered;

        req_h->ops.emplace_back();
        auto &slot = req_h->ops.back();
        slot.desc = i;
        slot.user_buf = data_ptr;
        slot.len = data_len;

        // One submit attempt, direct or staged. The staging buffer belongs to
        // the op and is freed when it is reaped, or quarantined if the op is
        // abandoned with a live tracker.
        auto submit_kv = [&](bool use_direct) -> int {
            void *io_buf = data_ptr;
            if (!use_direct) {
                // Non-zeroing: a write memcpys the whole span below and a read
                // copies back only what the device wrote, so the skipped
                // zero-fill is never observed.
                io_buf = spdk_shim_dma_alloc_raw(data_len);
                if (!io_buf) {
                    NIXL_ERROR << "SPDK: DMA buffer alloc failed (" << data_len << " bytes)";
                    return -ENOMEM;
                }
                if (operation == NIXL_WRITE) {
                    std::memcpy(io_buf, data_ptr, data_len);
                }
            }
            // A demoted retry re-enters here, so drop the op the failed direct
            // submit allocated before taking a fresh one.
            spdk_shim_op_release(shim_, slot.op);
            slot.op = spdk_shim_op_alloc(shim_);
            if (!slot.op) {
                if (!use_direct) {
                    spdk_shim_dma_free(io_buf);
                }
                return -ENOMEM;
            }
            slot.op->staging = use_direct ? nullptr : io_buf;
            slot.staged = !use_direct;
            const int r = (operation == NIXL_WRITE) ?
                spdk_shim_store(shim_,
                                slot.op,
                                key.data(),
                                static_cast<uint8_t>(key.size()),
                                io_buf,
                                static_cast<uint32_t>(data_len)) :
                spdk_shim_retrieve(shim_,
                                   slot.op,
                                   key.data(),
                                   static_cast<uint8_t>(key.size()),
                                   io_buf,
                                   static_cast<uint32_t>(data_len));
            if (r != 0 && !use_direct) {
                spdk_shim_dma_free(io_buf);
                slot.op->staging = nullptr;
            }
            return r;
        };

        int rc = submit_kv(direct);
        if (direct && rc < 0) {
            NIXL_WARN << "SPDK: direct submit failed for descriptor " << i << " (rc=" << rc
                      << "); demoting to a staged copy";
            rc = submit_kv(false);
        }
        if (rc != 0) {
            NIXL_ERROR << "SPDK: descriptor " << i << " submit failed: rc=" << rc;
            // The op never reached the qpair, so this frees it outright.
            spdk_shim_op_release(shim_, req_h->ops.back().op);
            req_h->ops.pop_back();
            req_h->status = NIXL_ERR_BACKEND;
            return NIXL_ERR_BACKEND;
        }
        ++req_h->outstanding;
    }

    // Every op is on the qpair. checkXfer reaps them.
    req_h->status = req_h->outstanding == 0 ? NIXL_SUCCESS : NIXL_IN_PROG;
    return req_h->status;
}

nixl_status_t
nixlSpdkEngine::computeBlockRange(const nixlMetaDesc &local_desc,
                                  const nixlMetaDesc &remote_desc,
                                  uint64_t &lba_out,
                                  uint32_t &nlba_out) const {
    lba_out = 0;
    nlba_out = 0;

    // local.len == remote.len still holds for block (both are the byte length of
    // the same transfer); reuse the KV check.
    if (local_desc.len != remote_desc.len) {
        NIXL_ERROR << "SPDK: block descriptor length mismatch: local=" << local_desc.len
                   << " remote=" << remote_desc.len;
        return NIXL_ERR_INVALID_PARAM;
    }

    const size_t len = local_desc.len;
    if (len == 0) {
        // Zero-length block transfer: a no-op (0 sectors); the caller skips it.
        return NIXL_SUCCESS;
    }

    const uint32_t sector = spdk_shim_sector_size(shim_);
    if (sector == 0) {
        // Not a block-bound shim (opened csi=kv): BLK_SEG is unavailable here.
        NIXL_ERROR << "SPDK: BLK_SEG transfer requires a block namespace "
                      "(open the engine with csi=block)";
        return NIXL_ERR_BACKEND;
    }

    // Sector alignment: a block op moves whole sectors, so the byte length must
    // be a multiple of the namespace sector size (KV had no alignment rule).
    if ((len % sector) != 0) {
        NIXL_ERROR << "SPDK: block length " << len << " is not a multiple of the sector size "
                   << sector;
        return NIXL_ERR_INVALID_PARAM;
    }

    // NO striping: a single op carries up to the block single-op bound (~64 MiB)
    // as one data-block descriptor per 2 MiB DMA region via the region-bounded
    // SGL (mirrors the KV large-value bound spdk_shim_max_value_len_op). A
    // range past the bound is REJECTED here, before staging any DMA, never split.
    const uint32_t max_op = spdk_shim_max_block_len_op(shim_);
    if (len > max_op) {
        NIXL_ERROR << "SPDK: block length " << len << " exceeds the single-op bound " << max_op
                   << " bytes; rejecting (no striping)";
        return NIXL_ERR_INVALID_PARAM;
    }

    const uint64_t lba = static_cast<uint64_t>(remote_desc.addr);
    const uint64_t nlba =
        static_cast<uint64_t>(len) / sector; // <= MAX_VALUE_LEN/sector, fits uint32
    const uint64_t capacity = spdk_shim_num_sectors(shim_);

    // Capacity: reject an LBA at/after the end, or a range running past it.
    // (lba >= capacity checked first so capacity - lba never underflows.)
    if (lba >= capacity || nlba > capacity - lba) {
        NIXL_ERROR << "SPDK: block range [" << lba << ", +" << nlba
                   << ") exceeds namespace capacity " << capacity << " sectors";
        return NIXL_ERR_INVALID_PARAM;
    }

    lba_out = lba;
    nlba_out = static_cast<uint32_t>(nlba);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkEngine::postXferBlock(const nixl_xfer_op_t &operation,
                              const nixl_meta_dlist_t &local,
                              const nixl_meta_dlist_t &remote,
                              nixlBackendReqH *handle) const {
    auto *req_h = static_cast<nixlSpdkBackendReqH *>(handle);
    // Block has no value auto-sizing (getReqTrueLen -> 0).
    req_h->true_len = 0;
    req_h->true_len_desc = -1;
    req_h->is_read = (operation == NIXL_READ);
    req_h->ops.clear();
    req_h->outstanding = 0;
    // See the note in postXfer: the shim holds each op by address, so this
    // vector must never reallocate once a submit has happened.
    req_h->ops.reserve(static_cast<size_t>(local.descCount()));

    // prepXfer validated and stashed every (lba, nlba); consume it here rather
    // than recomputing computeBlockRange per descriptor (mirrors gusli, which
    // builds the block IO at prep). A size mismatch means prep did not run for
    // this handle (or the list changed) -- refuse before any device op.
    if (req_h->block_ranges.size() != static_cast<size_t>(remote.descCount())) {
        NIXL_ERROR << "SPDK: block transfer handle is missing its prep-validated LBA ranges";
        req_h->status = NIXL_ERR_INVALID_PARAM;
        return NIXL_ERR_INVALID_PARAM;
    }

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];

        // The LBA range was validated (alignment/capacity/single-range) in
        // prepXfer; take it as-is -- no recompute.
        const uint64_t lba = req_h->block_ranges[i].lba;
        const uint32_t nlba = req_h->block_ranges[i].nlba;
        if (nlba == 0) {
            // Zero-length descriptor: nothing to transfer.
            continue;
        }

        const auto data_ptr = reinterpret_cast<void *>(local_desc.addr);
        const size_t data_len = local_desc.len;

        // Zero-copy: DMA straight to/from the caller's buffer when it was
        // DMA-registered in registerMem; otherwise stage through a 2 MiB-aligned
        // SPDK DMA buffer and copy. Aligning the staging buffer to a DMA region
        // makes each 2 MiB span its own region-bounded SGL data block; a
        // registered (4 KiB-aligned) buffer is bounded at each 2 MiB boundary
        // too, so on neither path can a descriptor straddle two
        // independently-mapped vfio-user regions. No value auto-sizing here; a
        // block transfer moves exactly len bytes. Submit failures demote to a
        // staged copy as they do on the KV path.
        auto *dram = static_cast<nixlSpdkDramMD *>(local_desc.metadataP);
        const bool direct = dram != nullptr && dram->dma_registered;

        req_h->ops.emplace_back();
        auto &slot = req_h->ops.back();
        slot.desc = i;
        slot.user_buf = data_ptr;
        slot.len = data_len;

        auto submit_blk = [&](bool use_direct) -> int {
            void *io_buf = data_ptr;
            if (!use_direct) {
                io_buf = spdk_shim_dma_alloc_raw_aligned(data_len, SPDK_SHIM_DMA_REGION);
                if (!io_buf) {
                    NIXL_ERROR << "SPDK: block DMA buffer alloc failed (" << data_len << " bytes)";
                    return -ENOMEM;
                }
                if (operation == NIXL_WRITE) {
                    std::memcpy(io_buf, data_ptr, data_len);
                }
            }
            // A demoted retry re-enters here, so drop the op the failed direct
            // submit allocated before taking a fresh one.
            spdk_shim_op_release(shim_, slot.op);
            slot.op = spdk_shim_op_alloc(shim_);
            if (!slot.op) {
                if (!use_direct) {
                    spdk_shim_dma_free(io_buf);
                }
                return -ENOMEM;
            }
            slot.op->staging = use_direct ? nullptr : io_buf;
            slot.staged = !use_direct;
            const int r = (operation == NIXL_WRITE) ?
                spdk_shim_write(shim_, slot.op, io_buf, lba, nlba) :
                spdk_shim_read(shim_, slot.op, io_buf, lba, nlba);
            if (r != 0 && !use_direct) {
                spdk_shim_dma_free(io_buf);
                slot.op->staging = nullptr;
            }
            return r;
        };

        int rc = submit_blk(direct);
        if (direct && rc < 0) {
            NIXL_WARN << "SPDK: direct submit failed for block descriptor " << i << " (rc=" << rc
                      << "); demoting to a staged copy";
            rc = submit_blk(false);
        }
        if (rc != 0) {
            NIXL_ERROR << "SPDK: block descriptor " << i << " submit failed: rc=" << rc;
            // The op never reached the qpair, so this frees it outright.
            spdk_shim_op_release(shim_, req_h->ops.back().op);
            req_h->ops.pop_back();
            req_h->status = NIXL_ERR_BACKEND;
            return NIXL_ERR_BACKEND;
        }
        ++req_h->outstanding;
    }

    // Every op is on the qpair. checkXfer reaps them.
    req_h->status = req_h->outstanding == 0 ? NIXL_SUCCESS : NIXL_IN_PROG;
    return req_h->status;
}

nixl_status_t
nixlSpdkEngine::checkXfer(nixlBackendReqH *handle) const {
    if (!handle) {
        NIXL_ERROR << "SPDK: transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    auto *req_h = static_cast<nixlSpdkBackendReqH *>(handle);
    if (req_h->outstanding == 0) {
        return req_h->status;
    }
    // This is where the transfer progresses. Nothing else polls the qpair, so a
    // request advances only as often as the application asks about it.
    NIXL_LOCK_GUARD(shim_lock_);
    if (!shim_) {
        NIXL_ERROR << "SPDK: shim not initialized";
        return NIXL_ERR_BACKEND;
    }
    const int poll_rc = spdk_shim_poll(shim_, 0);
    if (poll_rc < 0) {
        NIXL_ERROR << "SPDK: qpair poll failed: rc=" << poll_rc;
        // Fall through rather than returning: the poll also expired every
        // outstanding op, and reaping them is what routes their staging buffers
        // to the quarantine.
    }
    reapOps(req_h);
    return req_h->status;
}

// Collect every op that has completed or expired, apply its result to the
// request's status, and copy a staged READ back to the caller. Must run with
// shim_lock_ held.
void
nixlSpdkEngine::reapOps(nixlBackendReqH *handle) const {
    auto *req_h = static_cast<nixlSpdkBackendReqH *>(handle);
    for (auto &slot : req_h->ops) {
        if (slot.reaped || slot.op == nullptr || !spdk_shim_op_done(slot.op)) {
            continue;
        }
        uint32_t value_len = 0;
        const int rc = spdk_shim_op_result(shim_, slot.op, &value_len);
        slot.reaped = true;
        --req_h->outstanding;

        if (rc == 0 && slot.staged && req_h->is_read) {
            // Copy back only what the device wrote: the KV value length, or
            // the whole span for a block read, which fills every sector. The
            // staging buffer is not zeroed, so its untouched tail must never
            // reach the caller. Runs before the release below frees it.
            const size_t n = value_len != 0 ? std::min<size_t>(value_len, slot.len) : slot.len;
            std::memcpy(slot.user_buf, slot.op->staging, n);
        }
        spdk_shim_op_release(shim_, slot.op);
        slot.op = nullptr;

        if (req_h->is_read && rc == SPDK_SHIM_SC_BUFFER_TOO_SMALL) {
            // Value auto-sizing: the stored value is longer than the host
            // buffer, and value_len is its real length. Record it so the caller
            // can resize and re-Retrieve, and report MISMATCH rather than a
            // generic backend error, instead of handing back a truncated value.
            // Read the length back with getReqTrueLen(handle, desc).
            req_h->true_len = value_len;
            req_h->true_len_desc = slot.desc;
            NIXL_WARN << "SPDK: value (" << value_len << " B) exceeds host buffer (" << slot.len
                      << " B) for descriptor " << slot.desc
                      << "; reporting true length for resize+retry";
            if (req_h->status == NIXL_IN_PROG) {
                req_h->status = NIXL_ERR_MISMATCH;
            }
            continue;
        }
        if (rc != 0) {
            NIXL_ERROR << "SPDK: descriptor " << slot.desc << " failed: rc=" << rc;
            if (req_h->status == NIXL_IN_PROG) {
                req_h->status = NIXL_ERR_BACKEND;
            }
            continue;
        }
    }
    if (req_h->outstanding == 0 && req_h->status == NIXL_IN_PROG) {
        req_h->status = NIXL_SUCCESS;
    }
}

nixl_status_t
nixlSpdkEngine::releaseReqH(nixlBackendReqH *handle) const {
    if (!handle) {
        NIXL_ERROR << "SPDK: transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    auto *req_h = static_cast<nixlSpdkBackendReqH *>(handle);
    {
        NIXL_LOCK_GUARD(shim_lock_);
        // Drain first so an op that is merely slow is reaped normally rather
        // than abandoned. This is bounded rather than open-ended, because the
        // poll expires an op once it passes its deadline.
        while (req_h->outstanding != 0 && shim_ != nullptr) {
            if (spdk_shim_poll(shim_, 0) < 0) {
                reapOps(req_h);
                break;
            }
            reapOps(req_h);
        }
        // Whatever is left was abandoned by that expiry, and its tracker may
        // still write to it. Hand each one back so the shim quarantines it
        // until spdk_shim_close(); dropping the handle's vector on the floor
        // would strand it instead.
        for (auto &slot : req_h->ops) {
            spdk_shim_op_release(shim_, slot.op);
            slot.op = nullptr;
        }
    }
    delete req_h;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkEngine::queryMem(const nixl_reg_dlist_t &descs,
                         std::vector<nixl_query_resp_t> &resp) const {
    // Serialize the shim's single qpair (spdk_shim_exist submits+polls) and
    // shared completion state against a concurrent postXfer/registerMem.
    NIXL_LOCK_GUARD(shim_lock_);
    // Mirror the OBJ backend's queryMem result/absence convention exactly:
    //   - resp is sized to descCount() and defaulted to std::nullopt (absent).
    //   - present => resp[i] = nixl_query_resp_t{nixl_b_params_t{}} (engaged).
    //   - absent  => resp[i] = std::nullopt (left as the default).
    //   - a submit-/transport-level error returns an error status rather than
    //     encoding it as "absent", so a failure is never masked as a cache miss.
    resp.assign(descs.descCount(), std::nullopt);

    // QUERY maps to KV Exist and is OBJ_SEG-only. Block (BLK_SEG) has no per-LBA
    // "existence" notion, so queryMem(BLK_SEG) is NIXL_ERR_NOT_SUPPORTED.
    if (descs.getType() != OBJ_SEG) {
        NIXL_ERROR << "SPDK: queryMem is only supported for OBJ_SEG (KV Exist), got "
                   << descs.getType();
        return NIXL_ERR_NOT_SUPPORTED;
    }
    // Cross-mode guard: KV Exist requires a KV-bound engine. A block-bound
    // engine has no key space, so refuse OBJ_SEG here (before any device op)
    // rather than issuing a KV Exist against a block namespace.
    if (blockMode_) {
        NIXL_ERROR << "SPDK: queryMem (KV Exist) requires a KV namespace; this "
                      "engine is block-bound";
        return NIXL_ERR_NOT_SUPPORTED;
    }
    if (!shim_) {
        NIXL_ERROR << "SPDK: shim not initialized";
        return NIXL_ERR_BACKEND;
    }

    for (int i = 0; i < descs.descCount(); ++i) {
        // The OBJ_SEG descriptor carries the NIXL block identifier in metaInfo;
        // take it VERBATIM as the opaque inline key (same mapping as registerMem).
        std::vector<uint8_t> key;
        if (!spdkKvKeyFromBlobId(descs[i].metaInfo, maxKeyLen_, key)) {
            NIXL_ERROR << "SPDK: invalid KV key in metaInfo (empty or > "
                       << static_cast<unsigned>(maxKeyLen_) << " bytes) in queryMem descriptor "
                       << i;
            return NIXL_ERR_INVALID_PARAM;
        }

        int rc = spdk_shim_exist(shim_, key.data(), static_cast<uint8_t>(key.size()));
        if (rc == 0) {
            // Hit.
            resp[i] = nixl_query_resp_t{nixl_b_params_t{}};
        } else if (rc == SPDK_SHIM_SC_KEY_DOES_NOT_EXIST) {
            // Miss. Leave resp[i] as std::nullopt.
            resp[i] = std::nullopt;
        } else {
            // Positive device sc other than KEY_DOES_NOT_EXIST, or a negated
            // errno: a real error, NOT a miss.
            NIXL_ERROR << "SPDK: spdk_shim_exist failed for descriptor " << i << ": rc=" << rc;
            return NIXL_ERR_BACKEND;
        }
    }

    return NIXL_SUCCESS;
}

size_t
nixlSpdkEngine::getReqTrueLen(nixlBackendReqH *handle, int idx) const {
    if (!handle || idx < 0) {
        return 0;
    }
    const auto *req_h = static_cast<const nixlSpdkBackendReqH *>(handle);
    // A too-small READ records exactly one descriptor's true length. Return it
    // only for that descriptor; every other index (and the no-mismatch case,
    // true_len_desc == -1) reports 0, matching the old per-descriptor semantics.
    return (idx == req_h->true_len_desc) ? req_h->true_len : 0;
}

uint32_t
nixlSpdkEngine::blockSectorSize() const {
    return shim_ ? spdk_shim_sector_size(shim_) : 0;
}

bool
nixlSpdkEngine::dramIsDmaRegistered(const nixlBackendMD *md) const {
    const auto *dram = dynamic_cast<const nixlSpdkDramMD *>(md);
    return dram != nullptr && dram->dma_registered;
}
