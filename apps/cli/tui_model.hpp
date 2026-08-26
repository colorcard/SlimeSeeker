/*
 * SlimeSeeker TUI 的无界面状态模型。
 *
 * 输入校验、结果保留、国际化和 CSV 导出不依赖终端框架，便于独立验证。
 */
#pragma once

#include "slimeseeker/slimeseeker.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace ss::cli {

enum class Language { english, chinese };
enum class ResultRetention { top, all };

enum class TextKey : size_t {
    app_title, configure, seed, range, threshold, threads, backend, result_mode,
    top_k, all_results, start, quit, running, cancel, progress, candidates, hits,
    elapsed, eta, throughput, results, back, rerun, export_csv, rank, chunk_x,
    chunk_z, count, distance, block_bounds, retained, success, cancelled, failed,
    invalid_seed, invalid_range, invalid_threshold, invalid_threads, invalid_top_k,
    invalid_backend, memory_warning, confirm, dismiss, terminal_required,
    export_path, exporting, export_success, export_failed, partial_warning,
    overwrite_warning, no_results, sorting, language, status, requested_backend,
    partial_results, page, previous, next, details, auto_backend, scalar_backend,
    avx2_backend, neon_backend, cuda_backend, top_results, count_
};

std::string_view localized_text(Language language, TextKey key);
Language language_from_locale(std::string_view locale);

struct SearchForm {
    std::string seed = "0";
    std::string range = "10000";
    std::string threshold = "45";
    std::string threads = "0";
    std::string top_k = "1000";
    int backend = 0;
    int retention = 0;
};

enum class ValidationError {
    none, seed, range, threshold, threads, top_k, backend
};

struct SearchRequest {
    ss_search_params_v1 params{};
    ss_search_options_v1 options{};
    ResultRetention retention = ResultRetention::top;
    size_t top_k = 1000;
    uint64_t candidates = 0;
    uint64_t worst_result_bytes = 0;
};

struct ValidationResult {
    ValidationError error = ValidationError::none;
    SearchRequest request{};
    explicit operator bool() const { return error == ValidationError::none; }
};

ValidationResult validate_search_form(const SearchForm &form);
bool requires_memory_confirmation(const SearchRequest &request);
TextKey validation_error_text(ValidationError error);
std::string format_bytes(uint64_t bytes);
std::string default_export_filename(int64_t seed, bool partial);

struct BetterResult {
    bool operator()(const ss_result &a, const ss_result &b) const;
};

class ResultCollector {
public:
    ResultCollector(ResultRetention retention, size_t top_k);
    void add(const ss_result *results, size_t count);
    uint64_t total_hits() const { return total_hits_; }
    std::vector<ss_result> finish();

private:
    ResultRetention retention_;
    size_t top_k_;
    uint64_t total_hits_ = 0;
    std::vector<ss_result> all_;
    std::priority_queue<ss_result, std::vector<ss_result>, BetterResult> top_;
};

enum class ExportStatus { success, cancelled, exists, io_error };
ExportStatus export_csv_file(const std::filesystem::path &path,
                             const std::vector<ss_result> &results,
                             bool overwrite,
                             const std::atomic<bool> *cancel = nullptr,
                             std::atomic<uint64_t> *completed = nullptr);

} // namespace ss::cli
