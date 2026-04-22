#include "ops_status_view.h"

namespace locus {

void OpsStatusView::set_indexing(int done, int total)
{
    indexing_done_  = done;
    indexing_total_ = (total > 0 && done >= total) ? 0 : total;
}

void OpsStatusView::set_embedding(int done, int total)
{
    embedding_done_  = done;
    embedding_total_ = (total > 0 && done >= total) ? 0 : total;
}

wxString OpsStatusView::compose() const
{
    wxString out;
    auto append = [&out](const wxString& s) {
        if (!out.empty()) out += "  |  ";
        out += s;
    };

    if (indexing_total_ > 0) {
        double pct = 100.0 * indexing_done_ / indexing_total_;
        append(wxString::Format("indexing %d/%d files %.1f%%",
                                indexing_done_, indexing_total_, pct));
    }
    if (embedding_total_ > 0) {
        double pct = 100.0 * embedding_done_ / embedding_total_;
        append(wxString::Format("embedding %d/%d chunks %.1f%%",
                                embedding_done_, embedding_total_, pct));
    }

    return out;
}

} // namespace locus
