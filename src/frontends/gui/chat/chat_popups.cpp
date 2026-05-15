#include "chat_popups.h"

namespace locus {

ChatPopups::ChatPopups(wxWindow* parent, wxTextCtrl* input)
    : parent_(parent)
    , input_(input)
{}

void ChatPopups::set_slash_commands(std::vector<SlashItem> items)
{
    slash_commands_ = std::move(items);
    if (slash_popup_) {
        if (slash_popup_shown_) slash_popup_->Dismiss();
        slash_popup_.reset();
        slash_popup_shown_ = false;
    }
}

void ChatPopups::set_mention_paths(std::vector<std::string> paths)
{
    mention_paths_ = std::move(paths);
    if (mention_popup_) {
        if (mention_popup_shown_) mention_popup_->Dismiss();
        mention_popup_.reset();
        mention_popup_shown_ = false;
    }
}

bool ChatPopups::handle_key(wxKeyEvent& evt)
{
    const int key = evt.GetKeyCode();

    if (slash_visible()) {
        switch (key) {
        case WXK_ESCAPE:
            hide_slash();
            return true;
        case WXK_UP:
            slash_popup_->move_up();
            return true;
        case WXK_DOWN:
            slash_popup_->move_down();
            return true;
        case WXK_TAB: {
            auto sel = slash_popup_->selected_command();
            if (!sel.empty()) { accept_slash(sel); return true; }
            break;
        }
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            if (!evt.ShiftDown()) {
                auto sel = slash_popup_->selected_command();
                if (!sel.empty()) { accept_slash(sel); return true; }
            }
            break;
        default:
            break;
        }
    }

    if (mention_visible()) {
        switch (key) {
        case WXK_ESCAPE:
            hide_mention();
            return true;
        case WXK_UP:
            mention_popup_->move_up();
            return true;
        case WXK_DOWN:
            mention_popup_->move_down();
            return true;
        case WXK_TAB: {
            auto sel = mention_popup_->selected_path();
            if (!sel.empty()) { accept_mention(sel); return true; }
            break;
        }
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            if (!evt.ShiftDown()) {
                auto sel = mention_popup_->selected_path();
                if (!sel.empty()) { accept_mention(sel); return true; }
            }
            break;
        default:
            break;
        }
    }

    return false;
}

void ChatPopups::update()
{
    update_slash();
    update_mention();
}

void ChatPopups::dismiss_all()
{
    hide_slash();
    hide_mention();
}

bool ChatPopups::slash_visible() const
{
    return slash_popup_ && slash_popup_shown_;
}

bool ChatPopups::mention_visible() const
{
    return mention_popup_ && mention_popup_shown_;
}

wxString ChatPopups::active_slash_token() const
{
    wxString v = input_->GetValue();
    if (v.empty() || v[0] != '/') return wxEmptyString;

    wxString token;
    for (size_t i = 1; i < v.size(); ++i) {
        wxUniChar c = v[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            return wxString("\x01");  // sentinel: stop suggesting
        token += c;
    }
    return token;
}

ChatPopups::ActiveMention ChatPopups::active_mention_at_cursor() const
{
    ActiveMention out;
    if (!input_) return out;

    long ins = input_->GetInsertionPoint();
    wxString v = input_->GetValue();
    if (ins <= 0 || static_cast<size_t>(ins) > v.size()) return out;

    auto is_path_char = [](wxUniChar c) {
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) return true;
        switch (static_cast<wchar_t>(c)) {
            case '/': case '\\': case '.': case '_': case '-':
            case '+': case '~': case '#': return true;
            default: return false;
        }
    };
    auto allowed_before_at = [](wxUniChar c) {
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) return false;
        switch (static_cast<wchar_t>(c)) {
            case '.': case '_': case '-': return false;
            default: return true;
        }
    };

    long i = ins - 1;
    while (i >= 0) {
        wxUniChar c = v[static_cast<size_t>(i)];
        if (c == '@') {
            if (i == 0 || allowed_before_at(v[static_cast<size_t>(i - 1)])) {
                out.start  = static_cast<size_t>(i);
                out.prefix = v.SubString(i + 1, ins - 1);
                return out;
            }
            return out;
        }
        if (!is_path_char(c)) return out;
        --i;
    }
    return out;
}

void ChatPopups::update_slash()
{
    if (slash_commands_.empty()) return;

    wxString token = active_slash_token();
    if (token.empty() || token == "\x01") {
        hide_slash();
        return;
    }

    if (!slash_popup_) {
        slash_popup_ = std::make_unique<SlashPopup>(parent_, slash_commands_);
        slash_popup_->on_accept = [this](const std::string& name) {
            accept_slash(name);
        };
        slash_popup_->on_dismiss = [this]() {
            slash_popup_shown_ = false;
        };
    }

    bool any = slash_popup_->apply_filter(token);
    if (!any) { hide_slash(); return; }

    wxPoint anchor = input_->GetScreenPosition();
    int w = input_->GetSize().GetWidth();
    if (!slash_popup_shown_) {
        slash_popup_->show_anchored(anchor, w);
        slash_popup_shown_ = true;
        input_->SetFocus();
    } else {
        slash_popup_->show_anchored(anchor, w);
    }
}

void ChatPopups::update_mention()
{
    if (mention_paths_.empty()) return;

    ActiveMention m = active_mention_at_cursor();
    if (m.start == std::string::npos) {
        hide_mention();
        return;
    }

    if (!mention_popup_) {
        mention_popup_ = std::make_unique<MentionPopup>(parent_, mention_paths_);
        mention_popup_->on_accept = [this](const std::string& path) {
            accept_mention(path);
        };
        mention_popup_->on_dismiss = [this]() {
            mention_popup_shown_ = false;
        };
    }

    bool any = mention_popup_->apply_filter(m.prefix);
    if (!any) { hide_mention(); return; }

    wxPoint anchor = input_->GetScreenPosition();
    int w = input_->GetSize().GetWidth();
    if (!mention_popup_shown_) {
        mention_popup_->show_anchored(anchor, w);
        mention_popup_shown_ = true;
        input_->SetFocus();
    } else {
        mention_popup_->show_anchored(anchor, w);
    }
}

void ChatPopups::hide_slash()
{
    if (slash_popup_ && slash_popup_shown_) {
        slash_popup_shown_ = false;
        slash_popup_->Dismiss();
    }
}

void ChatPopups::hide_mention()
{
    if (mention_popup_ && mention_popup_shown_) {
        mention_popup_shown_ = false;
        mention_popup_->Dismiss();
    }
}

void ChatPopups::accept_slash(const std::string& name)
{
    if (name.empty()) return;
    wxString new_text = "/" + wxString::FromUTF8(name) + " ";
    input_->ChangeValue(new_text);
    input_->SetInsertionPointEnd();
    hide_slash();
    input_->SetFocus();
}

void ChatPopups::accept_mention(const std::string& path)
{
    if (path.empty() || !input_) return;
    ActiveMention m = active_mention_at_cursor();
    if (m.start == std::string::npos) { hide_mention(); return; }

    wxString v = input_->GetValue();
    long ins = input_->GetInsertionPoint();
    wxString before   = v.SubString(0, static_cast<long>(m.start) - 1);
    wxString after    = v.SubString(ins, v.size() - 1);
    wxString inserted = "@" + wxString::FromUTF8(path) + " ";
    input_->ChangeValue(before + inserted + after);
    input_->SetInsertionPoint(
        static_cast<long>(before.size() + inserted.size()));
    hide_mention();
    input_->SetFocus();
}

} // namespace locus
