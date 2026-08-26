/* SlimeSeeker TUI 输入、结果保留和导出契约测试。 */
#include "tui_model.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; ++failures; \
} } while (false)

using namespace ss::cli;

std::filesystem::path unique_path(const char *name) {
    return std::filesystem::temp_directory_path() /
        (std::string("slimeseeker-") + name + '-' + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".csv");
}

void test_translations_and_locale() {
    for (size_t i = 0; i < static_cast<size_t>(TextKey::count_); ++i) {
        const auto key = static_cast<TextKey>(i);
        CHECK(!localized_text(Language::english, key).empty());
        CHECK(!localized_text(Language::chinese, key).empty());
    }
    CHECK(language_from_locale("zh_CN.UTF-8") == Language::chinese);
    CHECK(language_from_locale("Chinese (Simplified)_China.936") == Language::chinese);
    CHECK(language_from_locale("en_US.UTF-8") == Language::english);
    CHECK(language_from_locale("") == Language::english);
}

void test_validation() {
    SearchForm form;
    auto result = validate_search_form(form);
    CHECK(result);
    CHECK(result.request.params.world_seed == 0);
    CHECK(result.request.params.x_begin == -10000 && result.request.params.x_end == 10000);
    CHECK(result.request.params.threshold == 45);
    CHECK(result.request.options.backend == SS_BACKEND_AUTO);
    CHECK(result.request.options.result_batch_capacity == 4096);
    CHECK(result.request.candidates == 400000000);
    CHECK(result.request.retention == ResultRetention::top);
    CHECK(!requires_memory_confirmation(result.request));

    form.retention = 1;
    result = validate_search_form(form);
    CHECK(result && result.request.retention == ResultRetention::all);
    CHECK(requires_memory_confirmation(result.request));
    CHECK(result.request.worst_result_bytes == result.request.candidates * sizeof(ss_result));

    form = {};
    form.seed = "x";
    CHECK(validate_search_form(form).error == ValidationError::seed);
    form = {}; form.range = "0";
    CHECK(validate_search_form(form).error == ValidationError::range);
    form = {}; form.range = "2147483640";
    CHECK(validate_search_form(form).error == ValidationError::range);
    form = {}; form.threshold = "193";
    CHECK(validate_search_form(form).error == ValidationError::threshold);
    form = {}; form.threads = "1025";
    CHECK(validate_search_form(form).error == ValidationError::threads);
    form = {}; form.top_k = "1000001";
    CHECK(validate_search_form(form).error == ValidationError::top_k);
    form = {}; form.backend = 5;
    CHECK(validate_search_form(form).error == ValidationError::backend);
}

void test_result_collection() {
    std::vector<ss_result> input;
    for (int i = 0; i < 100; ++i)
        input.push_back({i - 50, 50 - i, static_cast<uint16_t>(i % 17), 0});
    auto expected = input;
    std::sort(expected.begin(), expected.end(), BetterResult{});

    ResultCollector top(ResultRetention::top, 11);
    top.add(input.data(), 37);
    top.add(input.data() + 37, input.size() - 37);
    auto retained = top.finish();
    CHECK(top.total_hits() == input.size());
    CHECK(retained.size() == 11);
    CHECK(std::equal(retained.begin(), retained.end(), expected.begin(), [](const auto &a, const auto &b) {
        return a.x == b.x && a.z == b.z && a.count == b.count;
    }));

    ResultCollector all(ResultRetention::all, 0);
    all.add(input.data(), input.size());
    auto complete = all.finish();
    CHECK(complete.size() == input.size());
    CHECK(std::equal(complete.begin(), complete.end(), expected.begin(), [](const auto &a, const auto &b) {
        return a.x == b.x && a.z == b.z && a.count == b.count;
    }));
}

void test_export() {
    const std::vector<ss_result> results{{1, -2, 45, 0}, {3, 4, 44, 0}};
    const auto path = unique_path("export");
    std::atomic<uint64_t> completed{0};
    CHECK(export_csv_file(path, results, false, nullptr, &completed) == ExportStatus::success);
    CHECK(completed.load() == results.size());
    std::ifstream input(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)), {});
    CHECK(content == "x,z,count\n1,-2,45\n3,4,44\n");
    CHECK(export_csv_file(path, results, false) == ExportStatus::exists);

    const std::vector<ss_result> replacement{{9, 8, 47, 0}};
    CHECK(export_csv_file(path, replacement, true) == ExportStatus::success);
    std::ifstream replaced(path, std::ios::binary);
    const std::string replaced_content((std::istreambuf_iterator<char>(replaced)), {});
    CHECK(replaced_content == "x,z,count\n9,8,47\n");

    std::atomic<bool> cancel{true};
    const auto cancelled_path = unique_path("cancelled");
    CHECK(export_csv_file(cancelled_path, results, false, &cancel) == ExportStatus::cancelled);
    CHECK(!std::filesystem::exists(cancelled_path));
    std::filesystem::remove(path);
    CHECK(default_export_filename(-12, false) == "slimeseeker-seed--12-results.csv");
    CHECK(default_export_filename(-12, true) == "slimeseeker-seed--12-partial.csv");
}
} // namespace

int main() {
    test_translations_and_locale();
    test_validation();
    test_result_collection();
    test_export();
    if (failures) std::cerr << failures << " TUI model checks failed\n";
    return failures ? 1 : 0;
}
