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

#include <widgets/inductive_nav_panel.h>
#include <widgets/inductive_panel.h>

#include <wx/aui/framemanager.h>

#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/webview.h>

#include <nlohmann/json.hpp>

#include <algorithm>


/// The rail's width, and the narrowest the full pane may be dragged to.
static const int RAIL_WIDTH = 52;
static const int MIN_WIDTH = 200;
static const int DEFAULT_WIDTH = 250;


INDUCTIVE_NAV_PANEL::INDUCTIVE_NAV_PANEL( wxWindow* aParent, INDUCTIVE_PANEL* aChat ) :
        wxPanel( aParent, wxID_ANY ),
        m_chat( aChat )
{
    SetBackgroundColour( wxColour( 0, 0, 0 ) );    // the page is pure black too

    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );

    m_webView = wxWebView::New( this, wxID_ANY );

    if( m_webView )
    {
        m_webView->EnableContextMenu( false );

        // Registered before the page loads, so window.inductiveHost exists by the
        // time the page's script runs.
        m_webView->AddScriptMessageHandler( wxS( "inductiveHost" ) );

        m_webView->Bind( wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,
                         &INDUCTIVE_NAV_PANEL::onScriptMessage, this );
        m_webView->Bind( wxEVT_WEBVIEW_LOADED, &INDUCTIVE_NAV_PANEL::onPageLoaded, this );

        sizer->Add( m_webView, 1, wxEXPAND, 0 );

        const wxFileName page( INDUCTIVE_PANEL::WebDir(), wxS( "nav.html" ) );

        if( page.FileExists() )
        {
            m_webView->LoadURL( wxFileSystem::FileNameToURL( page ) );
        }
        else
        {
            m_webView->SetPage( wxS( "<html><body style='background:#000;color:#a0a0a0;"
                                     "font:13px -apple-system,sans-serif;padding:16px'>"
                                     "The Inductive navigation page is missing.</body></html>" ),
                                wxEmptyString );
        }
    }
    else
    {
        sizer->Add( new wxStaticText( this, wxID_ANY,
                                      _( "Inductive: no web view backend is available." ) ),
                    0, wxALL, 12 );
    }

    SetSizer( sizer );
    Layout();

    if( m_chat )
        m_chat->SetNavPanel( this );
}


INDUCTIVE_NAV_PANEL::~INDUCTIVE_NAV_PANEL()
{
    // The chat panel outliving this pane must not deliver into it.
    if( m_chat )
        m_chat->SetNavPanel( nullptr );
}


void INDUCTIVE_NAV_PANEL::Deliver( const std::string& aJson )
{
    if( !m_webView )
        return;

    // A JSON document is a JavaScript literal, so this needs no further escaping.
    m_webView->RunScriptAsync( wxString::Format( wxS( "window.inductiveDeliver(%s);" ),
                                                 wxString::FromUTF8( aJson ) ) );
}


void INDUCTIVE_NAV_PANEL::onScriptMessage( wxWebViewEvent& aEvent )
{
    // After WebKit's callback returns, as the chat panel does (INDUCTIVE_PANEL::onScriptMessage).
    const std::string text( aEvent.GetString().utf8_str() );
    CallAfter( [this, text]() { handleScriptMessage( text ); } );
}


void INDUCTIVE_NAV_PANEL::handleScriptMessage( const std::string& text )
{
    // The one thing this pane does for itself: change its own width. Everything
    // else is the chat panel's, which checks the kind against what this page may
    // send and catches everything -- nothing may escape a wx event handler.
    const nlohmann::json msg = nlohmann::json::parse( text, nullptr, false );

    if( msg.is_object() && msg.contains( "type" ) && msg["type"] == "nav_collapse" )
    {
        if( msg.contains( "collapsed" ) && msg["collapsed"].is_boolean() )
            setCollapsed( msg["collapsed"].get<bool>() );

        return;
    }

    if( m_chat )
        m_chat->HandleNavMessage( text );
}


void INDUCTIVE_NAV_PANEL::setCollapsed( bool aCollapsed )
{
    wxAuiManager* mgr = wxAuiManager::GetManager( this );

    // The first call always applies a width. On a fresh profile wxAUI lays the
    // dock out at a few pixels and nothing else would ever widen it.
    if( !mgr || ( aCollapsed == m_collapsed && m_sized ) )
        return;

    wxAuiPaneInfo& pane = mgr->GetPane( this );

    if( !pane.IsOk() )
        return;

    if( aCollapsed && m_sized && !m_collapsed )
        m_expandedWidth = GetSize().x;

    const int expanded = m_expandedWidth > 0 ? m_expandedWidth : FromDIP( DEFAULT_WIDTH );
    const int width = aCollapsed ? FromDIP( RAIL_WIDTH ) : std::max( expanded, FromDIP( MIN_WIDTH ) );

    // wxAUI will not resize a docked pane on request. What it does do: a dock
    // that is fixed is re-measured from its pane's best size (the minimum is only
    // a floor) -- but at the *start* of a layout, going by whether the dock was
    // fixed in the layout before. So: the width goes in as both best and minimum,
    // the pane is fixed, and the layout runs twice -- the first marks the dock
    // fixed, the second sizes it. One pass works only when the dock was already
    // fixed, which is why collapsing worked the first time and not after an
    // expand. (KiCad's SetAuiPaneSize() sets only the minimum, which can widen a
    // pane but never narrow it.)
    pane.BestSize( width, -1 ).MinSize( width, -1 ).Fixed();
    mgr->Update();
    mgr->Update();

    m_collapsed = aCollapsed;
    m_sized = true;

    // Collapsed, it stays fixed: the rail is exactly this wide through every
    // later update.
    if( aCollapsed )
        return;

    // Expanded: draggable again. A resizable dock keeps the width it has, and the
    // minimum goes back to the usual floor.
    pane.Resizable();
    mgr->Update();
    pane.MinSize( FromDIP( wxSize( MIN_WIDTH, 200 ) ) );
}


void INDUCTIVE_NAV_PANEL::onPageLoaded( wxWebViewEvent& aEvent )
{
    aEvent.Skip();

    if( m_chat )
        m_chat->SyncNav();
}
