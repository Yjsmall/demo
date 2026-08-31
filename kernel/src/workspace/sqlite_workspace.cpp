#include "context_reader/workspace/sqlite_workspace.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

namespace {

constexpr std::uint32_t current_schema_version = 1;

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

[[nodiscard]] Result<std::string> sha256_file(const std::filesystem::path& path) {
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
INSERT INTO workspace_metadata(singleton, workspace_id, schema_version, created_at)
VALUES (1, randomblob(16), 1, unixepoch());
PRAGMA user_version = 1;
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

    [[nodiscard]] Result<ImportDocumentResult> import_pdf(const std::filesystem::path& source) {
        const std::scoped_lock lock(mutex_);
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

        auto hash_result = sha256_file(source);
        if(!hash_result) {
            return Result<ImportDocumentResult>::failure(hash_result.error());
        }
        const auto& content_hash = hash_result.value();
        auto existing = find_by_hash(content_hash);
        if(!existing) {
            return Result<ImportDocumentResult>::failure(existing.error());
        }
        if(existing.value().has_value()) {
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
            auto object_hash = sha256_file(object_path);
            if(!object_hash || object_hash.value() != content_hash) {
                return Result<ImportDocumentResult>::failure(
                    Error(ErrorCode::conflict, "Existing PDF object does not match its content key")
                );
            }
        } else {
            std::filesystem::copy_file(source, object_path, std::filesystem::copy_options::none, filesystem_error);
            if(filesystem_error) {
                return Result<ImportDocumentResult>::failure(
                    Error(ErrorCode::storage_failure, "PDF object could not be stored")
                );
            }
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

        auto commit_result = execute(database_.get(), "COMMIT");
        if(!commit_result) {
            return Result<ImportDocumentResult>::failure(commit_result.error());
        }
        transaction.commit();

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

Result<ImportDocumentResult> SqliteWorkspace::import_pdf(const std::filesystem::path& source) {
    return implementation_->import_pdf(source);
}

Result<std::vector<DocumentRecord>> SqliteWorkspace::list_documents() {
    return implementation_->list_documents();
}

Result<ResolvedDocumentObject> SqliteWorkspace::resolve_document(DocumentId document_id) {
    return implementation_->resolve_document(document_id);
}

Result<WorkspaceVerification> SqliteWorkspace::verify() {
    return implementation_->verify();
}

}  // namespace context_reader
