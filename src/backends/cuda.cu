/*
 * CUDA 完整搜索后端。
 *
 * 固定数量 tile 通过二维 grid 合并提交，两个独立 stream slot 重叠 GPU 计算、结果回传
 * 和调用线程消费。设备及锁页缓冲由进程级单 workspace 有界复用，公共回调仍只在调用
 * ss_search 的线程串行触发。
 */
#include "backends/backend.hpp"
#include "core/domain.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>

#if __has_include(<nvtx3/nvToolsExt.h>)
#include <nvtx3/nvToolsExt.h>
#define SS_HAS_NVTX3 1
#else
#define SS_HAS_NVTX3 0
#endif

namespace ss {
namespace {

constexpr int kTilesPerBatch = 4;
constexpr int kPipelineSlots = 2;
constexpr int kCountThreads = 256;
constexpr size_t kPrefixElements = static_cast<size_t>(kTileMap) * (kTileMap + 1);
constexpr size_t kMaxResults = static_cast<size_t>(kTileCenters) * kTileCenters;

static_assert(sizeof(ss_result) == 12);

struct NvtxRange {
    explicit NvtxRange(const char *name) {
#if SS_HAS_NVTX3
        nvtxRangePushA(name);
#else
        (void)name;
#endif
    }
    ~NvtxRange() {
#if SS_HAS_NVTX3
        nvtxRangePop();
#endif
    }
};

struct TileDescriptor {
    int32_t bx;
    int32_t bz;
    int32_t cx;
    int32_t cz;
    int32_t mw;
    int32_t mh;
};

__constant__ uint8_t c_rows[20];
__constant__ uint8_t c_first[20];
__constant__ uint8_t c_last[20];

__device__ __forceinline__ int32_t d_mul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

__device__ __forceinline__ uint64_t d_xterm(int32_t x) {
    return static_cast<uint64_t>(static_cast<int64_t>(d_mul32(d_mul32(x, x), 4987142))) +
           static_cast<uint64_t>(static_cast<int64_t>(d_mul32(x, 5947611)));
}

__device__ __forceinline__ uint64_t d_zbase(int64_t seed, int32_t z) {
    return static_cast<uint64_t>(seed) +
           static_cast<uint64_t>(static_cast<int64_t>(d_mul32(z, z)) * 4392871LL) +
           static_cast<uint64_t>(static_cast<int64_t>(d_mul32(z, 389711)));
}

__device__ __forceinline__ bool d_slime(uint64_t chunk) {
    uint64_t state = (chunk ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
    for (;;) {
        state = (state * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        const uint32_t bits = static_cast<uint32_t>(state >> 17);
        if (bits < 0x80000000u - (0x80000000u % 10u)) return bits % 10u == 0;
    }
}

__global__ void build_fused_prefix_batch(int64_t seed, const TileDescriptor *descriptors,
                                         uint16_t *prefix) {
    const int tile = static_cast<int>(blockIdx.y);
    const int z = static_cast<int>(blockIdx.x);
    const int x = static_cast<int>(threadIdx.x);
    const TileDescriptor descriptor = descriptors[tile];
    if (z >= descriptor.mh) return;

    __shared__ uint16_t scan[kTileMap];
    if (x >= descriptor.mw) {
        scan[x] = 0;
    } else {
        const int32_t world_x = descriptor.bx - kRadius + x;
        const int32_t world_z = descriptor.bz - kRadius + z;
        const uint64_t chunk =
            (d_zbase(seed, world_z) + d_xterm(world_x)) ^ 987234911ULL;
        scan[x] = static_cast<uint16_t>(d_slime(chunk));
    }
    __syncthreads();
    for (int offset = 1; offset < kTileMap; offset <<= 1) {
        const uint16_t add = x >= offset ? scan[x - offset] : 0;
        __syncthreads();
        scan[x] = static_cast<uint16_t>(scan[x] + add);
        __syncthreads();
    }

    uint16_t *tile_prefix = prefix + static_cast<size_t>(tile) * kPrefixElements;
    const size_t row = static_cast<size_t>(z) * (kTileMap + 1);
    if (x == 0) tile_prefix[row] = 0;
    if (x < descriptor.mw) tile_prefix[row + x + 1] = scan[x];
}

__global__ void count_donut_prefix_batch(const TileDescriptor *descriptors,
                                         const uint16_t *prefix, uint16_t threshold,
                                         ss_result *results, unsigned *counts) {
    const int tile = static_cast<int>(blockIdx.y);
    const int i = static_cast<int>(blockIdx.x) * blockDim.x + threadIdx.x;
    const TileDescriptor descriptor = descriptors[tile];
    const int candidates = descriptor.cx * descriptor.cz;
    if (i >= candidates) return;

    const int x = i % descriptor.cx;
    const int z = i / descriptor.cx;
    const uint16_t *tile_prefix = prefix + static_cast<size_t>(tile) * kPrefixElements;
    unsigned count = 0;
    for (int r = 0; r < 20; ++r) {
        const uint16_t *row = tile_prefix +
            static_cast<size_t>(z + c_rows[r]) * (kTileMap + 1);
        count += row[x + c_last[r] + 1] - row[x + c_first[r]];
    }
    if (count >= threshold) {
        const unsigned position = atomicAdd(counts + tile, 1u);
        if (position < kMaxResults) {
            results[static_cast<size_t>(tile) * kMaxResults + position] =
                ss_result{descriptor.bx + x, descriptor.bz + z,
                          static_cast<uint16_t>(count), 0};
        }
    }
}

ss_status status_from_cuda(cudaError_t error) {
    if (error == cudaSuccess) return SS_OK;
    if (error == cudaErrorMemoryAllocation) return SS_OUT_OF_MEMORY;
    return SS_INTERNAL_ERROR;
}

struct PipelineSlot {
    cudaStream_t stream = nullptr;
    cudaEvent_t counts_ready = nullptr;
    cudaEvent_t results_ready = nullptr;
    TileDescriptor *device_descriptors = nullptr;
    uint16_t *device_prefix = nullptr;
    ss_result *device_results = nullptr;
    unsigned *device_counts = nullptr;
    TileDescriptor *host_descriptors = nullptr;
    ss_result *host_results = nullptr;
    unsigned *host_counts = nullptr;
    int tile_count = 0;
    bool in_flight = false;

    cudaError_t initialize() {
        cudaError_t error = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
        if (error != cudaSuccess) return error;
        error = cudaEventCreateWithFlags(&counts_ready, cudaEventDisableTiming);
        if (error != cudaSuccess) return error;
        error = cudaEventCreateWithFlags(&results_ready, cudaEventDisableTiming);
        if (error != cudaSuccess) return error;
        error = cudaMalloc(&device_descriptors, kTilesPerBatch * sizeof(TileDescriptor));
        if (error != cudaSuccess) return error;
        error = cudaMalloc(&device_prefix,
                           kTilesPerBatch * kPrefixElements * sizeof(uint16_t));
        if (error != cudaSuccess) return error;
        error = cudaMalloc(&device_results,
                           kTilesPerBatch * kMaxResults * sizeof(ss_result));
        if (error != cudaSuccess) return error;
        error = cudaMalloc(&device_counts, kTilesPerBatch * sizeof(unsigned));
        if (error != cudaSuccess) return error;
        error = cudaMallocHost(&host_descriptors, kTilesPerBatch * sizeof(TileDescriptor));
        if (error != cudaSuccess) return error;
        error = cudaMallocHost(&host_results,
                               kTilesPerBatch * kMaxResults * sizeof(ss_result));
        if (error != cudaSuccess) return error;
        return cudaMallocHost(&host_counts, kTilesPerBatch * sizeof(unsigned));
    }

    void destroy() noexcept {
        if (stream) cudaStreamSynchronize(stream);
        if (host_counts) cudaFreeHost(host_counts);
        if (host_results) cudaFreeHost(host_results);
        if (host_descriptors) cudaFreeHost(host_descriptors);
        if (device_counts) cudaFree(device_counts);
        if (device_results) cudaFree(device_results);
        if (device_prefix) cudaFree(device_prefix);
        if (device_descriptors) cudaFree(device_descriptors);
        if (results_ready) cudaEventDestroy(results_ready);
        if (counts_ready) cudaEventDestroy(counts_ready);
        if (stream) cudaStreamDestroy(stream);
        *this = {};
    }
};

struct CudaWorkspace {
    PipelineSlot slots[kPipelineSlots];

    ~CudaWorkspace() {
        for (auto &slot : slots) slot.destroy();
    }

    cudaError_t initialize() {
        const auto &runs = donut_runs();
        uint8_t rows[20], first[20], last[20];
        for (size_t i = 0; i < runs.size(); ++i) {
            rows[i] = runs[i].row;
            first[i] = runs[i].first;
            last[i] = runs[i].last;
        }
        cudaError_t error = cudaMemcpyToSymbol(c_rows, rows, sizeof(rows));
        if (error != cudaSuccess) return error;
        error = cudaMemcpyToSymbol(c_first, first, sizeof(first));
        if (error != cudaSuccess) return error;
        error = cudaMemcpyToSymbol(c_last, last, sizeof(last));
        if (error != cudaSuccess) return error;
        for (auto &slot : slots) {
            error = slot.initialize();
            if (error != cudaSuccess) return error;
        }
        return cudaSuccess;
    }
};

class WorkspaceManager {
public:
    bool available() {
        std::lock_guard lock(mutex_);
        if (!availability_checked_) {
            int count = 0;
            available_ = cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
            availability_checked_ = true;
        }
        return available_;
    }

    ss_status acquire(const ss_callbacks_v1 &callbacks, CudaWorkspace *&workspace) {
        std::unique_lock lock(mutex_);
        while (leased_) {
            lock.unlock();
            if (callbacks.should_cancel && callbacks.should_cancel(callbacks.context))
                return SS_CANCELLED;
            lock.lock();
            condition_.wait_for(lock, std::chrono::milliseconds(10), [&] { return !leased_; });
        }
        leased_ = true;
        if (!workspace_) {
            lock.unlock();
            NvtxRange range{"slimeseeker.cuda.initialize"};
            std::unique_ptr<CudaWorkspace> candidate{
                new (std::nothrow) CudaWorkspace()};
            if (!candidate) {
                lock.lock();
                leased_ = false;
                lock.unlock();
                condition_.notify_one();
                return SS_OUT_OF_MEMORY;
            }
            const cudaError_t error = candidate->initialize();
            lock.lock();
            if (error != cudaSuccess) {
                leased_ = false;
                lock.unlock();
                condition_.notify_one();
                return status_from_cuda(error);
            }
            workspace_ = std::move(candidate);
        }
        workspace = workspace_.get();
        return SS_OK;
    }

    void release(bool healthy) noexcept {
        std::unique_ptr<CudaWorkspace> stale;
        {
            std::lock_guard lock(mutex_);
            if (!healthy) stale = std::move(workspace_);
            leased_ = false;
        }
        condition_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::unique_ptr<CudaWorkspace> workspace_;
    bool availability_checked_ = false;
    bool available_ = false;
    bool leased_ = false;
};

WorkspaceManager &workspace_manager() {
    static WorkspaceManager manager;
    return manager;
}

thread_local bool cuda_search_active = false;

struct SearchActivityGuard {
    SearchActivityGuard() { cuda_search_active = true; }
    ~SearchActivityGuard() { cuda_search_active = false; }
};

struct WorkspaceLease {
    WorkspaceManager *manager = nullptr;
    bool healthy = true;
    ~WorkspaceLease() {
        if (manager) manager->release(healthy);
    }
};

bool cancelled(const ss_callbacks_v1 &callbacks) {
    return callbacks.should_cancel && callbacks.should_cancel(callbacks.context);
}

void fill_descriptor(TileDescriptor &descriptor, const ss_search_params_v1 &params,
                     uint64_t tiles_x, uint64_t tile_index) {
    const uint64_t tile_z = tile_index / tiles_x;
    const uint64_t tile_x = tile_index % tiles_x;
    const int64_t bx = static_cast<int64_t>(params.x_begin) +
        static_cast<int64_t>(tile_x * kTileCenters);
    const int64_t bz = static_cast<int64_t>(params.z_begin) +
        static_cast<int64_t>(tile_z * kTileCenters);
    descriptor.bx = static_cast<int32_t>(bx);
    descriptor.bz = static_cast<int32_t>(bz);
    descriptor.cx = static_cast<int32_t>(
        std::min<int64_t>(kTileCenters, static_cast<int64_t>(params.x_end) - bx));
    descriptor.cz = static_cast<int32_t>(
        std::min<int64_t>(kTileCenters, static_cast<int64_t>(params.z_end) - bz));
    descriptor.mw = descriptor.cx + 2 * kRadius;
    descriptor.mh = descriptor.cz + 2 * kRadius;
}

ss_status submit_batch(PipelineSlot &slot, const ss_search_params_v1 &params,
                       uint64_t tiles_x, uint64_t tile_count, uint64_t &next_tile) {
    NvtxRange range{"slimeseeker.cuda.submit"};
    slot.tile_count = static_cast<int>(
        std::min<uint64_t>(kTilesPerBatch, tile_count - next_tile));
    for (int tile = 0; tile < slot.tile_count; ++tile)
        fill_descriptor(slot.host_descriptors[tile], params, tiles_x, next_tile++);

    cudaError_t error = cudaMemcpyAsync(
        slot.device_descriptors, slot.host_descriptors,
        static_cast<size_t>(slot.tile_count) * sizeof(TileDescriptor),
        cudaMemcpyHostToDevice, slot.stream);
    if (error != cudaSuccess) return status_from_cuda(error);
    error = cudaMemsetAsync(slot.device_counts, 0,
                            static_cast<size_t>(slot.tile_count) * sizeof(unsigned), slot.stream);
    if (error != cudaSuccess) return status_from_cuda(error);

    build_fused_prefix_batch<<<dim3(kTileMap, slot.tile_count), kTileMap, 0, slot.stream>>>(
        params.world_seed, slot.device_descriptors, slot.device_prefix);
    error = cudaGetLastError();
    if (error != cudaSuccess) return status_from_cuda(error);
    count_donut_prefix_batch<<<
        dim3((kMaxResults + kCountThreads - 1) / kCountThreads, slot.tile_count),
        kCountThreads, 0, slot.stream>>>(
        slot.device_descriptors, slot.device_prefix, params.threshold,
        slot.device_results, slot.device_counts);
    error = cudaGetLastError();
    if (error != cudaSuccess) return status_from_cuda(error);

    error = cudaMemcpyAsync(slot.host_counts, slot.device_counts,
                            static_cast<size_t>(slot.tile_count) * sizeof(unsigned),
                            cudaMemcpyDeviceToHost, slot.stream);
    if (error != cudaSuccess) return status_from_cuda(error);
    error = cudaEventRecord(slot.counts_ready, slot.stream);
    if (error != cudaSuccess) return status_from_cuda(error);
    slot.in_flight = true;
    return SS_OK;
}

ss_status drain_batch(PipelineSlot &slot, const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks, uint64_t total,
                      uint64_t &completed) {
    cudaError_t error;
    {
        NvtxRange range{"slimeseeker.cuda.wait_counts"};
        error = cudaEventSynchronize(slot.counts_ready);
        if (error != cudaSuccess) return status_from_cuda(error);
    }

    bool has_results = false;
    {
        NvtxRange range{"slimeseeker.cuda.d2h_results"};
        for (int tile = 0; tile < slot.tile_count; ++tile) {
            const auto &descriptor = slot.host_descriptors[tile];
            const unsigned candidates =
                static_cast<unsigned>(descriptor.cx * descriptor.cz);
            if (slot.host_counts[tile] > candidates || slot.host_counts[tile] > kMaxResults)
                return SS_INTERNAL_ERROR;
            if (callbacks.on_results && slot.host_counts[tile] != 0) {
                has_results = true;
                error = cudaMemcpyAsync(
                    slot.host_results + static_cast<size_t>(tile) * kMaxResults,
                    slot.device_results + static_cast<size_t>(tile) * kMaxResults,
                    static_cast<size_t>(slot.host_counts[tile]) * sizeof(ss_result),
                    cudaMemcpyDeviceToHost, slot.stream);
                if (error != cudaSuccess) return status_from_cuda(error);
            }
        }
        if (has_results) {
            error = cudaEventRecord(slot.results_ready, slot.stream);
            if (error != cudaSuccess) return status_from_cuda(error);
            error = cudaEventSynchronize(slot.results_ready);
            if (error != cudaSuccess) return status_from_cuda(error);
        }
    }

    const size_t capacity = options.result_batch_capacity
        ? options.result_batch_capacity : 1024;
    NvtxRange callback_range{"slimeseeker.cuda.callback"};
    for (int tile = 0; tile < slot.tile_count; ++tile) {
        if (cancelled(callbacks)) return SS_CANCELLED;
        const auto *results =
            slot.host_results + static_cast<size_t>(tile) * kMaxResults;
        size_t offset = 0;
        while (callbacks.on_results && offset < slot.host_counts[tile]) {
            const size_t count = std::min<size_t>(
                capacity, static_cast<size_t>(slot.host_counts[tile]) - offset);
            if (callbacks.on_results(callbacks.context, results + offset, count) != 0)
                return SS_CALLBACK_ABORTED;
            offset += count;
        }
        const auto &descriptor = slot.host_descriptors[tile];
        completed += static_cast<uint64_t>(descriptor.cx) * descriptor.cz;
        if (callbacks.on_progress)
            callbacks.on_progress(callbacks.context, completed, total);
    }
    slot.in_flight = false;
    return SS_OK;
}

bool settle_workspace(CudaWorkspace &workspace) {
    bool healthy = true;
    for (auto &slot : workspace.slots) {
        if (slot.in_flight && cudaStreamSynchronize(slot.stream) != cudaSuccess)
            healthy = false;
        slot.in_flight = false;
    }
    return healthy;
}

} // namespace

bool cuda_available() {
    return workspace_manager().available();
}

ss_status search_cuda(const ss_search_params_v1 &params,
                      const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks) {
    NvtxRange search_range{"slimeseeker.cuda.search"};
    if (!cuda_available()) return SS_BACKEND_UNAVAILABLE;
    if (cuda_search_active) return SS_INTERNAL_ERROR;
    SearchActivityGuard activity;
    if (cudaSetDevice(0) != cudaSuccess) return SS_BACKEND_UNAVAILABLE;

    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(params.x_end) - params.x_begin);
    const uint64_t height = static_cast<uint64_t>(
        static_cast<int64_t>(params.z_end) - params.z_begin);
    const uint64_t total = width * height;
    if (total == 0) {
        if (callbacks.on_progress) callbacks.on_progress(callbacks.context, 0, 0);
        return SS_OK;
    }
    if (cancelled(callbacks)) return SS_CANCELLED;

    CudaWorkspace *workspace = nullptr;
    const ss_status acquire_status = workspace_manager().acquire(callbacks, workspace);
    if (acquire_status != SS_OK) return acquire_status;
    WorkspaceLease lease{&workspace_manager(), true};

    const uint64_t tiles_x = (width + kTileCenters - 1) / kTileCenters;
    const uint64_t tiles_z = (height + kTileCenters - 1) / kTileCenters;
    const uint64_t tile_count = tiles_x * tiles_z;
    uint64_t next_tile = 0;
    uint64_t completed = 0;
    int drain_slot = 0;
    int submitted_slots = 0;
    ss_status status = SS_OK;

    for (int slot = 0; slot < kPipelineSlots && next_tile < tile_count; ++slot) {
        status = submit_batch(workspace->slots[slot], params, tiles_x, tile_count, next_tile);
        if (status != SS_OK) break;
        ++submitted_slots;
    }

    while (status == SS_OK && submitted_slots > 0) {
        auto &slot = workspace->slots[drain_slot];
        status = drain_batch(slot, options, callbacks, total, completed);
        --submitted_slots;
        if (status == SS_OK && next_tile < tile_count) {
            status = submit_batch(slot, params, tiles_x, tile_count, next_tile);
            if (status == SS_OK) ++submitted_slots;
        }
        drain_slot = (drain_slot + 1) % kPipelineSlots;
    }

    const bool settled = settle_workspace(*workspace);
    if (!settled) {
        lease.healthy = false;
        if (status == SS_OK) status = SS_INTERNAL_ERROR;
    } else if (status == SS_INTERNAL_ERROR || status == SS_OUT_OF_MEMORY) {
        lease.healthy = false;
    }
    return status;
}

} // namespace ss
