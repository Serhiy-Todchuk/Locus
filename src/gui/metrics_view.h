#pragma once

#include <wx/wx.h>
#include <wx/timer.h>

namespace locus {

class MetricsAggregator;
class ILocusCore;

// Right-pane "Metrics" tab (S4.S). Displays aggregated agent telemetry —
// token totals, tokens/s, turn-time stats, retrieval hit rate, and a
// per-tool histogram. Two compact spark bars (turn duration + total
// tokens) give a coarse trend view without pulling in a charting library.
//
// The panel does no recording itself — it reads MetricsAggregator on a
// timer (1s) plus on demand whenever the parent forwards an activity event.
class MetricsView : public wxPanel {
public:
    MetricsView(wxWindow* parent, MetricsAggregator& metrics, ILocusCore& core);

    // Force an immediate re-render. Called by ActivityPanel whenever a new
    // activity event arrives — keeps the tab in sync with the log without
    // waiting for the timer.
    void refresh_now();

private:
    void on_paint_durations(wxPaintEvent& evt);
    void on_paint_tokens(wxPaintEvent& evt);
    void on_timer(wxTimerEvent& evt);
    void on_export_json(wxCommandEvent& evt);
    void on_export_csv(wxCommandEvent& evt);

    void rebuild_text();

    MetricsAggregator& metrics_;
    ILocusCore&        core_;

    wxStaticText* totals_text_   = nullptr;
    wxStaticText* tools_text_    = nullptr;
    wxPanel*      spark_dur_     = nullptr;
    wxPanel*      spark_tok_     = nullptr;
    wxStaticText* status_        = nullptr;
    wxTimer       timer_;
};

} // namespace locus
