#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

#include <sqlite3.h>
#include <windows.h>

#include "context_reader/pdf/mupdf_engine.hpp"
#include "context_reader/runtime/reader_runtime.hpp"
#include "context_reader/runtime/cancellation.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/workspace/sqlite_workspace.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if(!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool create_schema_v1_workspace(const std::filesystem::path& root) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if(filesystem_error) return false;
    sqlite3* database = nullptr;
    const auto path = root / "workspace.db";
    const auto path_utf8 = path.u8string();
    const std::string path_string(
        reinterpret_cast<const char*>(path_utf8.data()),
        path_utf8.size()
    );
    if(sqlite3_open(path_string.c_str(), &database) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        return false;
    }
    constexpr const char* schema = R"sql(
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
INSERT INTO workspace_metadata(singleton, workspace_id, schema_version, created_at)
VALUES (1, randomblob(16), 1, unixepoch());
PRAGMA user_version = 1;
)sql";
    const auto result = sqlite3_exec(database, schema, nullptr, nullptr, nullptr);
    sqlite3_close(database);
    return result == SQLITE_OK;
}

int database_user_version(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    const auto path_utf8 = path.u8string();
    const std::string path_string(
        reinterpret_cast<const char*>(path_utf8.data()),
        path_utf8.size()
    );
    if(sqlite3_open_v2(path_string.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        return -1;
    }
    sqlite3_stmt* statement = nullptr;
    int version = -1;
    if(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK
       && sqlite3_step(statement) == SQLITE_ROW) {
        version = sqlite3_column_int(statement, 0);
    }
    if(statement != nullptr) sqlite3_finalize(statement);
    sqlite3_close(database);
    return version;
}

}  // namespace

int main() {
    using namespace context_reader;

    const std::filesystem::path corpus_root(CONTEXT_READER_TEST_CORPUS);
    const std::filesystem::path test_output_root(CONTEXT_READER_TEST_OUTPUT);
    const auto workspace_root = test_output_root / std::filesystem::path(L"\u5de5\u4f5c\u533a");
    std::error_code filesystem_error;
    std::filesystem::remove_all(test_output_root, filesystem_error);
    check(!filesystem_error, "workspace test output is reset");

    MuPdfEngine pdf_engine;
    const auto missing = SqliteWorkspace::open(test_output_root / "missing", pdf_engine);
    check(!missing.has_value(), "missing workspace is rejected");
    if(!missing) {
        check(missing.error().code() == ErrorCode::not_found, "missing workspace error is stable");
    }

    auto create_result = SqliteWorkspace::create(workspace_root, pdf_engine);
    check(create_result.has_value(), "workspace is created");
    if(!create_result) {
        return 1;
    }
    auto workspace = std::move(create_result).value();
    const auto initial_info = workspace->info();
    check(!initial_info.id.is_nil(), "workspace ID is non-nil");
    check(initial_info.schema_version == 2U, "workspace schema version is two");
    check(
        std::filesystem::is_regular_file(workspace_root / "workspace.db"),
        "workspace database exists"
    );
    check(
        std::filesystem::is_regular_file(workspace_root / "workspace.lock"),
        "workspace lock file exists"
    );
    const auto busy_open = SqliteWorkspace::open(workspace_root, pdf_engine);
    check(!busy_open.has_value(), "second writer cannot open the workspace");
    if(!busy_open) {
        check(
            busy_open.error().code() == ErrorCode::workspace_busy,
            "second writer receives the stable workspace busy error"
        );
    }

    const auto corrupt = workspace->import_pdf(corpus_root / "generated" / "corrupt-truncated.pdf");
    check(!corrupt.has_value(), "corrupt PDF import is rejected");
    if(!corrupt) {
        check(corrupt.error().code() == ErrorCode::pdf_failure, "corrupt import error is stable");
    }

    const auto source = corpus_root / "generated" / "basic-rotated-cropbox.pdf";
    auto import_result = workspace->import_pdf(source);
    check(import_result.has_value(), "PDF is imported into workspace");
    if(!import_result) {
        return 1;
    }
    const auto imported = import_result.value();
    check(!imported.reused_existing, "first import creates a document version");
    check(!imported.document.document_id.is_nil(), "document ID is non-nil");
    check(!imported.document.version_id.is_nil(), "document version ID is non-nil");
    check(imported.document.title == "basic-rotated-cropbox", "document title uses source stem");
    check(imported.document.page_count == 1U, "document page count is persisted");
    check(
        imported.document.content_sha256 ==
            "f405ce309444499b939005d0cd1189891e19bc61bfa4440155ce5b4e8a065b8d",
        "document content hash is persisted"
    );
    const auto object_path = workspace_root / std::filesystem::path(imported.document.object_key);
    check(std::filesystem::is_regular_file(object_path), "content-addressed PDF object exists");

    const auto duplicate = workspace->import_pdf(source);
    check(duplicate.has_value(), "duplicate PDF import succeeds");
    if(duplicate) {
        check(duplicate.value().reused_existing, "duplicate import reuses content");
        check(
            duplicate.value().document.document_id == imported.document.document_id,
            "duplicate import reuses document ID"
        );
        check(
            duplicate.value().document.version_id == imported.document.version_id,
            "duplicate import reuses version ID"
        );
    }

    const auto documents = workspace->list_documents();
    check(documents.has_value(), "workspace documents can be listed");
    if(documents) {
        check(documents.value().size() == 1U, "duplicate import does not duplicate records");
    }
    const auto initial_verification = workspace->verify();
    check(initial_verification.has_value(), "workspace verification executes");
    if(initial_verification) {
        check(initial_verification.value().valid, "new workspace is valid");
        check(initial_verification.value().document_count == 1U, "verification counts documents");
        check(
            initial_verification.value().document_version_count == 1U,
            "verification counts document versions"
        );
        check(
            initial_verification.value().referenced_object_count == 1U,
            "verification counts referenced objects"
        );
        check(
            initial_verification.value().orphaned_object_count == 0U,
            "verification reports no orphaned objects"
        );
    }

    const CreateAnnotation annotation_command{
        .document_version_id = imported.document.version_id,
        .page_index = 0U,
        .quads = {{.x = 72.0, .y = 96.0, .width = 180.0, .height = 18.0}},
        .quote = {
            .exact = "Context Reader P1",
            .prefix = "",
            .suffix = " fixture",
        },
        .layout_version = "mupdf-1.28.3",
        .color = HighlightColor::yellow,
    };
    const auto annotation = workspace->create_annotation(annotation_command);
    check(annotation.has_value(), "annotation is created from a page-coordinate anchor");
    if(annotation) {
        check(!annotation.value().id.is_nil(), "annotation ID is stable and non-nil");
        check(annotation.value().quads == annotation_command.quads, "annotation quads round-trip");

        const auto note = workspace->update_note(UpdateNote{
            .annotation_id = annotation.value().id,
            .expected_revision = 0U,
            .markdown_source = "First **context** note",
        });
        check(note.has_value() && note.value().revision == 1U, "note is created at revision one");
        const auto conflict = workspace->update_note(UpdateNote{
            .annotation_id = annotation.value().id,
            .expected_revision = 0U,
            .markdown_source = "stale write",
        });
        check(!conflict.has_value(), "stale note revision is rejected");
        if(!conflict) {
            check(conflict.error().code() == ErrorCode::conflict, "note conflict error is stable");
        }
        const auto updated = workspace->update_note(UpdateNote{
            .annotation_id = annotation.value().id,
            .expected_revision = 1U,
            .markdown_source = "Updated **context** note",
        });
        check(updated.has_value() && updated.value().revision == 2U, "matching note revision updates");
    }

    const auto invalid_annotation = workspace->create_annotation(CreateAnnotation{
        .document_version_id = imported.document.version_id,
        .page_index = 1U,
        .quads = {{.x = 0.0, .y = 0.0, .width = 1.0, .height = 1.0}},
        .quote = {.exact = "missing", .prefix = "", .suffix = ""},
        .layout_version = "mupdf-1.28.3",
        .color = HighlightColor::pink,
    });
    check(!invalid_annotation.has_value(), "annotation outside the document is rejected");

    workspace.reset();
    auto reopen_result = SqliteWorkspace::open(workspace_root, pdf_engine);
    check(reopen_result.has_value(), "workspace reopens after close");
    if(!reopen_result) {
        return 1;
    }
    workspace = std::move(reopen_result).value();
    check(workspace->info().id == initial_info.id, "workspace ID survives restart");
    const auto reopened_documents = workspace->list_documents();
    check(reopened_documents.has_value(), "documents survive restart");
    if(reopened_documents) {
        check(reopened_documents.value().size() == 1U, "one document survives restart");
        if(!reopened_documents.value().empty()) {
            check(
                reopened_documents.value().front().version_id == imported.document.version_id,
                "active document version survives restart"
            );
        }
    }
    const auto reopened_annotations = workspace->list_annotations(imported.document.version_id);
    check(
        reopened_annotations.has_value() && reopened_annotations.value().size() == 1U,
        "annotation survives restart"
    );
    const auto reopened_notes = workspace->list_notes(imported.document.version_id);
    check(
        reopened_notes.has_value() && reopened_notes.value().size() == 1U
            && reopened_notes.value().front().revision == 2U
            && reopened_notes.value().front().markdown_source == "Updated **context** note",
        "latest note revision survives restart"
    );
    if(annotation) {
        SetEnvironmentVariableA("CONTEXT_READER_TEST_AUTOSAVE_FAULT", "before-commit");
        const auto before_autosave = workspace->update_note(UpdateNote{
            .annotation_id = annotation.value().id,
            .expected_revision = 2U,
            .markdown_source = "must roll back",
        });
        SetEnvironmentVariableA("CONTEXT_READER_TEST_AUTOSAVE_FAULT", nullptr);
        check(
            !before_autosave.has_value()
                && before_autosave.error().code() == ErrorCode::storage_failure,
            "autosave failure before commit is reported"
        );
        const auto notes_after_rollback = workspace->list_notes(imported.document.version_id);
        check(
            notes_after_rollback.has_value() && !notes_after_rollback.value().empty()
                && notes_after_rollback.value().front().revision == 2U
                && notes_after_rollback.value().front().markdown_source == "Updated **context** note",
            "autosave failure before commit rolls back authoritatively"
        );

        SetEnvironmentVariableA("CONTEXT_READER_TEST_AUTOSAVE_FAULT", "after-commit");
        const auto after_autosave = workspace->update_note(UpdateNote{
            .annotation_id = annotation.value().id,
            .expected_revision = 2U,
            .markdown_source = "Committed despite reported failure",
        });
        SetEnvironmentVariableA("CONTEXT_READER_TEST_AUTOSAVE_FAULT", nullptr);
        check(
            !after_autosave.has_value() && after_autosave.error().code() == ErrorCode::storage_failure,
            "autosave failure after commit is reported"
        );
        workspace.reset();
        auto autosave_reopen = SqliteWorkspace::open(workspace_root, pdf_engine);
        check(autosave_reopen.has_value(), "workspace reopens after autosave commit ambiguity");
        if(autosave_reopen) workspace = std::move(autosave_reopen).value();
        const auto notes_after_commit = workspace->list_notes(imported.document.version_id);
        check(
            notes_after_commit.has_value() && !notes_after_commit.value().empty()
                && notes_after_commit.value().front().revision == 3U
                && notes_after_commit.value().front().markdown_source
                    == "Committed despite reported failure",
            "autosave failure after commit is resolved from authoritative revision"
        );
    }

    const auto displaced_object = object_path.string() + ".missing";
    std::filesystem::rename(object_path, displaced_object, filesystem_error);
    check(!filesystem_error, "referenced object can be displaced for verification test");
    const auto broken_verification = workspace->verify();
    check(broken_verification.has_value(), "broken workspace produces verification report");
    if(broken_verification) {
        check(!broken_verification.value().valid, "missing object invalidates workspace");
        check(!broken_verification.value().issues.empty(), "missing object has an issue code");
    }
    std::filesystem::rename(displaced_object, object_path, filesystem_error);
    check(!filesystem_error, "referenced object is restored");
    const auto restored_verification = workspace->verify();
    check(
        restored_verification.has_value() && restored_verification.value().valid,
        "restored workspace verifies after restart"
    );

    const auto orphan_path = workspace_root / "objects" / "pdf" / "ff" / "orphan.pdf";
    std::filesystem::create_directories(orphan_path.parent_path(), filesystem_error);
    {
        std::ofstream orphan(orphan_path, std::ios::binary);
        orphan << "orphan";
    }
    const auto orphan_verification = workspace->verify();
    check(
        orphan_verification.has_value() && !orphan_verification.value().valid
            && orphan_verification.value().orphaned_object_count == 1U,
        "verification reports an unreferenced object"
    );
    const auto cleanup = workspace->cleanup_orphaned_objects();
    check(
        cleanup.has_value() && cleanup.value().removed_object_count == 1U
            && cleanup.value().reclaimed_bytes == 6U,
        "orphan cleanup reports removed bytes"
    );
    check(!std::filesystem::exists(orphan_path), "orphan cleanup removes the unreferenced object");
    const auto cleaned_verification = workspace->verify();
    check(
        cleaned_verification.has_value() && cleaned_verification.value().valid,
        "workspace verifies after orphan cleanup"
    );

    const auto duplicate_create = SqliteWorkspace::create(workspace_root, pdf_engine);
    check(!duplicate_create.has_value(), "existing workspace cannot be recreated");
    if(!duplicate_create) {
        check(
            duplicate_create.error().code() == ErrorCode::already_exists,
            "existing workspace error is stable"
        );
    }

    const auto legacy_root = test_output_root / "legacy-v1";
    check(create_schema_v1_workspace(legacy_root), "schema v1 migration fixture is created");
    auto legacy_open = SqliteWorkspace::open(legacy_root, pdf_engine);
    check(
        legacy_open.has_value() && legacy_open.value()->info().schema_version == 2U,
        "schema v1 workspace migrates to schema v2"
    );
    if(legacy_open) {
        auto migrated_workspace = std::move(legacy_open).value();
        migrated_workspace.reset();
    }
    std::vector<std::filesystem::path> migration_backups;
    const auto backup_root = legacy_root / "backups";
    if(std::filesystem::exists(backup_root)) {
        for(const auto& entry : std::filesystem::directory_iterator(backup_root)) {
            if(entry.is_regular_file()) migration_backups.push_back(entry.path());
        }
    }
    check(migration_backups.size() == 1U, "migration creates exactly one pre-migration backup");
    if(migration_backups.size() == 1U) {
        check(
            database_user_version(migration_backups.front()) == 1,
            "migration backup preserves the source schema"
        );
    }

    const auto retry_migration_root = test_output_root / "legacy-v1-retry";
    check(create_schema_v1_workspace(retry_migration_root), "retry migration fixture is created");
    SetEnvironmentVariableA("CONTEXT_READER_TEST_MIGRATION_FAULT", "after-backup");
    const auto failed_before_migration = SqliteWorkspace::open(retry_migration_root, pdf_engine);
    SetEnvironmentVariableA("CONTEXT_READER_TEST_MIGRATION_FAULT", nullptr);
    check(
        !failed_before_migration.has_value()
            && failed_before_migration.error().code() == ErrorCode::storage_failure
            && database_user_version(retry_migration_root / "workspace.db") == 1,
        "migration failure after backup leaves schema v1 authoritative"
    );
    auto retried_migration = SqliteWorkspace::open(retry_migration_root, pdf_engine);
    check(
        retried_migration.has_value() && retried_migration.value()->info().schema_version == 2U,
        "migration succeeds when retried after backup failure"
    );
    if(retried_migration) retried_migration.value().reset();

    const auto committed_migration_root = test_output_root / "legacy-v1-committed";
    check(create_schema_v1_workspace(committed_migration_root), "committed migration fixture is created");
    SetEnvironmentVariableA("CONTEXT_READER_TEST_MIGRATION_FAULT", "after-migration");
    const auto failed_after_migration = SqliteWorkspace::open(committed_migration_root, pdf_engine);
    SetEnvironmentVariableA("CONTEXT_READER_TEST_MIGRATION_FAULT", nullptr);
    check(
        !failed_after_migration.has_value()
            && failed_after_migration.error().code() == ErrorCode::storage_failure
            && database_user_version(committed_migration_root / "workspace.db") == 2,
        "migration failure after commit leaves schema v2 authoritative"
    );
    auto committed_migration_reopen = SqliteWorkspace::open(committed_migration_root, pdf_engine);
    check(
        committed_migration_reopen.has_value()
            && committed_migration_reopen.value()->info().schema_version == 2U,
        "workspace reopens after committed migration failure"
    );
    if(committed_migration_reopen) committed_migration_reopen.value().reset();

    const auto facade_root = test_output_root / "facade-workspace";
    auto runtime_result = ReaderRuntime::create();
    check(runtime_result.has_value(), "runtime is created for facade behavior");
    if(runtime_result) {
        auto runtime = std::move(runtime_result).value();
        auto& application = runtime->application();
        const auto facade_create = application.create_workspace(facade_root);
        check(facade_create.has_value(), "facade creates a workspace");
        CancellationSource cancelled_import;
        cancelled_import.request_cancellation();
        const auto cancelled_import_result = application.import_document(
            source,
            cancelled_import.token()
        );
        check(
            !cancelled_import_result.has_value()
                && cancelled_import_result.error().code() == ErrorCode::cancelled,
            "facade import honors a cancellation token"
        );
        const auto facade_import = application.import_document(source);
        check(facade_import.has_value(), "facade imports a PDF");
        if(facade_import) {
            CancellationSource cancelled_open;
            cancelled_open.request_cancellation();
            const auto cancelled_open_result = application.open_document(
                facade_import.value().document.document_id,
                cancelled_open.token()
            );
            check(
                !cancelled_open_result.has_value()
                    && cancelled_open_result.error().code() == ErrorCode::cancelled,
                "facade document open honors a cancellation token"
            );
            const auto opened_document = application.open_document(
                facade_import.value().document.document_id
            );
            check(opened_document.has_value(), "facade opens an imported document by stable ID");
            const auto page = application.page_info(0U);
            check(page.has_value(), "facade returns open document page metadata");
            if(page) {
                check(page.value().size.width == 540.0, "facade preserves CropBox width");
                check(
                    page.value().rotation == PageRotation::degrees_90,
                    "facade preserves page rotation"
                );
            }
            const auto rendered = application.render_page(0U, 1.0);
            check(rendered.has_value(), "facade renders an open document page");
            if(rendered) {
                constexpr std::array<std::uint8_t, 8> png_signature{
                    137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U,
                };
                check(
                    rendered.value().png.size() >= png_signature.size()
                        && std::equal(
                            png_signature.begin(),
                            png_signature.end(),
                            rendered.value().png.begin()
                        ),
                    "facade render returns PNG bytes"
                );
            }
            CancellationSource cancelled_render;
            cancelled_render.request_cancellation();
            const auto cancelled_render_result = application.render_page(
                0U,
                1.0,
                cancelled_render.token()
            );
            check(
                !cancelled_render_result.has_value()
                    && cancelled_render_result.error().code() == ErrorCode::cancelled,
                "facade render honors a cancellation token"
            );
            const auto text = application.extract_page_text(0U);
            check(
                text.has_value()
                    && text.value().text.find("Context Reader P1") != std::string::npos,
                "facade extracts page text"
            );
            const auto missing_page = application.page_info(1U);
            check(!missing_page.has_value(), "facade rejects an out-of-range page");
            const auto facade_annotation = application.create_annotation(CreateAnnotation{
                .document_version_id = facade_import.value().document.version_id,
                .page_index = 0U,
                .quads = {{.x = 72.0, .y = 96.0, .width = 180.0, .height = 18.0}},
                .quote = {.exact = "Context Reader P1", .prefix = "", .suffix = " fixture"},
                .layout_version = "mupdf-1.28.3",
                .color = HighlightColor::green,
            });
            check(facade_annotation.has_value(), "facade creates an annotation");
            if(facade_annotation) {
                const auto facade_note = application.update_note(UpdateNote{
                    .annotation_id = facade_annotation.value().id,
                    .expected_revision = 0U,
                    .markdown_source = "Facade note",
                });
                check(facade_note.has_value(), "facade creates a linked note");
            }
        }
        const auto facade_documents = application.list_documents();
        check(
            facade_documents.has_value() && facade_documents.value().size() == 1U,
            "facade lists imported documents"
        );
        const auto facade_verification = application.verify_workspace();
        check(
            facade_verification.has_value() && facade_verification.value().valid,
            "facade verifies the workspace"
        );
        check(application.close_workspace().has_value(), "facade closes the workspace");
        const auto closed_documents = application.list_documents();
        check(!closed_documents.has_value(), "closed facade rejects document queries");
        const auto facade_open = application.open_workspace(facade_root);
        check(facade_open.has_value(), "facade reopens the workspace");
        if(facade_create && facade_open) {
            check(facade_open.value().id == facade_create.value().id, "facade workspace ID persists");
        }
        const auto reopened_facade_documents = application.list_documents();
        check(
            reopened_facade_documents.has_value() && reopened_facade_documents.value().size() == 1U,
            "facade documents survive restart"
        );
        if(reopened_facade_documents && !reopened_facade_documents.value().empty()) {
            const auto annotations = application.list_annotations(
                reopened_facade_documents.value().front().version_id
            );
            const auto notes = application.list_notes(
                reopened_facade_documents.value().front().version_id
            );
            check(annotations.has_value() && annotations.value().size() == 1U, "facade annotation persists");
            check(notes.has_value() && notes.value().size() == 1U, "facade note persists");
            if(annotations && !annotations.value().empty()) {
                check(
                    application.delete_annotation(annotations.value().front().id).has_value(),
                    "facade deletes an annotation"
                );
                const auto deleted_notes = application.list_notes(
                    reopened_facade_documents.value().front().version_id
                );
                check(deleted_notes.has_value() && deleted_notes.value().empty(), "annotation deletion removes its note");
            }
            check(
                application.open_document(
                    reopened_facade_documents.value().front().document_id
                ).has_value(),
                "facade reopens a document after workspace restart"
            );
            check(application.close_document().has_value(), "facade closes the document session");
            check(!application.page_info(0U).has_value(), "closed document rejects page operations");
        }
    }

    if(failures == 0) {
        std::cout << "sqlite_workspace_test passed\n";
    }
    return failures == 0 ? 0 : 1;
}
