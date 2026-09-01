#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "context_reader/runtime/reader_runtime.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace context_reader;

struct Scenario final {
    std::string name;
    std::vector<double> samples;
    double p50{};
    double p95{};
    double hard_limit{};
    bool passed{};
};

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())) - 1.0
    );
    return values[std::min(index, values.size() - 1U)];
}

Scenario measure(
    std::string name,
    std::size_t warmups,
    std::size_t sample_count,
    double hard_limit,
    const std::function<void()>& operation
) {
    for(std::size_t index = 0; index < warmups; ++index) operation();
    Scenario result{.name = std::move(name), .hard_limit = hard_limit};
    result.samples.reserve(sample_count);
    for(std::size_t index = 0; index < sample_count; ++index) {
        const auto start = Clock::now();
        operation();
        result.samples.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count()
        );
    }
    result.p50 = percentile(result.samples, 0.50);
    result.p95 = percentile(result.samples, 0.95);
    result.passed = result.p95 <= result.hard_limit;
    return result;
}

template <typename T>
T require(Result<T> result, const char* operation) {
    if(!result) throw std::runtime_error(std::string(operation) + ": " + result.error().message());
    return std::move(result).value();
}

void require(Result<void> result, const char* operation) {
    if(!result) throw std::runtime_error(std::string(operation) + ": " + result.error().message());
}

std::uint64_t peak_working_set() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
        ? static_cast<std::uint64_t>(counters.PeakWorkingSetSize) : 0U;
}

std::uint32_t windows_build() {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto function = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion")
    );
    OSVERSIONINFOW version{.dwOSVersionInfoSize = sizeof(version)};
    return function != nullptr && function(&version) == 0 ? version.dwBuildNumber : 0U;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool smoke = argc == 4 && std::wstring_view(argv[1]) == L"--smoke";
    if((smoke && argc != 4) || (!smoke && argc != 3)) {
        std::cerr << "Usage: reader-bench [--smoke] <fixture.pdf> <output.json>\n";
        return 2;
    }
    const auto fixture = std::filesystem::absolute(argv[smoke ? 2 : 1]);
    const auto output = std::filesystem::absolute(argv[smoke ? 3 : 2]);
    const auto warmups = smoke ? 1U : 3U;
    const auto samples = smoke ? 3U : 20U;
    auto workspace_root = output.parent_path() /
        ("reader-bench-workspace-" + std::to_string(GetCurrentProcessId()));
    std::error_code filesystem_error;
    std::filesystem::remove_all(workspace_root, filesystem_error);
    std::filesystem::create_directories(output.parent_path(), filesystem_error);

    try {
        auto runtime = require(ReaderRuntime::create(), "runtime create");
        auto& application = runtime->application();
        require(application.create_workspace(workspace_root), "workspace create");
        const auto imported = require(application.import_document(fixture), "document import");
        require(application.open_document(imported.document.document_id), "document open");
        require(application.rebuild_search_index(), "search index rebuild");

        const TileRequest tile_request{
            .page_index = 0U,
            .pixels_per_point = 1.0,
            .x_pixels = 0U,
            .y_pixels = 0U,
            .width_pixels = 256U,
            .height_pixels = 256U,
            .generation = 1U,
        };
        std::vector<Scenario> scenarios;
        scenarios.push_back(measure("ColdOpen", warmups, samples, 3000.0, [] {
            auto cold = require(ReaderRuntime::create(), "cold runtime create");
        }));
        scenarios.push_back(measure("FirstPageVisible", warmups, samples, 1500.0, [&] {
            require(application.close_document(), "close before first page");
            require(application.open_document(imported.document.document_id), "open for first page");
            require(application.render_tile(tile_request), "first visible tile");
        }));
        scenarios.push_back(measure("RenderVisibleTile", warmups, samples, 200.0, [&] {
            require(application.render_tile(tile_request), "visible tile");
        }));
        scenarios.push_back(measure("ExtractPageText", warmups, samples, 500.0, [&] {
            require(application.extract_page_text(0U), "extract page text");
        }));
        scenarios.push_back(measure("SearchLargeWorkspace", warmups, samples, 500.0, [&] {
            require(application.search("Context Reader", 50U), "workspace search");
        }));
        scenarios.push_back(measure("FastScrollCancellation", warmups, samples, 500.0, [&] {
            CancellationSource cancellation;
            cancellation.request_cancellation();
            const auto result = application.render_tile(tile_request, cancellation.token());
            if(result || result.error().code() != ErrorCode::cancelled) {
                throw std::runtime_error("cancelled tile did not return CANCELLED");
            }
        }));
        scenarios.push_back(measure("ShutdownWithPendingJobs", warmups, samples, 2000.0, [] {
            auto pending = require(ReaderRuntime::create(), "pending runtime create");
            for(int index = 0; index < 4; ++index) {
                require(pending->executor().submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }), "submit pending task");
            }
        }));

        const auto runtime_info = application.runtime_info();
        const auto peak = peak_working_set();
        const bool passed = peak <= 768ULL * 1024ULL * 1024ULL && std::all_of(
            scenarios.begin(), scenarios.end(), [](const Scenario& item) { return item.passed; }
        );
        std::ofstream report(output, std::ios::binary | std::ios::trunc);
        report << std::fixed << std::setprecision(3)
               << "{\"schemaVersion\":1,\"mode\":\"" << (smoke ? "smoke" : "qualification")
               << "\",\"buildId\":\"" << runtime_info.build_id
               << "\",\"environment\":{\"os\":\"Windows\",\"windowsBuild\":" << windows_build()
               << ",\"sampleCount\":" << samples << ",\"warmupCount\":" << warmups
               << "},\"peakWorkingSetBytes\":" << peak << ",\"scenarios\":[";
        for(std::size_t index = 0; index < scenarios.size(); ++index) {
            if(index != 0U) report << ',';
            const auto& scenario = scenarios[index];
            report << "{\"name\":\"" << scenario.name << "\",\"samplesMs\":[";
            for(std::size_t sample = 0; sample < scenario.samples.size(); ++sample) {
                if(sample != 0U) report << ',';
                report << scenario.samples[sample];
            }
            report << "],\"p50Ms\":" << scenario.p50 << ",\"p95Ms\":" << scenario.p95
                   << ",\"hardLimitMs\":" << scenario.hard_limit
                   << ",\"passed\":" << (scenario.passed ? "true" : "false") << '}';
        }
        report << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
        report.close();
        runtime.reset();
        std::filesystem::remove_all(workspace_root, filesystem_error);
        return passed && report ? 0 : 1;
    } catch(const std::exception& exception) {
        std::filesystem::remove_all(workspace_root, filesystem_error);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
