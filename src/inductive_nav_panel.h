/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef INDUCTIVE_NAV_PANEL_H
#define INDUCTIVE_NAV_PANEL_H

#include <kicommon.h>
#include <wx/panel.h>

#include <string>

class wxWebView;
class wxWebViewEvent;
class INDUCTIVE_PANEL;

/**
 * Inductive's navigation pane, on the editor's left: search, customize, this
 * project's chat history, and the account with its plan.
 *
 * It owns nothing. The chat panel already holds the chats, the session, billing
 * and the settings, so this pane is a second view of that one panel: its page's
 * messages are handed to INDUCTIVE_PANEL::HandleNavMessage(), which acts on the
 * few kinds this page is allowed to send, and the chat panel forwards the events
 * this page shows through Deliver(). One owner means the two can never disagree
 * about which chat is open or who is signed in.
 *
 * The page is nav.html, beside sidebar.html, under the same rules: no network
 * access of its own, and nothing it is sent is parsed as HTML.
 */
class KICOMMON_API INDUCTIVE_NAV_PANEL : public wxPanel
{
public:
    INDUCTIVE_NAV_PANEL( wxWindow* aParent, INDUCTIVE_PANEL* aChat );
    ~INDUCTIVE_NAV_PANEL() override;

    /// AUI pane name, shared by every editor that hosts the pane.
    static const wxString PaneName() { return wxS( "InductiveNav" ); }

    /// Hand the page one event (a JSON object).
    void Deliver( const std::string& aJson );

    /// The chat panel is going away first: stop talking to it.
    void DetachChat() { m_chat = nullptr; }

private:
    void onScriptMessage( wxWebViewEvent& aEvent );
    void handleScriptMessage( const std::string& aText );
    void onPageLoaded( wxWebViewEvent& aEvent );

    /// Shrink the pane to its icon rail, or give it back its width. The page
    /// draws the rail; only the host can resize a docked pane.
    void setCollapsed( bool aCollapsed );

    wxWebView*       m_webView = nullptr;
    INDUCTIVE_PANEL* m_chat = nullptr;
    bool             m_collapsed = false;
    bool             m_sized = false;       ///< Whether a width has been applied yet.
    int              m_expandedWidth = 0;   ///< The width to restore, from before collapsing.
};

#endif
