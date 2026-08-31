#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>

#include "context_reader/pdf/mupdf_engine.hpp"
#include "context_reader/runtime/reader_runtime.hpp"
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

    const auto duplicate_create = SqliteWorkspace::create(workspace_root, pdf_engine);
    check(!duplicate_create.has_value(), "existing workspace cannot be recreated");
    if(!duplicate_create) {
        check(
            duplicate_create.error().code() == ErrorCode::already_exists,
            "existing workspace error is stable"
        );
    }

    const auto facade_root = test_output_root / "facade-workspace";
    auto runtime_result = ReaderRuntime::create();
    check(runtime_result.has_value(), "runtime is created for facade behavior");
    if(runtime_result) {
        auto runtime = std::move(runtime_result).value();
        auto& application = runtime->application();
        const auto facade_create = application.create_workspace(facade_root);
        check(facade_create.has_value(), "facade creates a workspace");
        const auto facade_import = application.import_document(source);
        check(facade_import.has_value(), "facade imports a PDF");
        if(facade_import) {
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
