#include "web_settings_panel.h"

#include "../../../core/global_config.h"
#include "../ui_names.h"
#include "../locus_accessible.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>

namespace locus {

namespace {

// Provider choice rows -- (display label, config key). Order matches the
// wxChoice index order.
struct ProviderRow { const wxString label; const char* key; };
const ProviderRow k_provider_rows[] = {
    {"Brave Search (API key required)", "brave"},
    {"SearXNG (self-hosted)",           "searxng"},
};

int provider_index_for(const std::string& key)
{
    for (std::size_t i = 0; i < std::size(k_provider_rows); ++i)
        if (key == k_provider_rows[i].key) return static_cast<int>(i);
    return 0;  // default brave
}

} // namespace

WebSettingsPanel::WebSettingsPanel(wxWindow* parent, const WorkspaceConfig& config)
    : wxPanel(parent)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto hint_font = GetFont();
    hint_font.SetPointSize(hint_font.GetPointSize() - 1);
    auto grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    auto add_hint = [&](const char* text) {
        auto* h = new wxStaticText(this, wxID_ANY, text);
        h->SetFont(hint_font);
        h->SetForegroundColour(grey);
        h->Wrap(540);
        outer->Add(h, 0, wxLEFT | wxRIGHT | wxBOTTOM, 26);
    };
    auto add_label = [&](const char* text) {
        outer->Add(new wxStaticText(this, wxID_ANY, text), 0, wxLEFT | wxRIGHT | wxTOP, 8);
    };

    auto* intro = new wxStaticText(this, wxID_ANY,
        "Web retrieval (RAG): web_search / web_fetch / web_read. Fetched pages "
        "are stripped to text and indexed in an ephemeral cache -- only ranked "
        "snippets reach the model, never whole pages. Untrusted by origin and "
        "injection-scanned on the way in.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    // -- enabled -------------------------------------------------------------
    enabled_ = new wxCheckBox(this, wxID_ANY, "Enable web retrieval (network access)");
    enabled_->SetValue(config.web.enabled);
    enabled_->SetToolTip(
        "Runtime kill-switch the web tools check before touching the network. "
        "The web_retrieval CAPABILITY (Capabilities tab) is the master gate "
        "that adds/removes the tools from the manifest.");
    enabled_->SetName(ui_names::kSettingsWebEnabled);
    gui::apply_locus_accessible_name(enabled_);
    outer->Add(enabled_, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    add_hint("The tools only appear when the web_retrieval capability is on "
             "(Capabilities tab). This toggle then gates live network access.");

    // -- provider ------------------------------------------------------------
    add_label("Search provider:");
    provider_ = new wxChoice(this, wxID_ANY);
    for (const auto& row : k_provider_rows) provider_->Append(row.label);
    provider_->SetSelection(provider_index_for(config.web.search_provider));
    provider_->SetToolTip(
        "Brave needs an API key (brave.com/search/api). SearXNG is a "
        "self-hosted metasearch engine with no key and no rate limits.");
    provider_->SetName(ui_names::kSettingsWebProvider);
    gui::apply_locus_accessible_name(provider_);
    outer->Add(provider_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    outer->AddSpacer(4);

    // -- api_key -------------------------------------------------------------
    add_label("API key:");
    api_key_ = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(config.web.api_key),
        wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    api_key_->SetToolTip(
        "Stored in .locus/config.json (workspace-local, gitignored). Required "
        "for Brave; optional bearer token for a secured SearXNG instance.");
    api_key_->SetName(ui_names::kSettingsWebApiKey);
    gui::apply_locus_accessible_name(api_key_);
    outer->Add(api_key_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    add_hint("Workspace-local. Never logged. Add .locus/ to .gitignore so the "
             "key is not committed.");

    // -- api_url -------------------------------------------------------------
    add_label("Search API URL:");
    api_url_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(config.web.api_url));
    api_url_->SetToolTip(
        "Brave default is prefilled. For SearXNG, point this at your "
        "instance's /search endpoint.");
    api_url_->SetName(ui_names::kSettingsWebApiUrl);
    gui::apply_locus_accessible_name(api_url_);
    outer->Add(api_url_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    outer->AddSpacer(4);

    // -- numeric row: max_results + cache knobs ------------------------------
    add_label("Max search results:");
    max_results_ = new wxTextCtrl(this, wxID_ANY,
        wxString::Format("%d", config.web.max_results));
    max_results_->SetName(ui_names::kSettingsWebMaxResults);
    gui::apply_locus_accessible_name(max_results_);
    outer->Add(max_results_, 0, wxLEFT | wxRIGHT, 8);
    outer->AddSpacer(4);

    add_label("Cache TTL (hours, 0 disables eviction):");
    cache_ttl_hours_ = new wxTextCtrl(this, wxID_ANY,
        wxString::Format("%d", config.web.cache_ttl_hours));
    cache_ttl_hours_->SetToolTip(
        "Cached pages older than this are evicted on workspace open.");
    cache_ttl_hours_->SetName(ui_names::kSettingsWebCacheTtlHours);
    gui::apply_locus_accessible_name(cache_ttl_hours_);
    outer->Add(cache_ttl_hours_, 0, wxLEFT | wxRIGHT, 8);
    outer->AddSpacer(4);

    add_label("Cache size cap (MB, 0 disables):");
    cache_max_mb_ = new wxTextCtrl(this, wxID_ANY,
        wxString::Format("%d", config.web.cache_max_mb));
    cache_max_mb_->SetToolTip(
        "When the cached content exceeds this, the oldest pages are evicted.");
    cache_max_mb_->SetName(ui_names::kSettingsWebCacheMaxMb);
    gui::apply_locus_accessible_name(cache_max_mb_);
    outer->Add(cache_max_mb_, 0, wxLEFT | wxRIGHT, 8);
    outer->AddSpacer(4);

    add_label("Max extracted text per page (KB):");
    max_page_kb_ = new wxTextCtrl(this, wxID_ANY,
        wxString::Format("%d", config.web.max_web_page_kb));
    max_page_kb_->SetToolTip(
        "A fetched page's stripped text is truncated to this before caching.");
    max_page_kb_->SetName(ui_names::kSettingsWebMaxPageKb);
    gui::apply_locus_accessible_name(max_page_kb_);
    outer->Add(max_page_kb_, 0, wxLEFT | wxRIGHT, 8);
    outer->AddSpacer(4);

    // -- allow_http ----------------------------------------------------------
    allow_http_ = new wxCheckBox(this, wxID_ANY,
        "Allow http:// URLs (HTTPS only by default)");
    allow_http_->SetValue(config.web.allow_http);
    allow_http_->SetToolTip(
        "Off by default -- web_fetch and the search API refuse plain http. "
        "Turn on only for an http-only intranet endpoint.");
    allow_http_->SetName(ui_names::kSettingsWebAllowHttp);
    gui::apply_locus_accessible_name(allow_http_);
    outer->Add(allow_http_, 0, wxLEFT | wxRIGHT | wxTOP, 8);

    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddStretchSpacer(1);
        auto* btn = new wxButton(this, wxID_ANY, "Reset to global defaults");
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            load_from_config(load_global_config_or_defaults());
        });
        row->Add(btn, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 8);
    }

    outer->AddStretchSpacer(1);
    SetSizer(outer);
}

void WebSettingsPanel::load_from_config(const WorkspaceConfig& cfg)
{
    if (enabled_)         enabled_->SetValue(cfg.web.enabled);
    if (provider_)        provider_->SetSelection(provider_index_for(cfg.web.search_provider));
    if (api_key_)         api_key_->SetValue(wxString::FromUTF8(cfg.web.api_key));
    if (api_url_)         api_url_->SetValue(wxString::FromUTF8(cfg.web.api_url));
    if (max_results_)     max_results_->SetValue(wxString::Format("%d", cfg.web.max_results));
    if (cache_ttl_hours_) cache_ttl_hours_->SetValue(wxString::Format("%d", cfg.web.cache_ttl_hours));
    if (cache_max_mb_)    cache_max_mb_->SetValue(wxString::Format("%d", cfg.web.cache_max_mb));
    if (max_page_kb_)     max_page_kb_->SetValue(wxString::Format("%d", cfg.web.max_web_page_kb));
    if (allow_http_)      allow_http_->SetValue(cfg.web.allow_http);
}

bool WebSettingsPanel::validate(wxString& out_error) const
{
    auto check_positive = [&](wxTextCtrl* c, const char* name, long min_val) {
        long v = 0;
        if (c && (!c->GetValue().ToLong(&v) || v < min_val)) {
            out_error = wxString::Format("Web: %s must be an integer >= %ld.",
                                         name, min_val);
            return false;
        }
        return true;
    };
    if (!check_positive(max_results_,     "max search results", 1)) return false;
    if (!check_positive(cache_ttl_hours_, "cache TTL hours",    0)) return false;
    if (!check_positive(cache_max_mb_,    "cache size cap MB",  0)) return false;
    if (!check_positive(max_page_kb_,     "max page KB",        1)) return false;
    return true;
}

void WebSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    if (enabled_)    cfg.web.enabled    = enabled_->IsChecked();
    if (allow_http_) cfg.web.allow_http = allow_http_->IsChecked();
    if (provider_) {
        int idx = provider_->GetSelection();
        if (idx >= 0 && static_cast<std::size_t>(idx) < std::size(k_provider_rows))
            cfg.web.search_provider = k_provider_rows[idx].key;
    }
    if (api_key_) cfg.web.api_key = api_key_->GetValue().utf8_string();
    if (api_url_) cfg.web.api_url = api_url_->GetValue().utf8_string();

    auto read_int = [](wxTextCtrl* c, int& dst) {
        long v = 0;
        if (c && c->GetValue().ToLong(&v)) dst = static_cast<int>(v);
    };
    read_int(max_results_,     cfg.web.max_results);
    read_int(cache_ttl_hours_, cfg.web.cache_ttl_hours);
    read_int(cache_max_mb_,    cfg.web.cache_max_mb);
    read_int(max_page_kb_,     cfg.web.max_web_page_kb);
}

} // namespace locus
