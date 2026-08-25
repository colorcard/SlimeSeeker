/*
 * CPU 搜索引擎实现。
 *
 * 负责 tile 划分、SAT 构建、动态多线程调度、有界结果队列、进度和取消；领域数学
 * 由 core 提供，区块位图生成由 backends 提供，本层不暴露平台相关指令细节。
 */
#include "engine/search.hpp"
#include "core/domain.hpp"
#include "backends/backend.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ss {
namespace {

struct BatchQueue {
    // worker 只向队列提交批次；用户回调由调用线程串行执行。
    // 有界容量既限制低阈值搜索的瞬时内存，也为慢消费者提供背压。
    std::mutex mutex;
    std::condition_variable readable;
    std::condition_variable writable;
    std::deque<std::vector<ss_result>> batches;
    size_t limit = 2;
};

struct Shared {
    const ss_search_params_v1 &params;
    BuildMapFn build_map;
    uint64_t tiles_x;
    uint64_t tiles_z;
    uint64_t tile_count;
    uint32_t batch_capacity;
    BatchQueue queue;
    std::atomic<uint64_t> next_tile{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<unsigned> active{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    Shared(const ss_search_params_v1 &p, BuildMapFn fn, uint64_t tx, uint64_t tz,
           uint64_t count, uint32_t capacity)
        : params(p), build_map(fn), tiles_x(tx), tiles_z(tz), tile_count(count),
          batch_capacity(capacity) {}
};

uint16_t rectangle_sum(const uint16_t *sat, size_t stride,
                       int z0, int x0, int z1, int x1) {
    // SAT 总值可以超过 uint16_t，但查询窗口至多 289 格；四项差分在模 2^16 下仍精确。
    return static_cast<uint16_t>(sat[static_cast<size_t>(z1) * stride + x1]
        - sat[static_cast<size_t>(z0) * stride + x1]
        - sat[static_cast<size_t>(z1) * stride + x0]
        + sat[static_cast<size_t>(z0) * stride + x0]);
}

bool submit(Shared &shared, std::vector<ss_result> &batch) {
    if (batch.empty()) return true;
    std::unique_lock lock(shared.queue.mutex);
    shared.queue.writable.wait(lock, [&] {
        return shared.stop.load(std::memory_order_relaxed) || shared.queue.batches.size() < shared.queue.limit;
    });
    if (shared.stop.load(std::memory_order_relaxed)) return false;
    shared.queue.batches.emplace_back();
    // 交换而非复制批次，worker 随后重新预留自己的下一批缓冲。
    shared.queue.batches.back().swap(batch);
    lock.unlock();
    shared.queue.readable.notify_one();
    batch.reserve(shared.batch_capacity);
    return true;
}

void process_tile(Shared &shared, uint64_t tile_index, std::vector<ss_result> &batch) {
    const auto &p = shared.params;
    const uint64_t tile_z = tile_index / shared.tiles_x;
    const uint64_t tile_x = tile_index % shared.tiles_x;
    const int64_t bx64 = static_cast<int64_t>(p.x_begin) + static_cast<int64_t>(tile_x * kTileCenters);
    const int64_t bz64 = static_cast<int64_t>(p.z_begin) + static_cast<int64_t>(tile_z * kTileCenters);
    const int cx = static_cast<int>(std::min<int64_t>(kTileCenters, static_cast<int64_t>(p.x_end) - bx64));
    const int cz = static_cast<int>(std::min<int64_t>(kTileCenters, static_cast<int64_t>(p.z_end) - bz64));
    const int mw = cx + 2 * kRadius;
    const int mh = cz + 2 * kRadius;
    const int32_t bx = static_cast<int32_t>(bx64);
    const int32_t bz = static_cast<int32_t>(bz64);

    // (bx,bz) 是第一个候选中心；位图向左上扩展 8 格，保证边缘中心也有完整窗口。
    std::vector<uint8_t> map(static_cast<size_t>(mw) * mh);
    std::vector<uint16_t> sat(static_cast<size_t>(mw + 1) * (mh + 1), 0);
    shared.build_map(p.world_seed, bx - kRadius, bz - kRadius, mw, mh, map.data());

    const size_t stride = static_cast<size_t>(mw + 1);
    // 融合行前缀与上一行 SAT，避免额外保存中间行前缀数组。
    for (int z = 0; z < mh; ++z) {
        uint16_t row_sum = 0;
        for (int x = 0; x < mw; ++x) {
            row_sum = static_cast<uint16_t>(row_sum + map[static_cast<size_t>(z) * mw + x]);
            sat[static_cast<size_t>(z + 1) * stride + x + 1] =
                static_cast<uint16_t>(sat[static_cast<size_t>(z) * stride + x + 1] + row_sum);
        }
    }

    const auto &runs = donut_runs();
    for (int z = 0; z < cz && !shared.stop.load(std::memory_order_relaxed); ++z) {
        for (int x = 0; x < cx; ++x) {
            // 圆环是 17×17 方框的子集；方框尚未达到阈值时可安全跳过 20 段精确查询。
            if (rectangle_sum(sat.data(), stride, z, x, z + kWindow, x + kWindow) < p.threshold) continue;
            uint16_t count = 0;
            for (const auto &run : runs) {
                count = static_cast<uint16_t>(count + rectangle_sum(sat.data(), stride,
                    z + run.row, x + run.first, z + run.row + 1, x + run.last + 1));
            }
            if (count >= p.threshold) {
                batch.push_back(ss_result{bx + x, bz + z, count, 0});
                if (batch.size() >= shared.batch_capacity && !submit(shared, batch)) return;
            }
        }
    }
    shared.completed.fetch_add(static_cast<uint64_t>(cx) * static_cast<uint64_t>(cz), std::memory_order_relaxed);
}

void worker(Shared &shared) noexcept {
    try {
        std::vector<ss_result> batch;
        batch.reserve(shared.batch_capacity);
        while (!shared.stop.load(std::memory_order_relaxed)) {
            // 动态领取 tile，避免边缘 tile 和不同命中密度造成静态分片尾部失衡。
            const uint64_t tile = shared.next_tile.fetch_add(1, std::memory_order_relaxed);
            if (tile >= shared.tile_count) break;
            process_tile(shared, tile, batch);
        }
        submit(shared, batch);
    } catch (...) {
        shared.failed.store(true, std::memory_order_relaxed);
        shared.stop.store(true, std::memory_order_relaxed);
    }
    shared.active.fetch_sub(1, std::memory_order_release);
    shared.queue.readable.notify_all();
    shared.queue.writable.notify_all();
}

} // namespace

ss_status search_impl(const ss_search_params_v1 &params, const ss_search_options_v1 &options,
                      const ss_callbacks_v1 &callbacks) {
    ss_backend selected{};
    BuildMapFn build_map = select_backend(options.backend, selected);
    if (!build_map) return SS_BACKEND_UNAVAILABLE;

    const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(params.x_end) - params.x_begin);
    const uint64_t height = static_cast<uint64_t>(static_cast<int64_t>(params.z_end) - params.z_begin);
    const uint64_t total = width * height;
    const uint64_t tiles_x = (width + kTileCenters - 1) / kTileCenters;
    const uint64_t tiles_z = (height + kTileCenters - 1) / kTileCenters;
    const uint64_t tile_count = tiles_x * tiles_z;
    if (tile_count == 0) {
        if (callbacks.on_progress) callbacks.on_progress(callbacks.context, 0, 0);
        return SS_OK;
    }

    unsigned threads = options.threads ? options.threads : std::thread::hardware_concurrency();
    if (threads == 0) threads = 1;
    threads = static_cast<unsigned>(std::min<uint64_t>(threads, tile_count));
    Shared shared{params, build_map, tiles_x, tiles_z, tile_count,
                  options.result_batch_capacity ? options.result_batch_capacity : 1024};
    shared.queue.limit = std::max<size_t>(2, static_cast<size_t>(threads) * 2);
    std::vector<std::thread> workers;
    workers.reserve(threads);
    try {
        for (unsigned i = 0; i < threads; ++i) {
            shared.active.fetch_add(1, std::memory_order_relaxed);
            try {
                workers.emplace_back(worker, std::ref(shared));
            } catch (...) {
                shared.active.fetch_sub(1, std::memory_order_relaxed);
                throw;
            }
        }
    } catch (...) {
        shared.stop.store(true, std::memory_order_relaxed);
        shared.queue.writable.notify_all();
        for (auto &thread : workers) thread.join();
        return SS_OUT_OF_MEMORY;
    }

    ss_status status = SS_OK;
    uint64_t last_progress = UINT64_MAX;
    // 调用线程同时负责消费结果、轮询取消和发送进度，因此所有用户回调天然串行。
    for (;;) {
        if (callbacks.should_cancel && callbacks.should_cancel(callbacks.context)) {
            status = SS_CANCELLED;
            shared.stop.store(true, std::memory_order_relaxed);
            shared.queue.writable.notify_all();
        }
        std::vector<ss_result> batch;
        {
            std::unique_lock lock(shared.queue.mutex);
            shared.queue.readable.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return !shared.queue.batches.empty() || shared.active.load(std::memory_order_acquire) == 0;
            });
            if (!shared.queue.batches.empty()) {
                batch.swap(shared.queue.batches.front());
                shared.queue.batches.pop_front();
            } else if (shared.active.load(std::memory_order_acquire) == 0) {
                break;
            }
        }
        shared.queue.writable.notify_one();
        if (!batch.empty() && status == SS_OK && callbacks.on_results &&
            callbacks.on_results(callbacks.context, batch.data(), batch.size()) != 0) {
            status = SS_CALLBACK_ABORTED;
            shared.stop.store(true, std::memory_order_relaxed);
            shared.queue.writable.notify_all();
        }
        const uint64_t done = shared.completed.load(std::memory_order_relaxed);
        if (callbacks.on_progress && done != last_progress) {
            callbacks.on_progress(callbacks.context, done, total);
            last_progress = done;
        }
    }
    for (auto &thread : workers) thread.join();
    if (shared.failed.load(std::memory_order_relaxed)) return SS_OUT_OF_MEMORY;
    if (callbacks.on_progress && status == SS_OK && last_progress != total)
        callbacks.on_progress(callbacks.context, total, total);
    return status;
}

} // namespace ss
