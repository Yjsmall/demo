#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <cstdint>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "context_reader/runtime/reader_runtime.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"
#include "context_reader/shared/stable_id.hpp"

using namespace context_reader;

TEST_CASE("reader runtime reports its API and executes work") {
    auto runtime_result = ReaderRuntime::create();
    REQUIRE(runtime_result.has_value());

    auto runtime = std::move(runtime_result).value();
    const auto info = runtime->application().runtime_info();
    CHECK(info.version == RuntimeVersion{0, 1, 0});
    CHECK(info.application_api_version == 6U);
    CHECK_FALSE(info.capabilities.empty());
    CHECK(runtime->executor().concurrency() > 0);

    std::atomic_bool task_ran{false};
    auto completion_result = runtime->executor().submit([&task_ran] { task_ran = true; });
    REQUIRE(completion_result.has_value());
    std::move(completion_result).value().get();
    CHECK(task_ran.load());
}

TEST_CASE("runtime executor bounds queued bytes and prioritizes visible work") {
    RuntimeExecutorLimits limits{
        .maximum_queue_items = 3,
        .maximum_queue_bytes = 8,
        .priority_aging_interval = std::chrono::seconds(2),
    };
    auto executor_result = RuntimeExecutor::create(1, limits);
    REQUIRE(executor_result.has_value());
    auto executor = std::move(executor_result).value();

    std::promise<void> blocker_started;
    std::promise<void> release_blocker;
    auto release = release_blocker.get_future().share();
    auto blocker = executor->submit([&blocker_started, release] {
        blocker_started.set_value();
        release.wait();
    });
    REQUIRE(blocker.has_value());
    blocker_started.get_future().wait();

    std::mutex order_mutex;
    std::vector<int> order;
    auto indexing = executor->submit(
        [&] {
            const std::scoped_lock lock(order_mutex);
            order.push_back(2);
        },
        RuntimeTaskPriority::indexing,
        4
    );
    auto visible = executor->submit(
        [&] {
            const std::scoped_lock lock(order_mutex);
            order.push_back(1);
        },
        RuntimeTaskPriority::visible_tile,
        4
    );
    REQUIRE(indexing.has_value());
    REQUIRE(visible.has_value());

    const auto exhausted = executor->submit([] {}, RuntimeTaskPriority::visible_tile, 1);
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error().code() == ErrorCode::resource_exhausted);

    release_blocker.set_value();
    std::move(blocker).value().get();
    std::move(indexing).value().get();
    std::move(visible).value().get();
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
}

TEST_CASE("stable IDs parse and compare by value") {
    const auto parsed_id = stable_id_from_hex<DocumentIdTag>(
        "00112233445566778899aabbccddeeff"
    );
    REQUIRE(parsed_id.has_value());
    CHECK(stable_id_to_hex(*parsed_id) == "00112233445566778899aabbccddeeff");
    CHECK_FALSE(stable_id_from_hex<DocumentIdTag>("not-a-document-id").has_value());

    DocumentId::Bytes document_bytes{};
    document_bytes.back() = std::uint8_t{1};
    const auto document_id = DocumentId::from_bytes(document_bytes);
    CHECK_FALSE(document_id.is_nil());
    CHECK(document_id == DocumentId::from_bytes(document_bytes));
}

TEST_CASE("result preserves a structured failure") {
    const auto failure = Result<int>::failure(Error(ErrorCode::not_found, "missing"));
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code() == ErrorCode::not_found);
}
