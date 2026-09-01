#pragma once

#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

enum class SessionPriority : std::uint8_t {
    visible_tile = 0,
    adjacent_page = 1,
    thumbnail = 2,
    indexing = 3,
};

class DocumentSession final {
public:
    [[nodiscard]] static Result<std::unique_ptr<DocumentSession>> open(
        PdfEngine& engine,
        const std::filesystem::path& source
    );

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;
    DocumentSession(DocumentSession&&) = delete;
    DocumentSession& operator=(DocumentSession&&) = delete;
    ~DocumentSession();

    void shutdown();

    [[nodiscard]] std::size_t page_count() const noexcept;
    [[nodiscard]] Result<PageInfo> page_info(std::size_t page_index);
    [[nodiscard]] Result<EncodedPageImage> render_page_png(
        std::size_t page_index,
        double pixels_per_point
    );
    [[nodiscard]] Result<RenderedTile> render_tile(
        const TileRequest& request,
        SessionPriority priority = SessionPriority::visible_tile
    );
    [[nodiscard]] Result<PageText> extract_text(std::size_t page_index);
    [[nodiscard]] Result<PageTextLayout> page_text_layout(std::size_t page_index);
    [[nodiscard]] Result<TextSelection> select_text(
        std::size_t page_index,
        PagePoint start_point,
        PagePoint end_point
    );

private:
    explicit DocumentSession(std::unique_ptr<PdfDocument> document);

    struct QueuedTask final {
        SessionPriority priority;
        std::chrono::steady_clock::time_point enqueued_at;
        std::function<void(PdfDocument&)> operation;
    };

    struct DisplayListEntry final {
        std::size_t page_index;
        std::size_t bytes;
        std::uint64_t last_used;
        std::shared_ptr<PdfTileDisplayList> display_list;
    };

    template <typename T, typename Operation>
    [[nodiscard]] Result<T> invoke(SessionPriority priority, Operation&& operation) {
        auto promise = std::make_shared<std::promise<Result<T>>>();
        auto future = promise->get_future();
        {
            const std::scoped_lock lock(mutex_);
            if(stopping_) {
                return Result<T>::failure(Error(ErrorCode::cancelled, "Document session is closing"));
            }
            if(queue_.size() >= maximum_queue_items) {
                return Result<T>::failure(Error(ErrorCode::resource_exhausted, "Document session queue is full"));
            }
            queue_.push_back(QueuedTask{
                .priority = priority,
                .enqueued_at = std::chrono::steady_clock::now(),
                .operation = [promise, operation = std::forward<Operation>(operation)](PdfDocument& document) mutable {
                    promise->set_value(operation(document));
                },
            });
        }
        wake_.notify_one();
        return future.get();
    }

    void run();
    [[nodiscard]] std::size_t next_task_index(std::chrono::steady_clock::time_point now) const;
    [[nodiscard]] Result<std::shared_ptr<PdfTileDisplayList>> display_list(
        PdfDocument& document,
        std::size_t page_index
    );

    static constexpr std::size_t maximum_queue_items = 256;
    static constexpr auto priority_aging_interval = std::chrono::seconds(2);
    static constexpr std::size_t maximum_display_list_pages = 16;
    static constexpr std::size_t maximum_display_list_bytes = 128U * 1024U * 1024U;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable render_idle_;
    std::deque<QueuedTask> queue_;
    std::unique_ptr<PdfDocument> document_;
    std::vector<DisplayListEntry> display_lists_;
    std::size_t display_list_bytes_{0};
    std::uint64_t display_list_clock_{0};
    std::size_t page_count_{};
    std::thread actor_;
    std::size_t active_renders_{0};
    bool stopping_{false};
};

}  // namespace context_reader
