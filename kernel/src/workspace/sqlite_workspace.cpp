#include "context_reader/workspace/sqlite_workspace.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "context_reader/annotation/annotation.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

namespace {

constexpr std::uint32_t current_schema_version = 2;

class AlgorithmHandle final {
public:
    AlgorithmHandle() = default;
    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    ~AlgorithmHandle() {
        if(value_ != nullptr) {
            BCryptCloseAlgorithmProvider(value_, 0);
        }
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE* address() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class HashHandle final {
public:
    HashHandle() = default;
    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    ~HashHandle() {
        if(value_ != nullptr) {
            BCryptDestroyHash(value_);
        }
    }

    [[nodiscard]] BCRYPT_HASH_HANDLE* address() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_HASH_HANDLE value_ = nullptr;
};

class Statement final {
public:
    Statement() = default;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept {
        if(this != &other) {
            if(value_ != nullptr) {
                sqlite3_finalize(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    ~Statement() {
        if(value_ != nullptr) {
            sqlite3_finalize(value_);
        }
    }

    [[nodiscard]] sqlite3_stmt** address() noexcept { return &value_; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return value_; }

private:
    sqlite3_stmt* value_ = nullptr;
};

class Transaction final {
public:
    explicit Transaction(sqlite3* database) noexcept : database_(database) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if(active_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void begin() noexcept { active_ = true; }
    void commit() noexcept { active_ = false; }

private:
    sqlite3* database_;
    bool active_ = false;
};

[[nodiscard]] bool nt_success(NTSTATUS status) noexcept {
    return status >= 0;
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] std::string hex_string(const std::array<std::uint8_t, 32>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for(const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] Result<std::string> sha256_file(
    const std::filesystem::path& path,
    const CancellationToken& cancellation = CancellationToken{}
) {
    AlgorithmHandle algorithm;
    if(!nt_success(BCryptOpenAlgorithmProvider(
           algorithm.address(),
           BCRYPT_SHA256_ALGORITHM,
           nullptr,
           0
       ))) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 provider initialization failed")
        );
    }

    DWORD object_size = 0;
    DWORD result_size = 0;
    if(!nt_success(BCryptGetProperty(
           algorithm.get(),
           BCRYPT_OBJECT_LENGTH,
           reinterpret_cast<PUCHAR>(&object_size),
           sizeof(object_size),
           &result_size,
           0
       ))) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 provider query failed")
        );
    }

    std::vector<std::uint8_t> hash_object(object_size);
    HashHandle hash;
    if(!nt_success(BCryptCreateHash(
           algorithm.get(),
           hash.address(),
           hash_object.data(),
           object_size,
           nullptr,
           0,
           0
       ))) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 state creation failed")
        );
    }

    std::ifstream input(path, std::ios::binary);
    if(!input) {
        return Result<std::string>::failure(
            Error(ErrorCode::not_found, "source file could not be opened")
        );
    }
    std::array<char, 64 * 1024> buffer{};
    while(input) {
        if(cancellation.is_cancellation_requested()) {
            return Result<std::string>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = input.gcount();
        if(bytes_read > 0 && !nt_success(BCryptHashData(
                               hash.get(),
                               reinterpret_cast<PUCHAR>(buffer.data()),
                               static_cast<ULONG>(bytes_read),
                               0
                           ))) {
            return Result<std::string>::failure(
                Error(ErrorCode::storage_failure, "SHA-256 update failed")
            );
        }
    }
    if(!input.eof()) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "source file could not be read")
        );
    }

    std::array<std::uint8_t, 32> bytes{};
    if(!nt_success(BCryptFinishHash(
           hash.get(),
           bytes.data(),
           static_cast<ULONG>(bytes.size()),
           0
       ))) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 finalization failed")
        );
    }
    return Result<std::string>::success(hex_string(bytes));
}

[[nodiscard]] Result<void> copy_file_cancellable(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const CancellationToken& cancellation
) {
    auto partial = destination;
    partial += ".partial";
    std::error_code filesystem_error;
    std::filesystem::remove(partial, filesystem_error);

    std::ifstream input(source, std::ios::binary);
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if(!input || !output) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "PDF object could not be stored")
        );
    }

    std::array<char, 64 * 1024> buffer{};
    while(input) {
        if(cancellation.is_cancellation_requested()) {
            input.close();
            output.close();
            std::filesystem::remove(partial, filesystem_error);
            return Result<void>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = input.gcount();
        if(bytes_read > 0) {
            output.write(buffer.data(), bytes_read);
        }
    }
    output.close();
    if(!input.eof() || !output) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "PDF object could not be stored")
        );
    }
    if(cancellation.is_cancellation_requested()) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::cancelled, "Document import was cancelled")
        );
    }

    std::filesystem::rename(partial, destination, filesystem_error);
    if(filesystem_error) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "PDF object could not be stored")
        );
    }
    return Result<void>::success();
}

void terminate_at_import_fault_point(std::string_view point) {
    std::array<char, 64> configured{};
    const auto length = GetEnvironmentVariableA(
        "CONTEXT_READER_TEST_IMPORT_FAULT",
        configured.data(),
        static_cast<DWORD>(configured.size())
    );
    if(length > 0 && length < configured.size()
       && std::string_view(configured.data(), length) == point) {
        std::_Exit(86);
    }
}

[[nodiscard]] Result<void> execute(sqlite3* database, const char* sql) {
    if(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "SQLite command failed")
        );
    }
    return Result<void>::success();
}

[[nodiscard]] Result<Statement> prepare(sqlite3* database, const char* sql) {
    Statement statement;
    if(sqlite3_prepare_v2(database, sql, -1, statement.address(), nullptr) != SQLITE_OK) {
        return Result<Statement>::failure(
            Error(ErrorCode::storage_failure, "SQLite statement preparation failed")
        );
    }
    return Result<Statement>::success(std::move(statement));
}

[[nodiscard]] bool bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    return sqlite3_bind_text64(
               statement,
               index,
               value.data(),
               static_cast<sqlite3_uint64>(value.size()),
               SQLITE_TRANSIENT,
               SQLITE_UTF8
           ) == SQLITE_OK;
}

template <typename Id>
[[nodiscard]] bool bind_id(sqlite3_stmt* statement, int index, const Id& id) {
    return sqlite3_bind_blob(
               statement,
               index,
               id.bytes().data(),
               static_cast<int>(id.bytes().size()),
               SQLITE_TRANSIENT
           ) == SQLITE_OK;
}

template <typename Id>
[[nodiscard]] Result<Id> read_id(sqlite3_stmt* statement, int column) {
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, column));
    const auto size = sqlite3_column_bytes(statement, column);
    if(data == nullptr || size != 16) {
        return Result<Id>::failure(
            Error(ErrorCode::storage_failure, "SQLite stable ID is invalid")
        );
    }
    typename Id::Bytes bytes{};
    std::copy_n(data, bytes.size(), bytes.begin());
    return Result<Id>::success(Id::from_bytes(bytes));
}

[[nodiscard]] Result<std::pair<DocumentId, DocumentVersionId>> create_id_pair(sqlite3* database) {
    auto statement_result = prepare(database, "SELECT randomblob(16), randomblob(16)");
    if(!statement_result) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(
            Error(ErrorCode::storage_failure, "SQLite ID generation failed")
        );
    }
    auto document_id = read_id<DocumentId>(statement.get(), 0);
    auto version_id = read_id<DocumentVersionId>(statement.get(), 1);
    if(!document_id) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(document_id.error());
    }
    if(!version_id) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(version_id.error());
    }
    return Result<std::pair<DocumentId, DocumentVersionId>>::success(
        std::make_pair(document_id.value(), version_id.value())
    );
}

template <typename Id>
[[nodiscard]] Result<Id> create_id(sqlite3* database) {
    auto statement_result = prepare(database, "SELECT randomblob(16)");
    if(!statement_result) {
        return Result<Id>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<Id>::failure(Error(ErrorCode::storage_failure, "SQLite ID generation failed"));
    }
    return read_id<Id>(statement.get(), 0);
}

[[nodiscard]] const char* color_name(HighlightColor color) noexcept {
    switch(color) {
        case HighlightColor::yellow: return "yellow";
        case HighlightColor::green: return "green";
        case HighlightColor::blue: return "blue";
        case HighlightColor::pink: return "pink";
    }
    return "yellow";
}

[[nodiscard]] Result<HighlightColor> read_color(std::string_view color) {
    if(color == "yellow") return Result<HighlightColor>::success(HighlightColor::yellow);
    if(color == "green") return Result<HighlightColor>::success(HighlightColor::green);
    if(color == "blue") return Result<HighlightColor>::success(HighlightColor::blue);
    if(color == "pink") return Result<HighlightColor>::success(HighlightColor::pink);
    return Result<HighlightColor>::failure(
        Error(ErrorCode::storage_failure, "SQLite annotation color is invalid")
    );
}

[[nodiscard]] Result<DocumentRecord> read_document_record(sqlite3_stmt* statement) {
    auto document_id = read_id<DocumentId>(statement, 0);
    auto version_id = read_id<DocumentVersionId>(statement, 1);
    if(!document_id) {
        return Result<DocumentRecord>::failure(document_id.error());
    }
    if(!version_id) {
        return Result<DocumentRecord>::failure(version_id.error());
    }

    const auto byte_length = sqlite3_column_int64(statement, 6);
    const auto page_count = sqlite3_column_int64(statement, 7);
    if(byte_length < 0 || page_count < 0) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::storage_failure, "SQLite document metadata is invalid")
        );
    }

    const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    const auto* content_hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
    const auto* object_key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
    if(title == nullptr || content_hash == nullptr || object_key == nullptr) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::storage_failure, "SQLite document text metadata is invalid")
        );
    }

    return Result<DocumentRecord>::success(DocumentRecord{
        .document_id = document_id.value(),
        .version_id = version_id.value(),
        .title = title,
        .content_sha256 = content_hash,
        .object_key = object_key,
        .byte_length = static_cast<std::uint64_t>(byte_length),
        .page_count = static_cast<std::size_t>(page_count),
    });
}

[[nodiscard]] Result<WorkspaceInfo> read_workspace_info(sqlite3* database) {
    auto statement_result = prepare(
        database,
        "SELECT workspace_id, schema_version FROM workspace_metadata WHERE singleton = 1"
    );
    if(!statement_result) {
        return Result<WorkspaceInfo>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<WorkspaceInfo>::failure(
            Error(ErrorCode::storage_failure, "Workspace metadata is missing")
        );
    }
    auto workspace_id = read_id<WorkspaceId>(statement.get(), 0);
    const auto schema_version = sqlite3_column_int64(statement.get(), 1);
    if(!workspace_id) {
        return Result<WorkspaceInfo>::failure(workspace_id.error());
    }
    if(schema_version != current_schema_version) {
        return Result<WorkspaceInfo>::failure(
            Error(ErrorCode::unsupported_document, "Workspace schema version is unsupported")
        );
    }
    return Result<WorkspaceInfo>::success(WorkspaceInfo{
        .id = workspace_id.value(),
        .schema_version = current_schema_version,
    });
}

[[nodiscard]] Result<void> configure_database(sqlite3* database) {
    if(sqlite3_busy_timeout(database, 5000) != SQLITE_OK) {
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "SQLite busy timeout configuration failed")
        );
    }
    for(const auto* command : {
            "PRAGMA foreign_keys = ON",
            "PRAGMA journal_mode = WAL",
            "PRAGMA synchronous = FULL",
        }) {
        auto result = execute(database, command);
        if(!result) {
            return result;
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::size_t> query_count(sqlite3* database, const char* sql);

[[nodiscard]] Result<void> apply_initial_migration(sqlite3* database) {
    constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE workspace_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    workspace_id BLOB NOT NULL CHECK (length(workspace_id) = 16),
    schema_version INTEGER NOT NULL,
    created_at INTEGER NOT NULL
) STRICT;
CREATE TABLE documents (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    title TEXT NOT NULL,
    active_version_id BLOB,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (active_version_id) REFERENCES document_versions(id)
) STRICT;
CREATE TABLE document_versions (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    document_id BLOB NOT NULL REFERENCES documents(id),
    content_hash TEXT NOT NULL UNIQUE CHECK (length(content_hash) = 64),
    object_key TEXT NOT NULL UNIQUE,
    byte_length INTEGER NOT NULL CHECK (byte_length >= 0),
    page_count INTEGER NOT NULL CHECK (page_count >= 0),
    created_at INTEGER NOT NULL
) STRICT;
CREATE INDEX document_versions_document_id_idx ON document_versions(document_id);
CREATE TABLE annotations (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    document_version_id BLOB NOT NULL REFERENCES document_versions(id) ON DELETE CASCADE,
    page_index INTEGER NOT NULL CHECK (page_index >= 0),
    quote_exact TEXT NOT NULL,
    quote_prefix TEXT NOT NULL,
    quote_suffix TEXT NOT NULL,
    layout_version TEXT NOT NULL,
    color TEXT NOT NULL CHECK (color IN ('yellow', 'green', 'blue', 'pink')),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
) STRICT;
CREATE INDEX annotations_version_page_idx ON annotations(document_version_id, page_index);
CREATE TABLE annotation_quads (
    annotation_id BLOB NOT NULL REFERENCES annotations(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
    x REAL NOT NULL,
    y REAL NOT NULL,
    width REAL NOT NULL CHECK (width > 0),
    height REAL NOT NULL CHECK (height > 0),
    PRIMARY KEY(annotation_id, ordinal)
) STRICT;
CREATE TABLE notes (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    annotation_id BLOB NOT NULL UNIQUE REFERENCES annotations(id) ON DELETE CASCADE,
    markdown_source TEXT NOT NULL,
    revision INTEGER NOT NULL CHECK (revision > 0),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
) STRICT;
INSERT INTO workspace_metadata(singleton, workspace_id, schema_version, created_at)
VALUES (1, randomblob(16), 2, unixepoch());
PRAGMA user_version = 2;
COMMIT;
)sql";

    auto result = execute(database, migration);
    if(!result) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return result;
}

[[nodiscard]] Result<void> migrate_database(sqlite3* database) {
    auto version_result = query_count(database, "PRAGMA user_version");
    if(!version_result) {
        return Result<void>::failure(version_result.error());
    }
    if(version_result.value() == current_schema_version) {
        return Result<void>::success();
    }
    if(version_result.value() != 1U) {
        return Result<void>::failure(
            Error(ErrorCode::unsupported_document, "Workspace schema version is unsupported")
        );
    }
    constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE annotations (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    document_version_id BLOB NOT NULL REFERENCES document_versions(id) ON DELETE CASCADE,
    page_index INTEGER NOT NULL CHECK (page_index >= 0),
    quote_exact TEXT NOT NULL,
    quote_prefix TEXT NOT NULL,
    quote_suffix TEXT NOT NULL,
    layout_version TEXT NOT NULL,
    color TEXT NOT NULL CHECK (color IN ('yellow', 'green', 'blue', 'pink')),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
) STRICT;
CREATE INDEX annotations_version_page_idx ON annotations(document_version_id, page_index);
CREATE TABLE annotation_quads (
    annotation_id BLOB NOT NULL REFERENCES annotations(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
    x REAL NOT NULL,
    y REAL NOT NULL,
    width REAL NOT NULL CHECK (width > 0),
    height REAL NOT NULL CHECK (height > 0),
    PRIMARY KEY(annotation_id, ordinal)
) STRICT;
CREATE TABLE notes (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    annotation_id BLOB NOT NULL UNIQUE REFERENCES annotations(id) ON DELETE CASCADE,
    markdown_source TEXT NOT NULL,
    revision INTEGER NOT NULL CHECK (revision > 0),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
) STRICT;
UPDATE workspace_metadata SET schema_version = 2 WHERE singleton = 1;
PRAGMA user_version = 2;
COMMIT;
)sql";
    auto result = execute(database, migration);
    if(!result) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return result;
}

[[nodiscard]] Result<std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>> open_database(
    const std::filesystem::path& database_path,
    bool create
) {
    sqlite3* raw_database = nullptr;
    const auto path = utf8_path(database_path);
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
                      (create ? SQLITE_OPEN_CREATE : 0);
    if(sqlite3_open_v2(path.c_str(), &raw_database, flags, nullptr) != SQLITE_OK) {
        if(raw_database != nullptr) {
            sqlite3_close_v2(raw_database);
        }
        return Result<std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>>::failure(
            Error(ErrorCode::storage_failure, "Workspace database could not be opened")
        );
    }
    return Result<std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>>::success(
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>(raw_database, sqlite3_close_v2)
    );
}

[[nodiscard]] Result<std::size_t> query_count(sqlite3* database, const char* sql) {
    auto statement_result = prepare(database, sql);
    if(!statement_result) {
        return Result<std::size_t>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::storage_failure, "SQLite count query failed")
        );
    }
    const auto value = sqlite3_column_int64(statement.get(), 0);
    if(value < 0) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::storage_failure, "SQLite count is invalid")
        );
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(value));
}

}  // namespace

class SqliteWorkspace::Impl final {
public:
    Impl(
        std::filesystem::path root,
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database,
        PdfEngine& pdf_engine,
        WorkspaceInfo info
    ) noexcept
        : root_(std::move(root)),
          database_(std::move(database)),
          pdf_engine_(pdf_engine),
          info_(info) {}

    [[nodiscard]] WorkspaceInfo info() const noexcept { return info_; }

    [[nodiscard]] Result<ImportDocumentResult> import_pdf(
        const std::filesystem::path& source,
        const CancellationToken& cancellation
    ) {
        const std::scoped_lock lock(mutex_);
        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        std::error_code filesystem_error;
        if(!std::filesystem::is_regular_file(source, filesystem_error) || filesystem_error) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::not_found, "PDF source file was not found")
            );
        }

        auto document_result = pdf_engine_.open(source);
        if(!document_result) {
            return Result<ImportDocumentResult>::failure(document_result.error());
        }
        const auto page_count = document_result.value()->page_count();

        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }

        auto hash_result = sha256_file(source, cancellation);
        if(!hash_result) {
            return Result<ImportDocumentResult>::failure(hash_result.error());
        }
        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        const auto& content_hash = hash_result.value();
        auto existing = find_by_hash(content_hash);
        if(!existing) {
            return Result<ImportDocumentResult>::failure(existing.error());
        }
        if(existing.value().has_value()) {
            if(cancellation.is_cancellation_requested()) {
                return Result<ImportDocumentResult>::failure(
                    Error(ErrorCode::cancelled, "Document import was cancelled")
                );
            }
            return Result<ImportDocumentResult>::success(
                ImportDocumentResult{.document = std::move(*existing.value()), .reused_existing = true}
            );
        }

        const auto byte_length = std::filesystem::file_size(source, filesystem_error);
        if(filesystem_error || byte_length > static_cast<std::uintmax_t>(
                                               std::numeric_limits<sqlite3_int64>::max()
                                           ) ||
           page_count > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "PDF source metadata is out of range")
            );
        }

        const std::filesystem::path object_key = std::filesystem::path("objects") / "pdf" /
                                                 content_hash.substr(0, 2) /
                                                 (content_hash + ".pdf");
        const auto object_path = root_ / object_key;
        std::filesystem::create_directories(object_path.parent_path(), filesystem_error);
        if(filesystem_error) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "PDF object directory could not be created")
            );
        }
        if(std::filesystem::exists(object_path, filesystem_error)) {
            auto object_hash = sha256_file(object_path, cancellation);
            if(!object_hash || object_hash.value() != content_hash) {
                return Result<ImportDocumentResult>::failure(
                    Error(ErrorCode::conflict, "Existing PDF object does not match its content key")
                );
            }
        } else {
            auto copy_result = copy_file_cancellable(source, object_path, cancellation);
            if(!copy_result) return Result<ImportDocumentResult>::failure(copy_result.error());
        }

        auto ids_result = create_id_pair(database_.get());
        if(!ids_result) {
            return Result<ImportDocumentResult>::failure(ids_result.error());
        }
        const auto [document_id, version_id] = ids_result.value();
        auto title = utf8_path(source.stem());
        if(title.empty()) {
            title = "Untitled";
        }

        Transaction transaction(database_.get());
        auto begin_result = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin_result) {
            return Result<ImportDocumentResult>::failure(begin_result.error());
        }
        transaction.begin();

        auto document_insert = prepare(
            database_.get(),
            "INSERT INTO documents(id, title, active_version_id, created_at) "
            "VALUES(?1, ?2, NULL, unixepoch())"
        );
        if(!document_insert || !bind_id(document_insert.value().get(), 1, document_id) ||
           !bind_text(document_insert.value().get(), 2, title) ||
           sqlite3_step(document_insert.value().get()) != SQLITE_DONE) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "Document record could not be inserted")
            );
        }

        const auto object_key_utf8 = generic_utf8_path(object_key);
        auto version_insert = prepare(
            database_.get(),
            "INSERT INTO document_versions("
            "id, document_id, content_hash, object_key, byte_length, page_count, created_at"
            ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, unixepoch())"
        );
        if(!version_insert || !bind_id(version_insert.value().get(), 1, version_id) ||
           !bind_id(version_insert.value().get(), 2, document_id) ||
           !bind_text(version_insert.value().get(), 3, content_hash) ||
           !bind_text(version_insert.value().get(), 4, object_key_utf8) ||
           sqlite3_bind_int64(
               version_insert.value().get(),
               5,
               static_cast<sqlite3_int64>(byte_length)
           ) != SQLITE_OK ||
           sqlite3_bind_int64(
               version_insert.value().get(),
               6,
               static_cast<sqlite3_int64>(page_count)
           ) != SQLITE_OK ||
           sqlite3_step(version_insert.value().get()) != SQLITE_DONE) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "Document version could not be inserted")
            );
        }

        auto activate = prepare(
            database_.get(),
            "UPDATE documents SET active_version_id = ?1 WHERE id = ?2"
        );
        if(!activate || !bind_id(activate.value().get(), 1, version_id) ||
           !bind_id(activate.value().get(), 2, document_id) ||
           sqlite3_step(activate.value().get()) != SQLITE_DONE ||
           sqlite3_changes(database_.get()) != 1) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "Document version could not be activated")
            );
        }

        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        terminate_at_import_fault_point("before-commit");
        auto commit_result = execute(database_.get(), "COMMIT");
        if(!commit_result) {
            return Result<ImportDocumentResult>::failure(commit_result.error());
        }
        transaction.commit();
        terminate_at_import_fault_point("after-commit");

        return Result<ImportDocumentResult>::success(ImportDocumentResult{
            .document = DocumentRecord{
                .document_id = document_id,
                .version_id = version_id,
                .title = std::move(title),
                .content_sha256 = content_hash,
                .object_key = object_key_utf8,
                .byte_length = static_cast<std::uint64_t>(byte_length),
                .page_count = page_count,
            },
            .reused_existing = false,
        });
    }

    [[nodiscard]] Result<std::vector<DocumentRecord>> list_documents() {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(
            database_.get(),
            "SELECT d.id, v.id, d.title, v.content_hash, v.object_key, d.active_version_id, "
            "v.byte_length, v.page_count "
            "FROM documents d JOIN document_versions v ON v.id = d.active_version_id "
            "ORDER BY d.created_at, d.rowid"
        );
        if(!statement_result) {
            return Result<std::vector<DocumentRecord>>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        std::vector<DocumentRecord> documents;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto record = read_document_record(statement.get());
            if(!record) {
                return Result<std::vector<DocumentRecord>>::failure(record.error());
            }
            documents.push_back(std::move(record).value());
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::vector<DocumentRecord>>::failure(
                Error(ErrorCode::storage_failure, "Document records could not be listed")
            );
        }
        return Result<std::vector<DocumentRecord>>::success(std::move(documents));
    }

    [[nodiscard]] Result<ResolvedDocumentObject> resolve_document(DocumentId document_id) {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(
            database_.get(),
            "SELECT d.id, v.id, d.title, v.content_hash, v.object_key, d.active_version_id, "
            "v.byte_length, v.page_count "
            "FROM documents d JOIN document_versions v ON v.id = d.active_version_id "
            "WHERE d.id = ?1 LIMIT 1"
        );
        if(!statement_result) {
            return Result<ResolvedDocumentObject>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        if(!bind_id(statement.get(), 1, document_id)) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document ID could not be bound")
            );
        }
        const auto step_result = sqlite3_step(statement.get());
        if(step_result == SQLITE_DONE) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::not_found, "Document was not found")
            );
        }
        if(step_result != SQLITE_ROW) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document lookup failed")
            );
        }
        auto record_result = read_document_record(statement.get());
        if(!record_result) {
            return Result<ResolvedDocumentObject>::failure(record_result.error());
        }
        auto record = std::move(record_result).value();
        const std::filesystem::path object_key(record.object_key);
        bool unsafe_path = object_key.empty() || object_key.is_absolute();
        for(const auto& component : object_key) {
            if(component == "..") {
                unsafe_path = true;
            }
        }
        if(unsafe_path) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document object path is invalid")
            );
        }
        const auto object_path = root_ / object_key;
        std::error_code filesystem_error;
        if(!std::filesystem::is_regular_file(object_path, filesystem_error) || filesystem_error) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document object is missing")
            );
        }
        return Result<ResolvedDocumentObject>::success(
            ResolvedDocumentObject{
                .document = std::move(record),
                .path = object_path,
            }
        );
    }

    [[nodiscard]] Result<AnnotationRecord> create_annotation(const CreateAnnotation& command) {
        const std::scoped_lock lock(mutex_);
        if(command.quads.empty() || command.quote.exact.empty() || command.layout_version.empty()
           || command.page_index > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::invalid_argument, "Annotation anchor is incomplete")
            );
        }
        for(const auto& quad : command.quads) {
            if(!std::isfinite(quad.x) || !std::isfinite(quad.y) || !std::isfinite(quad.width)
               || !std::isfinite(quad.height) || quad.width <= 0.0 || quad.height <= 0.0) {
                return Result<AnnotationRecord>::failure(
                    Error(ErrorCode::invalid_argument, "Annotation quad is invalid")
                );
            }
        }

        auto version_lookup = prepare(
            database_.get(),
            "SELECT page_count FROM document_versions WHERE id = ?1"
        );
        if(!version_lookup || !bind_id(version_lookup.value().get(), 1, command.document_version_id)) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::storage_failure, "Document version lookup could not be prepared")
            );
        }
        const auto version_step = sqlite3_step(version_lookup.value().get());
        if(version_step == SQLITE_DONE) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::not_found, "Document version was not found")
            );
        }
        if(version_step != SQLITE_ROW
           || command.page_index >= static_cast<std::size_t>(sqlite3_column_int64(version_lookup.value().get(), 0))) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::invalid_argument, "Annotation page index is out of range")
            );
        }

        auto id_result = create_id<AnnotationId>(database_.get());
        if(!id_result) {
            return Result<AnnotationRecord>::failure(id_result.error());
        }
        const auto id = id_result.value();
        Transaction transaction(database_.get());
        auto begin_result = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin_result) {
            return Result<AnnotationRecord>::failure(begin_result.error());
        }
        transaction.begin();

        auto annotation_insert = prepare(
            database_.get(),
            "INSERT INTO annotations(id, document_version_id, page_index, quote_exact, "
            "quote_prefix, quote_suffix, layout_version, color, created_at, updated_at) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, unixepoch(), unixepoch())"
        );
        if(!annotation_insert || !bind_id(annotation_insert.value().get(), 1, id)
           || !bind_id(annotation_insert.value().get(), 2, command.document_version_id)
           || sqlite3_bind_int64(annotation_insert.value().get(), 3, static_cast<sqlite3_int64>(command.page_index)) != SQLITE_OK
           || !bind_text(annotation_insert.value().get(), 4, command.quote.exact)
           || !bind_text(annotation_insert.value().get(), 5, command.quote.prefix)
           || !bind_text(annotation_insert.value().get(), 6, command.quote.suffix)
           || !bind_text(annotation_insert.value().get(), 7, command.layout_version)
           || !bind_text(annotation_insert.value().get(), 8, color_name(command.color))
           || sqlite3_step(annotation_insert.value().get()) != SQLITE_DONE) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::storage_failure, "Annotation could not be inserted")
            );
        }

        auto quad_insert = prepare(
            database_.get(),
            "INSERT INTO annotation_quads(annotation_id, ordinal, x, y, width, height) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
        );
        if(!quad_insert) {
            return Result<AnnotationRecord>::failure(quad_insert.error());
        }
        for(std::size_t index = 0; index < command.quads.size(); ++index) {
            const auto& quad = command.quads[index];
            auto* statement = quad_insert.value().get();
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if(!bind_id(statement, 1, id)
               || sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(index)) != SQLITE_OK
               || sqlite3_bind_double(statement, 3, quad.x) != SQLITE_OK
               || sqlite3_bind_double(statement, 4, quad.y) != SQLITE_OK
               || sqlite3_bind_double(statement, 5, quad.width) != SQLITE_OK
               || sqlite3_bind_double(statement, 6, quad.height) != SQLITE_OK
               || sqlite3_step(statement) != SQLITE_DONE) {
                return Result<AnnotationRecord>::failure(
                    Error(ErrorCode::storage_failure, "Annotation quad could not be inserted")
                );
            }
        }
        auto commit_result = execute(database_.get(), "COMMIT");
        if(!commit_result) {
            return Result<AnnotationRecord>::failure(commit_result.error());
        }
        transaction.commit();
        return Result<AnnotationRecord>::success(AnnotationRecord{
            .id = id,
            .document_version_id = command.document_version_id,
            .page_index = command.page_index,
            .quads = command.quads,
            .quote = command.quote,
            .layout_version = command.layout_version,
            .color = command.color,
        });
    }

    [[nodiscard]] Result<std::vector<AnnotationRecord>> list_annotations(
        DocumentVersionId document_version_id
    ) {
        const std::scoped_lock lock(mutex_);
        auto annotations_result = prepare(
            database_.get(),
            "SELECT id, document_version_id, page_index, quote_exact, quote_prefix, quote_suffix, "
            "layout_version, color FROM annotations WHERE document_version_id = ?1 "
            "ORDER BY page_index, created_at, rowid"
        );
        if(!annotations_result || !bind_id(annotations_result.value().get(), 1, document_version_id)) {
            return Result<std::vector<AnnotationRecord>>::failure(
                Error(ErrorCode::storage_failure, "Annotations could not be listed")
            );
        }
        auto annotations = std::move(annotations_result).value();
        std::vector<AnnotationRecord> records;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(annotations.get())) == SQLITE_ROW) {
            auto id = read_id<AnnotationId>(annotations.get(), 0);
            auto version_id = read_id<DocumentVersionId>(annotations.get(), 1);
            const auto page_index = sqlite3_column_int64(annotations.get(), 2);
            const auto* exact = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 3));
            const auto* prefix = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 4));
            const auto* suffix = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 5));
            const auto* layout = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 6));
            const auto* color_text = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 7));
            if(!id || !version_id || page_index < 0 || exact == nullptr || prefix == nullptr
               || suffix == nullptr || layout == nullptr || color_text == nullptr) {
                return Result<std::vector<AnnotationRecord>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite annotation record is invalid")
                );
            }
            auto color = read_color(color_text);
            if(!color) {
                return Result<std::vector<AnnotationRecord>>::failure(color.error());
            }
            auto quads_result = prepare(
                database_.get(),
                "SELECT x, y, width, height FROM annotation_quads WHERE annotation_id = ?1 ORDER BY ordinal"
            );
            if(!quads_result || !bind_id(quads_result.value().get(), 1, id.value())) {
                return Result<std::vector<AnnotationRecord>>::failure(
                    Error(ErrorCode::storage_failure, "Annotation quads could not be listed")
                );
            }
            std::vector<PageRect> quads;
            int quad_step = SQLITE_ROW;
            while((quad_step = sqlite3_step(quads_result.value().get())) == SQLITE_ROW) {
                quads.push_back(PageRect{
                    .x = sqlite3_column_double(quads_result.value().get(), 0),
                    .y = sqlite3_column_double(quads_result.value().get(), 1),
                    .width = sqlite3_column_double(quads_result.value().get(), 2),
                    .height = sqlite3_column_double(quads_result.value().get(), 3),
                });
            }
            if(quad_step != SQLITE_DONE || quads.empty()) {
                return Result<std::vector<AnnotationRecord>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite annotation quads are invalid")
                );
            }
            records.push_back(AnnotationRecord{
                .id = id.value(),
                .document_version_id = version_id.value(),
                .page_index = static_cast<std::size_t>(page_index),
                .quads = std::move(quads),
                .quote = QuoteAnchor{.exact = exact, .prefix = prefix, .suffix = suffix},
                .layout_version = layout,
                .color = color.value(),
            });
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::vector<AnnotationRecord>>::failure(
                Error(ErrorCode::storage_failure, "Annotations could not be listed")
            );
        }
        return Result<std::vector<AnnotationRecord>>::success(std::move(records));
    }

    [[nodiscard]] Result<void> delete_annotation(AnnotationId annotation_id) {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(database_.get(), "DELETE FROM annotations WHERE id = ?1");
        if(!statement_result || !bind_id(statement_result.value().get(), 1, annotation_id)
           || sqlite3_step(statement_result.value().get()) != SQLITE_DONE) {
            return Result<void>::failure(
                Error(ErrorCode::storage_failure, "Annotation could not be deleted")
            );
        }
        if(sqlite3_changes(database_.get()) != 1) {
            return Result<void>::failure(Error(ErrorCode::not_found, "Annotation was not found"));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<NoteRecord> update_note(const UpdateNote& command) {
        const std::scoped_lock lock(mutex_);
        auto lookup_result = prepare(
            database_.get(),
            "SELECT n.id, n.revision FROM annotations a LEFT JOIN notes n ON n.annotation_id = a.id "
            "WHERE a.id = ?1"
        );
        if(!lookup_result || !bind_id(lookup_result.value().get(), 1, command.annotation_id)) {
            return Result<NoteRecord>::failure(
                Error(ErrorCode::storage_failure, "Note lookup could not be prepared")
            );
        }
        auto lookup = std::move(lookup_result).value();
        const auto step_result = sqlite3_step(lookup.get());
        if(step_result == SQLITE_DONE) {
            return Result<NoteRecord>::failure(Error(ErrorCode::not_found, "Annotation was not found"));
        }
        if(step_result != SQLITE_ROW) {
            return Result<NoteRecord>::failure(Error(ErrorCode::storage_failure, "Note lookup failed"));
        }

        if(sqlite3_column_type(lookup.get(), 0) == SQLITE_NULL) {
            if(command.expected_revision != 0U) {
                return Result<NoteRecord>::failure(
                    Error(ErrorCode::conflict, "Note revision does not match")
                );
            }
            auto id_result = create_id<NoteId>(database_.get());
            if(!id_result) {
                return Result<NoteRecord>::failure(id_result.error());
            }
            auto insert_result = prepare(
                database_.get(),
                "INSERT INTO notes(id, annotation_id, markdown_source, revision, created_at, updated_at) "
                "VALUES(?1, ?2, ?3, 1, unixepoch(), unixepoch())"
            );
            if(!insert_result || !bind_id(insert_result.value().get(), 1, id_result.value())
               || !bind_id(insert_result.value().get(), 2, command.annotation_id)
               || !bind_text(insert_result.value().get(), 3, command.markdown_source)
               || sqlite3_step(insert_result.value().get()) != SQLITE_DONE) {
                return Result<NoteRecord>::failure(
                    Error(ErrorCode::storage_failure, "Note could not be created")
                );
            }
            return Result<NoteRecord>::success(NoteRecord{
                .id = id_result.value(),
                .annotation_id = command.annotation_id,
                .markdown_source = command.markdown_source,
                .revision = 1U,
            });
        }

        auto note_id = read_id<NoteId>(lookup.get(), 0);
        const auto revision = sqlite3_column_int64(lookup.get(), 1);
        if(!note_id || revision <= 0 || static_cast<std::uint64_t>(revision) != command.expected_revision
           || revision == std::numeric_limits<sqlite3_int64>::max()) {
            return Result<NoteRecord>::failure(
                Error(ErrorCode::conflict, "Note revision does not match")
            );
        }
        const auto next_revision = revision + 1;
        auto update_result = prepare(
            database_.get(),
            "UPDATE notes SET markdown_source = ?1, revision = ?2, updated_at = unixepoch() "
            "WHERE id = ?3 AND revision = ?4"
        );
        if(!update_result || !bind_text(update_result.value().get(), 1, command.markdown_source)
           || sqlite3_bind_int64(update_result.value().get(), 2, next_revision) != SQLITE_OK
           || !bind_id(update_result.value().get(), 3, note_id.value())
           || sqlite3_bind_int64(update_result.value().get(), 4, revision) != SQLITE_OK
           || sqlite3_step(update_result.value().get()) != SQLITE_DONE
           || sqlite3_changes(database_.get()) != 1) {
            return Result<NoteRecord>::failure(
                Error(ErrorCode::conflict, "Note revision does not match")
            );
        }
        return Result<NoteRecord>::success(NoteRecord{
            .id = note_id.value(),
            .annotation_id = command.annotation_id,
            .markdown_source = command.markdown_source,
            .revision = static_cast<std::uint64_t>(next_revision),
        });
    }

    [[nodiscard]] Result<std::vector<NoteRecord>> list_notes(
        DocumentVersionId document_version_id
    ) {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(
            database_.get(),
            "SELECT n.id, n.annotation_id, n.markdown_source, n.revision FROM notes n "
            "JOIN annotations a ON a.id = n.annotation_id WHERE a.document_version_id = ?1 "
            "ORDER BY n.created_at, n.rowid"
        );
        if(!statement_result || !bind_id(statement_result.value().get(), 1, document_version_id)) {
            return Result<std::vector<NoteRecord>>::failure(
                Error(ErrorCode::storage_failure, "Notes could not be listed")
            );
        }
        auto statement = std::move(statement_result).value();
        std::vector<NoteRecord> notes;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto note_id = read_id<NoteId>(statement.get(), 0);
            auto annotation_id = read_id<AnnotationId>(statement.get(), 1);
            const auto* markdown = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
            const auto revision = sqlite3_column_int64(statement.get(), 3);
            if(!note_id || !annotation_id || markdown == nullptr || revision <= 0) {
                return Result<std::vector<NoteRecord>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite note record is invalid")
                );
            }
            notes.push_back(NoteRecord{
                .id = note_id.value(),
                .annotation_id = annotation_id.value(),
                .markdown_source = markdown,
                .revision = static_cast<std::uint64_t>(revision),
            });
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::vector<NoteRecord>>::failure(
                Error(ErrorCode::storage_failure, "Notes could not be listed")
            );
        }
        return Result<std::vector<NoteRecord>>::success(std::move(notes));
    }

    [[nodiscard]] Result<WorkspaceVerification> verify() {
        const std::scoped_lock lock(mutex_);
        WorkspaceVerification verification{
            .valid = false,
            .document_count = 0,
            .document_version_count = 0,
            .referenced_object_count = 0,
            .issues = {},
        };

        auto document_count = query_count(database_.get(), "SELECT count(*) FROM documents");
        auto version_count = query_count(database_.get(), "SELECT count(*) FROM document_versions");
        if(!document_count) {
            return Result<WorkspaceVerification>::failure(document_count.error());
        }
        if(!version_count) {
            return Result<WorkspaceVerification>::failure(version_count.error());
        }
        verification.document_count = document_count.value();
        verification.document_version_count = version_count.value();

        auto quick_check = prepare(database_.get(), "PRAGMA quick_check");
        if(!quick_check) {
            return Result<WorkspaceVerification>::failure(quick_check.error());
        }
        if(sqlite3_step(quick_check.value().get()) != SQLITE_ROW ||
           std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(quick_check.value().get(), 0))) !=
               "ok") {
            verification.issues.emplace_back("sqlite_quick_check_failed");
        }

        auto active_mismatch = query_count(
            database_.get(),
            "SELECT count(*) FROM documents d "
            "LEFT JOIN document_versions v ON v.id = d.active_version_id AND v.document_id = d.id "
            "WHERE d.active_version_id IS NULL OR v.id IS NULL"
        );
        if(!active_mismatch) {
            return Result<WorkspaceVerification>::failure(active_mismatch.error());
        }
        if(active_mismatch.value() != 0) {
            verification.issues.emplace_back("document_active_version_mismatch");
        }

        auto foreign_keys = prepare(database_.get(), "PRAGMA foreign_key_check");
        if(!foreign_keys) {
            return Result<WorkspaceVerification>::failure(foreign_keys.error());
        }
        const auto foreign_key_step = sqlite3_step(foreign_keys.value().get());
        if(foreign_key_step == SQLITE_ROW) {
            verification.issues.emplace_back("sqlite_foreign_key_violation");
        } else if(foreign_key_step != SQLITE_DONE) {
            return Result<WorkspaceVerification>::failure(
                Error(ErrorCode::storage_failure, "SQLite foreign key check failed")
            );
        }

        auto missing_quads = query_count(
            database_.get(),
            "SELECT count(*) FROM annotations a LEFT JOIN annotation_quads q ON q.annotation_id = a.id "
            "WHERE q.annotation_id IS NULL"
        );
        auto page_mismatch = query_count(
            database_.get(),
            "SELECT count(*) FROM annotations a JOIN document_versions v ON v.id = a.document_version_id "
            "WHERE a.page_index >= v.page_count"
        );
        if(!missing_quads || !page_mismatch) {
            return Result<WorkspaceVerification>::failure(
                !missing_quads ? missing_quads.error() : page_mismatch.error()
            );
        }
        if(missing_quads.value() != 0U) {
            verification.issues.emplace_back("annotation_anchor_missing_quads");
        }
        if(page_mismatch.value() != 0U) {
            verification.issues.emplace_back("annotation_page_out_of_range");
        }

        auto objects_result = prepare(
            database_.get(),
            "SELECT content_hash, object_key, byte_length FROM document_versions ORDER BY object_key"
        );
        if(!objects_result) {
            return Result<WorkspaceVerification>::failure(objects_result.error());
        }
        auto objects = std::move(objects_result).value();
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(objects.get())) == SQLITE_ROW) {
            ++verification.referenced_object_count;
            const auto* hash_text = reinterpret_cast<const char*>(sqlite3_column_text(objects.get(), 0));
            const auto* key_text = reinterpret_cast<const char*>(sqlite3_column_text(objects.get(), 1));
            const auto expected_size = sqlite3_column_int64(objects.get(), 2);
            if(hash_text == nullptr || key_text == nullptr || expected_size < 0) {
                verification.issues.emplace_back("document_object_metadata_invalid");
                continue;
            }

            const std::string content_hash(hash_text);
            const std::filesystem::path object_key(key_text);
            bool unsafe_path = object_key.is_absolute();
            for(const auto& component : object_key) {
                if(component == "..") {
                    unsafe_path = true;
                }
            }
            if(unsafe_path) {
                verification.issues.emplace_back("document_object_key_unsafe");
                continue;
            }

            const auto object_path = root_ / object_key;
            std::error_code filesystem_error;
            if(!std::filesystem::is_regular_file(object_path, filesystem_error) || filesystem_error) {
                verification.issues.emplace_back("document_object_missing:" + generic_utf8_path(object_key));
                continue;
            }
            const auto actual_size = std::filesystem::file_size(object_path, filesystem_error);
            if(filesystem_error || actual_size != static_cast<std::uintmax_t>(expected_size)) {
                verification.issues.emplace_back("document_object_size_mismatch:" + generic_utf8_path(object_key));
                continue;
            }
            auto actual_hash = sha256_file(object_path);
            if(!actual_hash) {
                return Result<WorkspaceVerification>::failure(actual_hash.error());
            }
            if(actual_hash.value() != content_hash) {
                verification.issues.emplace_back("document_object_hash_mismatch:" + generic_utf8_path(object_key));
            }
        }
        if(step_result != SQLITE_DONE) {
            return Result<WorkspaceVerification>::failure(
                Error(ErrorCode::storage_failure, "Document objects could not be verified")
            );
        }

        verification.valid = verification.issues.empty();
        return Result<WorkspaceVerification>::success(std::move(verification));
    }

private:
    [[nodiscard]] Result<std::optional<DocumentRecord>> find_by_hash(std::string_view hash) {
        auto statement_result = prepare(
            database_.get(),
            "SELECT d.id, v.id, d.title, v.content_hash, v.object_key, d.active_version_id, "
            "v.byte_length, v.page_count "
            "FROM document_versions v JOIN documents d ON d.id = v.document_id "
            "WHERE v.content_hash = ?1 LIMIT 1"
        );
        if(!statement_result) {
            return Result<std::optional<DocumentRecord>>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        if(!bind_text(statement.get(), 1, hash)) {
            return Result<std::optional<DocumentRecord>>::failure(
                Error(ErrorCode::storage_failure, "Document hash could not be bound")
            );
        }
        const auto step_result = sqlite3_step(statement.get());
        if(step_result == SQLITE_DONE) {
            return Result<std::optional<DocumentRecord>>::success(std::nullopt);
        }
        if(step_result != SQLITE_ROW) {
            return Result<std::optional<DocumentRecord>>::failure(
                Error(ErrorCode::storage_failure, "Document hash lookup failed")
            );
        }
        auto record = read_document_record(statement.get());
        if(!record) {
            return Result<std::optional<DocumentRecord>>::failure(record.error());
        }
        return Result<std::optional<DocumentRecord>>::success(
            std::optional<DocumentRecord>(std::move(record).value())
        );
    }

    std::filesystem::path root_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database_;
    PdfEngine& pdf_engine_;
    WorkspaceInfo info_;
    std::mutex mutex_;
};

SqliteWorkspace::SqliteWorkspace(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

SqliteWorkspace::~SqliteWorkspace() = default;

Result<std::unique_ptr<SqliteWorkspace>> SqliteWorkspace::create(
    const std::filesystem::path& root,
    PdfEngine& pdf_engine
) {
    if(root.empty()) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::invalid_argument, "Workspace path is empty")
        );
    }
    std::error_code filesystem_error;
    const auto absolute_root = std::filesystem::absolute(root, filesystem_error);
    if(filesystem_error) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::storage_failure, "Workspace path could not be resolved")
        );
    }
    const auto database_path = absolute_root / "workspace.db";
    if(std::filesystem::exists(database_path, filesystem_error)) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::already_exists, "Workspace database already exists")
        );
    }

    for(const auto& directory : {
            absolute_root,
            absolute_root / "objects" / "pdf",
            absolute_root / "cache" / "render",
            absolute_root / "cache" / "thumbnail",
            absolute_root / "backups",
        }) {
        std::filesystem::create_directories(directory, filesystem_error);
        if(filesystem_error) {
            return Result<std::unique_ptr<SqliteWorkspace>>::failure(
                Error(ErrorCode::storage_failure, "Workspace directory could not be created")
            );
        }
    }

    auto database_result = open_database(database_path, true);
    if(!database_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(database_result.error());
    }
    auto database = std::move(database_result).value();
    auto configuration = configure_database(database.get());
    if(!configuration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(configuration.error());
    }
    auto migration = apply_initial_migration(database.get());
    if(!migration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(migration.error());
    }
    auto info_result = read_workspace_info(database.get());
    if(!info_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(info_result.error());
    }

    return Result<std::unique_ptr<SqliteWorkspace>>::success(
        std::unique_ptr<SqliteWorkspace>(new SqliteWorkspace(std::make_unique<Impl>(
            absolute_root,
            std::move(database),
            pdf_engine,
            info_result.value()
        )))
    );
}

Result<std::unique_ptr<SqliteWorkspace>> SqliteWorkspace::open(
    const std::filesystem::path& root,
    PdfEngine& pdf_engine
) {
    if(root.empty()) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::invalid_argument, "Workspace path is empty")
        );
    }
    std::error_code filesystem_error;
    const auto absolute_root = std::filesystem::absolute(root, filesystem_error);
    const auto database_path = absolute_root / "workspace.db";
    if(filesystem_error || !std::filesystem::is_regular_file(database_path, filesystem_error)) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::not_found, "Workspace database was not found")
        );
    }

    auto database_result = open_database(database_path, false);
    if(!database_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(database_result.error());
    }
    auto database = std::move(database_result).value();
    auto configuration = configure_database(database.get());
    if(!configuration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(configuration.error());
    }
    auto migration = migrate_database(database.get());
    if(!migration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(migration.error());
    }
    auto info_result = read_workspace_info(database.get());
    if(!info_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(info_result.error());
    }

    return Result<std::unique_ptr<SqliteWorkspace>>::success(
        std::unique_ptr<SqliteWorkspace>(new SqliteWorkspace(std::make_unique<Impl>(
            absolute_root,
            std::move(database),
            pdf_engine,
            info_result.value()
        )))
    );
}

WorkspaceInfo SqliteWorkspace::info() const noexcept {
    return implementation_->info();
}

Result<ImportDocumentResult> SqliteWorkspace::import_pdf(
    const std::filesystem::path& source,
    const CancellationToken& cancellation
) {
    return implementation_->import_pdf(source, cancellation);
}

Result<std::vector<DocumentRecord>> SqliteWorkspace::list_documents() {
    return implementation_->list_documents();
}

Result<ResolvedDocumentObject> SqliteWorkspace::resolve_document(DocumentId document_id) {
    return implementation_->resolve_document(document_id);
}

Result<AnnotationRecord> SqliteWorkspace::create_annotation(const CreateAnnotation& command) {
    return implementation_->create_annotation(command);
}

Result<std::vector<AnnotationRecord>> SqliteWorkspace::list_annotations(
    DocumentVersionId document_version_id
) {
    return implementation_->list_annotations(document_version_id);
}

Result<void> SqliteWorkspace::delete_annotation(AnnotationId annotation_id) {
    return implementation_->delete_annotation(annotation_id);
}

Result<NoteRecord> SqliteWorkspace::update_note(const UpdateNote& command) {
    return implementation_->update_note(command);
}

Result<std::vector<NoteRecord>> SqliteWorkspace::list_notes(
    DocumentVersionId document_version_id
) {
    return implementation_->list_notes(document_version_id);
}

Result<WorkspaceVerification> SqliteWorkspace::verify() {
    return implementation_->verify();
}

}  // namespace context_reader
