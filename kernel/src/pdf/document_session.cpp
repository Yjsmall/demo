#include "context_reader/pdf/document_session.hpp"

#include <memory>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <utility>

namespace context_reader {

Result<std::unique_ptr<DocumentSession>> DocumentSession::open(
    PdfEngine& engine,
    const std::filesystem::path& source
) {
    auto document_result = engine.open(source);
    if(!document_result) {
        return Result<std::unique_ptr<DocumentSession>>::failure(document_result.error());
    }
    return Result<std::unique_ptr<DocumentSession>>::success(
        std::unique_ptr<DocumentSession>(
            new DocumentSession(std::move(document_result).value())
        )
    );
}

DocumentSession::DocumentSession(std::unique_ptr<PdfDocument> document)
    : document_(std::move(document)), page_count_(document_->page_count()), actor_([this] { run(); }) {}

DocumentSession::~DocumentSession() {
    shutdown();
}

void DocumentSession::shutdown() {
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if(actor_.joinable()) {
        actor_.join();
    }
    std::unique_lock lock(mutex_);
    render_idle_.wait(lock, [this] { return active_renders_ == 0; });
}

std::size_t DocumentSession::page_count() const noexcept {
    return page_count_;
}

Result<PageInfo> DocumentSession::page_info(std::size_t page_index) {
    return invoke<PageInfo>(SessionPriority::adjacent_page, [page_index](PdfDocument& document) {
        return document.page_info(page_index);
    });
}

Result<EncodedPageImage> DocumentSession::render_page_png(
    std::size_t page_index,
    double pixels_per_point
) {
    return invoke<EncodedPageImage>(SessionPriority::thumbnail, [page_index, pixels_per_point](PdfDocument& document) {
        return document.render_page_png(page_index, pixels_per_point);
    });
}

Result<RenderedTile> DocumentSession::render_tile(const TileRequest& request, SessionPriority priority) {
    {
        const std::scoped_lock lock(mutex_);
        if(stopping_) {
            return Result<RenderedTile>::failure(
                Error(ErrorCode::cancelled, "Document session is closing")
            );
        }
        ++active_renders_;
    }
    const auto finish_render = [this](void*) {
        {
            const std::scoped_lock lock(mutex_);
            --active_renders_;
        }
        render_idle_.notify_all();
    };
    std::unique_ptr<void, decltype(finish_render)> active_guard(this, finish_render);

    auto source_result = invoke<std::shared_ptr<PdfTileDisplayList>>(priority, [this, request](PdfDocument& document) {
        return display_list(document, request.page_index);
    });
    if(source_result) {
        return source_result.value()->render(request);
    }
    if(source_result.error().code() != ErrorCode::unsupported_document) {
        return Result<RenderedTile>::failure(source_result.error());
    }
    return invoke<RenderedTile>(priority, [request](PdfDocument& document) {
        return document.render_tile(request);
    });
}

Result<PageText> DocumentSession::extract_text(std::size_t page_index) {
    return invoke<PageText>(SessionPriority::indexing, [page_index](PdfDocument& document) {
        return document.extract_text(page_index);
    });
}

Result<PageTextLayout> DocumentSession::page_text_layout(std::size_t page_index) {
    return invoke<PageTextLayout>(SessionPriority::adjacent_page, [page_index](PdfDocument& document) {
        return document.page_text_layout(page_index);
    });
}

Result<TextSelection> DocumentSession::select_text(
    std::size_t page_index,
    PagePoint start_point,
    PagePoint end_point
) {
    return invoke<TextSelection>(SessionPriority::visible_tile, [page_index, start_point, end_point](PdfDocument& document) {
        return document.select_text(page_index, start_point, end_point);
    });
}

std::size_t DocumentSession::next_task_index(std::chrono::steady_clock::time_point now) const {
    std::size_t selected = 0;
    auto selected_priority = static_cast<int>(SessionPriority::indexing);
    for(std::size_t index = 0; index < queue_.size(); ++index) {
        const auto waited = now - queue_[index].enqueued_at;
        const auto promotions = std::chrono::duration_cast<std::chrono::seconds>(waited).count() /
            priority_aging_interval.count();
        const auto effective = std::max(0, static_cast<int>(queue_[index].priority) - static_cast<int>(promotions));
        if(effective < selected_priority) {
            selected = index;
            selected_priority = effective;
        }
    }
    return selected;
}

Result<std::shared_ptr<PdfTileDisplayList>> DocumentSession::display_list(
    PdfDocument& document,
    std::size_t page_index
) {
    ++display_list_clock_;
    const auto existing = std::find_if(
        display_lists_.begin(),
        display_lists_.end(),
        [page_index](const DisplayListEntry& entry) { return entry.page_index == page_index; }
    );
    if(existing != display_lists_.end()) {
        existing->last_used = display_list_clock_;
        return Result<std::shared_ptr<PdfTileDisplayList>>::success(existing->display_list);
    }

    auto created = document.create_tile_display_list(page_index);
    if(!created) {
        return Result<std::shared_ptr<PdfTileDisplayList>>::failure(created.error());
    }
    auto source = std::move(created).value();
    const auto bytes = source->estimated_bytes();
    while(!display_lists_.empty() &&
          (display_lists_.size() >= maximum_display_list_pages ||
           bytes > maximum_display_list_bytes - std::min(display_list_bytes_, maximum_display_list_bytes))) {
        const auto oldest = std::min_element(
            display_lists_.begin(),
            display_lists_.end(),
            [](const DisplayListEntry& lhs, const DisplayListEntry& rhs) {
                return lhs.last_used < rhs.last_used;
            }
        );
        display_list_bytes_ -= oldest->bytes;
        display_lists_.erase(oldest);
    }
    if(bytes <= maximum_display_list_bytes) {
        display_list_bytes_ += bytes;
        display_lists_.push_back(DisplayListEntry{
            .page_index = page_index,
            .bytes = bytes,
            .last_used = display_list_clock_,
            .display_list = source,
        });
    }
    return Result<std::shared_ptr<PdfTileDisplayList>>::success(std::move(source));
}

void DocumentSession::run() {
    for(;;) {
        QueuedTask task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if(queue_.empty() && stopping_) {
                return;
            }
            const auto index = next_task_index(std::chrono::steady_clock::now());
            task = std::move(queue_[index]);
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        task.operation(*document_);
    }
}

}  // namespace context_reader
