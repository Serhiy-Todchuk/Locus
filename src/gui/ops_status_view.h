#pragma once

#include <wx/string.h>

namespace locus {

// Tracks indexing/embedding progress and composes the status text shown in
// the right-hand status bar pane. Stateless beyond the four counters; cheap
// to construct, no wx dependencies in the header beyond wxString.
class OpsStatusView {
public:
    void set_indexing(int done, int total);
    void set_embedding(int done, int total);

    // Composed status string ("indexing 12/30 files 40.0%  |  embedding ..."),
    // empty when no operation is currently active.
    wxString compose() const;

private:
    // total == 0 means the op is idle; total > 0 with done < total means
    // running; total > 0 with done >= total means just finished — collapsed
    // to idle by the setter so compose() omits it.
    int indexing_done_   = 0;
    int indexing_total_  = 0;
    int embedding_done_  = 0;
    int embedding_total_ = 0;
};

} // namespace locus
