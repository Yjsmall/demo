#pragma once

#include <filesystem>
#include <memory>

#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

class MuPdfEngine final : public PdfEngine {
public:
    [[nodiscard]] static constexpr const char* version() noexcept { return "1.28.3"; }

    [[nodiscard]] Result<std::unique_ptr<PdfDocument>> open(
        const std::filesystem::path& source
    ) override;
};

}  // namespace context_reader
