#include "backends/backend.hpp"
#include "core/domain.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ss {
namespace {

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
        uint32_t bits = static_cast<uint32_t>(state >> 17);
        if (bits < 0x80000000u - (0x80000000u % 10u)) return bits % 10u == 0;
    }
}

__global__ void build_fused_prefix(int64_t seed, int32_t x0, int32_t z0, int mw, int mh,
                                   uint16_t *prefix) {
    const int z = blockIdx.x;
    const int x = threadIdx.x;
    if (z >= mh) return;
    __shared__ uint16_t scan[512];
    if (x >= mw) {
        scan[x] = 0;
    } else {
        const int32_t world_x = x0 + x;
        const int32_t world_z = z0 + z;
        const uint64_t chunk = (d_zbase(seed, world_z) + d_xterm(world_x)) ^ 987234911ULL;
        scan[x] = static_cast<uint16_t>(d_slime(chunk));
    }
    __syncthreads();
    for (int offset = 1; offset < 512; offset <<= 1) {
        uint16_t add = x >= offset ? scan[x - offset] : 0;
        __syncthreads();
        scan[x] = static_cast<uint16_t>(scan[x] + add);
        __syncthreads();
    }
    if (x == 0) prefix[z * (mw + 1)] = 0;
    if (x < mw) prefix[z * (mw + 1) + x + 1] = scan[x];
}

__global__ void count_donut_prefix(const uint16_t *prefix, int stride, int cx, int cz, int32_t bx, int32_t bz,
                            uint16_t threshold, ss_result *out, unsigned *out_count) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= cx * cz) return;
    const int x = i % cx;
    const int z = i / cx;
    unsigned count = 0;
    for (int r = 0; r < 20; ++r) {
        const uint16_t *row = prefix + (z + c_rows[r]) * stride;
        count += row[x + c_last[r] + 1] - row[x + c_first[r]];
    }
    if (count >= threshold) {
        const unsigned pos = atomicAdd(out_count, 1u);
        if (pos < 262144u) out[pos] = ss_result{bx + x, bz + z, static_cast<uint16_t>(count), 0};
    }
}

bool check(cudaError_t e) { return e == cudaSuccess; }

} // namespace

bool cuda_available() {
    int n = 0;
    return check(cudaGetDeviceCount(&n)) && n > 0;
}

ss_status search_cuda(const ss_search_params_v1 &p, const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks) {
    if (!cuda_available()) return SS_BACKEND_UNAVAILABLE;
    cudaSetDevice(0);
    const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(p.x_end) - p.x_begin);
    const uint64_t height = static_cast<uint64_t>(static_cast<int64_t>(p.z_end) - p.z_begin);
    const uint64_t total = width * height;
    if (!total) { if (callbacks.on_progress) callbacks.on_progress(callbacks.context, 0, 0); return SS_OK; }

    std::vector<uint8_t> hr(20), hf(20), hl(20);
    const auto &runs = donut_runs();
    for (size_t i = 0; i < runs.size(); ++i) { hr[i] = runs[i].row; hf[i] = runs[i].first; hl[i] = runs[i].last; }
    uint16_t *dprefix = nullptr;
    ss_result *dout = nullptr; unsigned *dcount = nullptr;
    ss_result *host = nullptr;
    const size_t max_results = static_cast<size_t>(kTileCenters) * kTileCenters;
    if (!check(cudaMalloc(&dprefix, static_cast<size_t>(kTileMap) * (kTileMap + 1) * sizeof(uint16_t))) ||
        !check(cudaMalloc(&dout, max_results * sizeof(ss_result))) || !check(cudaMalloc(&dcount, sizeof(unsigned))) ||
        !check(cudaMallocHost(&host, max_results * sizeof(ss_result)))) {
        cudaFree(dprefix); cudaFree(dout); cudaFree(dcount); cudaFreeHost(host);
        return SS_OUT_OF_MEMORY;
    }
    if (!check(cudaMemcpyToSymbol(c_rows, hr.data(), 20)) ||
        !check(cudaMemcpyToSymbol(c_first, hf.data(), 20)) ||
        !check(cudaMemcpyToSymbol(c_last, hl.data(), 20))) {
        cudaFree(dprefix); cudaFree(dout); cudaFree(dcount); cudaFreeHost(host);
        return SS_INTERNAL_ERROR;
    }

    const uint32_t capacity = options.result_batch_capacity ? options.result_batch_capacity : 1024;
    std::vector<ss_result> batch;
    batch.reserve(capacity);
    ss_status status = SS_OK;
    uint64_t completed = 0;
    for (int64_t bz = p.z_begin; bz < p.z_end && status == SS_OK; bz += kTileCenters) {
        for (int64_t bx = p.x_begin; bx < p.x_end && status == SS_OK; bx += kTileCenters) {
            if (callbacks.should_cancel && callbacks.should_cancel(callbacks.context)) { status = SS_CANCELLED; break; }
            const int cx = static_cast<int>(std::min<int64_t>(kTileCenters, p.x_end - bx));
            const int cz = static_cast<int>(std::min<int64_t>(kTileCenters, p.z_end - bz));
            const int mw = cx + 2 * kRadius, mh = cz + 2 * kRadius;
            build_fused_prefix<<<mh, 512>>>(p.world_seed, static_cast<int32_t>(bx - kRadius),
                                             static_cast<int32_t>(bz - kRadius), mw, mh, dprefix);
            if (!check(cudaGetLastError())) { status = SS_INTERNAL_ERROR; break; }
            cudaMemset(dcount, 0, sizeof(unsigned));
            count_donut_prefix<<<(cx * cz + 255) / 256, 256>>>(dprefix, mw + 1, cx, cz, static_cast<int32_t>(bx),
                                                        static_cast<int32_t>(bz), p.threshold, dout, dcount);
            if (!check(cudaGetLastError()) || !check(cudaDeviceSynchronize())) { status = SS_INTERNAL_ERROR; break; }
            unsigned found = 0;
            if (!check(cudaMemcpy(&found, dcount, sizeof(found), cudaMemcpyDeviceToHost))) { status = SS_INTERNAL_ERROR; break; }
            found = std::min<unsigned>(found, static_cast<unsigned>(max_results));
            if (found && !check(cudaMemcpy(host, dout, found * sizeof(ss_result), cudaMemcpyDeviceToHost))) { status = SS_INTERNAL_ERROR; break; }
            // The GPU kernel emits all positive donut counts; apply the requested threshold on host
            // to keep the kernel independent of API policy and preserve exact uint16 semantics.
            batch.clear();
            batch.reserve(std::min<size_t>(found, capacity));
            for (unsigned i = 0; i < found && status == SS_OK; ++i) {
                if (host[i].count < p.threshold) continue;
                batch.push_back(host[i]);
                if (batch.size() == capacity) {
                    if (callbacks.on_results && callbacks.on_results(callbacks.context, batch.data(), batch.size()) != 0) status = SS_CALLBACK_ABORTED;
                    batch.clear();
                }
            }
            if (status == SS_OK && !batch.empty() && callbacks.on_results && callbacks.on_results(callbacks.context, batch.data(), batch.size()) != 0) status = SS_CALLBACK_ABORTED;
            completed += static_cast<uint64_t>(cx) * static_cast<uint64_t>(cz);
            if (callbacks.on_progress) callbacks.on_progress(callbacks.context, completed, total);
        }
    }
    cudaFree(dprefix); cudaFree(dout); cudaFree(dcount); cudaFreeHost(host);
    return status;
}

} // namespace ss
