#include "metrics_view.h"

#include "../agent/metrics.h"
#include "../frontend.h"

#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace locus {

namespace {

constexpr int k_spark_height = 48;

// Paint a row of vertical bars filling the panel width. `values` holds the
// raw series; the maximum value is used as the y-axis ceiling. Empty series
// renders an empty panel — no axes, no zero-baseline noise.
void draw_sparkbars(wxPanel* panel,
                    const std::vector<long long>& values,
                    const wxColour& fill)
{
    wxAutoBufferedPaintDC dc(panel);
    dc.SetBackground(wxBrush(panel->GetBackgroundColour()));
    dc.Clear();

    wxSize size = panel->GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;
    if (values.empty()) return;

    long long max_v = 0;
    for (auto v : values) max_v = std::max(max_v, v);
    if (max_v <= 0) return;

    int n = static_cast<int>(values.size());
    // Clamp series to last 60 — older points compress until they're sub-pixel
    // and the recent shape washes out.
    int start = std::max(0, n - 60);
    int shown = n - start;
    double bar_w = static_cast<double>(size.x) / static_cast<double>(shown);
    if (bar_w < 1.0) bar_w = 1.0;

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(fill));

    for (int i = 0; i < shown; ++i) {
        double v = static_cast<double>(values[start + i]);
        int h = static_cast<int>((v / static_cast<double>(max_v)) * (size.y - 2));
        if (h < 1 && v > 0) h = 1;
        int x = static_cast<int>(i * bar_w);
        int w = std::max(1, static_cast<int>(bar_w) - 1);
        dc.DrawRectangle(x, size.y - h, w, h);
    }
}

} // namespace

MetricsView::MetricsView(wxWindow* parent, MetricsAggregator& metrics,
                         ILocusCore& core)
    : wxPanel(parent, wxID_ANY)
    , metrics_(metrics)
    , core_(core)
    , timer_(this)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    totals_text_ = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(totals_text_, 0, wxALL | wxEXPAND, 6);

    auto* dur_label = new wxStaticText(this, wxID_ANY, "Turn duration (ms)");
    sizer->Add(dur_label, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    spark_dur_ = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                             wxSize(-1, k_spark_height));
    spark_dur_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    spark_dur_->Bind(wxEVT_PAINT, &MetricsView::on_paint_durations, this);
    sizer->Add(spark_dur_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    auto* tok_label = new wxStaticText(this, wxID_ANY, "Total tokens per turn");
    sizer->Add(tok_label, 0, wxLEFT | wxRIGHT | wxTOP, 6);
    spark_tok_ = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                             wxSize(-1, k_spark_height));
    spark_tok_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    spark_tok_->Bind(wxEVT_PAINT, &MetricsView::on_paint_tokens, this);
    sizer->Add(spark_tok_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    tools_text_ = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(tools_text_, 1, wxALL | wxEXPAND, 6);

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    auto* btn_json = new wxButton(this, wxID_ANY, "Export JSON…");
    auto* btn_csv  = new wxButton(this, wxID_ANY, "Export CSV…");
    btn_json->Bind(wxEVT_BUTTON, &MetricsView::on_export_json, this);
    btn_csv->Bind (wxEVT_BUTTON, &MetricsView::on_export_csv,  this);
    btn_row->Add(btn_json, 0, wxRIGHT, 4);
    btn_row->Add(btn_csv,  0, wxRIGHT, 4);
    sizer->Add(btn_row, 0, wxALL, 6);

    status_ = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(status_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

    SetSizer(sizer);

    Bind(wxEVT_TIMER, &MetricsView::on_timer, this);
    timer_.Start(1000);

    rebuild_text();
}

void MetricsView::refresh_now()
{
    rebuild_text();
    if (spark_dur_) spark_dur_->Refresh();
    if (spark_tok_) spark_tok_->Refresh();
}

void MetricsView::on_timer(wxTimerEvent& /*evt*/)
{
    refresh_now();
}

void MetricsView::rebuild_text()
{
    auto agg = metrics_.aggregates();

    {
        std::ostringstream o;
        o << "Turns: " << agg.turn_count
          << "    Tokens in: " << agg.tokens_in_total
          << "    out: "       << agg.tokens_out_total;
        if (agg.reasoning_total > 0)
            o << "    reasoning: " << agg.reasoning_total;
        o << "\nThroughput: " << std::fixed << std::setprecision(1)
          << agg.tokens_per_second << " tok/s    ("
          << (agg.stream_ms_total / 1000.0) << "s of stream)";
        o << "\nTurn time: avg=" << agg.avg_turn_ms << "ms"
          << "  p95=" << agg.p95_turn_ms << "ms"
          << "  max=" << agg.max_turn_ms << "ms";
        if (agg.retrieval_queries > 0) {
            o << "\nRetrieval: " << agg.retrieval_queries
              << " queries, hit_rate="
              << std::fixed << std::setprecision(1)
              << (agg.retrieval_hit_rate * 100.0) << "%";
        }
        if (totals_text_) totals_text_->SetLabel(wxString::FromUTF8(o.str()));
    }

    {
        std::ostringstream o;
        o << "Tool calls:";
        if (agg.tool_calls_by_name.empty()) {
            o << " (none yet)";
        } else {
            o << "\n";
            for (auto& [name, count] : agg.tool_calls_by_name) {
                o << "  " << name << "  ×" << count << "\n";
            }
        }
        if (tools_text_) tools_text_->SetLabel(wxString::FromUTF8(o.str()));
    }

    Layout();
}

void MetricsView::on_paint_durations(wxPaintEvent& /*evt*/)
{
    auto agg = metrics_.aggregates();
    draw_sparkbars(spark_dur_, agg.turn_durations_ms,
                   wxColour(0x40, 0x80, 0xC0));
}

void MetricsView::on_paint_tokens(wxPaintEvent& /*evt*/)
{
    auto agg = metrics_.aggregates();
    std::vector<long long> v;
    v.reserve(agg.turn_total_tokens.size());
    for (int t : agg.turn_total_tokens) v.push_back(t);
    draw_sparkbars(spark_tok_, v, wxColour(0x60, 0xA0, 0x60));
}

void MetricsView::on_export_json(wxCommandEvent& /*evt*/)
{
    wxFileDialog dlg(this, "Export metrics (JSON)",
                     wxEmptyString, "metrics.json",
                     "JSON files (*.json)|*.json|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::ofstream f(dlg.GetPath().ToStdString());
    if (!f.is_open()) {
        if (status_) status_->SetLabel("Cannot write file");
        return;
    }
    f << metrics_.to_json().dump(2) << '\n';
    if (status_) status_->SetLabel(wxString::Format("Saved %s", dlg.GetPath()));
}

void MetricsView::on_export_csv(wxCommandEvent& /*evt*/)
{
    wxFileDialog dlg(this, "Export metrics (CSV)",
                     wxEmptyString, "metrics.csv",
                     "CSV files (*.csv)|*.csv|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::ofstream f(dlg.GetPath().ToStdString());
    if (!f.is_open()) {
        if (status_) status_->SetLabel("Cannot write file");
        return;
    }
    f << metrics_.to_csv();
    if (status_) status_->SetLabel(wxString::Format("Saved %s", dlg.GetPath()));
}

} // namespace locus
