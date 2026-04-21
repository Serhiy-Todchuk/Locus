#include "pdf_extractor.h"

#include <spdlog/spdlog.h>

#include <fpdfview.h>
#include <fpdf_text.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace locus {

namespace {

// PDFium has global init state and is not thread-safe for init/destroy.
// Reference-counted init so multiple PdfiumExtractor instances share one lib.
std::mutex g_pdfium_mutex;
int        g_pdfium_refcount = 0;

void pdfium_addref()
{
    std::lock_guard<std::mutex> lk(g_pdfium_mutex);
    if (g_pdfium_refcount++ == 0) {
        FPDF_LIBRARY_CONFIG cfg{};
        cfg.version = 2;
        cfg.m_pUserFontPaths  = nullptr;
        cfg.m_pIsolate        = nullptr;
        cfg.m_v8EmbedderSlot  = 0;
        FPDF_InitLibraryWithConfig(&cfg);
    }
}

void pdfium_release()
{
    std::lock_guard<std::mutex> lk(g_pdfium_mutex);
    if (--g_pdfium_refcount == 0) {
        FPDF_DestroyLibrary();
    }
}

// UTF-16LE (from PDFium) → UTF-8.
std::string utf16le_to_utf8(const std::vector<unsigned short>& u16, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        unsigned int cp = u16[i];
        // Surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            unsigned int low = u16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace

PdfiumExtractor::PdfiumExtractor()
{
    pdfium_addref();
}

PdfiumExtractor::~PdfiumExtractor()
{
    pdfium_release();
}

ExtractionResult PdfiumExtractor::extract(const std::filesystem::path& abs_path)
{
    ExtractionResult result;

    std::string path_utf8 = abs_path.string();
    FPDF_DOCUMENT doc = FPDF_LoadDocument(path_utf8.c_str(), nullptr);

    if (!doc) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD) {
            spdlog::warn("PdfiumExtractor: encrypted PDF (skipped): {}", path_utf8);
        } else {
            spdlog::warn("PdfiumExtractor: failed to open {} (err={})", path_utf8, err);
        }
        result.is_binary = true;
        return result;
    }

    int page_count = FPDF_GetPageCount(doc);
    std::string& text = result.text;

    for (int page_idx = 0; page_idx < page_count; ++page_idx) {
        FPDF_PAGE page = FPDF_LoadPage(doc, page_idx);
        if (!page) continue;

        FPDF_TEXTPAGE textpage = FPDFText_LoadPage(page);
        if (!textpage) {
            FPDF_ClosePage(page);
            continue;
        }

        // Emit page marker as a pseudo-heading for outline / chunking.
        // Line number = current line count in accumulated text.
        int line_number = 1 + static_cast<int>(
            std::count(text.begin(), text.end(), '\n'));
        result.headings.push_back({
            1,
            "Page " + std::to_string(page_idx + 1),
            line_number
        });

        int char_count = FPDFText_CountChars(textpage);
        if (char_count > 0) {
            // +1 for trailing null PDFium writes
            std::vector<unsigned short> buffer(static_cast<size_t>(char_count) + 1, 0);
            int written = FPDFText_GetText(textpage, 0, char_count, buffer.data());
            if (written > 0) {
                // written includes the null terminator
                size_t len = static_cast<size_t>(written > 0 ? written - 1 : 0);
                text += utf16le_to_utf8(buffer, len);
            }
        }
        text += "\n\n";

        FPDFText_ClosePage(textpage);
        FPDF_ClosePage(page);
    }

    FPDF_CloseDocument(doc);

    if (text.empty() && result.headings.empty()) {
        spdlog::warn("PdfiumExtractor: no extractable text in {}", path_utf8);
        result.is_binary = true;
    }

    return result;
}

} // namespace locus
