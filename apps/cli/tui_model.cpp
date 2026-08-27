/* SlimeSeeker TUI 的输入、结果与导出模型实现。 */
#include "tui_model.hpp"

#include "core/domain.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <system_error>

namespace ss::cli {
namespace {

template<class T> bool parse_integer(std::string_view input, T &value) {
    if (input.empty()) return false;
    const char *begin = input.data();
    const char *end = begin + input.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && ptr == end;
}

constexpr uint64_t kMemoryWarningBytes = 512ull * 1024 * 1024;

uint64_t saturated_multiply(uint64_t a, uint64_t b) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        return std::numeric_limits<uint64_t>::max();
    return a * b;
}

} // namespace

std::string_view localized_text(Language language, TextKey key) {
    static constexpr std::string_view en[] = {
        "SlimeSeeker", "Search configuration", "World seed", "Range", "Threshold", "Threads",
        "Backend", "Result retention", "Top-K", "All results", "Start search", "Quit",
        "Search running", "Cancel", "Progress", "Candidates", "Hits", "Elapsed", "ETA",
        "Throughput", "Results", "Edit settings", "Run again", "Export CSV", "Rank", "Chunk X",
        "Chunk Z", "Count", "Distance squared", "Block bounds", "Retained", "Success",
        "Cancelled", "Failed", "Enter a signed 64-bit seed", "Range must be 1..2147483639",
        "Threshold must be 0..192", "Threads must be 0..1024", "Top-K must be 1..1000000",
        "Select a valid backend", "All-results mode can require up to {memory}. Continue?",
        "Confirm", "Dismiss", "An interactive terminal is required", "Export path", "Exporting",
        "Export completed", "Export failed", "This is a partial result set. Export it?",
        "The target already exists. Replace it?", "No matching chunks", "Sorting results",
        "Language", "Status", "Requested backend", "Partial results", "Page", "Previous", "Next",
        "Details", "Auto CPU", "Scalar", "AVX2", "NEON", "CUDA", "Top results",
        "Search mode", "Chunk density", "26.2 biome score", "Spawn feet Y", "Player feet Y",
        "Select a valid search mode", "Biome Top-K must be 1..1000",
        "Spawn Y must be -64..319", "Player Y must be -64..319", "Biome score",
        "Common-equivalent chunks", "AFK position", "AFK score", "Finding AFK points",
        "Slime chunks / AFK annulus", "AFK chunk", "Active slime", "Inactive slime",
        "Spawn area", "Boundary", "Outside annulus", "Equivalent", "Reset defaults",
        "Ranks centers by slime chunks in the fixed 192-chunk annulus.",
        "Re-scores every density match by 26.2 biomes, then finds an AFK point.",
        "Signed 64-bit Java world seed. The same seed always produces the same results.",
        "Searches center chunks in [-R, R) on both axes; work grows with (2R)^2.",
        "Minimum slime chunks required in the fixed annulus; valid range is 0..192.",
        "Worker count. Use 0 to select a suitable value from available CPU threads.",
        "Auto selects a verified CPU backend. CUDA is used only when selected explicitly.",
        "Top-K bounds memory; All results retains every density match.",
        "Keeps the globally best K density results after the entire search.",
        "Keeps the best K biome scores after re-scoring every density match.",
        "Feet Y of candidate slime spawns, used for biome and distance scoring.",
        "Player feet Y. The best X/Z is searched inside each retained center chunk.",
        "The configuration is valid and ready to run.", "Phase", "Searching chunk density",
        "Searching and scoring 26.2 biomes", "AFK candidates", "Cancel the search and quit?",
        "The search is incomplete; only completed results will be exported.",
        "Enter a non-empty export path", "Exported to", "No center met the threshold. Lower the threshold or increase the range.",
        "Worst-case memory"
    };
    static constexpr std::string_view zh[] = {
        "SlimeSeeker", "搜索配置", "世界种子", "搜索范围", "阈值", "线程数", "计算后端",
        "结果保留", "最佳 K 条", "全部结果", "开始搜索", "退出", "正在搜索", "取消",
        "进度", "候选数", "命中数", "已用时间", "预计剩余", "吞吐", "搜索结果", "修改参数",
        "重新搜索", "导出 CSV", "排名", "区块 X", "区块 Z", "计数", "距离平方", "方块边界",
        "已保留", "成功", "已取消", "失败", "请输入有符号 64 位种子",
        "搜索范围必须为 1..2147483639", "阈值必须为 0..192", "线程数必须为 0..1024",
        "最佳 K 条必须为 1..1000000", "请选择有效后端",
        "全部结果模式最坏可能占用 {memory}，是否继续？", "确认", "关闭",
        "需要交互式终端", "导出路径", "正在导出", "导出完成", "导出失败",
        "当前是部分结果，是否导出？", "目标文件已存在，是否替换？", "没有匹配区块",
        "正在整理结果", "语言", "状态", "请求后端", "部分结果", "页", "上一页", "下一页",
        "详情", "自动 CPU", "标量", "AVX2", "NEON", "CUDA", "最佳结果", "搜索模式",
        "区块密度", "26.2 群系评分", "生成脚部 Y", "玩家脚部 Y", "请选择有效搜索模式",
        "群系最佳 K 条必须为 1..1000", "生成 Y 必须为 -64..319",
        "玩家 Y 必须为 -64..319", "群系分", "普通群系等效区块", "挂机坐标",
        "挂机分", "正在寻找挂机点", "史莱姆区块 / 挂机圆环", "挂机区块",
        "有效史莱姆", "无效史莱姆", "可生成区", "圆环边界", "圆环外", "等效区块",
        "恢复默认值", "按固定 192 区块圆环中的史莱姆区块数量排序中心点。",
        "对全部密度命中按 26.2 群系重评分，再寻找挂机点。",
        "有符号 64 位 Java 世界种子；相同种子的结果始终一致。",
        "在两轴的 [-R, R) 搜索中心区块；工作量随 (2R)^2 增长。",
        "固定圆环内要求的最少史莱姆区块数，有效范围为 0..192。",
        "工作线程数；填写 0 会根据可用 CPU 线程自动选择。",
        "自动模式选择校验过的 CPU 后端；只有显式选择才会使用 CUDA。",
        "最佳 K 条可限制内存；全部结果会保留每一个密度命中。",
        "完成全部搜索后，保留全局最优的 K 个密度结果。",
        "对全部密度命中重评分后，保留群系分最高的 K 个结果。",
        "候选史莱姆生成位置的脚部 Y，用于群系与距离评分。",
        "玩家脚部 Y；每个保留结果会在中心区块内寻找最佳 X/Z。",
        "当前配置有效，可以开始搜索。", "当前阶段", "正在搜索区块密度",
        "正在搜索并计算 26.2 群系分", "挂机候选", "是否取消搜索并退出？",
        "搜索尚未完整结束，只会导出已经完成的结果。", "请输入非空导出路径",
        "已导出到", "没有中心达到阈值；可降低阈值或扩大搜索范围。", "最坏内存"
    };
    static_assert(std::size(en) == static_cast<size_t>(TextKey::count_));
    static_assert(std::size(zh) == static_cast<size_t>(TextKey::count_));
    const size_t index = static_cast<size_t>(key);
    if (index >= static_cast<size_t>(TextKey::count_)) return {};
    return language == Language::chinese ? zh[index] : en[index];
}

bool requires_memory_confirmation(const SearchRequest &request) {
    return request.retention == ResultRetention::all &&
           request.worst_result_bytes > kMemoryWarningBytes;
}

Language language_from_locale(std::string_view locale) {
    std::string normalized(locale);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return normalized.rfind("zh", 0) == 0 || normalized.find("chinese") != std::string::npos
        ? Language::chinese : Language::english;
}

ValidationResult validate_search_form(const SearchForm &form) {
    ValidationResult result;
    int64_t seed = 0;
    int64_t range = 0;
    unsigned threshold = 0;
    unsigned threads = 0;
    uint64_t top_k = 0;
    int32_t spawn_y = -63;
    int32_t player_y = -38;
    if (!parse_integer(form.seed, seed)) { result.error = ValidationError::seed; return result; }
    if (!parse_integer(form.range, range) || range <= 0 ||
        range > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) - 8) {
        result.error = ValidationError::range; return result;
    }
    if (!parse_integer(form.threshold, threshold) || threshold > SS_DONUT_CELLS) {
        result.error = ValidationError::threshold; return result;
    }
    if (!parse_integer(form.threads, threads) || threads > 1024) {
        result.error = ValidationError::threads; return result;
    }
    if (form.backend < 0 || form.backend > 4) {
        result.error = ValidationError::backend; return result;
    }
    if (form.mode < 0 || form.mode > 1) {
        result.error = ValidationError::mode; return result;
    }
    const auto mode = form.mode == 1 ? SearchMode::biome : SearchMode::density;
    const auto retention = form.retention == 1 ? ResultRetention::all : ResultRetention::top;
    const std::string &top_text = mode == SearchMode::biome ? form.biome_top_k : form.top_k;
    if ((mode == SearchMode::density && (form.retention < 0 || form.retention > 1)) ||
        ((mode == SearchMode::biome || retention == ResultRetention::top) &&
         (!parse_integer(top_text, top_k) || top_k == 0 ||
          top_k > (mode == SearchMode::biome ? 1000 : 1000000)))) {
        result.error = mode == SearchMode::biome
            ? ValidationError::biome_top_k : ValidationError::top_k;
        return result;
    }
    if (mode == SearchMode::biome) {
        if (!parse_integer(form.spawn_y, spawn_y) || spawn_y < -64 || spawn_y > 319) {
            result.error = ValidationError::spawn_y; return result;
        }
        if (!parse_integer(form.player_y, player_y) || player_y < -64 || player_y > 319) {
            result.error = ValidationError::player_y; return result;
        }
        if (range > (static_cast<int64_t>(std::numeric_limits<int32_t>::max()) - 15) / 16 - 8) {
            result.error = ValidationError::range; return result;
        }
    }

    static constexpr ss_backend backends[] = {
        SS_BACKEND_AUTO, SS_BACKEND_SCALAR, SS_BACKEND_AVX2, SS_BACKEND_NEON, SS_BACKEND_CUDA};
    const uint64_t side = static_cast<uint64_t>(range) * 2;
    result.request.params = {sizeof(ss_search_params_v1), seed,
        static_cast<int32_t>(-range), static_cast<int32_t>(range),
        static_cast<int32_t>(-range), static_cast<int32_t>(range),
        static_cast<uint16_t>(threshold), 0};
    result.request.options = {sizeof(ss_search_options_v1), threads, backends[form.backend], 4096};
    result.request.mode = mode;
    result.request.retention = mode == SearchMode::biome ? ResultRetention::top : retention;
    result.request.top_k = result.request.retention == ResultRetention::top ? static_cast<size_t>(top_k) : 0;
    result.request.spawn_y = spawn_y;
    result.request.player_y = player_y;
    result.request.candidates = saturated_multiply(side, side);
    result.request.worst_result_bytes = saturated_multiply(result.request.candidates, sizeof(ss_result));
    return result;
}

TextKey validation_error_text(ValidationError error) {
    switch (error) {
        case ValidationError::seed: return TextKey::invalid_seed;
        case ValidationError::range: return TextKey::invalid_range;
        case ValidationError::threshold: return TextKey::invalid_threshold;
        case ValidationError::threads: return TextKey::invalid_threads;
        case ValidationError::top_k: return TextKey::invalid_top_k;
        case ValidationError::biome_top_k: return TextKey::invalid_biome_top_k;
        case ValidationError::backend: return TextKey::invalid_backend;
        case ValidationError::mode: return TextKey::invalid_mode;
        case ValidationError::spawn_y: return TextKey::invalid_spawn_y;
        case ValidationError::player_y: return TextKey::invalid_player_y;
        case ValidationError::none: break;
    }
    return TextKey::failed;
}

std::string format_bytes(uint64_t bytes) {
    static constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) { value /= 1024.0; ++unit; }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), value >= 10.0 || unit == 0 ? "%.0f %s" : "%.1f %s",
                  value, units[unit]);
    return buffer;
}

std::string default_export_filename(int64_t seed, bool partial, SearchMode mode) {
    const std::string suffix = mode == SearchMode::biome ? "-biome" : "";
    return "slimeseeker-seed-" + std::to_string(seed) + suffix +
           (partial ? "-partial.csv" : "-results.csv");
}

SpawnMap build_spawn_map(int64_t seed, const BiomeRankedResult &result, int32_t spawn_y) {
    SpawnMap map{};
    for (int dz = -8; dz <= 8; ++dz) {
        for (int dx = -8; dx <= 8; ++dx) {
            auto &cell = map[static_cast<size_t>(dz + 8) * 17 + static_cast<size_t>(dx + 8)];
            cell.player = dx == 0 && dz == 0;
            cell.candidate = in_donut(dx, dz);
            if (!cell.candidate) continue;

            const int32_t chunk_x = result.source.x + dx;
            const int32_t chunk_z = result.source.z + dz;
            cell.slime = is_slime(seed, chunk_x, chunk_z);
            if (!afk_chunk_center_in_range(chunk_x, chunk_z, result.player_x, result.player_z))
                continue;

            const int64_t base_x = static_cast<int64_t>(chunk_x) * 16;
            const int64_t base_z = static_cast<int64_t>(chunk_z) * 16;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    if (afk_spawn_position_in_range(base_x + x, spawn_y, base_z + z,
                                                    result.player_x, result.player_y, result.player_z))
                        ++cell.spawnable_blocks;
                }
            }
        }
    }
    return map;
}

bool BetterResult::operator()(const ss_result &a, const ss_result &b) const {
    if (a.count != b.count) return a.count > b.count;
    const int64_t da = static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.z) * a.z;
    const int64_t db = static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.z) * b.z;
    if (da != db) return da < db;
    if (a.x != b.x) return a.x < b.x;
    return a.z < b.z;
}

ResultCollector::ResultCollector(ResultRetention retention, size_t top_k)
    : retention_(retention), top_k_(top_k) {}

void ResultCollector::add(const ss_result *results, size_t count) {
    total_hits_ += count;
    if (retention_ == ResultRetention::all) {
        all_.insert(all_.end(), results, results + count);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (top_.size() < top_k_) top_.push(results[i]);
        else if (BetterResult{}(results[i], top_.top())) {
            top_.pop();
            top_.push(results[i]);
        }
    }
}

std::vector<ss_result> ResultCollector::finish() {
    if (retention_ == ResultRetention::top) {
        all_.reserve(top_.size());
        while (!top_.empty()) { all_.push_back(top_.top()); top_.pop(); }
    }
    std::sort(all_.begin(), all_.end(), BetterResult{});
    return std::move(all_);
}

ExportStatus export_csv_file(const std::filesystem::path &path,
                             const std::vector<ss_result> &results,
                             bool overwrite, const std::atomic<bool> *cancel,
                             std::atomic<uint64_t> *completed) {
    std::error_code error;
    if (!overwrite && std::filesystem::exists(path, error)) return ExportStatus::exists;
    if (error || path.empty()) return ExportStatus::io_error;
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return ExportStatus::io_error;
    output << "x,z,count\n";
    for (size_t i = 0; i < results.size(); ++i) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            output.close();
            std::filesystem::remove(temporary, error);
            return ExportStatus::cancelled;
        }
        const auto &result = results[i];
        output << result.x << ',' << result.z << ',' << result.count << '\n';
        if (!output) break;
        if (completed && ((i & 4095u) == 0 || i + 1 == results.size()))
            completed->store(i + 1, std::memory_order_relaxed);
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, error);
        return ExportStatus::io_error;
    }
    std::filesystem::path backup;
    if (overwrite && std::filesystem::exists(path, error)) {
        backup = path;
        backup += ".bak-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::rename(path, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return ExportStatus::io_error;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        if (!backup.empty()) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
        }
        return ExportStatus::io_error;
    }
    if (!backup.empty()) std::filesystem::remove(backup, error);
    return ExportStatus::success;
}

ExportStatus export_biome_csv_file(const std::filesystem::path &path,
                                   const std::vector<BiomeRankedResult> &results,
                                   bool overwrite, const std::atomic<bool> *cancel,
                                   std::atomic<uint64_t> *completed) {
    std::error_code error;
    if (!overwrite && std::filesystem::exists(path, error)) return ExportStatus::exists;
    if (error || path.empty()) return ExportStatus::io_error;
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return ExportStatus::io_error;
    output << "rank,x,z,count,biome_score,common_equivalent_chunks,player_x,player_y,player_z,afk_score\n";
    for (size_t i = 0; i < results.size(); ++i) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            output.close();
            std::filesystem::remove(temporary, error);
            return ExportStatus::cancelled;
        }
        const auto &result = results[i];
        output << i + 1 << ',' << result.source.x << ',' << result.source.z << ','
               << result.source.count << ',' << std::fixed << std::setprecision(9)
               << result.biome_score << ',' << result.common_equivalent_chunks << ','
               << std::setprecision(1) << result.player_x << ',' << result.player_y << ','
               << result.player_z << ',' << std::setprecision(9) << result.afk_score << '\n';
        if (!output) break;
        if (completed && ((i & 4095u) == 0 || i + 1 == results.size()))
            completed->store(i + 1, std::memory_order_relaxed);
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, error);
        return ExportStatus::io_error;
    }
    std::filesystem::path backup;
    if (overwrite && std::filesystem::exists(path, error)) {
        backup = path;
        backup += ".bak-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::rename(path, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return ExportStatus::io_error;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        if (!backup.empty()) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
        }
        return ExportStatus::io_error;
    }
    if (!backup.empty()) std::filesystem::remove(backup, error);
    return ExportStatus::success;
}

} // namespace ss::cli
