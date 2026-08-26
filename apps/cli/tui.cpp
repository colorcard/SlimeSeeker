/*
 * SlimeSeeker FTXUI 交互工作台。
 *
 * 终端事件循环只负责渲染；搜索和文件导出各自在专用线程运行，并通过原子快照限频刷新。
 */
#include "tui.hpp"
#include "tui_model.hpp"

#include "slimeseeker/slimeseeker.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
# include <io.h>
#else
# include <unistd.h>
#endif

namespace ss::cli {
namespace {
using namespace std::chrono_literals;
using namespace ftxui;

constexpr size_t kPageSize = 50;

bool interactive_terminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}

Language system_language() {
    const char *locale = std::setlocale(LC_ALL, "");
    return language_from_locale(locale ? locale : "");
}

std::string replace_memory(std::string value, const std::string &memory) {
    const size_t position = value.find("{memory}");
    if (position != std::string::npos) value.replace(position, 8, memory);
    return value;
}

std::string seconds_text(double seconds) {
    if (seconds < 0 || seconds > 1e12) return "--";
    char buffer[64];
    if (seconds < 60) std::snprintf(buffer, sizeof(buffer), "%.1f s", seconds);
    else std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu",
        static_cast<unsigned long long>(seconds) / 3600,
        (static_cast<unsigned long long>(seconds) / 60) % 60,
        static_cast<unsigned long long>(seconds) % 60);
    return buffer;
}

struct RunState {
    SearchRequest request{};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> hits{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<int> status{SS_INTERNAL_ERROR};
    std::atomic<double> elapsed{0};
    std::vector<ss_result> results;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

struct CallbackContext {
    RunState *state;
    ResultCollector *collector;
};

int collect_results(void *opaque, const ss_result *results, size_t count) {
    auto &context = *static_cast<CallbackContext *>(opaque);
    context.collector->add(results, count);
    context.state->hits.store(context.collector->total_hits(), std::memory_order_relaxed);
    return 0;
}

void collect_progress(void *opaque, uint64_t completed, uint64_t total) {
    auto &context = *static_cast<CallbackContext *>(opaque);
    context.state->total.store(total, std::memory_order_relaxed);
    context.state->completed.store(completed, std::memory_order_relaxed);
}

int collect_cancel(void *opaque) {
    return static_cast<CallbackContext *>(opaque)->state->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}

enum class Page { configure, running, results };
enum class Dialog { none, memory, quit_running, export_path, overwrite, export_progress, export_done };

class TuiApplication {
public:
    explicit TuiApplication(Language language)
        : language_(language), language_selected_(language == Language::chinese ? 1 : 0),
          app_(App::Fullscreen()) {
        refresh_labels();
        build_components();
        alive_.store(true, std::memory_order_relaxed);
        refresher_ = std::thread([this] {
            while (alive_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(100ms);
                if (refresh_active_.load(std::memory_order_relaxed))
                    app_.PostEvent(Event::Custom);
            }
        });
    }

    ~TuiApplication() { shutdown(); }

    int run() {
        app_.Loop(root_);
        shutdown();
        return 0;
    }

private:
    void shutdown() {
        if (!alive_.exchange(false, std::memory_order_relaxed)) return;
        if (run_) run_->cancel.store(true, std::memory_order_relaxed);
        export_cancel_.store(true, std::memory_order_relaxed);
        if (refresher_.joinable()) refresher_.join();
        if (search_worker_.joinable()) search_worker_.join();
        if (export_worker_.joinable()) export_worker_.join();
    }

    std::string tr(TextKey key) const { return std::string(localized_text(language_, key)); }

    void refresh_labels() {
        language_ = language_selected_ == 1 ? Language::chinese : Language::english;
        labels_backend_ = {tr(TextKey::auto_backend), tr(TextKey::scalar_backend),
                           tr(TextKey::avx2_backend), tr(TextKey::neon_backend),
                           tr(TextKey::cuda_backend)};
        labels_retention_ = {tr(TextKey::top_results), tr(TextKey::all_results)};
        label_start_ = tr(TextKey::start);
        label_quit_ = tr(TextKey::quit);
        label_cancel_ = tr(TextKey::cancel);
        label_back_ = tr(TextKey::back);
        label_rerun_ = tr(TextKey::rerun);
        label_export_ = tr(TextKey::export_csv);
        label_previous_ = tr(TextKey::previous);
        label_next_ = tr(TextKey::next);
        label_confirm_ = tr(TextKey::confirm);
        label_dismiss_ = tr(TextKey::dismiss);
    }

    void build_components() {
        seed_input_ = Input(&form_.seed, "0");
        range_input_ = Input(&form_.range, "10000");
        threshold_input_ = Input(&form_.threshold, "45");
        threads_input_ = Input(&form_.threads, "0");
        top_k_input_ = Input(&form_.top_k, "1000");
        backend_dropdown_ = Toggle(&labels_backend_, &form_.backend);
        retention_toggle_ = Toggle(&labels_retention_, &form_.retention);
        language_toggle_ = Toggle(&labels_language_, &language_selected_);

        auto start_button = Button(&label_start_, [this] { begin_from_form(); }, ButtonOption::Simple());
        auto quit_button = Button(&label_quit_, [this] { request_quit(); }, ButtonOption::Simple());
        auto config_buttons = Container::Horizontal({start_button, quit_button});
        config_container_ = Container::Vertical({seed_input_, range_input_, threshold_input_,
            threads_input_, backend_dropdown_, retention_toggle_, top_k_input_, config_buttons});

        config_page_ = Renderer(config_container_, [this, start_button, quit_button] {
            std::vector<Element> fields = {
                field_row(TextKey::seed, seed_input_), field_row(TextKey::range, range_input_),
                field_row(TextKey::threshold, threshold_input_), field_row(TextKey::threads, threads_input_),
                field_row(TextKey::backend, backend_dropdown_), field_row(TextKey::result_mode, retention_toggle_)};
            if (form_.retention == 0) fields.push_back(field_row(TextKey::top_k, top_k_input_));
            fields.push_back(separator());
            if (!notice_.empty()) fields.push_back(text(notice_) | color(Color::Red));
            fields.push_back(hbox({start_button->Render(), text("  "), quit_button->Render()}) | center);
            return window(text(tr(TextKey::configure)) | bold, vbox(std::move(fields))) |
                   size(WIDTH, LESS_THAN, 78) | center;
        });

        auto cancel_button = Button(&label_cancel_, [this] { cancel_search(false); }, ButtonOption::Simple());
        running_container_ = Container::Vertical({cancel_button});
        running_page_ = Renderer(running_container_, [this, cancel_button] { return render_running(cancel_button); });

        result_rows_.push_back(" ");
        result_menu_ = Menu(&result_rows_, &selected_result_);
        auto previous_button = Button(&label_previous_, [this] { change_result_page(-1); });
        auto next_button = Button(&label_next_, [this] { change_result_page(1); });
        auto back_button = Button(&label_back_, [this] { page_ = Page::configure; page_index_ = 0; });
        auto rerun_button = Button(&label_rerun_, [this] { rerun(); });
        auto export_button = Button(&label_export_, [this] { open_export(); });
        auto result_quit_button = Button(&label_quit_, [this] { request_quit(); });
        result_container_ = Container::Vertical({result_menu_, previous_button, next_button,
            back_button, rerun_button, export_button, result_quit_button});
        results_page_ = Renderer(result_container_, [this, previous_button, next_button,
            back_button, rerun_button, export_button, result_quit_button] {
            return render_results(previous_button, next_button, back_button,
                                  rerun_button, export_button, result_quit_button);
        });

        pages_ = Container::Tab({config_page_, running_page_, results_page_}, &page_index_);
        body_ = Container::Vertical({language_toggle_, pages_});

        export_path_input_ = Input(&export_path_, "results.csv");
        auto confirm_button = Button(&label_confirm_, [this] { confirm_dialog(); }, ButtonOption::Simple());
        auto dismiss_button = Button(&label_dismiss_, [this] { dismiss_dialog(); });
        dialog_container_ = Container::Vertical({export_path_input_, confirm_button, dismiss_button});
        dialog_component_ = Renderer(dialog_container_, [this, confirm_button, dismiss_button] {
            return render_dialog(confirm_button, dismiss_button);
        });

        root_ = Renderer(body_, [this] {
            observe_workers();
            if ((language_selected_ == 1) != (language_ == Language::chinese)) refresh_labels();
            return vbox({
                hbox({text(" SlimeSeeker ") | bold | color(Color::Green), filler(),
                      text(tr(TextKey::language) + ": "), language_toggle_->Render()}),
                separator(), pages_->Render() | flex
            });
        });
        root_ |= Modal(dialog_component_, &dialog_visible_);
        root_ |= CatchEvent([this](Event event) {
            if (event == Event::F5 && !dialog_visible_ && page_ == Page::configure) {
                begin_from_form();
                return true;
            }
            if (event != Event::Escape) return false;
            if (dialog_visible_) {
                dismiss_dialog();
                return true;
            }
            if (page_ == Page::running) {
                cancel_search(false);
                return true;
            }
            if (page_ == Page::results) {
                page_ = Page::configure;
                page_index_ = 0;
                return true;
            }
            return false;
        });
    }

    Element field_row(TextKey label, const Component &component) const {
        return hbox({text(tr(label)) | size(WIDTH, EQUAL, 20),
                     component->Render() | flex}) | size(HEIGHT, EQUAL, 1);
    }

    void begin_from_form() {
        notice_.clear();
        const auto validation = validate_search_form(form_);
        if (!validation) {
            notice_ = tr(validation_error_text(validation.error));
            return;
        }
        pending_request_ = validation.request;
        if (requires_memory_confirmation(pending_request_)) {
            dialog_ = Dialog::memory;
            dialog_visible_ = true;
            return;
        }
        launch_search(pending_request_);
    }

    void launch_search(const SearchRequest &request) {
        if (search_worker_.joinable()) search_worker_.join();
        notice_.clear();
        run_ = std::make_shared<RunState>();
        run_->request = request;
        run_->total.store(request.candidates, std::memory_order_relaxed);
        run_->started = std::chrono::steady_clock::now();
        page_ = Page::running;
        page_index_ = 1;
        refresh_active_.store(true, std::memory_order_relaxed);
        dialog_visible_ = false;
        exit_after_search_ = false;
        auto state = run_;
        search_worker_ = std::thread([this, state] {
            ResultCollector collector(state->request.retention, state->request.top_k);
            CallbackContext context{state.get(), &collector};
            ss_callbacks_v1 callbacks{sizeof(callbacks), &context,
                collect_results, collect_progress, collect_cancel};
            ss_status status = SS_INTERNAL_ERROR;
            try {
                status = ss_search(&state->request.params, &state->request.options, &callbacks);
                state->results = collector.finish();
            } catch (const std::bad_alloc &) {
                status = SS_OUT_OF_MEMORY;
            } catch (...) {
                status = SS_INTERNAL_ERROR;
            }
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - state->started).count();
            state->elapsed.store(elapsed, std::memory_order_relaxed);
            state->status.store(status, std::memory_order_relaxed);
            state->done.store(true, std::memory_order_release);
            app_.PostEvent(Event::Custom);
        });
    }

    void observe_workers() {
        if (page_ == Page::running && run_ && run_->done.load(std::memory_order_acquire)) {
            if (search_worker_.joinable()) search_worker_.join();
            refresh_active_.store(false, std::memory_order_relaxed);
            if (exit_after_search_) { app_.Exit(); return; }
            page_ = Page::results;
            page_index_ = 2;
            result_page_ = 0;
            selected_result_ = 0;
            refresh_result_rows();
        }
        if (export_running_.load(std::memory_order_relaxed) &&
            export_done_.load(std::memory_order_acquire)) {
            if (export_worker_.joinable()) export_worker_.join();
            export_running_.store(false, std::memory_order_relaxed);
            refresh_active_.store(false, std::memory_order_relaxed);
            dialog_ = Dialog::export_done;
            dialog_visible_ = true;
        }
    }

    Element render_running(const Component &cancel_button) const {
        if (!run_) return text("");
        const uint64_t completed = run_->completed.load(std::memory_order_relaxed);
        const uint64_t total = run_->total.load(std::memory_order_relaxed);
        const uint64_t hits = run_->hits.load(std::memory_order_relaxed);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_->started).count();
        const double fraction = total ? static_cast<double>(completed) / total : 0;
        const double eta = completed ? elapsed * static_cast<double>(total - completed) / completed : -1;
        const double throughput = elapsed > 0 ? static_cast<double>(completed) / elapsed : 0;
        char speed[64];
        std::snprintf(speed, sizeof(speed), "%.3f G/s", throughput / 1e9);
        return window(text(tr(TextKey::running)) | bold,
            vbox({
                gauge(std::clamp(fraction, 0.0, 1.0)) | color(Color::Green),
                separator(),
                metric(TextKey::progress, std::to_string(completed) + " / " + std::to_string(total)),
                metric(TextKey::hits, std::to_string(hits)),
                metric(TextKey::elapsed, seconds_text(elapsed)),
                metric(TextKey::eta, seconds_text(eta)),
                metric(TextKey::throughput, speed),
                metric(TextKey::requested_backend, ss_backend_name(run_->request.options.backend)),
                separator(), cancel_button->Render() | center
            })) | size(WIDTH, LESS_THAN, 76) | center;
    }

    Element metric(TextKey label, std::string value) const {
        return hbox({text(tr(label)) | size(WIDTH, EQUAL, 22), text(std::move(value)) | bold});
    }

    void refresh_result_rows() {
        result_rows_.clear();
        if (!run_) { result_rows_.push_back(" "); return; }
        const size_t begin = result_page_ * kPageSize;
        const size_t end = std::min(run_->results.size(), begin + kPageSize);
        for (size_t i = begin; i < end; ++i) {
            const auto &result = run_->results[i];
            char row[128];
            std::snprintf(row, sizeof(row), "%6zu  %11d  %11d  %5u", i + 1,
                          result.x, result.z, result.count);
            result_rows_.emplace_back(row);
        }
        if (result_rows_.empty()) result_rows_.push_back(" ");
        selected_result_ = 0;
    }

    size_t result_page_count() const {
        return !run_ || run_->results.empty() ? 1 : (run_->results.size() + kPageSize - 1) / kPageSize;
    }

    void change_result_page(int delta) {
        const size_t pages = result_page_count();
        if (delta < 0 && result_page_ > 0) --result_page_;
        if (delta > 0 && result_page_ + 1 < pages) ++result_page_;
        refresh_result_rows();
    }

    Element render_results(const Component &previous_button, const Component &next_button,
                           const Component &back_button, const Component &rerun_button,
                           const Component &export_button, const Component &quit_button) const {
        if (!run_) return text("");
        const ss_status status = static_cast<ss_status>(run_->status.load(std::memory_order_relaxed));
        std::string status_text = status == SS_OK ? tr(TextKey::success)
            : status == SS_CANCELLED ? tr(TextKey::partial_results)
            : tr(TextKey::failed) + ": " + ss_status_string(status);
        Color status_color = status == SS_OK ? Color::Green
            : status == SS_CANCELLED ? Color::Yellow : Color::Red;
        std::vector<Element> content = {
            hbox({text(tr(TextKey::status) + ": "), text(status_text) | bold | color(status_color),
                  filler(), text(tr(TextKey::retained) + ": " + std::to_string(run_->results.size()) +
                  " / " + std::to_string(run_->hits.load(std::memory_order_relaxed)))}),
            separator()
        };
        if (run_->results.empty()) {
            content.push_back(text(tr(TextKey::no_results)) | center | flex);
        } else {
            content.push_back(text("  #       X            Z      Count") | dim);
            content.push_back(result_menu_->Render() | frame | flex | border);
            const size_t index = result_page_ * kPageSize + static_cast<size_t>(selected_result_);
            if (index < run_->results.size()) {
                const auto &result = run_->results[index];
                const int64_t distance = static_cast<int64_t>(result.x) * result.x +
                                         static_cast<int64_t>(result.z) * result.z;
                const int64_t x0 = static_cast<int64_t>(result.x) * 16;
                const int64_t z0 = static_cast<int64_t>(result.z) * 16;
                content.push_back(window(text(tr(TextKey::details)), vbox({
                    metric(TextKey::distance, std::to_string(distance)),
                    metric(TextKey::block_bounds, "X " + std::to_string(x0) + ".." +
                        std::to_string(x0 + 15) + ", Z " + std::to_string(z0) + ".." +
                        std::to_string(z0 + 15))
                })));
            }
        }
        content.push_back(hbox({
            previous_button->Render(), text(" "), next_button->Render(),
            text("  " + tr(TextKey::page) + " " + std::to_string(result_page_ + 1) + "/" +
                 std::to_string(result_page_count())), filler(),
            back_button->Render(), text(" "), rerun_button->Render(), text(" "),
            export_button->Render(), text(" "), quit_button->Render()
        }));
        return window(text(tr(TextKey::results)) | bold, vbox(std::move(content))) | flex;
    }

    void cancel_search(bool exit_after) {
        if (!run_ || page_ != Page::running) return;
        exit_after_search_ = exit_after;
        run_->cancel.store(true, std::memory_order_relaxed);
    }

    void request_quit() {
        if (page_ == Page::running && run_ && !run_->done.load(std::memory_order_acquire)) {
            dialog_ = Dialog::quit_running;
            dialog_visible_ = true;
        } else {
            app_.Exit();
        }
    }

    void rerun() {
        if (run_) launch_search(run_->request);
    }

    void open_export() {
        if (!run_ || run_->results.empty()) return;
        const bool partial = run_->status.load(std::memory_order_relaxed) != SS_OK;
        export_path_ = default_export_filename(run_->request.params.world_seed, partial);
        dialog_ = Dialog::export_path;
        dialog_visible_ = true;
    }

    void start_export(bool overwrite) {
        if (!run_) return;
        if (export_worker_.joinable()) export_worker_.join();
        export_cancel_.store(false, std::memory_order_relaxed);
        export_completed_.store(0, std::memory_order_relaxed);
        export_done_.store(false, std::memory_order_relaxed);
        export_running_.store(true, std::memory_order_relaxed);
        refresh_active_.store(true, std::memory_order_relaxed);
        dialog_ = Dialog::export_progress;
        dialog_visible_ = true;
        auto state = run_;
        const std::filesystem::path path = export_path_;
        export_worker_ = std::thread([this, state, path, overwrite] {
            const auto status = export_csv_file(path, state->results, overwrite,
                                                &export_cancel_, &export_completed_);
            export_status_.store(static_cast<int>(status), std::memory_order_relaxed);
            export_done_.store(true, std::memory_order_release);
            app_.PostEvent(Event::Custom);
        });
    }

    void confirm_dialog() {
        switch (dialog_) {
            case Dialog::memory: launch_search(pending_request_); break;
            case Dialog::quit_running:
                dialog_visible_ = false;
                cancel_search(true);
                break;
            case Dialog::export_path: {
                std::error_code error;
                if (std::filesystem::exists(export_path_, error) && !error) {
                    dialog_ = Dialog::overwrite;
                } else {
                    start_export(false);
                }
                break;
            }
            case Dialog::overwrite: start_export(true); break;
            case Dialog::export_progress:
                export_cancel_.store(true, std::memory_order_relaxed);
                break;
            case Dialog::export_done:
                dialog_ = Dialog::none;
                dialog_visible_ = false;
                break;
            case Dialog::none: break;
        }
    }

    void dismiss_dialog() {
        if (dialog_ == Dialog::export_progress)
            export_cancel_.store(true, std::memory_order_relaxed);
        else {
            dialog_ = Dialog::none;
            dialog_visible_ = false;
        }
    }

    Element render_dialog(const Component &confirm_button, const Component &dismiss_button) {
        std::string message;
        bool show_path = false;
        bool show_dismiss = true;
        switch (dialog_) {
            case Dialog::memory:
                message = replace_memory(tr(TextKey::memory_warning),
                                         format_bytes(pending_request_.worst_result_bytes));
                label_confirm_ = tr(TextKey::confirm);
                break;
            case Dialog::quit_running:
                message = tr(TextKey::partial_warning);
                label_confirm_ = tr(TextKey::cancel);
                break;
            case Dialog::export_path:
                message = run_ && run_->status.load(std::memory_order_relaxed) != SS_OK
                    ? tr(TextKey::partial_warning) : tr(TextKey::export_path);
                show_path = true;
                label_confirm_ = tr(TextKey::export_csv);
                break;
            case Dialog::overwrite:
                message = tr(TextKey::overwrite_warning);
                show_path = true;
                label_confirm_ = tr(TextKey::confirm);
                break;
            case Dialog::export_progress: {
                const uint64_t done = export_completed_.load(std::memory_order_relaxed);
                const uint64_t total = run_ ? run_->results.size() : 0;
                message = tr(TextKey::exporting) + "  " + std::to_string(done) + " / " +
                          std::to_string(total);
                label_confirm_ = tr(TextKey::cancel);
                show_dismiss = false;
                break;
            }
            case Dialog::export_done: {
                const auto status = static_cast<ExportStatus>(export_status_.load(std::memory_order_relaxed));
                message = status == ExportStatus::success ? tr(TextKey::export_success)
                    : status == ExportStatus::cancelled ? tr(TextKey::cancelled)
                    : tr(TextKey::export_failed);
                label_confirm_ = tr(TextKey::dismiss);
                show_dismiss = false;
                break;
            }
            case Dialog::none: break;
        }
        label_dismiss_ = tr(TextKey::dismiss);
        std::vector<Element> content{text(message) | center, separator()};
        if (show_path) content.push_back(hbox({text(tr(TextKey::export_path) + ": "),
                                              export_path_input_->Render() | border | flex}));
        content.push_back(hbox({confirm_button->Render(),
                                show_dismiss ? hbox({text("  "), dismiss_button->Render()}) : text("")}) | center);
        return window(text("SlimeSeeker") | bold, vbox(std::move(content))) |
               size(WIDTH, LESS_THAN, 70) | center;
    }

    Language language_;
    int language_selected_ = 0;
    SearchForm form_;
    SearchRequest pending_request_{};
    Page page_ = Page::configure;
    int page_index_ = 0;
    Dialog dialog_ = Dialog::none;
    bool dialog_visible_ = false;
    bool exit_after_search_ = false;
    std::string notice_;

    std::vector<std::string> labels_language_{"English", "中文"};
    std::vector<std::string> labels_backend_;
    std::vector<std::string> labels_retention_;
    std::string label_start_, label_quit_, label_cancel_, label_back_, label_rerun_;
    std::string label_export_, label_previous_, label_next_, label_confirm_, label_dismiss_;

    App app_;
    Component seed_input_, range_input_, threshold_input_, threads_input_, top_k_input_;
    Component backend_dropdown_, retention_toggle_, language_toggle_, export_path_input_;
    Component config_container_, running_container_, result_container_, dialog_container_;
    Component config_page_, running_page_, results_page_, pages_, body_, dialog_component_, root_;

    std::shared_ptr<RunState> run_;
    std::thread search_worker_;
    std::thread refresher_;
    std::atomic<bool> alive_{false};
    std::atomic<bool> refresh_active_{false};

    std::vector<std::string> result_rows_;
    Component result_menu_;
    size_t result_page_ = 0;
    int selected_result_ = 0;

    std::string export_path_;
    std::thread export_worker_;
    std::atomic<bool> export_cancel_{false};
    std::atomic<bool> export_running_{false};
    std::atomic<bool> export_done_{false};
    std::atomic<uint64_t> export_completed_{0};
    std::atomic<int> export_status_{static_cast<int>(ExportStatus::io_error)};
};

} // namespace

int run_tui() {
    if (!interactive_terminal()) {
        std::fprintf(stderr, "%s\n", localized_text(system_language(), TextKey::terminal_required).data());
        return 1;
    }
    TuiApplication application(system_language());
    return application.run();
}

} // namespace ss::cli
