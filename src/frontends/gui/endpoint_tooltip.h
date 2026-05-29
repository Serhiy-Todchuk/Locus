#pragma once

// S6.16 -- shared helpers for rendering an endpoint profile's key/url in the
// chat-footer chip tooltip and the Settings > Endpoints list. Header-only so
// every consumer (ChatPanel wiring, AgentEventRouter, endpoints panel) gets
// the same masking without a new translation unit. The key is shown masked
// only (never in full) -- the "Show key" toggle in the edit modal is the only
// place the raw key is revealed.

#include "../../llm/endpoint_profile.h"

#include <wx/string.h>

#include <string>

namespace locus::gui {

// "" -> "(none)"; otherwise "****" + last 4 chars (or the whole short key
// behind asterisks). Never returns the full key for keys longer than 4 chars.
inline wxString mask_api_key(const std::string& key)
{
    if (key.empty()) return "(none)";
    if (key.size() <= 4) return wxString(std::string(key.size(), '*'));
    return wxString::FromUTF8("****" + key.substr(key.size() - 4));
}

inline wxString endpoint_chip_tooltip(const EndpointProfile& p)
{
    wxString tip = wxString::FromUTF8(p.base_url);
    tip += "\nKey: " + mask_api_key(p.api_key);
    if (!p.default_model.empty())
        tip += "\nModel: " + wxString::FromUTF8(p.default_model);
    tip += "\nSwitch applies on your next message.";
    return tip;
}

} // namespace locus::gui
