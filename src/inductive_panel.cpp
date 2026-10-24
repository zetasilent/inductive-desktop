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

#include <widgets/inductive_panel.h>
#include <widgets/inductive_attachments.h>
#include <widgets/inductive_parts.h>
#include <widgets/inductive_review_store.h>
#include <widgets/inductive_version.h>
#include <widgets/inductive_updates.h>
#include <pgm_base.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <kiid.h>
#include <wx/ffile.h>
#include <widgets/inductive_cloud_auth.h>
#include <widgets/inductive_nav_panel.h>
#include <widgets/inductive_reports.h>

#include <wx/app.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>
#include <wx/toplevel.h>

#include <kicad_curl/kicad_curl_easy.h>
#include <ki_exception.h>
#include <kiway_holder.h>
#include <project.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <ostream>
#include <random>
#include <map>
#include <regex>
#include <streambuf>
#include <thread>

#ifndef INDUCTIVE_WEB_DIR
#define INDUCTIVE_WEB_DIR ""
#endif

/**
 * The rungs of the sidebar's thinking-level slider.
 *
 * The level *is* the model choice, but the panel no longer knows which model that
 * is: it sends the rung number to Inductive's backend (POST /v1/generate), which
 * owns the model credential and the rung-to-model table, and makes exactly one model call
 * with no retry and no fallback. A retired model is then a redeploy rather than an
 * app release, and no credential or model id ships in this binary.
 *
 * The sidebar likewise sends a rung number and never an engine id, so what crosses
 * each boundary is an integer to bounds-check.
 */
static const char* const INTELLIGENCE_NAMES[] = { "Light", "Normal", "High", "Max", "Ultra" };

static const int INTELLIGENCE_LEVELS =
        (int) ( sizeof( INTELLIGENCE_NAMES ) / sizeof( *INTELLIGENCE_NAMES ) );


/// True for a rung number as the sidebar counts them (1-based).
static bool isIntelligenceLevel( int aLevel )
{
    return aLevel >= 1 && aLevel <= INTELLIGENCE_LEVELS;
}


/**
 * The rung a sidebar message carries, or 0 if it carries none.
 *
 * Checked for type before reading: json::value() throws on a mismatched type,
 * and nothing above the message handler would catch it.
 */
static int intelligenceLevelFrom( const nlohmann::json& aMsg )
{
    const auto it = aMsg.find( "level" );

    if( it == aMsg.end() || !it->is_number_integer() )
        return 0;

    const int level = it->get<int>();
    return isIntelligenceLevel( level ) ? level : 0;
}


// Customize's settings; defined with the other files this panel keeps, below.
static nlohmann::json LoadSettings();
static bool           SaveSettings( const nlohmann::json& aSettings );
static std::string    BoundedInstructions( const std::string& aText );
static bool           IsModeName( const std::string& aMode );


static std::string intelligenceName( int aLevel )
{
    if( !isIntelligenceLevel( aLevel ) )
        aLevel = INDUCTIVE_PANEL::DEFAULT_INTELLIGENCE;

    return INTELLIGENCE_NAMES[aLevel - 1];
}


/// Hard ceiling on one request, in seconds.
static const long REQUEST_TIMEOUT_S = 120;

/**
 * Cut a UTF-8 string to at most aMaxBytes, without splitting a character.
 *
 * A plain resize() would be a crash, not a cosmetic defect.  Board text is
 * routinely non-ASCII -- "10 µF", "±5 %", a part number with an en dash -- and
 * cutting a multi-byte sequence in half produces a std::string that is not valid
 * UTF-8.  nlohmann::json::dump() then throws type_error.316 when the result is
 * serialised into the request.
 *
 * That throw would happen inside a wxTheApp->CallAfter() continuation, where
 * there is nothing above it to catch: the editor terminates, with the user's
 * board unsaved, because a footprint value contained a µ.
 *
 * Continuation bytes are 10xxxxxx, so backing off while the last byte is one of
 * those lands on a character boundary. At most three steps.
 */
static std::string truncateUtf8( const std::string& aText, size_t aMaxBytes )
{
    if( aText.size() <= aMaxBytes )
        return aText;

    size_t cut = aMaxBytes;

    while( cut > 0 && ( static_cast<unsigned char>( aText[cut] ) & 0xC0 ) == 0x80 )
        --cut;

    return aText.substr( 0, cut );
}


/**
 * Product copy for a failed model request.
 *
 * The backend answers a failure with { error, message, request_id }, where
 * `error` is one of its own codes and never Google's text (which carries project
 * numbers and billing prose). Copy is keyed on the code; the request id is quoted
 * where the cause is on the server's side, so it can be found in the logs.
 */
static std::string friendlyError( int aStatus, const std::string& aRaw, int aLevel )
{
    const std::string    level = intelligenceName( aLevel );
    const nlohmann::json body  = nlohmann::json::parse( aRaw, nullptr, false );

    std::string code;
    std::string message;
    std::string ref;

    if( body.is_object() )
    {
        if( body.contains( "error" ) && body["error"].is_string() )
            code = body["error"].get<std::string>();

        if( body.contains( "message" ) && body["message"].is_string() )
            message = truncateUtf8( body["message"].get<std::string>(), 300 );

        if( body.contains( "request_id" ) && body["request_id"].is_string() )
            ref = " (ref " + truncateUtf8( body["request_id"].get<std::string>(), 64 ) + ")";
    }

    if( aStatus == 0 )
        return "Could not reach Inductive's server. Check the network connection.";

    if( aStatus == 426 || code == "upgrade_required" )
    {
        std::string link;

        if( body.is_object() && body.contains( "download_url" ) && body["download_url"].is_string() )
            link = truncateUtf8( body["download_url"].get<std::string>(), 300 );

        return "This version of Inductive is out of date. Download the new version"
               + ( link.empty() ? std::string( " from the Inductive website" ) : " from " + link )
               + ", install it, then send again.";
    }

    if( aStatus == 401 )
        return "Your sign-in has expired. Sign in again from the account menu, then resend.";

    // The server says which plan and how many turns it has (Pro 20 a month, Max 100), and
    // the next plan up; "free turns" was wrong for a paying account that ran out.
    if( aStatus == 402 )
    {
        if( !message.empty() )
            return message;

        return "You've used this month's Agent turns on your plan. Ask mode still works. "
               "Upgrade from the account menu, or wait until they reset on the 1st.";
    }

    if( code == "level_not_in_plan" )
    {
        return ( message.empty() ? "The " + level + " thinking level isn't included in your plan."
                                 : message )
               + " Pick a lower thinking level, or upgrade from the account menu.";
    }

    if( code == "turn_ended" )
    {
        return "This request ran out of model rounds. Send it again to continue from the "
               "current design.";
    }

    if( code == "quota_exhausted" )
    {
        return "The " + level + " thinking level has reached its usage limit for now. Pick "
               "another thinking level, or try again later.";
    }

    if( code == "model_unavailable" )
    {
        return "The model behind the " + level + " thinking level isn't available. Pick another "
               "thinking level.";
    }

    if( code == "upstream_unavailable" )
    {
        return "Inductive is busy right now at the " + level + " thinking level. Send again in "
               "a moment, or pick another level.";
    }

    if( code == "credits_depleted" )
    {
        return "Inductive can't answer right now: its AI service is out of credits. That's on "
               "our side, not yours. Try again later." + ref;
    }

    if( code == "upstream_auth" || code == "model_not_configured" || code == "server_misconfigured" )
    {
        return "Inductive's AI service isn't available right now. That's on our side, not yours. "
               "Try again later." + ref;
    }

    if( code == "upstream_rejected" )
        return "Inductive couldn't process this request. Try rephrasing it, or start a new "
               "chat." + ref;

    if( code == "busy" )
    {
        return "Another request from this account is still running. Wait for it to finish, "
               "then resend.";
    }

    if( aStatus == 413 || code == "request_too_large" )
        return "This request is too large to send. Try fewer or smaller images.";

    if( code == "daily_limit" )
    {
        return "You've reached today's limit for this plan. It resets at midnight UTC; upgrade "
               "from the account menu for a higher limit.";
    }

    if( aStatus == 429 )
        return "Too many requests in a short time. Wait a moment, then resend.";

    if( aStatus == 400 && !message.empty() )
        return "Inductive's server refused the request: " + message + ref;

    if( aStatus == 200 )
    {
        // The transport worked and the model said nothing usable.  Almost always a
        // safety filter or a truncated stream; either way, rephrasing helps.
        return "The model returned an empty reply. Try rephrasing the request.";
    }

    return "Inductive's server returned HTTP " + std::to_string( aStatus ) + "." + ref;
}

/// Escape a string for use inside HTML text content.
static wxString htmlEscape( const wxString& aStr )
{
    wxString out;

    for( wxUniChar c : aStr )
    {
        if( c == '<' )       out += wxS( "&lt;" );
        else if( c == '>' )  out += wxS( "&gt;" );
        else if( c == '&' )  out += wxS( "&amp;" );
        else                 out += c;
    }

    return out;
}


/**
 * A std::streambuf that parses the model's SSE stream as libcurl writes into it.
 *
 * KICAD_CURL_EASY::SetOutputStream() hands us bytes as they arrive, so text can
 * reach the sidebar while the model is still generating rather than after it has
 * finished.  Each complete `data:` line is decoded and its text parts handed to
 * the callback; everything is also kept verbatim, because a failed request
 * answers with a plain JSON error rather than an event stream.
 */
class SSE_SINK : public std::streambuf
{
public:
    explicit SSE_SINK( std::function<void( const std::string& )> aOnDelta ) :
            m_onDelta( std::move( aOnDelta ) )
    {}

    /// Everything the server sent, joined.
    const std::string& Raw() const { return m_raw; }

    /// The reply text, with thought parts excluded.
    const std::string& Text() const { return m_text; }

    /**
     * The model's parts, replayed verbatim into the history.
     *
     * The model attaches a signature to some parts and rejects a follow-up
     * request that drops it, so parts are merged only when they are plain text
     * and carry nothing else.
     */
    const nlohmann::json& Parts() const { return m_parts; }

    /// True if the model asked to call a tool.
    bool HasCalls() const { return m_hasCalls; }

    /// True if a payload could not be parsed; the stream is truncated from there.
    bool Broken() const { return m_broken; }

    /// Flush a trailing `data:` line the server sent without a closing newline.
    void Finish()
    {
        if( !m_pending.empty() && !m_broken )
        {
            m_pending += '\n';
            safeDrain();
        }
    }

protected:
    std::streamsize xsputn( const char* aData, std::streamsize aCount ) override
    {
        m_raw.append( aData, aCount );
        m_pending.append( aData, aCount );
        safeDrain();
        return aCount;
    }

    int overflow( int aChar ) override
    {
        if( aChar != EOF )
        {
            const char c = static_cast<char>( aChar );
            xsputn( &c, 1 );
        }

        return aChar;
    }

private:
    /**
     * std::ostream::write() swallows anything thrown from the streambuf, which
     * would truncate the reply with no error shown.  Failures are latched here
     * instead and reported once the transfer ends.
     */
    void safeDrain()
    {
        if( m_broken )
            return;

        try
        {
            drain();
        }
        catch( ... )
        {
            m_broken = true;
            m_pending.clear();
        }
    }

    void drain()
    {
        size_t nl;

        while( ( nl = m_pending.find( '\n' ) ) != std::string::npos )
        {
            std::string line = m_pending.substr( 0, nl );
            m_pending.erase( 0, nl + 1 );

            if( !line.empty() && line.back() == '\r' )
                line.pop_back();

            if( line.rfind( "data:", 0 ) != 0 )
                continue;

            std::string payload = line.substr( 5 );

            if( !payload.empty() && payload.front() == ' ' )
                payload.erase( 0, 1 );

            const nlohmann::json ev = nlohmann::json::parse( payload, nullptr, false );

            if( ev.is_discarded() || !ev.is_object() )
                continue;

            const auto cands = ev.find( "candidates" );

            if( cands == ev.end() || !cands->is_array() )
                continue;

            // Every key is probed rather than indexed: the final chunk of a reply
            // carries a finishReason and no "content" at all, and const operator[]
            // on a missing key is undefined behaviour.
            for( const auto& candidate : *cands )
            {
                if( !candidate.is_object() )
                    continue;

                const auto content = candidate.find( "content" );

                if( content == candidate.end() || !content->is_object() )
                    continue;

                const auto partsIt = content->find( "parts" );

                if( partsIt == content->end() || !partsIt->is_array() )
                    continue;

                for( const auto& part : *partsIt )
                {
                    if( !part.is_object() )
                        continue;

                    if( part.contains( "functionCall" ) )
                    {
                        m_hasCalls = true;
                        addPart( part );
                        continue;
                    }

                    // Thought summaries are narration, not the answer.
                    if( part.value( "thought", false ) )
                        continue;

                    const std::string delta = part.value( "text", std::string() );

                    if( delta.empty() )
                        continue;

                    m_text += delta;
                    m_onDelta( delta );
                    addPart( part );
                }
            }
        }
    }

    /// Append a part, merging it into the previous one only when both are bare text.
    void addPart( const nlohmann::json& aPart )
    {
        const bool bareText = aPart.contains( "text" ) && aPart.size() == 1;

        if( bareText && !m_parts.empty() )
        {
            nlohmann::json& last = m_parts.back();

            if( last.contains( "text" ) && last.size() == 1 )
            {
                last["text"] = last["text"].get<std::string>()
                               + aPart["text"].get<std::string>();
                return;
            }
        }

        m_parts.push_back( aPart );
    }

    std::function<void( const std::string& )> m_onDelta;
    std::string                               m_pending;
    std::string                               m_raw;
    std::string                               m_text;
    nlohmann::json                            m_parts = nlohmann::json::array();
    bool                                      m_hasCalls = false;
    bool                                      m_broken = false;
};


wxString INDUCTIVE_PANEL::WebDir()
{
    wxString dir;

    if( wxGetEnv( wxS( "INDUCTIVE_WEB_DIR" ), &dir ) && !dir.IsEmpty() )
        return dir;

    // Installed bundles carry the UI and starter assets; source builds fall back
    // to the configured checkout. Walk enclosing app bundles for editor launches.
    auto parent=std::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToStdString()).parent_path();
    for(int level=0;level<7 && !parent.empty();++level,parent=parent.parent_path()) {
        auto candidate=parent/"SharedSupport"/"inductive"/"web";
        if(std::filesystem::exists(candidate/"sidebar.html"))return wxString::FromUTF8(candidate.string());
    }
    return wxString::FromUTF8( INDUCTIVE_WEB_DIR );
}


wxString INDUCTIVE_PANEL::ProjectDir()
{
    wxFileName dir;
    dir.AssignDir( WebDir() );   // <src>/inductive/web
    dir.RemoveLastDir();         // <src>/inductive
    return dir.GetPath();
}


INDUCTIVE_PANEL::INDUCTIVE_PANEL( wxWindow* aParent, wxWindowID aId ) :
        wxPanel( aParent, aId ),
        m_webView( nullptr ),
        m_toolRounds( 0 ),
        m_busy( false ),
        m_mutationsThisTurn( 0 ),
        m_turnStart( 0 ),
        m_state( std::make_shared<TURN_STATE>() )
{
    // KICAD_CURL_EASY turns on CURLOPT_VERBOSE whenever this is set, and verbose
    // libcurl prints every outgoing request header to stderr -- including the
    // Authorization header. That is pre-existing KiCad behaviour and it was
    // harmless until this panel became the first thing in the build to put a live
    // credential in a curl header. Cleared once, here, because a debugging switch
    // must not be a credential disclosure.
    if( wxGetEnv( wxS( "KICAD_CURL_VERBOSE" ), nullptr ) )
    {
        wxUnsetEnv( wxS( "KICAD_CURL_VERBOSE" ) );
        wxLogTrace( wxS( "INDUCTIVE" ),
                    wxS( "KICAD_CURL_VERBOSE was set; cleared, because it would print the "
                         "session token to stderr." ) );
    }

    SetBackgroundColour( wxColour( 0, 0, 0 ) );

    // An editor opened on its own (a schematic double-clicked in Finder) never shows
    // Home, so it asks for updates too; once per process either way.
    CallAfter( [] { INDUCTIVE_UPDATES::CheckAtLaunch(); } );

    // The pipeline's plumbing: a timer that drains the runner's output, and the
    // notification that it has exited. Both fire on the UI thread, which is where
    // the document handlers have to run anyway.
    m_pipelineTimer.SetOwner( this, wxWindow::NewControlId() );
    Bind( wxEVT_TIMER, [this]( wxTimerEvent& ) { pumpPipeline(); }, m_pipelineTimer.GetId() );

    // Notice when the editor opens a different project, so its chats are not mixed
    // with this one's. Polled: there is no project-changed event a panel can hear.
    m_projectTimer.SetOwner( this, wxWindow::NewControlId() );
    Bind( wxEVT_TIMER, [this]( wxTimerEvent& ) { checkProject(); }, m_projectTimer.GetId() );
    m_projectTimer.Start( 1500 );
    Bind( wxEVT_END_PROCESS, &INDUCTIVE_PANEL::onPipelineEnd, this );

    // Coming back to the editor is the only sign a browser sign-in was abandoned:
    // a closed tab says nothing to anyone.
    if( wxWindow* top = wxGetTopLevelParent( this ) )
    {
        m_activateSource = top;
        top->Bind( wxEVT_ACTIVATE, &INDUCTIVE_PANEL::onFrameActivate, this );
    }

    wxBoxSizer* sizer = new wxBoxSizer( wxVERTICAL );

    m_webView = wxWebView::New( this, wxID_ANY );

    if( m_webView )
    {
        m_webView->EnableContextMenu( false );

        // Registered before the page loads, so window.inductiveHost exists by the
        // time the page's script runs.
        m_webView->AddScriptMessageHandler( wxS( "inductiveHost" ) );

        m_webView->Bind( wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,
                         &INDUCTIVE_PANEL::onScriptMessage, this );
        m_webView->Bind( wxEVT_WEBVIEW_ERROR, &INDUCTIVE_PANEL::onError, this );
        m_webView->Bind( wxEVT_WEBVIEW_LOADED, &INDUCTIVE_PANEL::onPageLoaded, this );

        sizer->Add( m_webView, 1, wxEXPAND, 0 );
        ReloadUI();
    }
    else
    {
        sizer->Add( new wxStaticText( this, wxID_ANY,
                                      _( "Inductive: no web view backend is available." ) ),
                    0, wxALL, 12 );
    }

    SetSizer( sizer );
    Layout();
}


INDUCTIVE_PANEL::~INDUCTIVE_PANEL()
{
    m_projectTimer.Stop();

    // The pane outliving this panel must not deliver into it.
    if( m_nav )
        m_nav->DetachChat();

    if( m_activateSource )
        m_activateSource->Unbind( wxEVT_ACTIVATE, &INDUCTIVE_PANEL::onFrameActivate, this );

    // A runner outliving its panel would write events into freed memory. Killing
    // it first is also the honest thing: nobody is going to read its answer.
    if( m_pipeline )
    {
        // SIGKILL rather than SIGTERM: the panel is going away now, and a runner
        // inside a synchronous export will not act on a polite signal in time.
        // One left behind would hold the API key and keep writing files.
        if( m_pipelinePid )
            wxProcess::Kill( m_pipelinePid, wxSIGKILL, wxKILL_CHILDREN );

        m_pipelineTimer.Stop();
        m_pipeline->Detach();   // it may still be running; let wx clean it up
        m_pipeline = nullptr;
    }

    // A worker may still be waiting on curl.  It holds its own shared_ptr to this
    // state, so the state outlives the panel and clearing the flag makes every
    // callback a no-op rather than a use-after-free.
    m_state->alive = false;
}


void INDUCTIVE_PANEL::ReloadUI()
{
    if( !m_webView )
        return;

    const wxFileName page( WebDir(), wxS( "sidebar.html" ) );

    if( !page.FileExists() )
    {
        ShowMessage( _( "The Inductive sidebar is missing" ), page.GetFullPath() );
        return;
    }

    m_webView->LoadURL( wxFileSystem::FileNameToURL( page ) );
}


void INDUCTIVE_PANEL::onScriptMessage( wxWebViewEvent& aEvent )
{
    // Handled after WebKit's callback returns, never inside it: anything that runs an
    // event loop from here (a progress dialog's yield, a KiCad prompt) lets WebKit
    // deliver the next message re-entrantly, and the home window crashed exactly so.
    const std::string text( aEvent.GetString().utf8_str() );
    CallAfter( [this, text]() { handleMessage( text, false ); } );
}


void INDUCTIVE_PANEL::HandleNavMessage( const std::string& aJson )
{
    handleMessage( aJson, true );
}


/// What the navigation pane may ask for. It cannot start a turn, run a command or
/// touch the document: a page that only lists chats and shows an account has no
/// reason to, so it is not given the means.
static bool NavMayAsk( const std::string& aType )
{
    static const char* const allowed[] = {
        "history_list", "history_open", "history_delete", "history_search", "new_chat",
        "auth_status", "auth_sign_in", "auth_sign_out", "auth_cancel",
        "billing_open", "billing_portal", "billing_refresh",
        "settings_get", "settings_set"
    };

    for( const char* type : allowed )
    {
        if( aType == type )
            return true;
    }

    return false;
}


static wxString ChatsRoot();
static bool modeAllowsMutation( const wxString& );

void INDUCTIVE_PANEL::handleMessage( const std::string& aJson, bool aFromNav )
{
    // Wrapped whole.  This is a wxWidgets event handler, so anything that escapes
    // it terminates the editor rather than becoming an error -- and the body is
    // full of throwing calls: nlohmann's value() returns the default only when a
    // key is ABSENT, and throws type_error.302 when the key is present with the
    // wrong type. A host message of {"type": 5} is enough.
    //
    // The shipped page never sends such a message, so this is belt and braces
    // rather than a live defect. It is here because "the page only sends strings"
    // is a property of a file that is edited, reloadable from disk, and one line
    // away from not being true.
    try
    {
        const nlohmann::json msg = nlohmann::json::parse( aJson, nullptr, false );

        if( msg.is_discarded() || !msg.is_object() )
            return;

    const std::string type = msg.value( "type", std::string() );

    if( aFromNav && !NavMayAsk( type ) )
        return;

    if( type == "send" )
    {
        nlohmann::json attachments = nlohmann::json::array();

        if( const auto it = msg.find( "attachments" ); it != msg.end() && it->is_array() )
            attachments = *it;

        // Every prompt carries the slider's level, so the turn runs on the model the
        // user can see selected -- including before they have ever touched it.
        if( const int level = intelligenceLevelFrom( msg ) )
            m_intelligence = level;

        const wxString prompt = wxString::FromUTF8( msg.value( "prompt", std::string() ) );

        // "/redeem CODE" is an account action, answered here: no model request is made.
        if( attachments.empty() && redeemPromo( prompt ) )
            return;

        ask( prompt, wxString::FromUTF8( msg.value( "mode", std::string( "agent" ) ) ), attachments );
    }
    else if(type.rfind("workflow_",0)==0)
    {
        try { workflowMessage(msg); }
        catch(const std::exception& e) {emitEvent(nlohmann::json{{"type","workflow_error"},{"message",e.what()}}.dump());}
        catch(const KI_PARAM_ERROR& e) {emitEvent(nlohmann::json{{"type","workflow_error"},{"message",e.What().ToStdString()}}.dump());}
        catch(...) {emitEvent(nlohmann::json{{"type","workflow_error"},{"message","Could not complete this step. Refresh and inspect the native document before retrying."}}.dump());}
    }
    else if(type.rfind("review_",0)==0)
    {
        try { reviewMessage(msg); }
        catch(const std::exception& e) {emitEvent(nlohmann::json{{"type","review_error"},{"message",e.what()}}.dump());}
        catch(const KI_PARAM_ERROR& e) {emitEvent(nlohmann::json{{"type","review_error"},{"message",e.What().ToStdString()}}.dump());}
    }
    else if(type=="circuit_focus")
    {
        try {
            if(m_busy || !m_circuitEditHandler)throw std::runtime_error("Wait for the assistant before inspecting this change.");
            wxString result;m_circuitEditHandler("focus",msg,result);
            emitEvent(nlohmann::json{{"type","circuit_focus_result"},{"message",result.ToStdString()}}.dump());
        }catch(const std::exception& e){emitEvent(nlohmann::json{{"type","circuit_focus_result"},{"message",e.what()}}.dump());}
    }
    else if( type == "circuit_apply" || type == "circuit_discard" )
    {
        wxString result;
        bool ok = false;
        try
        {
            if( m_busy ) throw std::runtime_error( "Wait for the answer to finish before applying the preview." );
            if( type == "circuit_apply" && !modeAllowsMutation( wxString::FromUTF8( msg.value( "mode", "ask" ) ) ) )
                throw std::runtime_error( "Switch to Agent mode to apply this preview." );
            if( !m_circuitEditHandler ) throw std::runtime_error( "Circuit changes belong in the schematic editor." );
            if(type=="circuit_discard") ok=m_circuitEditHandler("discard",msg,result);
            else
            {
                ok = applyCircuitEdit( msg, result );
            }
        }
        catch( const std::exception& e ) { result = wxString::FromUTF8( e.what() ); }
        catch( const KI_PARAM_ERROR& e ) { result = e.What(); }
        catch( ... ) { result = "Could not apply this preview. Request a fresh preview."; }
        emitEvent( nlohmann::json{ { "type", "circuit_result" }, { "token", msg.value( "token", "" ) },
                   { "ok", ok }, { "discarded", type == "circuit_discard" }, { "message", result.ToStdString() } }.dump() );
    }
    else if( type.rfind( "parts_", 0 ) == 0 )
    {
        try
        {
            INDUCTIVE_PARTS::STORE store( wxFileName( ChatsRoot() ).GetPath() + "/parts" );
            if( type == "parts_get" )
            {
                emitEvent( nlohmann::json{ { "type", "parts_detail" }, { "part", store.Get( msg.at( "id" ) ) } }.dump() );
                return;
            }
            if( type == "parts_add" )
            {
                const auto file = msg.at( "file" );
                const bool pdf = INDUCTIVE_DATASHEETS::IsPDF(file);
                INDUCTIVE_DATASHEETS::Summary( file );
                wxString why;
                if( !pdf && m_attachmentHandler && !m_attachmentHandler( file, false, why ) )
                    throw std::runtime_error( why.ToStdString() );
                auto entry = store.Add( file );
                if( pdf && msg.contains("part_id") )
                {
                    try { store.Update(msg.at("part_id"),{{"datasheet_id",entry["id"]}},msg.at("revision")); }
                    catch( const std::exception& e )
                    {
                        sendParts();
                        throw std::runtime_error("The PDF was saved to My Parts, but could not be linked: " + std::string(e.what()) + " Refresh the part and select the saved PDF.");
                    }
                }
                emitEvent( nlohmann::json{ { "type", "parts_saved" }, { "part", entry }, {"linked_part_id",msg.value("part_id",entry["id"].get<std::string>())} }.dump() );
            }
            else if( type == "parts_update" )
            {
                auto entry = store.Update( msg.at( "id" ), msg.at( "changes" ), msg.at( "revision" ) );
                emitEvent( nlohmann::json{ { "type", "parts_saved" }, { "part", entry } }.dump() );
            }
            else if(type=="parts_specs_save")
            {
                auto entry=store.SaveSpecifications(msg.at("id"),msg.at("facts"),msg.at("revision"));
                emitEvent(nlohmann::json{{"type","parts_saved"},{"part",entry},{"specifications_saved",true}}.dump());
                emitEvent(nlohmann::json{{"type","review_invalidated"},{"message","Datasheet specifications changed. Rerun review and confirm affected bindings."}}.dump());
            }
            else if( type == "parts_open_datasheet" )
            {
                if( !wxLaunchDefaultApplication( store.PDFPath(msg.at("id")) ) ) throw std::runtime_error("Could not open the PDF viewer.");
            }
            else if( type == "parts_archive" ) store.Archive( msg.at( "id" ) );
            else if( type == "parts_use" )
            {
                if( m_busy ) throw std::runtime_error( "Wait for the current turn before using a part." );
                if( !modeAllowsMutation( wxString::FromUTF8( msg.value( "mode", "ask" ) ) ) )
                    throw std::runtime_error( "Switch to Agent mode to use a part in the project." );
                auto result = usePart( msg.at( "id" ) );
                m_selectedPart = result; m_selectedPartProject = currentProject();
                emitEvent( nlohmann::json{ { "type", "parts_used" }, { "result", result } }.dump() );
            }
            sendParts();
        }
        catch( const std::exception& e )
        { emitEvent( nlohmann::json{ { "type", "parts_error" }, { "message", e.what() } }.dump() ); }
        catch( const KI_PARAM_ERROR& e )
        { emitEvent( nlohmann::json{ { "type", "parts_error" }, { "message", e.What().ToStdString() } }.dump() ); }
    }
    else if( type == "command" )
    {
        if( const int level = intelligenceLevelFrom( msg ) )
            m_intelligence = level;

        runCommand( wxString::FromUTF8( msg.value( "command", std::string() ) ) );
    }
    else if( type == "stop" )
    {
        stopTurn();
    }
    else if( type == "auth_sign_in" )
    {
        const INDUCTIVE_CLOUD_AUTH::CONFIG cfg = INDUCTIVE_CLOUD_AUTH::LoadConfig();

        if( !cfg.configured() )
        {
            emitEvent( nlohmann::json{
                    { "type", "auth" },
                    { "status", "error" },
                    { "message", "Set INDUCTIVE_API_BASE, INDUCTIVE_GCP_PROJECT_ID, "
                                 "INDUCTIVE_FIREBASE_API_KEY, and "
                                 "INDUCTIVE_FIREBASE_AUTH_DOMAIN" } }
                               .dump() );
            return;
        }

        emitEvent( nlohmann::json{ { "type", "auth" }, { "status", "pending" } }.dump() );

        // Each click starts afresh. An earlier attempt keeps listening -- finishing
        // in its browser tab still signs in -- but only the newest may report
        // that it failed.
        const unsigned attempt = ++m_signInAttempt;
        m_signInPending = true;

        // Capture a weak-ish alive flag via shared turn state.
        auto state = m_state;

        INDUCTIVE_CLOUD_AUTH::BeginBrowserSignIn(
                cfg,
                [this, state, attempt]( const wxString& error, bool abandoned )
                {
                    if( !state || !state->alive.load() )
                        return;

                    CallAfter(
                            [this, state, attempt, error, abandoned]()
                            {
                                if( !state || !state->alive.load() )
                                    return;

                                // A success is a success, whichever attempt it was.
                                // A failure matters only if it is the latest attempt's.
                                if( !error.IsEmpty() && attempt != m_signInAttempt )
                                    return;

                                if( attempt == m_signInAttempt )
                                    m_signInPending = false;

                                // Never came back from the browser: not an error, just
                                // not signed in.
                                if( !error.IsEmpty() && abandoned )
                                {
                                    emitAuthStatus();
                                    return;
                                }

                                const auto cfgNow = INDUCTIVE_CLOUD_AUTH::LoadConfig();
                                const std::string base( INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized(
                                                                cfgNow.apiBase )
                                                                .utf8_str() );

                                if( !error.IsEmpty() )
                                {
                                    emitEvent( nlohmann::json{
                                            { "type", "auth" },
                                            { "status", "error" },
                                            { "configured", cfgNow.configured() },
                                            { "apiBase", base },
                                            { "message", std::string( error.utf8_str() ) } }
                                                       .dump() );
                                    return;
                                }

                                // The session is already stored and held; /v1/me fills in
                                // the plan and the account.
                                emitEvent( nlohmann::json{ { "type", "auth" },
                                                           { "status", "signed_in" },
                                                           { "apiBase", base },
                                                           { "configured", true } }
                                                   .dump() );
                                refreshEntitlementAsync();
                            } );
                } );
    }
    else if( type == "auth_cancel" )
    {
        // The browser may still finish -- and if it does, that sign-in counts --
        // but the pages stop waiting for it, and its timeout will not report.
        ++m_signInAttempt;
        m_signInPending = false;
        emitAuthStatus();
    }
    else if( type == "auth_sign_out" )
    {
        m_signInPending = false;
        INDUCTIVE_CLOUD_AUTH::SignOut();
        emitEvent( nlohmann::json{ { "type", "auth" },
                                   { "status", "signed_out" },
                                   { "configured", INDUCTIVE_CLOUD_AUTH::LoadConfig().configured() },
                                   { "plan", "free" } }
                           .dump() );
    }
    else if( type == "auth_status" )
    {
        emitAuthStatus();
        refreshEntitlementAsync();
    }
    else if( type == "billing_open" || type == "billing_checkout" )
    {
        // billing_checkout is what older sidebars send; both open the plans page.
        const auto str = [&]( const char* aKey )
        {
            const auto it = msg.find( aKey );
            return it != msg.end() && it->is_string() ? it->get<std::string>() : std::string();
        };

        openPlansPage( str( "plan" ), str( "account" ) );
    }
    else if( type == "billing_portal" )
    {
        handleBillingPortal();
    }
    else if( type == "intelligence" )
    {
        // Out of range leaves the previous choice in place: the slider is a
        // preference, and a bad value is not worth interrupting the user over.
        if( const int level = intelligenceLevelFrom( msg ) )
            m_intelligence = level;
    }
    else if( type == "billing_refresh" )
    {
        refreshEntitlementAsync();
    }
    else if( type == "reset" )
        {
            stopTurn();
            m_history.clear();
            m_turnStart = 0;
            m_imageTurn = 0;
            startNewChat();
            pushNavHistory();
        }
    else if( type == "history_list" )
        {
            sendHistoryList();
        }
    else if( type == "history_open" )
        {
            openChat( msg.value( "id", std::string() ) );
        }
    else if( type == "history_delete" )
        {
            deleteChat( msg.value( "id", std::string() ) );
        }
    else if( type == "history_search" )
        {
            searchHistory( msg.value( "query", std::string() ) );
        }
    else if( type == "new_chat" )
        {
            // The chat page does what its own New button does -- stop, reset, clear
            // -- so a new chat looks the same wherever it was asked for.
            emitEvent( nlohmann::json{ { "type", "new_chat" } }.dump() );
        }
    else if( type == "settings_get" )
        {
            emitSettings();
        }
    else if( type == "settings_set" )
        {
            nlohmann::json settings = LoadSettings();

            if( const auto it = msg.find( "instructions" ); it != msg.end() && it->is_string() )
                settings["instructions"] = BoundedInstructions( it->get<std::string>() );

            if( const auto it = msg.find( "mode" ); it != msg.end() && it->is_string()
                && IsModeName( it->get<std::string>() ) )
                settings["mode"] = it->get<std::string>();

            if( const int level = intelligenceLevelFrom( msg ) )
                settings["level"] = level;

            if( const auto it = msg.find( "navCollapsed" ); it != msg.end() && it->is_boolean() )
                settings["navCollapsed"] = it->get<bool>();

            SaveSettings( settings );
            emitSettings();
        }
    }
    catch( const std::exception& e )
    {
        wxLogTrace( wxS( "INDUCTIVE" ), wxS( "bad host message: %s" ),
                    wxString::FromUTF8( e.what() ) );

        if( INDUCTIVE_REPORTS::Unexpected( &e ) )
            INDUCTIVE_REPORTS::Error( reportApp(), "sidebar message", e.what() );
    }
    catch( ... )
    {
        wxLogTrace( wxS( "INDUCTIVE" ), wxS( "bad host message" ) );
        INDUCTIVE_REPORTS::Error( reportApp(), "sidebar message", "unknown exception" );
    }
}


void INDUCTIVE_PANEL::onFrameActivate( wxActivateEvent& aEvent )
{
    aEvent.Skip();

    // Back in the editor with the sign-in still open: the user closed the tab or
    // gave up on it. Show Sign in again. The listener stays up, so finishing in
    // that tab after all still signs in; its timeout, being stale, says nothing.
    if( aEvent.GetActive() && m_signInPending && !INDUCTIVE_CLOUD_AUTH::HasSession() )
    {
        m_signInPending = false;
        ++m_signInAttempt;
        emitAuthStatus();
    }
}


/// Whether the stored session has been read yet. Until it has, "signed out" means
/// "not known yet", and the chat page must not tell anyone to sign in.
static std::atomic<bool> g_sessionSettled{ false };


void INDUCTIVE_PANEL::emitAuthStatus()
{
    const bool signedIn = INDUCTIVE_CLOUD_AUTH::HasSession();
    const auto cfg = INDUCTIVE_CLOUD_AUTH::LoadConfig();
    nlohmann::json ev{ { "type", "auth" },
                       { "status", signedIn ? "signed_in" : "signed_out" },
                       { "settled", g_sessionSettled.load() },
                       { "configured", cfg.configured() },
                       { "apiBase",
                         std::string( INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized( cfg.apiBase )
                                              .utf8_str() ) } };
    emitEvent( ev.dump() );
}


void INDUCTIVE_PANEL::refreshEntitlementAsync()
{
    const auto cfg = INDUCTIVE_CLOUD_AUTH::LoadConfig();

    if( !INDUCTIVE_CLOUD_AUTH::HasSession() || !cfg.configured() )
        return;

    auto state = m_state;

    std::thread(
            [this, state]()
            {
                const auto resp = INDUCTIVE_CLOUD_AUTH::ApiRequest( wxS( "GET" ), wxS( "/v1/me" ) );

                if( !state || !state->alive.load() )
                    return;

                CallAfter(
                        [this, state, resp]()
                        {
                            if( !state || !state->alive.load() )
                                return;

                            // The server ended the session (signed out elsewhere, or
                            // gone unused too long): show that, not a billing error.
                            if( resp.signedOut )
                            {
                                emitAuthStatus();
                                return;
                            }

                            if( !resp.error.IsEmpty() || resp.status < 200 || resp.status >= 300 )
                            {
                                nlohmann::json ev{
                                        { "type", "billing" },
                                        { "ok", false },
                                        { "action", "entitlement" },
                                        { "message",
                                          resp.error.IsEmpty()
                                                  ? ( "Could not load plan (HTTP "
                                                      + std::to_string( resp.status ) + ")" )
                                                  : std::string( resp.error.utf8_str() ) } };
                                emitEvent( ev.dump() );
                                return;
                            }

                            nlohmann::json body = nlohmann::json::parse( resp.body, nullptr, false );
                            std::string    plan = "free";
                            std::string    subStatus = "none";
                            std::string    email;
                            std::string    name;
                            nlohmann::json turnsUsed;
                            nlohmann::json turnQuota;
                            bool           canStartTurn = true;

                            if( body.is_object() )
                            {
                                if( body.contains( "entitlement" ) && body["entitlement"].is_object() )
                                {
                                    plan = body["entitlement"].value( "plan", std::string( "free" ) );
                                    subStatus =
                                            body["entitlement"].value( "status", std::string( "none" ) );

                                    // This month's use against the plan's allowance; a
                                    // null quota is unlimited, and is passed on as such.
                                    const nlohmann::json& ent = body["entitlement"];

                                    if( ent.contains( "turnsUsedThisMonth" )
                                        && ent["turnsUsedThisMonth"].is_number_integer() )
                                        turnsUsed = ent["turnsUsedThisMonth"];

                                    if( ent.contains( "turnQuota" )
                                        && ent["turnQuota"].is_number() )
                                        turnQuota = ent["turnQuota"];

                                    // The server's own verdict; the page only mirrors it.
                                    if( ent.contains( "canStartAgentTurn" )
                                        && ent["canStartAgentTurn"].is_boolean() )
                                        canStartTurn = ent["canStartAgentTurn"].get<bool>();
                                }

                                if( body.contains( "user" ) && body["user"].is_object() )
                                {
                                    const nlohmann::json& user = body["user"];

                                    if( user.contains( "email" ) && user["email"].is_string() )
                                        email = user["email"].get<std::string>();

                                    // The display name Google or GitHub gave at sign-in.
                                    if( user.contains( "name" ) && user["name"].is_string() )
                                        name = user["name"].get<std::string>();
                                }
                            }

                            const auto cfg = INDUCTIVE_CLOUD_AUTH::LoadConfig();
                            emitEvent( nlohmann::json{
                                    { "type", "auth" },
                                    { "status", "signed_in" },
                                    { "configured", true },
                                    { "apiBase",
                                      std::string( INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized(
                                                           cfg.apiBase )
                                                           .utf8_str() ) },
                                    { "plan", plan },
                                    { "entitlementStatus", subStatus },
                                    { "email", email },
                                    { "name", name },
                                    { "turnsUsed", turnsUsed },
                                    { "turnQuota", turnQuota },
                                    { "canStartTurn", canStartTurn },
                                    { "settled", true } }
                                               .dump( -1, ' ', false,
                                                      nlohmann::json::error_handler_t::replace ) );
                        } );
            } )
            .detach();
}


/// Percent-encode for a query string value.
static wxString QueryValue( const wxString& aValue )
{
    static const char* const hex = "0123456789ABCDEF";
    const std::string        utf8( aValue.utf8_str() );
    std::string              out;

    for( unsigned char c : utf8 )
    {
        if( std::isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' )
        {
            out += (char) c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }

    return wxString::FromUTF8( out );
}


/**
 * The plans page on Inductive's server, which the browser opens like the sign-in page.
 * It carries the public Firebase settings so the page can use the browser's own
 * sign-in, the plan the user picked (if any), and the account the app is signed in as,
 * so the page can warn before a plan is bought for a different account.
 */
static wxString PlansPageUrl( const INDUCTIVE_CLOUD_AUTH::CONFIG& aCfg, const std::string& aPlan,
                              const std::string& aAccount )
{
    wxString url = INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized( aCfg.apiBase ) + wxS( "/billing?apiKey=" )
                   + QueryValue( aCfg.firebaseApiKey ) + wxS( "&projectId=" )
                   + QueryValue( aCfg.projectId ) + wxS( "&authDomain=" )
                   + QueryValue( aCfg.firebaseAuthDomain );

    if( aPlan == "pro" || aPlan == "max" || aPlan == "ultra" )
        url += wxS( "&plan=" ) + wxString::FromUTF8( aPlan );

    // An email address, and nothing that could be anything else.
    const bool plausible = !aAccount.empty() && aAccount.size() <= 254
                           && aAccount.find( '@' ) != std::string::npos
                           && std::none_of( aAccount.begin(), aAccount.end(),
                                            []( unsigned char c ) { return c < 0x21 || c == 0x7f; } );

    if( plausible )
        url += wxS( "&account=" ) + QueryValue( wxString::FromUTF8( aAccount ) );

    return url;
}


void INDUCTIVE_PANEL::openPlansPage( const std::string& aPlan, const std::string& aAccount )
{
    const auto cfg = INDUCTIVE_CLOUD_AUTH::LoadConfig();

    if( !cfg.configured() )
    {
        emitEvent( nlohmann::json{ { "type", "billing" },
                                   { "ok", false },
                                   { "action", "plans" },
                                   { "message", "Inductive isn't connected to its server." } }
                           .dump() );
        return;
    }

    // Plans, checkout and Stripe all happen in the browser, like sign-in: the page
    // there signs in with the browser's own session and hands off to Stripe.
    const bool opened = wxLaunchDefaultBrowser( PlansPageUrl( cfg, aPlan, aAccount ) );

    emitEvent( nlohmann::json{ { "type", "billing" },
                               { "ok", opened },
                               { "action", "plans" },
                               { "message", opened ? "" : "Couldn't open your browser." } }
                       .dump() );
}


/// "2026-11-18T..." as "November 18, 2026"; the text as given when it is not a date.
static std::string ReadableDate( const std::string& aIso )
{
    static const char* MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December" };
    int y = 0, m = 0, d = 0;

    if( std::sscanf( aIso.c_str(), "%d-%d-%d", &y, &m, &d ) != 3 || m < 1 || m > 12 )
        return aIso;

    return std::string( MONTHS[m - 1] ) + " " + std::to_string( d ) + ", " + std::to_string( y );
}


bool INDUCTIVE_PANEL::redeemPromo( const wxString& aText )
{
    static const std::regex redeem( R"(^\s*/redeem(?:\s+(\S+))?\s*$)", std::regex::icase );
    const std::string       text( aText.utf8_str() );
    std::smatch             match;

    if( !std::regex_match( text, match, redeem ) )
        return false;

    auto answer = [this]( const std::string& aText, bool aFailed )
    {
        if( aFailed )
            emitEvent( nlohmann::json{ { "type", "error" }, { "message", aText } }.dump() );
        else
            emitEvent( nlohmann::json{ { "type", "done" }, { "text", aText } }.dump() );
    };

    const std::string code = match[1].matched ? match[1].str() : std::string();

    if( code.empty() )
    {
        answer( "Type /redeem followed by your promotion code, for example: /redeem CONF2026", false );
        return true;
    }

    if( !INDUCTIVE_CLOUD_AUTH::LoadConfig().configured() || !INDUCTIVE_CLOUD_AUTH::HasSession() )
    {
        answer( "Sign in first (bottom left of the sidebar), then send /redeem " + code + " again.", true );
        return true;
    }

    emitEvent( nlohmann::json{ { "type", "trace" }, { "verb", "Redeeming" }, { "object", code } }.dump() );

    auto state = m_state;

    std::thread(
            [this, state, code, answer]()
            {
                const auto resp = INDUCTIVE_CLOUD_AUTH::ApiRequest(
                        wxS( "POST" ), wxS( "/v1/billing/redeem" ),
                        nlohmann::json{ { "code", code } }.dump() );

                wxAppConsole* app = wxTheApp;

                if( !app || !state->alive.load() )
                    return;

                app->CallAfter(
                        [this, state, resp, code, answer]()
                        {
                            if( !state->alive.load() )
                                return;

                            if( resp.signedOut )
                            {
                                emitAuthStatus();
                                answer( "Your session ended. Sign in again, then send /redeem " + code + " again.", true );
                                return;
                            }

                            const auto body = nlohmann::json::parse( resp.body, nullptr, false );

                            if( resp.status == 200 && body.is_object() )
                            {
                                static const std::map<std::string, std::string> names = {
                                    { "pro", "Pro" }, { "max", "Max" }, { "ultra", "Ultra" } };
                                const std::string plan = body.value( "plan", std::string( "pro" ) );
                                const auto        name = names.count( plan ) ? names.at( plan ) : plan;

                                answer( "Code " + body.value( "code", code ) + " applied: you're on " + name + " until "
                                                + ReadableDate( body.value( "endsAt", std::string() ) )
                                                + ". Nothing is charged, and the plan is active now.",
                                        false );
                                refreshEntitlementAsync();
                                return;
                            }

                            std::string message = !resp.error.IsEmpty()
                                                          ? std::string( resp.error.utf8_str() )
                                                          : "The code couldn't be applied (HTTP "
                                                                    + std::to_string( resp.status ) + ").";

                            if( body.is_object() && body.contains( "message" ) && body["message"].is_string() )
                                message = body["message"].get<std::string>();

                            answer( message, true );
                        } );
            } )
            .detach();

    return true;
}


void INDUCTIVE_PANEL::handleBillingPortal()
{
    const auto cfg = INDUCTIVE_CLOUD_AUTH::LoadConfig();

    if( !cfg.configured() || !INDUCTIVE_CLOUD_AUTH::HasSession() )
    {
        emitEvent( nlohmann::json{ { "type", "billing" },
                                   { "ok", false },
                                   { "action", "portal" },
                                   { "message", "Sign in to manage your subscription" } }
                           .dump() );
        return;
    }

    emitEvent( nlohmann::json{ { "type", "billing" },
                               { "ok", true },
                               { "action", "portal_pending" } }
                       .dump() );

    auto state = m_state;
    const wxString base = INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized( cfg.apiBase );

    std::thread(
            [this, state, base]()
            {
                nlohmann::json req{ { "returnUrl",
                                      std::string( PlansPageUrl( INDUCTIVE_CLOUD_AUTH::LoadConfig(),
                                                                 "", "" ).utf8_str() ) } };

                const auto resp = INDUCTIVE_CLOUD_AUTH::ApiRequest(
                        wxS( "POST" ), wxS( "/v1/billing/portal" ), req.dump() );

                if( !state || !state->alive.load() )
                    return;

                CallAfter(
                        [this, state, resp]()
                        {
                            if( !state || !state->alive.load() )
                                return;

                            if( resp.signedOut )
                            {
                                emitAuthStatus();
                                emitEvent( nlohmann::json{
                                        { "type", "billing" },
                                        { "ok", false },
                                        { "action", "portal" },
                                        { "message", "Sign in to manage your subscription" } }
                                                   .dump() );
                                return;
                            }

                            if( !resp.error.IsEmpty() || resp.status < 200 || resp.status >= 300 )
                            {
                                std::string message = resp.error.IsEmpty()
                                                              ? ( "Portal failed (HTTP "
                                                                  + std::to_string( resp.status )
                                                                  + ")" )
                                                              : std::string( resp.error.utf8_str() );
                                const auto body =
                                        nlohmann::json::parse( resp.body, nullptr, false );

                                if( body.is_object() && body.contains( "message" ) )
                                    message = body.value( "message", message );

                                // A plan from a promotion code has no subscription to
                                // manage: the plans page shows the code and when it ends.
                                if( resp.status == 409 && body.is_object()
                                    && body.value( "error", std::string() ) == "no_subscription" )
                                {
                                    openPlansPage( "", "" );
                                    return;
                                }

                                if( message.find( "Compact JWS" ) != std::string::npos
                                    || ( body.is_object()
                                         && body.value( "error", std::string() )
                                                    == "invalid_token" ) )
                                {
                                    message = "Session invalid — Sign out, then Sign in again";
                                }

                                emitEvent( nlohmann::json{ { "type", "billing" },
                                                           { "ok", false },
                                                           { "action", "portal" },
                                                           { "message", message } }
                                                   .dump() );
                                return;
                            }

                            const auto body = nlohmann::json::parse( resp.body, nullptr, false );
                            const std::string url =
                                    body.is_object() ? body.value( "url", std::string() )
                                                     : std::string();

                            if( url.empty() || url.rfind( "https://", 0 ) != 0 )
                            {
                                emitEvent( nlohmann::json{
                                        { "type", "billing" },
                                        { "ok", false },
                                        { "action", "portal" },
                                        { "message", "Portal returned no URL" } }
                                                   .dump() );
                                return;
                            }

                            wxLaunchDefaultBrowser( wxString::FromUTF8( url ) );
                            emitEvent( nlohmann::json{ { "type", "billing" },
                                                       { "ok", true },
                                                       { "action", "portal" } }
                                               .dump() );
                            // Portal may change plan; refresh after a short delay once the
                            // user returns. Immediate refresh is still useful.
                            refreshEntitlementAsync();
                        } );
            } )
            .detach();
}


void INDUCTIVE_PANEL::onPageLoaded( wxWebViewEvent& aEvent )
{
    // The page cannot know what the host editor can do, so it is told on load.
    nlohmann::json commands = nlohmann::json::array();

    for( const INDUCTIVE_COMMAND& cmd : m_commands )
    {
        commands.push_back( { { "name", std::string( cmd.name.utf8_str() ) },
                              { "hint", std::string( cmd.hint.utf8_str() ) } } );
    }

    // Both editors: an account action, not a document one (redeemPromo).
    commands.push_back( { { "name", "redeem" }, { "hint", "Apply a promotion code: /redeem CODE" } } );

    emitEvent( nlohmann::json{ { "type", "ready" }, { "commands", commands }, { "editor", editorKind() }, { "project", currentProject().ToStdString() } }.dump() );

    // The stored session is read on a worker. The Keychain is allowed to ask the
    // user first -- a rebuilt binary is a new identity, so the next launch after an
    // install puts up an "allow access" dialog -- and the read blocks until that is
    // answered. Made on the UI thread, from this very handler, it once left the
    // whole sidebar dead behind a dialog nobody had been told to expect. Until it
    // lands the page shows signed-out, which is right for an unknown state and is
    // shown immediately.
    auto state = m_state;

    std::thread(
            [this, state]()
            {
                INDUCTIVE_CLOUD_AUTH::LoadSession();
                g_sessionSettled = true;

                // Crashes are sent as the signed-in user, so only now can they go.
                INDUCTIVE_REPORTS::SendCrashes();

                wxAppConsole* app = wxTheApp;

                if( !app || !state->alive.load() )
                    return;

                app->CallAfter(
                        [this, state]()
                        {
                            if( !state->alive.load() )
                                return;

                            emitAuthStatus();

    // The default mode and thinking level from Customize.
    emitSettings();
                            refreshEntitlementAsync();
                        } );
            } )
            .detach();

    emitAuthStatus();

    // A prompt in $INDUCTIVE_PROMPT is run once, as soon as the page is ready.
    //
    // The sidebar's input lives in a web view, which nothing outside the app can
    // type into -- so without this there is no way to exercise the whole path
    // (panel, runner, model, document) from a script, and every change has to be
    // verified by hand. It spends a model request on launch, so it is opt-in and
    // fires exactly once per panel.
    static bool promptFired = false;
    wxString    startupPrompt;

    if( !promptFired && wxGetEnv( wxS( "INDUCTIVE_PROMPT" ), &startupPrompt )
        && !startupPrompt.IsEmpty() )
    {
        promptFired = true;
        ask( startupPrompt, wxS( "Agent" ), nlohmann::json::array() );
    }
}


void INDUCTIVE_PANEL::beforeRequest()
{
    if( !m_beforeRequest )
        return;

    // Called from a wxWidgets event handler: nothing may escape it. Ending a tool can
    // pump events, so a second message arriving meanwhile finds the panel busy.
    const bool wasBusy = m_busy;
    m_busy = true;

    try
    {
        m_beforeRequest();
    }
    catch( ... )
    {
    }

    m_busy = wasBusy;
}


void INDUCTIVE_PANEL::runCommand( const wxString& aCommand )
{
    wxString result;

    if( aCommand.Lower().StartsWith( wxS( "redeem" ) ) && redeemPromo( wxS( "/" ) + aCommand ) )
        return;

    checkProject();
    beginRecord( wxS( "/" ) + aCommand.Lower(), 0 );

    if( !m_commandHandler )
    {
        emitEvent( nlohmann::json{ { "type", "error" },
                                   { "message", "This editor offers no board commands." } }
                           .dump() );
        return;
    }

    beforeRequest();

    // The handler runs on the UI thread: it touches the open document, and KiCad's
    // data model is not safe to reach from anywhere else.
    //
    // It is also allowed to refuse by throwing -- a command is gated exactly like
    // a tool, and "the editor is busy" is a refusal, not a failure. This runs
    // inside a wxWidgets event handler, so an exception that escapes here does not
    // become an error message, it becomes std::terminate and the editor exits with
    // the user's board unsaved. Every path out of the handler is caught.
    bool ok = false;
    bool known = true;

    try
    {
        ok = m_commandHandler( aCommand, result );

        if( ok && aCommand.CmpNoCase( wxS( "pcb" ) ) == 0 )
            notePublished( result );

        // The editor answers the commands that are about the open document.
        // Anything else is a question for the designer, which knows about the
        // circuit rather than the canvas -- /bom, /netlist, /parts and friends.
        known = ok || !result.IsEmpty();
    }
    catch( const KI_PARAM_ERROR& e )   // does not derive from std::exception
    {
        result = e.What();
    }
    catch( const std::exception& e )
    {
        result = wxString::FromUTF8( e.what() );

        if( INDUCTIVE_REPORTS::Unexpected( &e ) )
            INDUCTIVE_REPORTS::Error( reportApp(), "command /" + std::string( aCommand.utf8_str() ),
                                      e.what() );
    }
    catch( ... )
    {
        result = _( "The command failed for an unknown reason." );
        INDUCTIVE_REPORTS::Error( reportApp(), "command /" + std::string( aCommand.utf8_str() ),
                                  "unknown exception" );
    }

    if( !known && usesPipeline() )
    {
        ask( wxS( "/" ) + aCommand, m_mode.IsEmpty() ? wxS( "Agent" ) : m_mode,
             nlohmann::json::array() );
        return;
    }

    emitEvent( nlohmann::json{
            { "type", ok ? "trace" : "error" },
            { "verb", ok ? std::string( ( aCommand.Left( 1 ).Upper() + aCommand.Mid( 1 ).Lower() ).utf8_str() )
                         : std::string( "Failed" ) },
            { "object", std::string( result.utf8_str() ) },
            { "message", std::string( result.utf8_str() ) },
            { "fail", !ok } }
                       .dump() );

    // The result is the answer: "Done" alone hid /layout's summary, /drc's counts and
    // where /jlcpcb wrote the files. (A failure already shows its message.)
    if( ok )
        emitEvent( nlohmann::json{ { "type", "done" }, { "text", std::string( result.utf8_str() ) } }.dump() );
}


void INDUCTIVE_PANEL::stopTurn()
{
    if( !m_busy )
        return;

    // The in-flight request is left to finish and be dropped: libcurl is blocking
    // in a detached worker, and tearing that down mid-flight buys less than it costs.
    ++m_state->turnId;
    m_busy = false;

    // A pipeline turn is a child process, so Stop can actually stop the work --
    // including a compile that would otherwise run for another minute. SIGTERM,
    // not SIGKILL: the runner aborts its own turn cleanly on it.
    if( m_pipeline && m_pipelinePid )
        wxProcess::Kill( m_pipelinePid, wxSIGTERM, wxKILL_CHILDREN );

    // The turn may have added a prompt, a model turn and tool results; drop all
    // of them, or the next request replays a functionCall nothing answered.
    if( m_turnStart <= m_history.size() )
        m_history.resize( m_turnStart );

    // The turn that held them has just been truncated away.
    m_imageTurn = 0;

    emitEvent( nlohmann::json{ { "type", "cancelled" } }.dump() );
}


/**
 * Whether a mode may call tools that change the open document.
 *
 * This is the enforcement point for the mode selector, and it is deliberately a
 * predicate over the mode rather than a sentence in the prompt.  The model reads
 * board content -- reference designators, net names, silkscreen text -- that a
 * third party may have authored, and text in a context window has repeatedly
 * proven able to talk a model out of an instruction.  It cannot talk a model
 * into a function declaration that was never sent.
 */
static bool modeAllowsMutation( const wxString& aMode )
{
    return aMode.CmpNoCase( wxS( "ask" ) ) != 0 && aMode.CmpNoCase( wxS( "plan" ) ) != 0;
}




/**
 * A random tag used to fence untrusted document content off from instructions.
 *
 * Generated once per panel from the system entropy source.  It has to be
 * unguessable rather than merely unique: the whole mechanism is that content
 * inside the fence cannot close it, and a fixed or predictable delimiter can be
 * closed by anything that knows what it is.
 */
static std::string contentNonce()
{
    static const std::string nonce = []
    {
        std::random_device        rd;
        std::string               out;
        const char*               digits = "0123456789abcdef";

        for( int i = 0; i < 24; ++i )
            out += digits[rd() & 0xF];

        return out;
    }();

    return nonce;
}


/**
 * How many rounds of tool calls one turn may take before we stop feeding it back.
 *
 * Six was enough for a chatbot that could set a field.  Real board work is a
 * loop -- look at the board, place a part, check the clearance, move it, run DRC,
 * fix what it reported -- and each pass is a round.  A model can request several
 * calls in one round, so this is a bound on *conversation* turns, not on edits.
 */
static const int MAX_TOOL_ROUNDS = 32;

/**
 * How many document-changing calls one turn may make.
 *
 * The bound that matters is on damage, not on rounds: MAX_TOOL_ROUNDS would let a
 * model that has decided to delete things delete a great many of them.  Every
 * change is a separate undo step, so recovery is possible either way -- but only
 * if the user notices, and a hundred silent edits is not something anyone
 * reviews.  Sixty is comfortably more than a real request needs (placing a
 * twenty-part board is twenty calls) and far less than a runaway loop produces.
 */
static const int MAX_TURN_MUTATIONS = 60;

/**
 * How many tool calls one round may actually run.
 *
 * MAX_TOOL_ROUNDS bounds conversation turns, not work: the model may put any number
 * of functionCall parts in a single reply, so without this one round could carry
 * hundreds of edits and neither bound would notice.  Twelve is comfortably more
 * than a model needs to inspect a board and act on it, and small enough that the
 * user watching the trace can still read what happened.
 */
static const int MAX_CALLS_PER_ROUND = 12;

/**
 * Ceiling on one tool result handed back to the model, in bytes.
 *
 * An inspection tool on a large board can answer with tens of kilobytes.  That is
 * legitimate -- the model asked for the listing -- but every later request in the
 * turn replays the whole history, so an unbounded result is paid for again on
 * each round and eventually crowds out the conversation itself.
 */
static const size_t MAX_TOOL_RESULT_BYTES = 16 * 1024;




/**
 * A one-line version of a tool result, for the trace row in the sidebar.
 *
 * The row is a single line in a narrow docked panel.  A listing's first line is
 * written to be its own summary ("17 footprints"), so taking it is enough.
 */
static std::string traceSummary( const wxString& aResult )
{
    wxString line = aResult.BeforeFirst( '\n' );
    line.Trim( true ).Trim( false );

    if( line.IsEmpty() )
        line = aResult.Strip( wxString::both );

    if( line.length() > 160 )
        line = line.Left( 157 ) + wxS( "..." );

    return std::string( line.utf8_str() );
}


/**
 * Turn the page's attachments into inlineData parts.
 *
 * The sidebar reads each file with FileReader and posts a `data:` URL, so what
 * arrives is already base64 and already declares its own type.  Neither of those
 * may be trusted: the string reaches an HTTPS request body, so it is parsed
 * strictly rather than forwarded.
 *
 * Bounded on three axes -- how many, how large each, and how large in total --
 * because this is the one input that can be megabytes.  The caps are well under
 * the API's own request ceiling and are what stops a mis-drag of a hundred
 * screenshots from becoming a very slow, very expensive request.
 */
static nlohmann::json imagePartsFrom( const nlohmann::json& aAttachments, int& aAccepted,
                                      int& aRejected )
{
    // Deliberately a fixed allowlist rather than "anything image/*": the value
    // is echoed into the request as the declared mime type.
    static const std::vector<std::string> ALLOWED = { "image/png", "image/jpeg", "image/gif",
                                                      "image/webp" };

    const size_t MAX_ONE   = 4 * 1024 * 1024;    // base64 characters
    const size_t MAX_TOTAL = 8 * 1024 * 1024;
    const size_t MAX_COUNT = 6;

    nlohmann::json parts = nlohmann::json::array();
    size_t         total = 0;

    aAccepted = 0;
    aRejected = 0;

    if( !aAttachments.is_array() )
        return parts;

    for( const auto& attachment : aAttachments )
    {
        if( parts.size() >= MAX_COUNT )
        {
            ++aRejected;
            continue;
        }

        if( !attachment.is_object() )
        {
            ++aRejected;
            continue;
        }

        const std::string url = attachment.value( "url", std::string() );

        // Exactly "data:<mime>;base64,<payload>" and nothing else. A file:// or
        // http:// URL here would be a request to go and fetch something, which
        // is not a thing this panel does.
        if( url.rfind( "data:", 0 ) != 0 )
        {
            ++aRejected;
            continue;
        }

        const size_t marker = url.find( ";base64," );

        if( marker == std::string::npos )
        {
            ++aRejected;
            continue;
        }

        const std::string mime = url.substr( 5, marker - 5 );

        if( std::find( ALLOWED.begin(), ALLOWED.end(), mime ) == ALLOWED.end() )
        {
            ++aRejected;
            continue;
        }

        const std::string data = url.substr( marker + 8 );

        if( data.empty() || data.size() > MAX_ONE || total + data.size() > MAX_TOTAL )
        {
            ++aRejected;
            continue;
        }

        // Base64 only. Anything else is either a corrupt read or an attempt to
        // put something structural into the request body.
        if( data.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                    "0123456789+/=" )
            != std::string::npos )
        {
            ++aRejected;
            continue;
        }

        total += data.size();
        parts.push_back( { { "inlineData", { { "mimeType", mime }, { "data", data } } } } );
        ++aAccepted;
    }

    return parts;
}


// ───────────────────────── chat history ─────────────────────────
//
// Every conversation is kept on this machine, one folder per KiCad project, and
// never sent anywhere. A chat file holds what the page showed -- each turn's prompt
// and the events that drew its reply, so reopening it draws exactly the same thing --
// and the model's own history, so the conversation can carry on where it stopped.
//
//   <data dir>/chats/<project name>-<hash of its path>/<id>.json        the chat
//   <data dir>/chats/<project name>-<hash of its path>/<id>.meta.json   title, time
//
// The small meta file is what the History menu reads, so listing fifty chats does
// not parse fifty model transcripts. Board and schematic editors keep separate
// lists: a chat's history names the tools of the editor it was held in.

/// Events that draw a reply. Everything else the page receives is not a turn.
static bool IsTurnEvent( const std::string& aType )
{
    return aType == "text" || aType == "text-reset" || aType == "trace" || aType == "plan"
           || aType == "generated" || aType == "circuit_edited" || aType == "error" || aType == "cancelled" || aType == "done";
}


static long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch() )
            .count();
}


/// Where chats live: per user, outside every project, so none of it ends up in git.
static wxString ChatsRoot()
{
    wxString root;

#if defined( __WXMAC__ )
    root = wxFileName::GetHomeDir() + wxS( "/Library/Application Support/Inductive" );
#elif defined( __WXMSW__ )
    root = wxStandardPaths::Get().GetUserConfigDir() + wxS( "\\Inductive" );
#else
    wxString xdg;
    root = wxGetEnv( wxS( "XDG_DATA_HOME" ), &xdg ) && !xdg.IsEmpty()
                   ? xdg + wxS( "/inductive" )
                   : wxFileName::GetHomeDir() + wxS( "/.local/share/inductive" );
#endif

    return root + wxFileName::GetPathSeparator() + wxS( "chats" );
}


/// A folder name for a project: readable, and unique to its path.
static wxString ProjectFolder( const wxString& aProject )
{
    if( aProject.IsEmpty() )
        return wxS( "no-project" );

    // FNV-1a: stable across runs and builds, unlike std::hash.
    uint64_t          hash = 1469598103934665603ULL;
    const std::string path = std::string( aProject.utf8_str() );

    for( unsigned char c : path )
    {
        hash ^= c;
        hash *= 1099511628211ULL;
    }

    wxString name;

    for( wxUniChar c : wxFileName( aProject ).GetName() )
    {
        if( name.length() >= 40 )
            break;

        name += ( wxIsalnum( c ) || c == '-' || c == '_' ) ? c : wxUniChar( '_' );
    }

    return name + wxString::Format( wxS( "-%016llx" ), (unsigned long long) hash );
}


static wxString ChatDir( const wxString& aProject )
{
    return ChatsRoot() + wxFileName::GetPathSeparator() + ProjectFolder( aProject );
}


/// Chat ids are generated here; anything else is refused before it touches a path.
static bool ValidChatId( const std::string& aId )
{
    if( aId.empty() || aId.size() > 64 )
        return false;

    for( char c : aId )
    {
        if( !std::isalnum( (unsigned char) c ) && c != '-' )
            return false;
    }

    return true;
}


/// Write a file only its owner can read, all at once: a crash mid-write must not
/// leave a half chat that can no longer be opened.
static bool WritePrivate( const wxString& aPath, const std::string& aText )
{
    const wxString tmp = aPath + wxS( ".tmp" );

    {
        wxFile file;

        if( !file.Create( tmp, true, wxS_IRUSR | wxS_IWUSR ) )
            return false;

        if( !file.Write( aText.data(), aText.size() ) )
            return false;
    }

    return wxRenameFile( tmp, aPath, true );
}


static nlohmann::json ReadJson( const wxString& aPath )
{
    wxFile file;

    if( !wxFileName::FileExists( aPath ) || !file.Open( aPath ) )
        return nlohmann::json();

    std::string text( (size_t) file.Length(), '\0' );

    if( file.Read( text.data(), text.size() ) != (ssize_t) text.size() )
        return nlohmann::json();

    return nlohmann::json::parse( text, nullptr, false );
}


/// Customize's settings: beside the chats, per user, read on every use so that a
/// change made in one editor holds in the other.
static wxString SettingsPath()
{
    return wxFileName( ChatsRoot() ).GetPath() + wxFileName::GetPathSeparator()
           + wxS( "settings.json" );
}


/// The longest standing instruction kept, in characters: enough for a page of
/// rules, and a bound on what every request carries.
static const size_t MAX_INSTRUCTIONS = 4000;


static std::string BoundedInstructions( const std::string& aText )
{
    wxString text = wxString::FromUTF8( aText );

    if( text.length() > MAX_INSTRUCTIONS )
        text = text.Left( MAX_INSTRUCTIONS );

    return std::string( text.utf8_str() );
}


static bool IsModeName( const std::string& aMode )
{
    return aMode == "Agent" || aMode == "Ask" || aMode == "Plan";
}


/// The settings, every field present and in range whatever the file holds.
static nlohmann::json LoadSettings()
{
    const nlohmann::json file = ReadJson( SettingsPath() );
    nlohmann::json       out = { { "instructions", "" },
                                 { "mode", "Agent" },
                                 { "level", INDUCTIVE_PANEL::DEFAULT_INTELLIGENCE } };

    if( !file.is_object() )
        return out;

    if( const auto it = file.find( "instructions" ); it != file.end() && it->is_string() )
        out["instructions"] = BoundedInstructions( it->get<std::string>() );

    if( const auto it = file.find( "mode" ); it != file.end() && it->is_string()
        && IsModeName( it->get<std::string>() ) )
        out["mode"] = it->get<std::string>();

    if( const int level = intelligenceLevelFrom( file ) )
        out["level"] = level;

    // The navigation pane's own state, kept here so it survives a restart.
    out["navCollapsed"] = file.contains( "navCollapsed" ) && file["navCollapsed"].is_boolean()
                                  && file["navCollapsed"].get<bool>();

    // Crash and error reports (inductive_reports.h): only ever set by hand, and kept
    // through every save from Customize.
    if( const auto it = file.find( "send_reports" ); it != file.end() && it->is_boolean() )
        out["send_reports"] = it->get<bool>();

    return out;
}


static bool SaveSettings( const nlohmann::json& aSettings )
{
    const wxString dir = wxFileName( SettingsPath() ).GetPath();

    if( !wxFileName::DirExists( dir ) && !wxFileName::Mkdir( dir, 0700, wxPATH_MKDIR_FULL ) )
        return false;

    return WritePrivate( SettingsPath(),
                         aSettings.dump( 2, ' ', false, nlohmann::json::error_handler_t::replace ) );
}


wxString INDUCTIVE_PANEL::currentProject() const
{
    const auto* holder = dynamic_cast<const KIWAY_HOLDER*>( wxGetTopLevelParent(
            const_cast<INDUCTIVE_PANEL*>( this ) ) );

    if( !holder || !holder->HasKiway() )
        return wxEmptyString;

    PROJECT& project = holder->Prj();

    return project.IsNullProject() ? wxString() : project.GetProjectFullName();
}


/// The editor, as a crash or error report names it.
std::string INDUCTIVE_PANEL::reportApp() const
{
    return "Inductive " + editorKind();
}


std::string INDUCTIVE_PANEL::editorKind() const
{
    const wxWindow* top = wxGetTopLevelParent( const_cast<INDUCTIVE_PANEL*>( this ) );
    return top && top->GetName() == wxS( "PcbFrame" ) ? "pcb" : "sch";
}


void INDUCTIVE_PANEL::startNewChat()
{
    m_attachments.clear();
    m_turnDatasheets.clear();
    m_chatId.clear();
    m_chatTitle.clear();
    m_chatCreated = 0;
    m_chatTurns = nlohmann::json::array();
    m_recording = false;
    m_chatProject = currentProject();
}


void INDUCTIVE_PANEL::beginRecord( const wxString& aPrompt, int aImages )
{
    requestBoundary( true );
    m_recording = true;
    m_turnRecord = { { "prompt", std::string( aPrompt.utf8_str() ) },
                     { "images", aImages },
                     { "started", NowMs() },
                     { "events", nlohmann::json::array() } };
}


void INDUCTIVE_PANEL::requestBoundary( bool aBegin )
{
    // Runs inside wxWidgets event handlers: an escaping exception would end the
    // editor, and an undo history left unfolded is only an inconvenience.
    if( !m_requestBoundary )
        return;

    try
    {
        m_requestBoundary( aBegin );
    }
    catch( ... )
    {
    }
}


void INDUCTIVE_PANEL::recordEvent( const std::string& aJson )
{
    if( !m_recording )
        return;

    nlohmann::json ev = nlohmann::json::parse( aJson, nullptr, false );

    if( !ev.is_object() )
        return;

    const std::string type = ev.value( "type", std::string() );

    if( !IsTurnEvent( type ) )
        return;

    nlohmann::json& events = m_turnRecord["events"];

    // Streamed text arrives a few words at a time; one event per run is enough.
    if( type == "text" && !events.empty() && events.back().value( "type", std::string() ) == "text" )
    {
        events.back()["delta"] = events.back().value( "delta", std::string() )
                                 + ev.value( "delta", std::string() );
    }
    else
    {
        events.push_back( std::move( ev ) );
    }

    if( type != "done" && type != "error" && type != "cancelled" )
        return;

    // The turn is over.
    requestBoundary( false );
    const long long started = m_turnRecord.value( "started", NowMs() );
    m_turnRecord["seconds"] = std::max( 1LL, ( NowMs() - started + 500 ) / 1000 );
    m_turnRecord.erase( "started" );
    m_chatTurns.push_back( std::move( m_turnRecord ) );
    m_turnRecord = nlohmann::json();
    m_recording = false;

    // The turn may have used the last of the month's allowance: re-read it, so the
    // counter moves and the composer locks without waiting for the next launch.
    refreshEntitlementAsync();

    // A conversation is worth keeping once something in it worked: a run of
    // "sign in first" errors is not a chat anyone wants in their history.
    if( !m_chatId.empty() || type == "done" )
    {
        saveChat();
        pushNavHistory();
    }
}


void INDUCTIVE_PANEL::saveChat()
{
    if( m_chatTurns.empty() )
        return;

    if( m_chatId.empty() )
    {
        // Sortable by time, unique by the random tail.
        std::random_device rd;
        const long long    now = NowMs();
        const time_t       secs = (time_t) ( now / 1000 );
        char               stamp[32];
        std::strftime( stamp, sizeof( stamp ), "%Y%m%d-%H%M%S", std::localtime( &secs ) );
        m_chatId = wxString::Format( wxS( "%s-%06x" ), stamp, rd() & 0xffffff ).ToStdString();
        m_chatCreated = now;

        // Titled by what it was about: the first prompt, on one line.
        wxString title = wxString::FromUTF8( m_chatTurns[0].value( "prompt", std::string() ) );
        title.Replace( wxS( "\n" ), wxS( " " ) );
        title.Trim( true ).Trim( false );

        if( title.length() > 80 )
            title = title.Left( 79 ) + wxS( "…" );

        m_chatTitle = std::string( ( title.IsEmpty() ? wxString( wxS( "Untitled chat" ) ) : title ).utf8_str() );
    }

    // The model's history, without image data: that is megabytes, and the replay
    // shows the images were there without needing them.
    nlohmann::json history = nlohmann::json::array();

    for( const std::string& entry : m_history )
    {
        nlohmann::json e = nlohmann::json::parse( entry, nullptr, false );

        if( !e.is_object() )
            continue;

        if( e.contains( "parts" ) && e["parts"].is_array() )
        {
            for( nlohmann::json& part : e["parts"] )
            {
                if( part.is_object() && part.contains( "inlineData" ) )
                    part = { { "text", "[image]" } };
            }
        }

        history.push_back( std::move( e ) );
    }

    const long long now = NowMs();
    const wxString  dir = ChatDir( m_chatProject );

    if( !wxFileName::DirExists( dir ) && !wxFileName::Mkdir( dir, 0700, wxPATH_MKDIR_FULL ) )
        return;

    const nlohmann::json meta = { { "id", m_chatId },
                                  { "title", m_chatTitle },
                                  { "editor", editorKind() },
                                  { "created", m_chatCreated },
                                  { "updated", now },
                                  { "turns", m_chatTurns.size() } };

    nlohmann::json chat = meta;
    chat["version"] = 1;
    chat["project"] = std::string( m_chatProject.utf8_str() );
    chat["chat"] = m_chatTurns;
    chat["history"] = std::move( history );

    const wxString base = dir + wxFileName::GetPathSeparator() + wxString::FromUTF8( m_chatId );
    const auto     dump = []( const nlohmann::json& j )
    {
        // Text from the board or the model is not guaranteed to be valid UTF-8.
        return j.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace );
    };

    if( WritePrivate( base + wxS( ".json" ), dump( chat ) ) )
        WritePrivate( base + wxS( ".meta.json" ), dump( meta ) );

    // Keep the newest hundred chats per project and editor.
    wxArrayString metas;
    wxDir::GetAllFiles( dir, &metas, wxS( "*.meta.json" ), wxDIR_FILES );

    if( metas.size() > 120 )
    {
        std::vector<std::pair<long long, wxString>> byAge;

        for( const wxString& path : metas )
        {
            const nlohmann::json m = ReadJson( path );

            if( m.is_object() && m.value( "editor", std::string() ) == editorKind() )
                byAge.emplace_back( m.value( "updated", 0LL ), path );
        }

        std::sort( byAge.begin(), byAge.end() );

        for( size_t i = 0; i + 100 < byAge.size(); ++i )
        {
            wxString chatPath = byAge[i].second;
            chatPath.Replace( wxS( ".meta.json" ), wxS( ".json" ) );
            wxRemoveFile( chatPath );
            wxRemoveFile( byAge[i].second );
        }
    }
}


void INDUCTIVE_PANEL::sendHistoryList()
{
    checkProject();

    const wxString project = currentProject();
    const wxString dir = ChatDir( project );

    std::vector<nlohmann::json> items;
    wxArrayString               metas;

    if( wxFileName::DirExists( dir ) )
        wxDir::GetAllFiles( dir, &metas, wxS( "*.meta.json" ), wxDIR_FILES );

    for( const wxString& path : metas )
    {
        nlohmann::json m = ReadJson( path );

        if( !m.is_object() || m.value( "editor", std::string() ) != editorKind() )
            continue;

        if( !ValidChatId( m.value( "id", std::string() ) ) )
            continue;

        m["current"] = m.value( "id", std::string() ) == m_chatId;
        items.push_back( std::move( m ) );
    }

    std::sort( items.begin(), items.end(),
               []( const nlohmann::json& a, const nlohmann::json& b )
               {
                   return a.value( "updated", 0LL ) > b.value( "updated", 0LL );
               } );

    if( items.size() > 50 )
        items.resize( 50 );

    emitEvent( nlohmann::json{ { "type", "history" },
                               { "project", project.IsEmpty()
                                                    ? std::string( "No project" )
                                                    : std::string( wxFileName( project ).GetName().utf8_str() ) },
                               { "items", items } }
                       .dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
}


void INDUCTIVE_PANEL::searchHistory( const std::string& aQuery )
{
    const wxString query = wxString::FromUTF8( aQuery ).Left( 200 ).Trim( true ).Trim( false ).Lower();

    checkProject();

    const wxString project = currentProject();
    const wxString dir = ChatDir( project );

    std::vector<nlohmann::json> items;
    wxArrayString               metas;

    if( !query.IsEmpty() && wxFileName::DirExists( dir ) )
        wxDir::GetAllFiles( dir, &metas, wxS( "*.meta.json" ), wxDIR_FILES );

    auto contains = [&query]( const std::string& aText )
    {
        return wxString::FromUTF8( aText ).Lower().Contains( query );
    };

    for( const wxString& path : metas )
    {
        nlohmann::json m = ReadJson( path );

        if( !m.is_object() || m.value( "editor", std::string() ) != editorKind()
            || !ValidChatId( m.value( "id", std::string() ) ) )
        {
            continue;
        }

        // The title first: it is in hand. Then what was said, both ways.
        bool hit = contains( m.value( "title", std::string() ) );

        if( !hit )
        {
            wxString chatPath = path;
            chatPath.Replace( wxS( ".meta.json" ), wxS( ".json" ) );
            const nlohmann::json chat = ReadJson( chatPath );

            if( chat.is_object() && chat.contains( "chat" ) && chat["chat"].is_array() )
            {
                for( const nlohmann::json& turn : chat["chat"] )
                {
                    if( !turn.is_object() )
                        continue;

                    if( contains( turn.value( "prompt", std::string() ) ) )
                        hit = true;

                    if( !hit && turn.contains( "events" ) && turn["events"].is_array() )
                    {
                        for( const nlohmann::json& ev : turn["events"] )
                        {
                            if( ev.is_object() && ev.value( "type", std::string() ) == "text"
                                && contains( ev.value( "delta", std::string() ) ) )
                            {
                                hit = true;
                                break;
                            }
                        }
                    }

                    if( hit )
                        break;
                }
            }
        }

        if( hit )
        {
            m["current"] = m.value( "id", std::string() ) == m_chatId;
            items.push_back( std::move( m ) );
        }
    }

    std::sort( items.begin(), items.end(),
               []( const nlohmann::json& a, const nlohmann::json& b )
               {
                   return a.value( "updated", 0LL ) > b.value( "updated", 0LL );
               } );

    if( items.size() > 50 )
        items.resize( 50 );

    emitEvent( nlohmann::json{ { "type", "history" },
                               { "query", aQuery },
                               { "items", items } }
                       .dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
}


void INDUCTIVE_PANEL::pushNavHistory()
{
    if( m_nav )
        sendHistoryList();
}


void INDUCTIVE_PANEL::emitSettings()
{
    nlohmann::json ev = LoadSettings();
    ev["type"] = "settings";
    emitEvent( ev.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
}


void INDUCTIVE_PANEL::SyncNav()
{
    if( !m_nav )
        return;

    // The account as last reported; or, before any report, as known right now.
    if( !m_lastAuthEvent.empty() )
        m_nav->Deliver( m_lastAuthEvent );
    else
        emitAuthStatus();

    emitSettings();
    sendHistoryList();
}


void INDUCTIVE_PANEL::openChat( const std::string& aId )
{
    auto refuse = [&]( const std::string& aWhy )
    {
        emitEvent( nlohmann::json{ { "type", "history_error" }, { "message", aWhy } }.dump() );
    };

    if( m_busy )
        return refuse( "Wait for the current answer, or stop it, before opening another chat." );

    if( !ValidChatId( aId ) )
        return refuse( "That chat could not be found." );

    checkProject();

    const nlohmann::json chat = ReadJson( ChatDir( currentProject() )
                                          + wxFileName::GetPathSeparator()
                                          + wxString::FromUTF8( aId ) + wxS( ".json" ) );

    if( !chat.is_object() || !chat.contains( "chat" ) || !chat["chat"].is_array() )
        return refuse( "That chat could not be opened; its file is missing or damaged." );

    if( chat.value( "editor", std::string() ) != editorKind() )
        return refuse( "That chat belongs to the other editor." );

    m_history.clear();

    if( chat.contains( "history" ) && chat["history"].is_array() )
    {
        for( const nlohmann::json& entry : chat["history"] )
        {
            if( entry.is_object() )
                m_history.push_back( entry.dump() );
        }
    }

    m_attachments.clear();
    m_chatId      = aId;
    m_chatTitle   = chat.value( "title", std::string() );
    m_chatCreated = chat.value( "created", NowMs() );
    m_chatTurns   = chat["chat"];
    m_chatProject = currentProject();
    m_recording   = false;
    m_turnStart   = m_history.size();
    m_imageTurn   = 0;
    m_toolRounds  = 0;

    emitEvent( nlohmann::json{ { "type", "restore" },
                               { "id", m_chatId },
                               { "title", m_chatTitle },
                               { "turns", m_chatTurns } }
                       .dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
    pushNavHistory();
}


void INDUCTIVE_PANEL::deleteChat( const std::string& aId )
{
    if( !ValidChatId( aId ) )
        return;

    const wxString base = ChatDir( currentProject() ) + wxFileName::GetPathSeparator()
                          + wxString::FromUTF8( aId );

    wxRemoveFile( base + wxS( ".json" ) );
    wxRemoveFile( base + wxS( ".meta.json" ) );

    // Deleting the chat on screen clears the screen: it is no longer anywhere.
    if( aId == m_chatId && !m_busy )
    {
        startNewChat();
        m_history.clear();
        m_turnStart = 0;
        m_imageTurn = 0;
        emitEvent( nlohmann::json{ { "type", "cleared" } }.dump() );
    }

    sendHistoryList();
}


void INDUCTIVE_PANEL::checkProject()
{
    const wxString project = currentProject();

    if( !m_projectKnown )
    {
        m_projectKnown = true;
        m_watchedProject = project;
        m_chatProject = project;
        return;
    }

    if( project == m_watchedProject || m_busy )
        return;

    // Another project was opened in this editor. Its conversation is not this one:
    // the old chat is already saved under its own project; start afresh.
    m_watchedProject = project;
    startNewChat();
    m_history.clear();
    m_turnStart = 0;
    m_imageTurn = 0;
    emitEvent( nlohmann::json{ { "type", "cleared" } }.dump() );
    pushNavHistory();
}


void INDUCTIVE_PANEL::ask( const wxString& aPrompt, const wxString& aMode,
                           const nlohmann::json& aAttachments )
{
    if( m_busy || aPrompt.IsEmpty() )
        return;

    // Everything from here on is a turn the page has drawn, so it is history --
    // including a refusal, which ends the turn with an error event.
    checkProject();
    beginRecord( aPrompt, (int) aAttachments.size() );
    if( !aAttachments.is_array() || aAttachments.size() > 6 )
    {
        emitEvent( nlohmann::json{ { "type", "error" },
                   { "message", "Attach at most six files per message." } }.dump() );
        return;
    }

    // Model requests go through Inductive's backend, which holds the model credential,
    // so a turn needs a configured server and a signed-in account -- and nothing
    // on this machine.
    if( !INDUCTIVE_CLOUD_AUTH::LoadConfig().configured() )
    {
        emitEvent( nlohmann::json{
                { "type", "error" },
                { "message", "Inductive isn't connected to its server. Set INDUCTIVE_API_BASE "
                             "and the sign-in settings in ~/.config/inductive/env." } }
                           .dump() );
        return;
    }

    if( !INDUCTIVE_CLOUD_AUTH::HasSession() )
    {
        emitEvent( nlohmann::json{
                { "type", "error" },
                { "message", "Sign in to use Inductive: open the account menu and choose "
                             "Sign in." } }
                           .dump() );
        return;
    }

    // A tool left selected or a half-drawn item is not a reason to refuse: the user
    // sending this means they are done with it, so the editor goes back to its
    // selection arrow first (see SetBeforeRequest). A dialog is still theirs to close.
    if( m_beforeRequest && modeAllowsMutation( aMode ) )
        beforeRequest();

    // Refuse now rather than after the turn is paid for.
    //
    // The escape hatch is deliberate: this check decides whether *any* turn may
    // start, and if its idea of "busy" is ever wrong the product is dead rather
    // than degraded. INDUCTIVE_SKIP_READY_CHECK=1 turns it off without a rebuild.
    if( m_readyCheck && modeAllowsMutation( aMode ) && !wxGetEnv( wxS( "INDUCTIVE_SKIP_READY_CHECK" ), nullptr ) )
    {
        wxString why;

        if( !m_readyCheck( why ) )
        {
            emitEvent( nlohmann::json{ { "type", "error" },
                                       { "message", std::string( why.utf8_str() ) } }
                               .dump() );
            return;
        }
    }

    // Vendor part downloads are opened by the server, never on this machine;
    // the turn starts once only their checked contents have come back.
    for( const auto& file : aAttachments )
    {
        if( INDUCTIVE_ATTACHMENTS::IsPartDownload( file ) )
        {
            openPartDownloads( aPrompt, aMode, aAttachments );
            return;
        }
    }

    beginTurn( aPrompt, aMode, aAttachments );
}


void INDUCTIVE_PANEL::openPartDownloads( const wxString& aPrompt, const wxString& aMode,
                                         const nlohmann::json& aAttachments )
{
    m_busy = true;
    for( const auto& file : aAttachments )
        if( INDUCTIVE_ATTACHMENTS::IsPartDownload( file ) )
            emitEvent( nlohmann::json{ { "type", "trace" }, { "verb", "Opening" },
                                       { "object", file.value( "name", std::string() ) + " on the server" },
                                       { "fail", false } }.dump() );
    emitEvent( nlohmann::json{ { "type", "status" }, { "state", "thinking" } }.dump() );

    auto           state = m_state;
    const unsigned turn  = m_state->turnId.load();

    std::thread(
            [this, state, turn, aPrompt, aMode, aAttachments]()
            {
                nlohmann::json resolved = nlohmann::json::array();
                std::string    error;
                for( const auto& file : aAttachments )
                {
                    if( !INDUCTIVE_ATTACHMENTS::IsPartDownload( file ) || !error.empty() )
                    {
                        resolved.push_back( file );
                        continue;
                    }
                    const std::string url  = file.value( "url", std::string() );
                    const std::string name = file.value( "name", std::string() );
                    const nlohmann::json body = { { "name", name }, { "data", url.substr( url.find( ',' ) + 1 ) } };
                    INDUCTIVE_CLOUD_AUTH::ApiResponse resp;
                    try
                    {
                        resp = INDUCTIVE_CLOUD_AUTH::ApiRequest( wxS( "POST" ), wxS( "/v1/parts/unpack" ), body.dump() );
                    }
                    catch( ... )
                    {
                        resp.status = 0;
                    }
                    const nlohmann::json reply = nlohmann::json::parse( resp.body, nullptr, false );
                    if( resp.status == 200 && reply.is_object() )
                    {
                        resolved.push_back( { { "name", name }, { "bundle", reply } } );
                        continue;
                    }
                    if( resp.signedOut )
                        error = "Sign in to open part downloads.";
                    else if( reply.is_object() && reply.contains( "message" ) && reply["message"].is_string() )
                        error = name + ": " + reply["message"].get<std::string>();
                    else
                        error = name + " could not be opened on the server. Try again shortly.";
                }

                wxAppConsole* app = wxTheApp;
                if( !app || !state->alive.load() )
                    return;
                app->CallAfter(
                        [this, state, turn, aPrompt, aMode, resolved, error]()
                        {
                            if( !state->alive.load() || turn != state->turnId.load() )
                                return;     // panel gone, or Stop was pressed
                            m_busy = false;
                            if( !error.empty() )
                            {
                                emitEvent( nlohmann::json{ { "type", "error" }, { "message", error } }.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
                                return;
                            }
                            beginTurn( aPrompt, aMode, resolved );
                        } );
            } )
            .detach();
}


void INDUCTIVE_PANEL::beginTurn( const wxString& aPrompt, const wxString& aMode,
                                 const nlohmann::json& aAttachments )
{
    m_turnDatasheets.clear();
    m_mode              = aMode;
    m_toolRounds        = 0;
    m_turnLevel         = m_intelligence;
    m_serverTurnId.clear();
    m_mutationsThisTurn = 0;
    m_busy              = true;
    m_turnStart         = m_history.size();

    nlohmann::json parts =
            nlohmann::json::array( { { { "text", std::string( aPrompt.utf8_str() ) } } } );

    int accepted = 0;
    int rejected = 0;

    nlohmann::json images = nlohmann::json::array();
    std::map<std::string, nlohmann::json> pendingAttachments;
    for( const auto& file : aAttachments )
    {
        const bool pdf = INDUCTIVE_DATASHEETS::IsPDF(file);
        const bool bundle = INDUCTIVE_ATTACHMENTS::IsBundle( file );
        if( !pdf && !bundle && ( !file.is_object() || !file.contains( "text" ) ) )
        {
            images.push_back( file );
            continue;
        }
        try
        {
            if( m_attachments.size() + pendingAttachments.size() >= 24 )
                throw std::runtime_error( "Attachment limit reached; start a new chat." );
            auto summary = INDUCTIVE_DATASHEETS::Summary( file );
            wxString why;
            if( bundle && m_attachmentHandler )
            {
                for( const auto& piece : INDUCTIVE_ATTACHMENTS::BundlePieces( file ) )
                    if( !m_attachmentHandler( piece, false, why ) )
                        throw std::runtime_error( file["name"].get<std::string>() + ": " + why.ToStdString() );
            }
            else if( !pdf && m_attachmentHandler && !m_attachmentHandler( file, false, why ) )
                throw std::runtime_error( why.ToStdString() );
            std::string id = KIID().AsString().ToStdString();
            pendingAttachments[id] = pdf || bundle ? file : nlohmann::json{{ "name", file["name"] }, { "text", file["text"] }};
            if( pdf ) queueDatasheet(id,file);
            summary["attachment_id"] = id;
            const auto tag = contentNonce();
            parts.push_back( { { "text", "ATTACHMENT DATA " + tag + "\n" + summary.dump()
                    + "\nEND ATTACHMENT DATA " + tag } } );
            emitEvent( nlohmann::json{ { "type", "trace" }, { "verb", "Attached" },
                       { "object", summary["name"] }, { "fail", false } }.dump() );
        }
        catch( const std::exception& e )
        {
            // Fail the whole request, so the model cannot act on a partial set of parts.
            emitEvent( nlohmann::json{ { "type", "error" }, { "message", e.what() } }.dump() );
            m_busy = false;
            m_turnDatasheets.clear();
            return;
        }
        catch( ... )
        {
            emitEvent( nlohmann::json{ { "type", "error" },
                       { "message", "KiCad could not parse this attachment." } }.dump() );
            m_busy = false;
            m_turnDatasheets.clear();
            return;
        }
    }
    m_attachments.insert( pendingAttachments.begin(), pendingAttachments.end() );
    for( const auto& part : imagePartsFrom( images, accepted, rejected ) )
        parts.push_back( part );

    // The attach button is in the UI, so a dropped attachment has to be said out
    // loud. Silently discarding it is worse than refusing it: the prompt the page
    // sends for a bare image is "Look at this image", and a model that receives
    // those words with no image attached will confidently describe one.
    if( rejected )
    {
        emitEvent( nlohmann::json{
                { "type", "trace" },
                { "verb", "Skipped" },
                { "object", std::to_string( rejected )
                                    + ( rejected == 1 ? " attachment" : " attachments" )
                                    + " (too large, or not a PNG/JPEG/GIF/WebP)" },
                { "fail", true } }
                           .dump() );
    }

    if( accepted )
    {
        emitEvent( nlohmann::json{ { "type", "trace" },
                                   { "verb", "Attached" },
                                   { "object", std::to_string( accepted )
                                                       + ( accepted == 1 ? " image" : " images" ) },
                                   { "fail", false } }
                           .dump() );
    }

    m_history.push_back(
            nlohmann::json{ { "role", "user" }, { "parts", parts } }.dump() );

    // Where the images live, so they can be dropped once the turn is over.
    m_imageTurn = accepted ? m_history.size() : 0;

    emitEvent( nlohmann::json{ { "type", "status" }, { "state", "thinking" } }.dump() );
    runTurn( m_state->turnId.load() );
}


/// Roughly how much conversation may be resent on each request.
static const size_t MAX_HISTORY_BYTES = 200 * 1024;


/// True for a plain user prompt: the only entry a request may legally start on.
static bool isUserTextTurn( const std::string& aEntry )
{
    const nlohmann::json entry = nlohmann::json::parse( aEntry, nullptr, false );

    if( entry.is_discarded() || !entry.is_object() )
        return false;

    if( entry.value( "role", std::string() ) != "user" )
        return false;

    const auto parts = entry.find( "parts" );

    return parts != entry.end() && parts->is_array() && !parts->empty()
           && parts->front().is_object() && parts->front().contains( "text" );
}


void INDUCTIVE_PANEL::trimHistory()
{
    size_t bytes = 0;

    for( const std::string& entry : m_history )
        bytes += entry.size();

    if( bytes <= MAX_HISTORY_BYTES )
        return;

    // Trim only to a plain user prompt: starting a request on a model turn or on
    // an orphaned tool result is rejected, and never trim into the turn in flight.
    size_t cut = 0;

    for( size_t i = 0; i < m_turnStart; ++i )
    {
        if( !isUserTextTurn( m_history[i] ) )
            continue;

        size_t remaining = 0;

        for( size_t j = i; j < m_history.size(); ++j )
            remaining += m_history[j].size();

        cut = i;

        if( remaining <= MAX_HISTORY_BYTES )
            break;
    }

    if( !cut )
        return;

    m_history.erase( m_history.begin(), m_history.begin() + cut );
    m_turnStart -= cut;

    // Image positions are one-based, with zero meaning no images. Preserve the
    // first entry when trimming shifts an image turn to the front.
    m_imageTurn = ( m_imageTurn > cut ) ? m_imageTurn - cut : 0;
}


void INDUCTIVE_PANEL::dropImageData()
{
    if( !m_imageTurn || m_imageTurn > m_history.size() )
    {
        m_imageTurn = 0;
        return;
    }

    nlohmann::json entry = nlohmann::json::parse( m_history[m_imageTurn - 1], nullptr, false );

    if( entry.is_discarded() || !entry.is_object() )
    {
        m_imageTurn = 0;
        return;
    }

    const auto parts = entry.find( "parts" );

    if( parts == entry.end() || !parts->is_array() )
    {
        m_imageTurn = 0;
        return;
    }

    nlohmann::json kept = nlohmann::json::array();
    int            dropped = 0;

    for( const auto& part : *parts )
    {
        if( part.is_object() && part.contains( "inlineData" ) )
            ++dropped;
        else
            kept.push_back( part );
    }

    if( dropped )
    {
        // A note rather than nothing, so a follow-up question about the image
        // gets "you cannot see it any more" instead of a confident invention.
        kept.push_back( { { "text", "[" + std::to_string( dropped )
                                    + ( dropped == 1 ? " image was" : " images were" )
                                    + " attached to this message and is no longer visible to "
                                      "you. Ask the user to attach it again if you need it.]" } } );

        entry["parts"] = kept;
        m_history[m_imageTurn - 1] = entry.dump();
    }

    m_imageTurn = 0;
}


// ───────────────────────── the tscircuit pipeline ─────────────────────────
//
// The schematic editor does not design by calling tools.  A turn goes to
// agent/host.mjs, which spends one model request on the whole circuit, compiles
// it, runs the design rule checks and writes KiCad files -- and the panel draws
// the result through the host's draw handler.  It is the same conversation, the
// same events and the same sidebar; only the thing on the other end changed.
//
// Everything here runs on the UI thread.  wxProcess does not push data at us, so
// a short timer drains the pipes; that is less machinery than a worker thread
// plus the marshalling the HTTP path needs, and a pipeline turn touches the
// document through handlers that must be on the UI thread anyway.

/** How often the runner's output is drained, in milliseconds. */
static const int PIPELINE_POLL_MS = 40;

/** A runaway runner must not grow this process without bound. */
static const size_t MAX_PIPELINE_BYTES = 16 * 1024 * 1024;


wxString INDUCTIVE_PANEL::NodePath()
{
    wxString fromEnv;

    if( wxGetEnv( wxS( "INDUCTIVE_NODE" ), &fromEnv ) && wxFileName::FileExists( fromEnv ) )
        return fromEnv;

    // An app launched from Finder inherits a minimal PATH with neither Homebrew
    // nor /usr/local on it, so the install locations are tried directly before
    // the environment's own PATH is consulted.
    for( const wxString& candidate : { wxS( "/opt/homebrew/bin/node" ),
                                       wxS( "/usr/local/bin/node" ),
                                       wxS( "/usr/bin/node" ) } )
    {
        if( wxFileName::FileExists( candidate ) )
            return candidate;
    }

    wxPathList search;
    search.AddEnvList( wxS( "PATH" ) );

    return search.FindAbsoluteValidPath( wxS( "node" ) );
}


void INDUCTIVE_PANEL::runPipelineTurn( unsigned aTurn )
{
    const wxString node = NodePath();
    const wxFileName runner( ProjectDir() + wxFileName::GetPathSeparator() + wxS( "agent" ),
                             wxS( "host.mjs" ) );

    auto fail =
            [this]( const wxString& aMessage )
            {
                m_busy = false;

                if( m_turnStart <= m_history.size() )
                    m_history.resize( m_turnStart );

                emitEvent( nlohmann::json{ { "type", "error" },
                                           { "message", std::string( aMessage.utf8_str() ) } }
                                   .dump() );
            };

    if( node.IsEmpty() )
    {
        fail( _( "Node.js was not found. Install it, or set INDUCTIVE_NODE to the binary." ) );
        return;
    }

    if( !runner.FileExists() )
    {
        fail( wxString::Format( _( "The designer is missing: %s" ), runner.GetFullPath() ) );
        return;
    }

    // The turn's own prompt is the last thing in the history; everything before
    // it is the conversation the runner should continue.
    if( m_history.empty() )
    {
        fail( _( "There is nothing to build." ) );
        return;
    }

    std::string    prompt;
    nlohmann::json attachments = nlohmann::json::array();

    const nlohmann::json last = nlohmann::json::parse( m_history.back(), nullptr, false );

    if( last.is_object() )
    {
        const auto parts = last.find( "parts" );

        if( parts != last.end() && parts->is_array() )
        {
            for( const auto& part : *parts )
            {
                if( !part.is_object() )
                    continue;

                if( part.contains( "text" ) && part["text"].is_string() )
                {
                    prompt += part["text"].get<std::string>();
                }
                else if( part.contains( "inlineData" ) && part["inlineData"].is_object() )
                {
                    // The runner takes attachments the way the page sends them.
                    const auto& data = part["inlineData"];

                    attachments.push_back(
                            { { "url", "data:" + data.value( "mimeType", std::string() )
                                               + ";base64," + data.value( "data", std::string() ) } } );
                }
            }
        }
    }

    nlohmann::json history = nlohmann::json::array();

    for( size_t i = 0; i + 1 < m_history.size(); ++i )
    {
        const nlohmann::json entry = nlohmann::json::parse( m_history[i], nullptr, false );

        if( !entry.is_discarded() && entry.is_object() )
            history.push_back( entry );
    }

    wxString mode = m_mode.Lower();

    if( mode != wxS( "plan" ) && mode != wxS( "ask" ) )
        mode = wxS( "agent" );

    nlohmann::json request = {
        { "prompt", prompt },
        { "mode", std::string( mode.utf8_str() ) },
        { "history", history },
        { "attachments", attachments },
        { "context", m_contextProvider
                             ? std::string( m_contextProvider().utf8_str() )
                             : std::string() }
    };

    // A previous runner may still be shutting down after a Stop. It belongs to a
    // turn nobody is watching, and leaving it attached would mean its exit event
    // closes out the turn that is starting here.
    if( m_pipeline )
    {
        if( m_pipelinePid )
            wxProcess::Kill( m_pipelinePid, wxSIGKILL, wxKILL_CHILDREN );

        m_pipelineTimer.Stop();
        m_pipeline->Detach();
        m_pipeline = nullptr;
        m_pipelinePid = 0;
    }

    m_pipelineOut.clear();
    m_pipelineDrew = false;
    m_pipelineDrawAttempted = false;
    m_pipelineDrawError.Clear();
    m_pipelineTurn = aTurn;

    m_pipeline = new wxProcess( this );
    m_pipeline->Redirect();

    wxExecuteEnv env;
    wxGetEnvMap( &env.env );
    env.cwd = ProjectDir();

    // wxExecute wants a NUL-terminated argv, not a command line: the project path
    // can contain spaces, and quoting it back into a string is a bug waiting for
    // the first user whose directory has one.
    const std::string nodeArg   = std::string( node.utf8_str() );
    const std::string runnerArg = std::string( runner.GetFullPath().utf8_str() );

    const char* const argv[] = { nodeArg.c_str(), runnerArg.c_str(), nullptr };

    // MAKE_GROUP_LEADER is not optional. Every kill site here passes
    // wxKILL_CHILDREN, which wx implements as kill(-pid, sig) -- and without a
    // process group of its own there is no -pid to signal, so the call returns
    // ESRCH and the runner keeps going. The visible symptom is not "Stop is
    // slow": it is a cancelled turn finishing in the background, writing
    // board.tsx and generated/ underneath the next turn, and the *previous*
    // prompt's circuit appearing on screen while the sidebar describes a
    // different one. It also makes the compile worker a grandchild that dies
    // with the group rather than outliving the app.
    m_pipelinePid =
            wxExecute( argv, wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER, m_pipeline, &env );

    if( !m_pipelinePid )
    {
        delete m_pipeline;
        m_pipeline = nullptr;
        fail( _( "The designer could not be started." ) );
        return;
    }

    if( wxOutputStream* in = m_pipeline->GetOutputStream() )
    {
        const std::string payload = request.dump();

        in->Write( payload.data(), payload.size() );
        m_pipeline->CloseOutput();
    }

    m_pipelineTimer.Start( PIPELINE_POLL_MS );
}


void INDUCTIVE_PANEL::pumpPipeline()
{
    if( !m_pipeline || m_pipelinePumping )
        return;

    m_pipelinePumping = true;

    // Restored on every exit path, including the ones an exception takes.
    struct DONE_PUMPING
    {
        bool* flag;
        ~DONE_PUMPING() { *flag = false; }
    } donePumping{ &m_pipelinePumping };

    char buffer[8192];

    // stderr first, and unconditionally: the runner logs there, and a pipe nobody
    // drains fills at 64KB and blocks the process that is writing to it.
    if( wxInputStream* err = m_pipeline->GetErrorStream() )
    {
        while( m_pipeline->IsErrorAvailable() )
        {
            err->Read( buffer, sizeof( buffer ) );

            const size_t read = err->LastRead();

            if( !read )
                break;

            wxLogTrace( wxS( "INDUCTIVE" ), wxS( "designer: %s" ),
                        wxString::FromUTF8( buffer, read ) );
        }
    }

    if( wxInputStream* out = m_pipeline->GetInputStream() )
    {
        while( m_pipeline->IsInputAvailable() )
        {
            out->Read( buffer, sizeof( buffer ) );

            const size_t read = out->LastRead();

            if( !read )
                break;

            if( m_pipelineOut.size() + read > MAX_PIPELINE_BYTES )
            {
                m_pipelineOut.clear();
                wxLogTrace( wxS( "INDUCTIVE" ), wxS( "designer output exceeded the cap" ) );
                break;
            }

            m_pipelineOut.append( buffer, read );
        }
    }

    // One event per line, and a partial line waits for the rest of itself.
    for( ;; )
    {
        const size_t newline = m_pipelineOut.find( '\n' );

        if( newline == std::string::npos )
            break;

        const std::string line = m_pipelineOut.substr( 0, newline );
        m_pipelineOut.erase( 0, newline + 1 );

        if( line.empty() )
            continue;

        const nlohmann::json event = nlohmann::json::parse( line, nullptr, false );

        if( event.is_discarded() || !event.is_object() )
        {
            wxLogTrace( wxS( "INDUCTIVE" ), wxS( "designer wrote a non-event line" ) );
            continue;
        }

        onPipelineEvent( event );
    }

    // The runner exited while we were dispatching. Finishing here, rather than in
    // the re-entrant exit handler, is what keeps a successful draw from being
    // reported as a crash.
    if( m_pipelineEnded )
    {
        m_pipelineEnded = false;
        m_pipelinePumping = false;   // finishPipeline drains, and must be allowed to
        finishPipeline( m_pipelineExit );
    }
}


void INDUCTIVE_PANEL::onPipelineEvent( const nlohmann::json& aEvent )
{
    // Stop was pressed, or the panel moved on. The runner is being killed; until
    // it dies its remaining output belongs to a turn nobody is watching.
    if( m_pipelineTurn != m_state->turnId.load() )
        return;

    // Everything below can end up inside KiCad's own commit machinery, and an
    // exception escaping a wx event handler does not become an error message --
    // it terminates the editor. In front of an audience that is the end of the
    // demo and of any unsaved work, so the handler owns its failures the same way
    // onScriptMessage does.
    try
    {
        handlePipelineEvent( aEvent );
    }
    catch( const std::exception& e )
    {
        m_busy = false;
        emitEvent( nlohmann::json{ { "type", "error" },
                                   { "message", std::string( e.what() ) } }
                           .dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );

        if( INDUCTIVE_REPORTS::Unexpected( &e ) )
            INDUCTIVE_REPORTS::Error( reportApp(), "pipeline event", e.what() );
    }
    catch( ... )
    {
        m_busy = false;
        emitEvent( nlohmann::json{ { "type", "error" },
                                   { "message", "The designer hit an unexpected error." } }
                           .dump() );
        INDUCTIVE_REPORTS::Error( reportApp(), "pipeline event", "unknown exception" );
    }
}


void INDUCTIVE_PANEL::handlePipelineEvent( const nlohmann::json& aEvent )
{

    const std::string type = aEvent.value( "type", std::string() );

    if( type == "generated" )
    {
        const wxString dir = wxString::FromUTF8( aEvent.value( "directory", std::string() ) );

        wxString detail;
        const bool drew = m_onDraw && m_onDraw( dir, detail );

        m_pipelineDrew = drew;
        m_pipelineDrawAttempted = true;
        m_pipelineDrawError = drew ? wxString() : detail;

        emitEvent( nlohmann::json{ { "type", "trace" },
                                   { "verb", drew ? "Drew" : "Failed" },
                                   { "object", std::string( detail.utf8_str() ) },
                                   { "fail", !drew },
                                   { "id", "draw" } }
                           .dump() );
        return;
    }

    if( type == "done" )
    {
        const auto ops = aEvent.find( "ops" );

        if( ops != aEvent.end() && ops->is_array() && !ops->empty() && m_onOps )
        {
            wxString detail;
            const bool applied = m_onOps( wxString::FromUTF8( ops->dump() ), detail );

            emitEvent( nlohmann::json{ { "type", "trace" },
                                       { "verb", applied ? "Changed" : "Failed" },
                                       { "object", std::string( detail.utf8_str() ) },
                                       { "fail", !applied } }
                               .dump() );
        }

        // The runner owns the conversation while it runs and hands it back here,
        // already compacted -- no board source, no repair scaffolding.
        const auto history = aEvent.find( "history" );

        if( history != aEvent.end() && history->is_array() )
        {
            m_history.clear();

            for( const auto& entry : *history )
            {
                if( entry.is_object() )
                    m_history.push_back( entry.dump() );
            }
        }

        m_busy = false;
        dropImageData();

        // The reply was written before the drawing was attempted, and it does not
        // know whether that worked. Saying "drew 8 symbols" over an unchanged
        // canvas is the worst thing this panel can do, so the outcome the panel
        // actually observed wins.
        std::string text = aEvent.value( "text", std::string() );

        if( m_pipelineDrawAttempted && !m_pipelineDrew )
        {
            text = std::string( m_pipelineDrawError.utf8_str() )
                   + "\n\nThe circuit was built, but nothing was drawn — your schematic is "
                     "unchanged.";
        }

        emitEvent( nlohmann::json{ { "type", "done" }, { "text", text } }
                           .dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
        return;
    }

    if( type == "error" )
    {
        m_busy = false;
        dropImageData();

        // Roll the whole turn back, so a retry does not resend a prompt that was
        // never answered.
        if( m_turnStart <= m_history.size() )
            m_history.resize( m_turnStart );

        emitEvent( aEvent.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
        return;
    }

    if( type == "cancelled" )
    {
        m_busy = false;
        emitEvent( aEvent.dump() );
        return;
    }

    // trace, text, text-reset, status: the sidebar already knows these.
    emitEvent( aEvent.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
}


void INDUCTIVE_PANEL::onPipelineEnd( wxProcessEvent& aEvent )
{
    // Drawing a schematic pumps the event loop, so this can dispatch *inside*
    // pumpPipeline(). Tearing the process down from there would delete the
    // streams the outer call is still reading, and the turn would be reported as
    // a crash moments after it drew the board perfectly. Let the pump finish and
    // close out on its way through.
    if( m_pipelinePumping )
    {
        m_pipelineEnded = true;
        m_pipelineExit = aEvent.GetExitCode();
        return;
    }

    finishPipeline( aEvent.GetExitCode() );
}


void INDUCTIVE_PANEL::finishPipeline( int aExitCode )
{
    // Whatever the runner wrote just before exiting is still in the pipe.
    if( m_pipeline )
    {
        char buffer[8192];

        if( wxInputStream* out = m_pipeline->GetInputStream() )
        {
            for( ;; )
            {
                out->Read( buffer, sizeof( buffer ) );

                const size_t read = out->LastRead();

                if( !read || m_pipelineOut.size() + read > MAX_PIPELINE_BYTES )
                    break;

                m_pipelineOut.append( buffer, read );
            }
        }

        // Dispatch the tail without re-reading the (now closed) streams.
        if( !m_pipelineOut.empty() && m_pipelineOut.back() != '\n' )
            m_pipelineOut.push_back( '\n' );

        for( ;; )
        {
            const size_t newline = m_pipelineOut.find( '\n' );

            if( newline == std::string::npos )
                break;

            const std::string line = m_pipelineOut.substr( 0, newline );
            m_pipelineOut.erase( 0, newline + 1 );

            if( line.empty() )
                continue;

            const nlohmann::json event = nlohmann::json::parse( line, nullptr, false );

            if( !event.is_discarded() && event.is_object() )
                onPipelineEvent( event );
        }
    }

    const int code = aExitCode;

    endPipeline();

    // A runner that died without saying so leaves the sidebar spinning forever.
    if( m_busy && m_pipelineTurn == m_state->turnId.load() )
    {
        m_busy = false;
        dropImageData();

        if( m_turnStart <= m_history.size() )
            m_history.resize( m_turnStart );

        emitEvent( nlohmann::json{
                { "type", "error" },
                { "message", std::string( wxString::Format(
                                     _( "The designer stopped unexpectedly (exit %d)." ), code )
                                                  .utf8_str() ) } }
                           .dump() );
    }
}


void INDUCTIVE_PANEL::endPipeline()
{
    m_pipelineTimer.Stop();

    // wxProcess( parent ) is ours to delete: wx only self-deletes the detached form.
    delete m_pipeline;
    m_pipeline = nullptr;
    m_pipelinePid = 0;
}


/**
 * Run the round's search_parts calls here, on the network thread, so the editor
 * never waits on a catalogue. The UI-thread dispatcher only looks the results up.
 */
static std::map<std::string, std::pair<bool, std::string>>
prefetchPartSearches( const std::string& aParts, const std::function<bool()>& aAbandoned )
{
    std::map<std::string, std::pair<bool, std::string>> out;
    const nlohmann::json parts = nlohmann::json::parse( aParts, nullptr, false );

    if( !parts.is_array() )
        return out;

    for( const auto& part : parts )
    {
        if( out.size() >= 6 || aAbandoned() )
            break;

        if( !part.contains( "functionCall" ) || !part["functionCall"].is_object() )
            continue;

        const auto& call = part["functionCall"];

        if( call.value( "name", std::string() ) != "search_parts" )
            continue;

        const nlohmann::json args = call.contains( "args" ) && call["args"].is_object()
                                            ? call["args"] : nlohmann::json::object();
        const std::string key = args.dump();

        if( out.count( key ) )
            continue;

        const auto resp = INDUCTIVE_CLOUD_AUTH::ApiRequest( wxS( "POST" ), wxS( "/v1/parts/search" ), key );

        if( resp.status == 200 )
        {
            out[key] = { true, resp.body };
            continue;
        }

        const nlohmann::json body = nlohmann::json::parse( resp.body, nullptr, false );
        std::string message = body.is_object() && body.contains( "message" ) && body["message"].is_string()
                                      ? body["message"].get<std::string>()
                                      : std::string( "The parts catalogue is unavailable right now." );

        if( resp.signedOut )
            message = "Sign in to search for parts.";

        out[key] = { false, message };
    }

    return out;
}


void INDUCTIVE_PANEL::runTurn( unsigned aTurn )
{
    // Both editors use the native tool agent. The legacy runner is retained for CLI compatibility.
    if( usesPipeline() )
    {
        runPipelineTurn( aTurn );
        return;
    }

    trimHistory();

    nlohmann::json contents = nlohmann::json::array();

    for( const std::string& entry : m_history )
        contents.push_back( nlohmann::json::parse( entry, nullptr, false ) );

    // The server adds the system prompt and the tool declarations; the app sends only
    // what it alone knows: the user's standing instructions, what the editor has open,
    // and which tools it can run. Documents are fenced on the server with this nonce,
    // the one tool output is fenced with, after removing it from the text so the text
    // cannot close its own fence.
    // PDF payloads are request-local: do not inflate, truncate or persist the transcript with them.
    try { INDUCTIVE_DATASHEETS::AttachToRequest(contents,m_turnDatasheets); }
    catch( const std::exception& e )
    {
        m_busy = false; dropImageData(); m_turnDatasheets.clear();
        emitEvent(nlohmann::json{{"type","error"},{"message",e.what()}}.dump());
        return;
    }

    const std::string nonce = contentNonce();
    const wxString    tag   = wxString::FromUTF8( nonce );
    nlohmann::json    documents = nlohmann::json::array();

    if( m_contextProvider )
    {
        wxString context = m_contextProvider();

        if( !context.IsEmpty() )
        {
            context.Replace( tag, wxS( "[redacted]" ) );
            documents.push_back( { { "kind", "editor" }, { "text", std::string( context.utf8_str() ) } } );
        }
    }

    if( m_selectedPart.is_object() && m_selectedPartProject == currentProject() )
    {
        wxString part = wxString::FromUTF8( m_selectedPart.dump() );
        part.Replace( tag, "[redacted]" );
        documents.push_back( { { "kind", "selected_part" }, { "text", std::string( part.utf8_str() ) } } );
    }

    // In Ask and Plan the tools that change the design are not offered (and the
    // server would not declare them either) -- see modeAllowsMutation().
    nlohmann::json tools = nlohmann::json::array();

    if( m_toolHandler )
    {
        const bool mayMutate = modeAllowsMutation( m_mode );

        for( const INDUCTIVE_TOOL& tool : m_tools )
            if( !tool.mutates || mayMutate )
                tools.push_back( std::string( tool.name.utf8_str() ) );
    }

    nlohmann::json request = {
        { "contents", contents },
        { "agent",
          { { "editor", editorKind() == "pcb" ? "pcb" : "schematic" },
            { "nonce", nonce },
            { "tools", tools },
            { "instructions", LoadSettings().value( "instructions", std::string() ) },
            { "documents", documents } } }
    };

    // The backend picks the model from the turn's level and holds the key; the
    // request itself is ours, unchanged. The turn is opened on the server before
    // its first request (see the worker below), and its id goes with every request.
    // level, mode and turn_start are for a server from before turns, which ignores
    // turn_id; a current one takes all three from the turn.
    const int         level   = m_turnLevel;
    const std::string mode    = std::string( m_mode.Lower().utf8_str() );
    const std::string turnKey = m_toolRounds == 0 ? std::string() : m_serverTurnId;
    nlohmann::json    body    = nlohmann::json{ { "level", level },
                                                { "mode", mode },
                                                { "turn_start", m_toolRounds == 0 },
                                                { "request", request } };
    const std::string url =
            std::string( INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized(
                                 INDUCTIVE_CLOUD_AUTH::LoadConfig().apiBase )
                                 .utf8_str() )
            + "/v1/generate";

    auto           state = m_state;
    const unsigned turn  = aTurn;

    std::thread(
            [this, state, turn, level, mode, turnKey, body = std::move( body ), url]() mutable
            {
                // The UI may only be touched from the main thread, and a panel that
                // has been destroyed or a turn that has been stopped must produce
                // nothing at all.  Every event raised here goes through this gate.
                auto postEvent =
                        [this, state, turn]( const nlohmann::json& aEvent )
                        {
                            const std::string dumped = aEvent.dump();

                            // wxTheApp is null once the app object has been
                            // destroyed, which happens while this thread may still
                            // be blocked in curl.
                            wxAppConsole* app = wxTheApp;

                            if( !app || !state->alive.load() )
                                return;

                            app->CallAfter(
                                    [this, state, turn, dumped]()
                                    {
                                        if( !state->alive.load()
                                            || turn != state->turnId.load() )
                                        {
                                            return;
                                        }

                                        emitEvent( dumped );
                                    } );
                        };

                std::string parts;
                bool        hasCalls        = false;
                bool        succeeded       = false;
                bool        streamedAnyText = false;

                // Reads only through the shared state: `this` is not touched from
                // this thread at all.
                auto abandoned = [state, turn]()
                {
                    return !state->alive.load() || turn != state->turnId.load();
                };

                if( abandoned() )
                    return;

                int         status = 0;
                std::string raw;
                bool        broken = false;

                // A turn's first request opens the turn on the server, which is
                // where the plan's thinking levels and the month's turns are
                // decided. A 404 is a server from before turns: send without one.
                std::string serverTurn = turnKey;
                bool        opened     = true;

                if( serverTurn.empty() )
                {
                    const INDUCTIVE_CLOUD_AUTH::ApiResponse reply = INDUCTIVE_CLOUD_AUTH::ApiRequest(
                            wxS( "POST" ), wxS( "/v1/generate/turn" ),
                            nlohmann::json{ { "mode", mode }, { "level", level } }.dump() );

                    if( abandoned() )
                        return;

                    const nlohmann::json json = nlohmann::json::parse( reply.body, nullptr, false );

                    if( reply.status == 200 && json.is_object() && json.contains( "turn_id" )
                        && json["turn_id"].is_string() )
                    {
                        serverTurn = json["turn_id"].get<std::string>();
                    }
                    else if( reply.status != 404 )
                    {
                        opened = false;
                        status = reply.signedOut ? 401 : reply.status;
                        raw    = reply.body;
                    }
                }

                if( !serverTurn.empty() )
                    body["turn_id"] = serverTurn;

                const std::string payload = body.dump();

                // One request, to the model the thinking level chose. A failure is
                // the answer: it is not retried and no other model is tried behind
                // the user's back. The one exception is a refused access token,
                // which is refused before anything runs: that is sent once more
                // with a fresh token, since the server may have ended the old one
                // early (a sign-out elsewhere) or the clock may have outrun it.
                for( int attempt = 0; opened; ++attempt )
                {
                    bool              signedOut = false;
                    const std::string accessToken =
                            INDUCTIVE_CLOUD_AUTH::AccessToken( &signedOut );

                    if( accessToken.empty() )
                    {
                        status = signedOut ? 401 : 0;   // friendlyError() says which
                        raw.clear();
                        break;
                    }

                    try
                    {
                        // Each delta reaches the UI as it arrives, so the answer
                        // appears while the model is still writing it.
                        SSE_SINK sink(
                                [&]( const std::string& aDelta )
                                {
                                    streamedAnyText = true;
                                    postEvent( nlohmann::json{ { "type", "text" },
                                                               { "delta", aDelta } } );
                                } );

                        std::ostream    out( &sink );
                        KICAD_CURL_EASY curl;

                        curl.SetURL( url );
                        curl.SetHeader( "Content-Type", "application/json" );
                        curl.SetHeader( "Authorization", "Bearer " + accessToken );
                        curl.SetHeader( "X-Inductive-Version", INDUCTIVE_VERSION );

                        curl.SetPostFields( payload );
                        curl.SetTimeout( REQUEST_TIMEOUT_S );
                        curl.SetFollowRedirects( false );
                        curl.SetOutputStream( &out );

                        // Installing a progress callback is what makes this
                        // transfer abortable at shutdown: KiCad's own
                        // KICAD_CURL::IsShuttingDown() check lives inside the
                        // progress hook, which is only registered when a transfer
                        // callback is set. Returning non-zero aborts, so a turn
                        // the user stopped also drops its connection -- and the
                        // server, seeing it close, stops the model call.
                        curl.SetTransferCallback(
                                [abandoned]( size_t, size_t, size_t, size_t )
                                {
                                    return abandoned() ? 1 : 0;
                                },
                                250 );

                        // A dead connection is cut well before the two-minute
                        // ceiling -- but not before a slow model's first byte:
                        // the higher thinking levels were measured at over a
                        // minute before answering "hello", and nothing arrives
                        // while they think.
                        curl.SetStallTimeout( 1, 90 );

                        const int performed = curl.Perform();

                        out.flush();
                        sink.Finish();      // a final data: line may lack its newline

                        raw    = sink.Raw();
                        broken = sink.Broken();

                        if( performed == 0 )
                        {
                            status = curl.GetResponseStatusCode();

                            if( !broken && status == 200 && !sink.Parts().empty() )
                            {
                                parts     = sink.Parts().dump();
                                hasCalls  = sink.HasCalls();
                                succeeded = true;
                            }
                        }
                    }
                    catch( const std::exception& e )
                    {
                        // Nothing may escape a detached thread: that is
                        // std::terminate, which takes the whole editor with it.
                        raw    = e.what();
                        status = 0;
                    }
                    catch( ... )
                    {
                        status = 0;
                    }

                    if( status == 401 && attempt == 0 && !streamedAnyText && !abandoned() )
                    {
                        INDUCTIVE_CLOUD_AUTH::InvalidateAccessToken( accessToken );
                        continue;
                    }

                    break;
                }

                const std::string error = succeeded ? std::string()
                                                    : friendlyError( status, raw, level );
                // A 200 with nothing in it: the model answered, with no text and no call.
                const bool emptyReply = !succeeded && status == 200;

                std::map<std::string, std::pair<bool, std::string>> prefetched;

                if( succeeded && hasCalls && !abandoned() )
                    prefetched = prefetchPartSearches( parts, abandoned );

                // The reply streams into the pane as it arrives. If the request
                // failed after emitting some text, that half-sentence is still on
                // screen and the error is appended below it, reading as though the
                // model said both. Clear it.
                if( !succeeded && streamedAnyText )
                    postEvent( nlohmann::json{ { "type", "text-reset" } } );

                // Marshalled through the app rather than the panel: if the panel is
                // gone, calling its CallAfter would already be a use-after-free.
                // The app itself can also be gone by now.
                wxAppConsole* app = wxTheApp;

                if( !app || !state->alive.load() )
                    return;

                app->CallAfter(
                        [this, state, turn, parts, error, emptyReply, hasCalls, prefetched, serverTurn]()
                        {
                            if( !state->alive.load() || turn != state->turnId.load() )
                                return;             // panel destroyed, or Stop was pressed

                            m_serverTurnId = error.empty() ? serverTurn : std::string();

                            // Tools ran, then the model said nothing: the work is in the
                            // design and only its summary is missing. "Try rephrasing"
                            // would read as though nothing happened. Say what the tools
                            // did, from their own results, and keep the turn so that
                            // "continue" picks up from it.
                            if( !error.empty() && emptyReply && m_toolRounds > 0 )
                            {
                                const std::string summary = toolStepsSummary();
                                m_history.push_back(
                                        nlohmann::json{ { "role", "model" },
                                                        { "parts", nlohmann::json::array(
                                                                           { { { "text", summary } } } ) } }
                                                .dump() );
                                m_busy = false;
                                dropImageData();
                                emitEvent( nlohmann::json{ { "type", "done" }, { "text", summary } }
                                                   .dump( -1, ' ', false,
                                                          nlohmann::json::error_handler_t::replace ) );
                                return;
                            }

                            if( !error.empty() )
                            {
                                m_busy = false;
                                dropImageData();

                                // Roll the whole turn back so a retry does not
                                // resend a question the model never answered.
                                if( m_turnStart <= m_history.size() )
                                    m_history.resize( m_turnStart );

                                // Serialised leniently: this message can contain
                                // bytes from an error body we did not generate --
                                // an intercepting proxy returning a Windows-1252
                                // page, say -- and strict dump() throws on invalid
                                // UTF-8 with nothing above this to catch it.
                                emitEvent( nlohmann::json{ { "type", "error" },
                                                           { "message", error } }
                                                   .dump( -1, ' ', false,
                                                          nlohmann::json::error_handler_t::
                                                                  replace ) );

                                // A session the server ended shows as signed out.
                                if( !INDUCTIVE_CLOUD_AUTH::HasSession() )
                                    emitAuthStatus();

                                return;
                            }

                            // Replayed verbatim: the parts carry thought signatures.
                            m_history.push_back(
                                    nlohmann::json{
                                            { "role", "model" },
                                            { "parts", nlohmann::json::parse( parts ) } }
                                            .dump() );

                            if( hasCalls && m_toolRounds >= MAX_TOOL_ROUNDS )
                            {
                                // Drop the model turn we cannot answer: nothing else
                                // removes it, and every later request replays the
                                // history verbatim, functionCall included.
                                m_history.pop_back();
                                m_busy = false;
                                dropImageData();

                                emitEvent( nlohmann::json{
                                        { "type", "trace" },
                                        { "verb", "Stopped" },
                                        { "object", "tool budget exhausted" },
                                        { "fail", true } }
                                                   .dump() );

                                // "done" rather than "error": the page renders an
                                // error by rejecting the turn, which prints transport
                                // copy that would be wrong here.
                                emitEvent( nlohmann::json{
                                        { "type", "done" },
                                        { "text", "I ran out of steps for this request before "
                                                  "finishing. What I changed so far is kept; "
                                                  "reply \"continue\" and I'll pick up where "
                                                  "I stopped." } }
                                                   .dump() );
                                return;
                            }

                            if( hasCalls )
                            {
                                ++m_toolRounds;

                                m_prefetchedParts = prefetched;

                                // source_bom calls the server; that happens off this
                                // thread, and the round continues when it answers.
                                if( sourceBomThenContinue( parts, turn ) )
                                    return;

                                continueToolRound( parts, turn );
                                return;
                            }

                            // A turn that changed the schematic and left ERC errors the
                            // reply did not mention: said under it ("verified and
                            // published" came back over two errors).
                            if( const std::string note = ercNoteAfterTurn( parts ); !note.empty() )
                                emitEvent( nlohmann::json{ { "type", "text" }, { "delta", note } }
                                                   .dump( -1, ' ', false,
                                                          nlohmann::json::error_handler_t::replace ) );

                            m_busy = false;
                            dropImageData();
                            emitEvent( nlohmann::json{ { "type", "done" } }.dump() );
                        } );
            } )
            .detach();
}


std::string INDUCTIVE_PANEL::ercNoteAfterTurn( const std::string& aParts )
{
    if( editorKind() != "sch" || m_mutationsThisTurn == 0 || !m_commandHandler )
        return std::string();

    wxString result;

    try
    {
        if( !m_commandHandler( wxS( "erc" ), result ) )
            return std::string();
    }
    catch( ... )
    {
        return std::string();   // the editor refused (busy, a dialog open): say nothing
    }

    const std::string erc = result.ToStdString( wxConvUTF8 );
    std::smatch       counts;

    if( !std::regex_search( erc, counts, std::regex( R"(ERC: (\d+) error\(s\), (\d+) warning)" ) ) )
        return std::string();

    const int errors = std::stoi( counts[1].str() );

    if( errors == 0 )
        return std::string();

    // What the model said this round.
    std::string said;

    try
    {
        for( const auto& part : nlohmann::json::parse( aParts ) )
        {
            if( part.is_object() && part.contains( "text" ) && part["text"].is_string()
                && !part.value( "thought", false ) )
            {
                said += part["text"].get<std::string>();
            }
        }
    }
    catch( ... )
    {
    }

    // "2 errors", "2 ERC errors": stated, nothing to add.
    if( std::regex_search( said, std::regex( "\\b" + std::to_string( errors ) + R"(\s+(ERC\s+)?errors?\b)",
                                             std::regex::icase ) ) )
    {
        return std::string();
    }

    std::string list;
    int         shown = 0;
    std::smatch line;

    for( auto it = erc.cbegin();
         shown < 4 && std::regex_search( it, erc.cend(), line, std::regex( R"(  ERROR\s+(\d+)  ([^\n]+))" ) );
         it = line.suffix().first, ++shown )
    {
        list += "\n- " + line[2].str() + ( line[1].str() == "1" ? std::string() : " (" + line[1].str() + ")" );
    }

    return "\n\n**ERC still reports " + std::to_string( errors ) + " error" + ( errors == 1 ? "" : "s" )
           + "**, marked on the sheet:" + list + "\nAsk me to fix " + ( errors == 1 ? "it" : "them" )
           + ", or run /erc to see the list.";
}


std::string INDUCTIVE_PANEL::toolStepsSummary() const
{
    std::vector<std::string> steps;

    if( m_turnRecord.is_object() && m_turnRecord.contains( "events" ) )
    {
        for( const auto& ev : m_turnRecord["events"] )
        {
            if( !ev.is_object() || ev.value( "type", std::string() ) != "trace" )
                continue;

            const std::string verb = ev.value( "verb", std::string() );

            if( verb == "Read" || verb == "Attached" )
                continue;   // looking, not doing

            std::string object = ev.contains( "object" ) && ev["object"].is_string()
                                         ? ev["object"].get<std::string>() : std::string();

            if( object.size() > 180 )
                object = object.substr( 0, 177 ) + "...";

            steps.push_back( "- " + verb + ": " + object );
        }
    }

    std::string out = "I stopped before writing a reply.";

    if( steps.empty() )
        return out + " Nothing in the design was changed. Try asking again.";

    out += " My last steps, kept in the design (Undo takes the whole request back):\n";

    for( size_t i = steps.size() > 4 ? steps.size() - 4 : 0; i < steps.size(); ++i )
        out += steps[i] + "\n";

    return out + "Reply \"continue\" and I'll pick up from here.";
}


void INDUCTIVE_PANEL::continueToolRound( const std::string& aParts, unsigned aTurn )
{
    auto state = m_state;
    const bool more = executeToolCalls( aParts, aTurn );

    // A tool handler pumps the event loop, so a queued stop or reset can
    // dispatch re-entrantly; anything below would trample the new state.
    if( !state->alive.load() || aTurn != state->turnId.load() )
        return;

    if( more )
    {
        runTurn( aTurn );   // hand the results back
        return;
    }

    m_busy = false;
    dropImageData();
    emitEvent( nlohmann::json{ { "type", "done" } }.dump() );
}


bool INDUCTIVE_PANEL::sourceBomThenContinue( const std::string& aParts, unsigned aTurn )
{
    m_prefetchedSourcing.clear();
    const nlohmann::json parts = nlohmann::json::parse( aParts, nullptr, false );
    std::vector<std::pair<std::string, nlohmann::json>> requests;   // call key -> request body

    for( const auto& part : parts.is_array() ? parts : nlohmann::json::array() )
    {
        if( !part.is_object() || !part.contains( "functionCall" ) || !part["functionCall"].is_object()
            || part["functionCall"].value( "name", std::string() ) != "source_bom" || requests.size() >= 1 )
            continue;
        const nlohmann::json args = part["functionCall"].contains( "args" ) && part["functionCall"]["args"].is_object()
                                            ? part["functionCall"]["args"] : nlohmann::json::object();
        if( !m_sourcingHandler )
        {
            m_prefetchedSourcing[args.dump()] = { false, "Open the schematic editor to source its BOM." };
            continue;
        }
        try
        {
            // Read on this thread: the schematic is only safe to touch here.
            const nlohmann::json lines = m_sourcingHandler( "collect", nlohmann::json::object() );
            if( lines.empty() )
            {
                m_prefetchedSourcing[args.dump()] = { false, "The schematic has no physical components to source." };
                continue;
            }
            int quantity = args.contains( "quantity" ) && args["quantity"].is_number_integer() ? args["quantity"].get<int>() : 10;
            quantity = std::max( 1, std::min( 10000, quantity ) );
            requests.push_back( { args.dump(), { { "quantity", quantity }, { "lines", lines } } } );
        }
        catch( const std::exception& e )
        {
            m_prefetchedSourcing[args.dump()] = { false, e.what() };
        }
    }

    if( requests.empty() )
        return false;

    emitEvent( nlohmann::json{ { "type", "trace" }, { "verb", "Sourcing" }, { "object", "the BOM" }, { "fail", false } }.dump() );
    auto state = m_state;

    std::thread(
            [this, state, aTurn, aParts, requests]()
            {
                std::map<std::string, std::pair<bool, std::string>> answers;
                for( const auto& [key, body] : requests )
                {
                    INDUCTIVE_CLOUD_AUTH::ApiResponse resp;
                    try
                    {
                        resp = INDUCTIVE_CLOUD_AUTH::ApiRequest( wxS( "POST" ), wxS( "/v1/parts/source" ), body.dump() );
                    }
                    catch( ... )
                    {
                        resp.status = 0;
                    }
                    if( resp.status == 200 )
                    {
                        answers[key] = { true, resp.body };
                        continue;
                    }
                    const nlohmann::json reply = nlohmann::json::parse( resp.body, nullptr, false );
                    answers[key] = { false, resp.signedOut ? std::string( "Sign in to source parts." )
                                            : reply.is_object() && reply.contains( "message" ) && reply["message"].is_string()
                                                    ? reply["message"].get<std::string>()
                                                    : std::string( "The parts catalogue is unavailable right now." ) };
                }

                wxAppConsole* app = wxTheApp;
                if( !app || !state->alive.load() )
                    return;
                app->CallAfter(
                        [this, state, aTurn, aParts, answers]()
                        {
                            if( !state->alive.load() || aTurn != state->turnId.load() )
                                return;
                            for( const auto& [key, answer] : answers ) m_prefetchedSourcing[key] = answer;
                            continueToolRound( aParts, aTurn );
                        } );
            } )
            .detach();
    return true;
}


bool INDUCTIVE_PANEL::executeToolCalls( const std::string& aPartsJson, unsigned aTurn )
{
    const nlohmann::json parts = nlohmann::json::parse( aPartsJson, nullptr, false );

    if( parts.is_discarded() || !parts.is_array() || !m_toolHandler )
        return false;

    nlohmann::json responses = nlohmann::json::array();

    int executed = 0;

    for( const auto& part : parts )
    {
        if( !part.contains( "functionCall" ) )
            continue;

        // A tool handler can pump the event loop -- a canvas refresh, a queued
        // event -- so Stop, a reset, or the panel being destroyed can all
        // dispatch between two calls of the same round.  Without this the
        // remaining calls would go on editing the user's board for a turn they
        // have already cancelled, and the history append below would then run
        // against a panel that may no longer exist.
        //
        // Abandoning here drops the partial response list, which is safe because
        // whatever cancelled the turn has already truncated m_history back to
        // m_turnStart. Note this covers only a cancellation between calls; one
        // that lands during the final call is caught by the re-check before the
        // history append below.
        if( !m_state->alive.load() || aTurn != m_state->turnId.load() )
            return false;

        // Checked, not assumed: SSE_SINK only tests contains(), so a reply part of
        // {"functionCall": 5} gets this far, and value() throws on a non-object
        // from inside a CallAfter continuation.
        if( !part["functionCall"].is_object() )
            continue;

        const auto&       call = part["functionCall"];
        const std::string name = call.value( "name", std::string() );
        const nlohmann::json args = call.contains( "args" ) ? call["args"] : nlohmann::json::object();

        wxString result;
        bool     ok = false;

        // Find the declaration, which is also how we learn whether this call
        // changes the document.  A name we never declared is refused outright:
        // the model occasionally invents one, and dispatching an unknown string
        // into the editor's handler is not something to find out about later.
        const INDUCTIVE_TOOL* declared = nullptr;

        for( const INDUCTIVE_TOOL& tool : m_tools )
        {
            if( tool.name == wxString::FromUTF8( name ) )
            {
                declared = &tool;
                break;
            }
        }

        if( executed >= MAX_CALLS_PER_ROUND )
        {
            // The model emits parallel function calls freely, and nothing bounds how
            // many arrive in one round -- MAX_TOOL_ROUNDS counts rounds, not
            // calls, so a single round could otherwise carry hundreds of edits.
            //
            // The extras still get a response.  Every functionCall must be
            // answered or the history, which is replayed verbatim on every later
            // request, carries an unanswered call and the API rejects the whole
            // conversation from then on.
            result = wxString::Format(
                    _( "I asked for too many things at once. %s was not run -- reply with the "
                       "next step on its own." ),
                    wxString::FromUTF8( name ) );
        }
        else if( !declared )
        {
            result = wxString::Format( _( "There is no tool called %s." ),
                                       wxString::FromUTF8( name ) );
        }
        else if( declared->mutates && !modeAllowsMutation( m_mode ) )
        {
            // Belt and braces: the declaration was withheld this turn, so arriving
            // here means the model asked for something it was never offered --
            // replayed from earlier history, or invented.  Refuse and say why.
            result = wxString::Format(
                    _( "%s changes the design, and this is %s mode. Switch to Agent to edit." ),
                    wxString::FromUTF8( name ), m_mode );
        }
        else if( declared->mutates && m_mutationsThisTurn >= MAX_TURN_MUTATIONS )
        {
            // A runaway loop that deletes in a circle is the failure mode that
            // costs the user real work.  Undo would recover it, but only if they
            // notice; stopping is better than being recoverable.
            result = wxString::Format(
                    _( "I have already made %d changes in this turn and stopped there. "
                       "Check the result and ask again if that was right." ),
                    m_mutationsThisTurn );
        }
        else
        {
            ++executed;

            if( declared->mutates )
                ++m_mutationsThisTurn;

            // The handler touches the open document, so it must run here on the UI
            // thread -- KiCad's data model is not safe to reach from anywhere else.
            // A throwing tool becomes a failed tool call rather than a dead editor.
            try
            {
                if( name == "edit_circuit" )
                {
                    // Staged here and committed just below, in this same call: changes apply
                    // as soon as they validate, with no before/after card to approve.
                    ok = m_circuitEditHandler && m_circuitEditHandler( "stage", args, result );
                }
                else if( name == "search_my_parts" || name == "get_my_part" || name == "save_to_my_parts" || name == "use_my_part" )
                    ok = partsTool( name, args, result );
                else if( name == "read_datasheet" ) ok = readDatasheetTool( args, result );
                else if( name == "search_parts" )
                {
                    const auto hit = m_prefetchedParts.find( args.dump() );

                    if( hit != m_prefetchedParts.end() )
                    {
                        ok = hit->second.first;
                        result = wxString::FromUTF8( hit->second.second );
                    }
                    else
                    {
                        result = _( "That part search did not run. Ask for it again on its own." );
                    }
                }
                else if( name == "source_bom" )
                {
                    const auto hit = m_prefetchedSourcing.find( args.dump() );
                    if( hit == m_prefetchedSourcing.end() )
                        result = _( "Source the BOM once per reply, on its own." );
                    else if( !hit->second.first )
                        result = wxString::FromUTF8( hit->second.second );
                    else
                    {
                        nlohmann::json answer = nlohmann::json::parse( hit->second.second, nullptr, false );
                        if( !answer.is_object() )
                            result = _( "The parts catalogue answered with something unreadable. Try again." );
                        else
                        {
                            const bool record = !args.contains( "record" ) || !args["record"].is_boolean() || args["record"].get<bool>();
                            const bool overwrite = args.contains( "overwrite" ) && args["overwrite"].is_boolean() && args["overwrite"].get<bool>();
                            answer["recorded"] = record ? m_sourcingHandler( "apply", { { "response", answer }, { "overwrite", overwrite } } )
                                                        : nlohmann::json( "not recorded (record=false)" );
                            answer["next"] = "Picked lines are recorded on the schematic as one undo step. For ambiguous, "
                                             "not_found and out_of_stock lines, ask the user what they need (LED colour, "
                                             "switch type, crystal load capacitance, module variant) or choose from candidates "
                                             "with search_parts, then set_part_number. Report totals and setup fees.";
                            result = wxString::FromUTF8( answer.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
                            ok = true;
                        }
                    }
                }
                else if( name == "list_attachments" || name == "read_attachment" || name == "import_attachment" )
                    ok = attachmentTool( name, args, result );
                else
                    ok = m_toolHandler( wxString::FromUTF8( name ),
                                        wxString::FromUTF8( args.dump() ), result );
                if( ok && ( name == "prepare_pcb" || name == "open_pcb" ) )
                    notePublished( result );
                if( ok && m_circuitEditHandler )
                {
                    auto preview = nlohmann::json::parse( result.ToStdString(), nullptr, false );
                    if( preview.is_object() && preview.contains( "preview_token" ) ) {
                        // Mutating tools stage and commit in this same UI-thread call.
                        // No preview card or second user action interrupts a requested edit.
                        ok = false; // A failed checkpoint or commit must return a failed tool result.
                        ok = applyCircuitEdit( { { "token", preview.at( "preview_token" ) } }, result, true );
                        if( ok ) emitEvent( nlohmann::json{ { "type", "circuit_edited" } }.dump() );
                    }
                }
            }
            catch( const KI_PARAM_ERROR& e )   // does not derive from std::exception
            {
                result = e.What();
            }
            catch( const std::exception& e )
            {
                result = wxString::FromUTF8( e.what() );

                if( INDUCTIVE_REPORTS::Unexpected( &e ) )
                    INDUCTIVE_REPORTS::Error( reportApp(), "tool " + name, e.what() );
            }
            catch( ... )
            {
                result = _( "The tool threw an unknown exception." );
                INDUCTIVE_REPORTS::Error( reportApp(), "tool " + name, "unknown exception" );
            }
        }

        // An inspection tool answers with a listing -- every footprint, every net --
        // which belongs in the model's context but not in a one-line trace row.
        // The row gets a summary; the model gets the whole thing, bounded.
        // Fenced for the same reason the document summary is, and it was an
        // oversight that it was not.  The summary is capped at sixty parts;
        // everything else the model learns about the board arrives here --
        // list_footprints returns every value and reference, list_nets returns
        // every net name, get_selection returns whatever the user clicked. All of
        // it is text stored in the board file, and a board can come from anywhere.
        //
        // The nonce is stripped from the content first: a fence the fenced text
        // can close is not a fence.
        const std::string nonce = contentNonce();

        result.Replace( wxString::FromUTF8( nonce ), wxS( "[redacted]" ) );

        std::string full = std::string( result.utf8_str() );

        if( full.size() > MAX_TOOL_RESULT_BYTES )
            full = truncateUtf8( full, MAX_TOOL_RESULT_BYTES ) + "\n...(truncated)";

        full = "--- BEGIN TOOL OUTPUT " + nonce + " ---\n" + full + "\n--- END TOOL OUTPUT "
               + nonce + " ---";

        emitEvent( nlohmann::json{
                { "type", "trace" },
                { "verb", !ok ? "Failed" : ( declared && declared->mutates ? "Edited" : "Read" ) },
                { "object", traceSummary( result ) },
                { "fail", !ok } }
                           .dump() );

        nlohmann::json response = {
            { "name", name },
            { "response", { { ok ? "result" : "error", full } } }
        };

        // The model tags each call with an id; echoing it back pairs the response to
        // the right call when the model asks for several at once.
        if( call.contains( "id" ) )
            response["id"] = call["id"];

        responses.push_back( { { "functionResponse", response } } );
    }

    if( responses.empty() )
        return false;

    // Re-checked here, not only at the top of the loop.  A cancellation can
    // dispatch during the *last* tool call, after the final iteration's check has
    // already passed -- and stopTurn has by then truncated away the model turn
    // holding the matching functionCalls. Appending these responses would leave
    // the history with answers to questions it no longer contains, which the API
    // rejects on every subsequent request for the rest of the session.
    if( !m_state->alive.load() || aTurn != m_state->turnId.load() )
        return false;

    m_history.push_back(
            nlohmann::json{ { "role", "user" }, { "parts", responses } }.dump() );

    // The reply so far described what it was about to do; the real answer follows.
    emitEvent( nlohmann::json{ { "type", "text-reset" } }.dump() );
    return true;
}


void INDUCTIVE_PANEL::emitEvent( const std::string& aJson )
{
    // Before the page check: a turn is history even if the page is reloading.
    recordEvent( aJson );

    // Billing and the chat list are shown by the navigation pane and nowhere else.
    // The settings and the account go to both: the chat page takes its defaults
    // from one, and from the other whether it may send at all. This runs for every streamed word of a reply, so a cheap text
    // match comes first (nlohmann writes "type":"…" with no spaces, and a quote
    // inside a string is escaped), and only a hit is parsed -- to read the event's
    // own type, since a restored chat nests turn events that carry types too.
    std::string type;

    for( const char* candidate : { "auth", "billing", "history", "history_error", "settings" } )
    {
        if( aJson.find( std::string( "\"type\":\"" ) + candidate + "\"" ) != std::string::npos )
        {
            const nlohmann::json ev = nlohmann::json::parse( aJson, nullptr, false );

            if( ev.is_object() && ev.contains( "type" ) && ev["type"].is_string() )
                type = ev["type"].get<std::string>();

            break;
        }
    }

    const bool bothPages = type == "settings" || type == "auth";
    const bool navOnly = type == "billing" || type == "history" || type == "history_error";

    if( type == "auth" )
        m_lastAuthEvent = aJson;

    if( ( navOnly || bothPages ) && m_nav )
        m_nav->Deliver( aJson );

    if( navOnly || !m_webView )
        return;

    // A JSON document is a JavaScript literal, so this needs no further escaping.
    m_webView->RunScriptAsync( wxString::Format( wxS( "window.inductiveDeliver(%s);" ),
                                                 wxString::FromUTF8( aJson ) ) );
}


void INDUCTIVE_PANEL::ShowMessage( const wxString& aTitle, const wxString& aDetail )
{
    if( !m_webView )
        return;

    m_webView->SetPage(
            wxString::Format(
                    wxS( "<html><body style='background:#000;color:#a0a0a0;"
                         "font:13px -apple-system,sans-serif;padding:18px;line-height:1.6'>"
                         "<b style='color:#ededed'>%s</b>"
                         "<pre style='white-space:pre-wrap;font:11.5px ui-monospace,Menlo,monospace;"
                         "color:#666;margin-top:10px'>%s</pre>"
                         "</body></html>" ),
                    htmlEscape( aTitle ), htmlEscape( aDetail ) ),
            wxEmptyString );
}


void INDUCTIVE_PANEL::onError( wxWebViewEvent& aEvent )
{
    wxLogTrace( wxS( "INDUCTIVE" ), wxS( "web view error: %s" ), aEvent.GetString() );
}


bool INDUCTIVE_PANEL::applyCircuitEdit( const nlohmann::json& aArgs, wxString& aResult, bool aDirect )
{
    if(!m_workflowSnapshot) throw std::runtime_error("Native checkpoint support is unavailable.");
    // Validate the exact token/document before creating any journal entry.
    wxString summaryText;
    if(!m_circuitEditHandler("summary",aArgs,summaryText)) throw std::runtime_error("Refresh this preview before applying.");
    auto summary=nlohmann::json::parse(summaryText.ToStdString());
    INDUCTIVE_REVIEW::CHANGE_STORE store(currentProject());auto stepLock=store.BeginStep();auto state=store.Get();
    auto snapshot=m_workflowSnapshot(true);auto checkpoint=store.Checkpoint(snapshot.at("files"));
    state=store.Update(state.at("revision"),[&](auto& j){
        j={{"version",1},{"revision",j.at("revision")},{"id",aArgs.at("token")},{"stage","applying"},
           {"direct",aDirect},{"summary",summary},{"schematic",snapshot.at("document")},{"checkpoint",checkpoint.ToStdString()},
           {"before_fingerprint",snapshot.at("fingerprint")},{"recovery_points",nlohmann::json::array({{{"path",checkpoint.ToStdString()},{"label","Before schematic change"},{"at",INDUCTIVE_REVIEW::Now()}}})},{"events",nlohmann::json::array()}};
    });
    bool ok = false;
    try {ok=m_circuitEditHandler("apply",aArgs,aResult);}
    catch(...) {
        try {store.Update(state.at("revision"),[](auto& j){j["stage"]="apply_failed";});}catch(...){}
        throw;
    }
    // Once committed, journal failures must never misreport an applied edit as unapplied.
    try {
        auto after=m_workflowSnapshot(false);
        store.Update(state.at("revision"),[&](auto& j){j["stage"]=ok?"applied":"apply_failed";
            j["sch_fingerprint"]=after.at("fingerprint");j["message"]=aResult.ToStdString();});
    } catch(...) {aResult+=" Its journal could not be updated. Inspect the native document before continuing; the pre-step checkpoint is retained.";}
    aResult+="\nNative schematic checkpoint: "+checkpoint;
    return ok;
}

// A netlist published outside the inline handoff (/pcb, or the model's own
// prepare_pcb after a direct edit) still hands the latest change to the PCB.
void INDUCTIVE_PANEL::notePublished( wxString& aResult )
{
    if( !m_workflowSnapshot || editorKind() != "sch" )
        return;

    nlohmann::json snapshot, state;
    bool           stale = false;

    try
    {
        INDUCTIVE_REVIEW::CHANGE_STORE store( currentProject() );
        snapshot = m_workflowSnapshot( false );
        wxFileName net( wxString::FromUTF8( snapshot.at( "document" ).get<std::string>() ) );
        net.SetExt( "inductive.net" );
        state = INDUCTIVE_REVIEW::PublishHandoff( store, snapshot.at( "fingerprint" ).get<std::string>(),
                                                  net.GetFullPath(), aResult.ToStdString(), stale );
    }
    catch( ... )
    {
        // An unsaved project has no journal; the netlist is published either way.
        return;
    }

    if( stale )
        aResult += "\nThe schematic changed after the last recorded change; in the PCB editor, "
                   "/layout or /sync brings the board up to date.";

    if( !state.is_null() )
        emitEvent( nlohmann::json{ { "type", "workflow_view" }, { "editor", editorKind() },
                                   { "project", currentProject().ToStdString() },
                                   { "snapshot", snapshot }, { "state", state } }.dump() );
}

void INDUCTIVE_PANEL::addAttachmentTools()
{
    if( m_circuitEditHandler )
        m_tools.push_back( { "edit_circuit", true } );
    m_tools.push_back( { "read_datasheet", false } );
    m_tools.push_back( { "search_my_parts", false } );
    m_tools.push_back( { "get_my_part", false } );
    m_tools.push_back( { "save_to_my_parts", true } );
    m_tools.push_back( { "use_my_part", true } );
    m_tools.push_back( { "search_parts", false } );
    m_tools.push_back( { "source_bom", true } );
    m_tools.push_back( { "list_attachments", false } );
    m_tools.push_back( { "read_attachment", false } );
    m_tools.push_back( { "import_attachment", true } );
}

bool INDUCTIVE_PANEL::attachmentTool( const std::string& aName, const nlohmann::json& aArgs,
                                      wxString& aResult )
{
    if( aName == "list_attachments" )
    {
        auto list = nlohmann::json::array();
        for( const auto& [id, file] : m_attachments )
        {
            auto entry = INDUCTIVE_DATASHEETS::Summary( file );
            entry["attachment_id"] = id;
            if( file.contains( "import_result" ) ) entry["import_result"] = file["import_result"];
            list.push_back( entry );
        }
        aResult = wxString::FromUTF8( list.dump() );
        return true;
    }
    const std::string id = aArgs.value( "attachment_id", std::string() );
    auto it = m_attachments.find( id );
    if( it == m_attachments.end() )
        throw std::runtime_error( "Attachment unavailable. Ask the user to attach the file again." );
    auto& file = it->second;
    if( INDUCTIVE_DATASHEETS::IsPDF(file) )
    {
        if( aName == "read_attachment" ) return readDatasheetTool({{"attachment_id",id}},aResult);
        throw std::runtime_error("A PDF is reference data. Use read_datasheet or save_to_my_parts; it is not a circuit to import.");
    }
    if( aName == "read_attachment" )
    {
        std::string text;
        if( INDUCTIVE_ATTACHMENTS::IsBundle( file ) )
        {
            // The symbol library first, then each footprint, as one readable text.
            for( const auto& piece : INDUCTIVE_ATTACHMENTS::BundlePieces( file ) )
                text += "; ---- " + piece["name"].get<std::string>() + "\n" + piece["text"].get<std::string>() + "\n";
        }
        else
            text = file.at( "text" ).get<std::string>();
        int offset = aArgs.value( "offset", 0 );
        if( offset < 0 || (size_t) offset > text.size() )
            throw std::runtime_error( "Attachment offset is out of range." );
        size_t end = std::min( text.size(), (size_t) offset + 12000 );
        while( end < text.size() && ( (unsigned char) text[end] & 0xc0 ) == 0x80 ) --end;
        if( (size_t) offset < text.size() && ( (unsigned char) text[offset] & 0xc0 ) == 0x80 )
            throw std::runtime_error( "Offset must start on a UTF-8 character boundary." );
        nlohmann::json result = { { "name", file["name"] }, { "text", text.substr( offset, end - offset ) },
                                  { "next_offset", end < text.size() ? nlohmann::json( end ) : nlohmann::json() } };
        aResult = wxString::FromUTF8( result.dump() );
        return true;
    }
    if( m_readyCheck && !m_readyCheck( aResult ) ) return false;
    if( currentProject() != m_chatProject )
        throw std::runtime_error( "The active project changed. Start a new chat and reattach the file." );
    if( currentProject().IsEmpty() || !wxFileName::FileExists( currentProject() ) )
        throw std::runtime_error( "Save the KiCad project before importing attached files." );
    if( file.contains( "import_result" ) )
    {
        aResult = wxString::FromUTF8( file["import_result"].get<std::string>() )
                  + "\nAlready imported. If the insertion was undone, redo it in KiCad.";
        return true;
    }
    if( INDUCTIVE_ATTACHMENTS::IsBundle( file ) )
    {
        const auto imported = ImportPartBundle( currentProject(), file, id );
        aResult = wxString::FromUTF8( imported.dump() );
        file["import_result"] = aResult.ToStdString();
        return true;
    }
    auto summary = INDUCTIVE_ATTACHMENTS::Inspect( file );
    bool ok = false;
    if( summary["kind"] == "symbol_library" )
    {
        auto imported = ImportSymbolAttachment( currentProject(), file, id );
        aResult = wxString::FromUTF8( imported.dump() );
        ok = true;
    }
    else if( summary["kind"] == "schematic" )
    {
        if( !m_attachmentHandler ) throw std::runtime_error( "Import schematics in the schematic editor." );
        ok = m_attachmentHandler( file, true, aResult );
    }
    else
    {
        const wxString libraryId = ImportFootprintAttachment( currentProject(), file, id );
        aResult = "Imported footprint as " + libraryId
                  + ". Assign this exact id to the matching schematic symbol, or place it on the PCB. "
                    "Original file preserved; external 3D model files are not included. "
                    "Library files are project-local and remain after undo.";
        ok = true;
    }
    if( ok ) file["import_result"] = aResult.ToStdString();
    return ok;
}

wxString INDUCTIVE_PANEL::ImportFootprintAttachment( const wxString& aProject,
                                                     const nlohmann::json& aFile,
                                                     const std::string& aId )
{
    if( INDUCTIVE_ATTACHMENTS::Inspect( aFile )["kind"] != "footprint" )
        throw std::runtime_error( "Expected a footprint attachment." );
    if( aId.empty() || aId.find_first_not_of( "0123456789abcdef-" ) != std::string::npos )
        throw std::runtime_error( "Invalid attachment id." );
    if( !wxFileName::FileExists( aProject ) || !wxFileName( aProject ).IsAbsolute() )
        throw std::runtime_error( "Save the project before importing a footprint." );
    auto& manager = Pgm().GetLibraryManager();
    auto table = manager.Table( LIBRARY_TABLE_TYPE::FOOTPRINT, LIBRARY_TABLE_SCOPE::PROJECT );
    if( !table ) throw std::runtime_error( "No project footprint library table is available." );
    const wxString nickname = "Attached_" + wxString::FromUTF8( aId );
    wxString basename;
    for( wxUniChar c : wxFileName( wxString::FromUTF8( aFile["name"].get<std::string>() ) ).GetName() )
        basename += ( wxIsalnum( c ) || c == '-' || c == '_' ) ? c : wxUniChar( '_' );
    if( basename.empty() ) basename = "Part";
    const wxString dirname = nickname + ".pretty";
    const wxString directory = wxFileName( aProject ).GetPathWithSep() + dirname;
    const wxString path = wxFileName( directory, basename + ".kicad_mod" ).GetFullPath();
    bool matchingRow = false;
    for( const auto& row : ( *table )->Rows() )
        if( row.Nickname() == nickname && row.URI() == "${KIPRJMOD}/" + dirname && row.Type() == "KiCad" ) matchingRow = true;
    if( wxDirExists( directory ) )
    {
        if( INDUCTIVE_PARTS::Read( path ) != aFile["text"].get<std::string>() || !matchingRow )
            throw std::runtime_error( "An existing project library conflicts with this saved part." );
        manager.ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT, { LIBRARY_TABLE_TYPE::FOOTPRINT } );
        return nickname + ":" + basename;
    }
    if( ( *table )->HasRow( nickname ) ) throw std::runtime_error( "A project library already uses this name. Resolve its missing or conflicting asset first." );
    if( !wxMkdir( directory, 0700 ) ) throw std::runtime_error( "Could not create the project footprint library." );
    try
    {
        wxFFile output( path, "wb" );
        const std::string content = aFile["text"];
        if( !output.IsOpened() || output.Write( content.data(), content.size() ) != content.size()
            || !output.Close() ) throw std::runtime_error( "Could not save the attached footprint." );
        auto& row = ( *table )->InsertRow();
        row.SetNickname( nickname );
        row.SetURI( "${KIPRJMOD}/" + dirname );
        row.SetType( "KiCad" );
        row.SetDescription( wxString::FromUTF8( aFile["name"].get<std::string>() ) );
        const auto saved = ( *table )->Save();
        if( !saved )
        {
            ( *table )->Rows().pop_back();
            throw std::runtime_error( "Could not save the project's footprint library table." );
        }
    }
    catch( ... )
    {
        if( wxFileExists( path ) ) wxRemoveFile( path );
        wxRmdir( directory );
        throw;
    }
    manager.ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT, { LIBRARY_TABLE_TYPE::FOOTPRINT } );
    return nickname + ":" + basename;
}

void INDUCTIVE_PANEL::sendParts()
{
    INDUCTIVE_PARTS::STORE store( wxFileName( ChatsRoot() ).GetPath() + "/parts" );
    emitEvent( nlohmann::json{ { "type", "parts_list" }, { "parts", store.List() } }.dump() );
}

// Explicit UI actions share the same native tool handlers as the agent, with
// project, revision and live-document checks at every boundary.
void INDUCTIVE_PANEL::workflowMessage(const nlohmann::json& msg)
{
    using namespace INDUCTIVE_REVIEW;
    const auto type=msg.at("type").get<std::string>();
    if(!m_workflowSnapshot) throw std::runtime_error("This editor does not support engineering changes.");
    if(m_busy) throw std::runtime_error("Wait for the assistant or stop its current turn first.");
    CHANGE_STORE store(currentProject());
    if(type=="workflow_history" || type=="workflow_recover") {
        if(msg.value("project","")!=currentProject().ToStdString())throw std::runtime_error("The project changed. Reopen history.");
        if(type=="workflow_history") {emitEvent(json{{"type","workflow_history"},{"project",currentProject().ToStdString()},{"entries",store.History()}}.dump());return;}
        auto recovery=store.Recover(msg.at("checkpoint").get<std::string>());
        // Launch a separate project manager; never reload either live editor.
        auto parent=std::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToStdString()).parent_path();
        wxString executable;
        for(int level=0;level<7 && !parent.empty();++level,parent=parent.parent_path()) {
            for(const auto& candidate:{parent/"Inductive",parent/"Contents"/"MacOS"/"Inductive"})
                if(std::filesystem::is_regular_file(candidate)){executable=wxString::FromUTF8(candidate.string());break;}
            if(!executable.empty())break;
        }
        const wxString project=wxString::FromUTF8(recovery.at("project").get<std::string>());
        bool opened=false;
        if(!executable.empty()){const wxChar* argv[]={executable.wc_str(),project.wc_str(),nullptr};opened=wxExecute(argv,wxEXEC_ASYNC)>0;}
        recovery["type"]="workflow_recovered";recovery["opened"]=opened;emitEvent(recovery.dump());return;
    }
    auto stepLock=type=="workflow_get"?nullptr:store.BeginStep();
    auto state=store.Get();auto snapshot=m_workflowSnapshot(false);
    if(type=="workflow_get") {
        emitEvent(json{{"type","workflow_view"},{"editor",editorKind()},{"project",currentProject().ToStdString()},
                       {"snapshot",snapshot},{"state",state}}.dump());return;
    }
    if(msg.value("project","")!=currentProject().ToStdString() || msg.value("revision",-1)!=state.at("revision").get<int>()
       || msg.value("fingerprint","")!=snapshot.at("fingerprint").get<std::string>())
        throw std::runtime_error("The project or document changed. Refresh the inline handoff before continuing.");
    const bool sch=editorKind()=="sch";
    const auto action=msg.value("action","");
    const bool check=action=="check";
    if(!check && !modeAllowsMutation(wxString::FromUTF8(msg.value("mode","ask"))))
        throw std::runtime_error("Switch to Agent mode before changing the project.");
    const auto stage=state.value("stage","");
    wxString tool;json args=json::object();
    wxFileName document(wxString::FromUTF8(snapshot.at("document").get<std::string>()));
    if(!document.IsAbsolute()) throw std::runtime_error("Save this native document in the project first.");
    // Manual PCB refinements are an explicit new baseline, never inferred from
    // refresh or a DRC run. The same schematic-to-board identity still applies.
    if(action=="accept_manual" && !sch && (stage=="synced" || stage=="placed" || stage=="routed")) {
        wxFileName expected(wxString::FromUTF8(state.at("schematic").get<std::string>()));expected.SetExt("kicad_pcb");
        if(expected.GetFullPath()!=document.GetFullPath()) throw std::runtime_error("Open the PCB matching this change before accepting manual edits.");
        auto checkpoint=store.Checkpoint(m_workflowSnapshot(true).at("files"));
        state=store.Update(state.at("revision"),[&](auto& j){
            j["pcb_fingerprint"]=snapshot.at("fingerprint");j["stage"]="placed";
            j["message"]="Accepted the current PCB as the baseline after your manual edits. Inspect placement, then route and run DRC.";
            j["manual_checkpoint"]=checkpoint.ToStdString();if(!j.contains("recovery_points"))j["recovery_points"]=json::array();j["recovery_points"].push_back({{"path",checkpoint.ToStdString()},{"label","Before manual baseline"},{"at",Now()}});
            if(!j.contains("events"))j["events"]=json::array();
            j["events"].push_back({{"action",action},{"ok",true},{"at",Now()},{"message",j["message"]}});
            if(j["events"].size()>30)j["events"].erase(j["events"].begin());
        });
        emitEvent(json{{"type","workflow_view"},{"editor",editorKind()},{"project",currentProject().ToStdString()},
                       {"snapshot",snapshot},{"state",state}}.dump());return;
    }
    if(check) tool=sch?"run_erc":"run_drc";
    else if(action=="publish" && sch && (stage=="applied" || stage=="published")) {
        if(state.value("sch_fingerprint","")!=snapshot.at("fingerprint").get<std::string>())
            throw std::runtime_error("The schematic changed after Apply. Prepare a fresh engineering change before publishing this handoff.");
        tool="prepare_pcb";
    }
    else if(action=="sync" && !sch && stage=="published") {
        wxFileName expected(wxString::FromUTF8(state.at("schematic").get<std::string>()));expected.SetExt("kicad_pcb");
        if(expected.GetFullPath()!=document.GetFullPath()) throw std::runtime_error("Open the PCB matching this schematic before synchronizing.");
        wxFileName net(document);net.SetExt("inductive.net");
        if(Hash(INDUCTIVE_PARTS::Read(net.GetFullPath(),16*1024*1024))!=state.value("netlist_hash",""))
            throw std::runtime_error("The published netlist changed. Publish a fresh handoff from the schematic.");
        tool="sync_schematic";
    }
    else if(action=="place" && !sch && stage=="synced") {
        json refs=json::array();for(const auto& d:state.at("summary").value("diff",json::array()))
            if(d.value("kind","")=="add") refs.push_back(d.at("reference"));
        if(refs.empty()) throw std::runtime_error("This change added no components. Inspect placement manually.");
        tool="auto_place";args={{"scope","unplaced"},{"references",refs},{"preserve_copper",true}};
    }
    else if(action=="route" && !sch && (stage=="synced" || stage=="placed")) {tool="route";args={{"rip_up",false}};}
    else throw std::runtime_error("This step is not available here. Refresh and follow the next action in the inline handoff.");
    if(!sch && action!="sync" && !check && state.value("pcb_fingerprint","")!=snapshot.at("fingerprint").get<std::string>())
        throw std::runtime_error("The PCB changed since the previous step. Continue manually or prepare a new change; existing edits are retained.");
    if(!m_toolHandler) throw std::runtime_error("The native tool handler is unavailable.");
    wxString checkpoint;
    if(!check && !sch) checkpoint=store.Checkpoint(m_workflowSnapshot(true).at("files"));
    // Reserve the journal before touching the document. An interrupted step is
    // never silently rerun when the editor reopens.
    auto previous=stage;
    state=store.Update(state.at("revision"),[&](auto& j){j["stage"]=check?previous:"running_"+action;
        if(!checkpoint.IsEmpty()) {
            j["pcb_checkpoint"]=checkpoint.ToStdString();
            if(!j.contains("recovery_points"))j["recovery_points"]=json::array();
            j["recovery_points"].push_back({{"path",checkpoint.ToStdString()},{"label","Before PCB "+action},{"at",Now()}});
        }});
    wxString result;bool ok=false;
    try {ok=m_toolHandler(tool,wxString::FromUTF8(args.dump()),result);}
    catch(const std::exception& e){result=wxString::FromUTF8(e.what());}
    catch(const KI_PARAM_ERROR& e){result=e.What();}
    catch(...){result="Native step could not finish. Inspect the document before continuing.";}
    json after;
    try {
        after=m_workflowSnapshot(false);
        const std::string next=action=="publish"?"published":action=="sync"?"synced":action=="place"?"placed":"routed";
        state=store.Update(state.at("revision"),[&](auto& j){
            const bool unchanged=after.at("fingerprint")==snapshot.at("fingerprint");
            j["stage"]=check?previous:ok?next:unchanged?previous:"needs_attention";
            j["last_step"]={{"action",action},{"completed",ok}};
            j["message"]=result.ToStdString();
            // A check after manual editing cannot authorize a stale handoff.
            const auto key=sch?"sch_fingerprint":"pcb_fingerprint";
            if(!check || j.value(key,"")==snapshot.at("fingerprint").get<std::string>())j[key]=after.at("fingerprint");
            if(!j.contains("events"))j["events"]=json::array();
            j["events"].push_back({{"action",action},{"ok",ok},{"at",Now()},{"message",result.ToStdString()}});
            if(j["events"].size()>30)j["events"].erase(j["events"].begin());
            if(check)j["last_check"]={{"editor",editorKind()},{"fingerprint",after.at("fingerprint")},
                                     {"at",Now()},{"message",result.ToStdString()},{"completed",ok}};
            if(ok && action=="publish") {
                wxFileName net(document);net.SetExt("inductive.net");
                j["netlist_hash"]=Hash(INDUCTIVE_PARTS::Read(net.GetFullPath(),16*1024*1024));
            }
        });
    } catch(const std::exception& e) {
        throw std::runtime_error("Native step returned: "+result.ToStdString()+
            "\nIts journal could not be finalized: "+e.what()+
            "\nInspect the native document before retrying. The pre-step checkpoint is retained.");
    } catch(...) {
        throw std::runtime_error("Native step returned: "+result.ToStdString()+
            "\nCould not finalize its journal. Inspect the document; the pre-step checkpoint is retained.");
    }

    emitEvent(json{{"type","workflow_view"},{"editor",editorKind()},{"project",currentProject().ToStdString()},
                   {"snapshot",after},{"state",state}}.dump());
}

nlohmann::json INDUCTIVE_PANEL::reviewSnapshot()
{
    if(!m_reviewHandler) throw std::runtime_error("Circuit review is available in the schematic editor.");
    auto snapshot=m_reviewHandler("snapshot",nlohmann::json::object());
    if(snapshot.at("project").get<std::string>()!=currentProject().ToStdString()) throw std::runtime_error("The active project changed. Open review again.");
    return snapshot;
}

nlohmann::json INDUCTIVE_PANEL::reviewSources(const nlohmann::json& state)
{
    using namespace INDUCTIVE_REVIEW;
    INDUCTIVE_PARTS::STORE parts(wxFileName(ChatsRoot()).GetPath()+"/parts");
    json sources=json::object();
    for(const auto& b:state.value("bindings",json::object()))
    {
        const auto id=b.value("source_id",""); if(sources.contains(id)) continue;
        if(sources.size()>=128) throw std::runtime_error("Review at most 128 distinct datasheets per project.");
        try
        {
            auto source=parts.Get(id,true);
            if(source.value("kind","")!="datasheet" || source.value("archived",false)) throw std::runtime_error("PDF was removed or is not a datasheet.");
            auto summary=INDUCTIVE_DATASHEETS::Inspect(source);
            if(Hash(INDUCTIVE_DATASHEETS::Data(source))!=source.value("pdf_sha256","")) throw std::runtime_error("Original PDF changed or has no saved specifications.");
            source["specifications"]=Facts(source.value("specifications",json::array()),summary.at("pages"),true);
            source.erase("url");sources[id]=source;
        }
        catch(const std::exception& e) {sources[id]={{"id",id},{"error",e.what()}};}
    }
    return sources;
}

nlohmann::json INDUCTIVE_PANEL::reviewInventory()
{
    INDUCTIVE_PARTS::STORE parts(wxFileName(ChatsRoot()).GetPath()+"/parts");
    auto inventory=parts.List();size_t count=0;
    for(auto& p:inventory)if(p.value("kind","")=="datasheet") {
        if(++count>512)throw std::runtime_error("Automatic setup supports up to 512 saved PDFs. Narrow your My Parts library before setup.");
        try {auto source=parts.Get(p.at("id"));p["specifications"]=INDUCTIVE_REVIEW::Facts(source.value("specifications",nlohmann::json::array()),source.at("summary").at("pages"),true);}
        catch(const std::exception& e) {p["specifications"]=nlohmann::json::array();p["setup_error"]=e.what();}
    }
    return inventory;
}

void INDUCTIVE_PANEL::reviewMessage(const nlohmann::json& msg)
{
    using namespace INDUCTIVE_REVIEW;
    const auto type=msg.at("type").get<std::string>();
    if(type!="review_get" && msg.value("project","")!=currentProject().ToStdString()) throw std::runtime_error("The project changed. Reopen circuit review.");
    if(m_busy && type!="review_get" && type!="review_check") throw std::runtime_error("Wait for the assistant before changing or running review.");
    auto snapshot=reviewSnapshot(); STORE store(currentProject());auto state=store.Get();
    if(type=="review_get" || type=="review_check")
    {
        auto sources=reviewSources(state);const auto fingerprint=Fingerprint(snapshot,state,sources);
        auto report=state.value("last_report",json::object());
        if(m_reviewReport.value("fingerprint","")==fingerprint) report=m_reviewReport;
        const bool stale=report.empty() || report.value("fingerprint","")!=fingerprint;
        if(type=="review_check") {emitEvent(json{{"type","review_freshness"},{"project",snapshot["project"]},{"fingerprint",fingerprint},{"stale",stale},{"revision",state["revision"]}}.dump());return;}
        INDUCTIVE_PARTS::STORE parts(wxFileName(ChatsRoot()).GetPath()+"/parts");json choices=json::array();
        for(const auto& p:parts.List()) if(p["kind"]=="datasheet") choices.push_back(p);
        auto setup=Setup(snapshot,state,reviewInventory());state.erase("last_report");
        emitEvent(json{{"type","review_view"},{"snapshot",snapshot},{"state",state},{"sources",sources},{"datasheets",choices},{"setup",setup},{"report",report},{"stale",stale}}.dump());return;
    }
    if(type=="review_focus") {m_reviewHandler("focus",msg);return;}
    if(msg.value("design_fingerprint","")!=snapshot.at("fingerprint").get<std::string>()) throw std::runtime_error("The schematic changed. Refresh review before continuing.");
    const int revision=msg.at("revision");
    if(type=="review_requirements")
    {
        auto rails=Rails(msg.at("rails"));std::set<std::string> nets;
        for(const auto& c:snapshot["components"])for(const auto& p:c["pins"])if(!p.value("net","").empty())nets.insert(p["net"]);
        for(const auto& r:rails)if(!nets.count(r.at("net")) || !nets.count(r.at("reference_net")))throw std::runtime_error("Choose actual full net names from the live schematic.");
        store.Update(revision,"requirements",[&](json& s){s["rails"]=rails;});
    }
    else if(type=="review_setup_apply")
    {
        if(msg.value("setup_fingerprint","")!=Setup(snapshot,state,reviewInventory()).at("fingerprint").get<std::string>())throw std::runtime_error("Setup suggestions changed. Refresh before confirming.");
        const auto& entries=msg.at("bindings");if(!entries.is_array() || entries.empty() || entries.size()>1000)throw std::runtime_error("Select 1 to 1,000 bindings to confirm.");
        json staged=json::object(),verified=json::object();std::set<std::string> ids;
        INDUCTIVE_PARTS::STORE parts(wxFileName(ChatsRoot()).GetPath()+"/parts");
        std::set<std::string> sourceIds;
        for(const auto& entry:entries) {
            const auto cid=Text(entry,"component_id",80),sid=Text(entry,"source_id",80);const json* component=nullptr;
            if(!ids.insert(cid).second)throw std::runtime_error("A component was selected twice.");
            for(const auto& c:snapshot.at("components"))if(c["id"]==cid)component=&c;
            if(!component || component->value("excluded",false))throw std::runtime_error("A selected component is unavailable.");
            if(!entry.value("confirmed",false))throw std::runtime_error("Confirm applicability for every selected component.");
            if(!verified.contains(sid)) {
                auto source=parts.Get(sid,true);auto summary=INDUCTIVE_DATASHEETS::Inspect(source);
                if(Hash(INDUCTIVE_DATASHEETS::Data(source))!=source.value("pdf_sha256",""))throw std::runtime_error("The original PDF changed. Save reviewed specifications again.");
                source["specifications"]=Facts(source.value("specifications",json::array()),summary.at("pages"),true);source.erase("url");verified[sid]=source;
            }
            const auto& source=verified.at(sid);
            if(entry.value("source_digest","")!=SourceDigest(source))throw std::runtime_error("Source facts changed. Refresh setup before confirming.");
            bool approved=false;for(const auto& f:source["specifications"])approved|=f.value("approved",false);
            if(!approved)throw std::runtime_error("Approve source facts before binding a component.");
            staged[cid]=Binding(*component,source,Text(entry,"context",1000));
        }
        auto merged=state.value("bindings",json::object());for(const auto& [id,b]:staged.items())merged[id]=b;
        for(const auto& b:merged)sourceIds.insert(b.value("source_id",""));
        if(sourceIds.size()>128)throw std::runtime_error("Review supports at most 128 distinct bound PDFs.");
        store.Update(revision,"automatic_setup_bindings",[&](json& s){for(const auto& [id,b]:staged.items())s["bindings"][id]=b;});
    }
    else if(type=="review_bind")
    {
        const auto cid=Text(msg,"component_id",80),sid=Text(msg,"source_id",80,false); const json* component=nullptr;
        for(const auto& c:snapshot["components"])if(c["id"]==cid)component=&c;
        if(!component)throw std::runtime_error("Component is no longer present.");
        json binding;
        if(!sid.empty())
        {
            if(!msg.value("confirmed",false))throw std::runtime_error("Confirm the exact device, variant and all relevant operating conditions.");
            INDUCTIVE_PARTS::STORE parts(wxFileName(ChatsRoot()).GetPath()+"/parts");auto source=parts.Get(sid,true);
            auto summary=INDUCTIVE_DATASHEETS::Inspect(source);
            if(Hash(INDUCTIVE_DATASHEETS::Data(source))!=source.value("pdf_sha256",""))throw std::runtime_error("The PDF changed. Save reviewed specifications again.");
            source["specifications"]=Facts(source.value("specifications",json::array()),summary.at("pages"),true);
            bool approved=false;for(const auto& f:source["specifications"])approved|=f.value("approved",false);
            if(!approved)throw std::runtime_error("Approve at least one specification against the PDF before binding.");
            binding=Binding(*component,source,Text(msg,"context",1000));
        }
        store.Update(revision,"binding",[&](json& s){if(sid.empty())s["bindings"].erase(cid);else s["bindings"][cid]=binding;});
    }
    else if(type=="review_run" || type=="review_export" || type=="review_exception")
    {
        auto sources=reviewSources(state);auto report=Run(snapshot,state,sources);report["created_at"]=Now();report["audit"]=state.value("audit",json::array());
        if(type=="review_exception")
        {
            if(msg.value("fingerprint","")!=report["fingerprint"].get<std::string>())throw std::runtime_error("The finding is stale. Rerun review before accepting an exception.");
            const auto id=Text(msg,"finding_id",80),reason=Text(msg,"reason",1000);bool found=false;
            for(const auto& f:report["findings"]) if(f["id"]==id && f["status"]=="supported_violation")found=true;
            if(!found)throw std::runtime_error("Only a current supported violation can be accepted as an exception.");
            store.Update(revision,"exception",[&](json& s){s["exceptions"][id]={{"reason",reason},{"at",Now()},{"fingerprint",report["fingerprint"]},{"actor","local engineer"}};auto updated=Run(snapshot,s,sources);updated["created_at"]=Now();s["last_report"]=updated;});
            m_reviewReport=nlohmann::json::object();
        }
        else
        {
            store.Update(revision,type=="review_run"?"run":"export",[&](json& s){s["last_report"]=report;});m_reviewReport=report;
            if(type=="review_export") emitEvent(json{{"type","review_exported"},{"path",store.Export(report).ToStdString()}}.dump());
        }
    }
    else throw std::runtime_error("Unknown review action.");
    reviewMessage(json{{"type","review_get"}});
}

bool INDUCTIVE_PANEL::reviewTool(const std::string& name,const nlohmann::json& args,wxString& result)
{
    using namespace INDUCTIVE_REVIEW;json out;
    if(name=="draft_datasheet_facts")
    {
        const auto id=Text(args,"datasheet_id",80);
        const auto it=m_turnDatasheets.find(id);if(it==m_turnDatasheets.end())throw std::runtime_error("Load this saved PDF with read_datasheet in this turn before extracting facts.");
        INDUCTIVE_PARTS::STORE parts(wxFileName(ChatsRoot()).GetPath()+"/parts");auto source=parts.Get(id,true);
        if(source.value("archived",false))throw std::runtime_error("This PDF was removed.");
        auto summary=INDUCTIVE_DATASHEETS::Inspect(source);
        if(INDUCTIVE_DATASHEETS::Data(source)!=INDUCTIVE_DATASHEETS::Data(it->second))throw std::runtime_error("The source changed. Load it again before extracting.");
        auto facts=Facts(args.at("facts"),summary.at("pages"),false);source.erase("url");
        emitEvent(json{{"type","facts_draft"},{"part",source},{"facts",facts}}.dump());
        out={{"status","Draft shown in My Parts. No facts saved or approved; engineer must inspect, edit and approve."},{"count",facts.size()}};
    }
    else
    {
        const int offset=args.value("offset",0),limit=args.value("limit",10);
        if(offset<0 || offset>10000 || limit<1 || limit>10)throw std::runtime_error("Use an offset 0 to 10,000 and page size 1 to 10.");
        auto snapshot=reviewSnapshot();STORE store(currentProject());auto state=store.Get();auto sources=reviewSources(state);
        if(name=="get_review_setup")
        {
            auto setup=Setup(snapshot,state,reviewInventory());
            out={{"design_fingerprint",snapshot["fingerprint"]},{"setup_fingerprint",setup["fingerprint"]},{"components",json::array()},{"component_count",setup["components"].size()},{"missing_rails",setup["missing_rails"]}};
            for(size_t i=offset;i<setup["components"].size() && i<(size_t)(offset+limit);++i) {
                auto c=setup["components"][i];for(auto& candidate:c["candidates"])candidate.erase("applicability");out["components"].push_back(c);
            }
            if((size_t)(offset+limit)<setup["components"].size())out["next_offset"]=offset+limit;
            reviewMessage(json{{"type","review_get"}});
        }
        else if(name=="get_circuit_review")
        {
            auto rails=state.value("rails",json::array());
            out={{"design_fingerprint",snapshot["fingerprint"]},{"rails",json::array()},{"components",json::array()},{"component_count",snapshot["components"].size()},{"requirement_count",rails.size()}};
            const auto id=args.value("component_id","");
            int index=0;for(auto c:snapshot["components"])
            {
                if(!id.empty() && c["id"]!=id)continue;
                if(id.empty() && (index++<offset || index>offset+limit))continue;
                if(id.empty()) {c.erase("pins");c.erase("pads");c.erase("instances");}
                if(state["bindings"].contains(c.at("id").get<std::string>()))c["binding"]=state["bindings"][c.at("id").get<std::string>()];
                out["components"].push_back(c);
            }
            for(size_t i=offset;i<rails.size() && i<(size_t)(offset+limit);++i)out["rails"].push_back(rails[i]);
            if(id.empty() && (size_t)(offset+limit)<std::max(rails.size(),snapshot["components"].size()))out["next_offset"]=offset+limit;
            if(!id.empty() && out["components"].empty())throw std::runtime_error("This native component id is no longer present.");
        }
        else {auto report=Run(snapshot,state,sources);report["created_at"]=Now();m_reviewReport=report;emitEvent(json{{"type","review_result"},{"report",report}}.dump());
            out={{"scope",report["scope"]},{"fingerprint",report["fingerprint"]},{"counts",report["counts"]},{"findings",json::array()}};
            const auto id=args.value("finding_id","");
            int index=0;for(auto f:report["findings"]) {if(!id.empty() && f["id"]!=id)continue;if(id.empty() && (index++<offset || index>offset+limit))continue;if(id.empty())f.erase("evidence");out["findings"].push_back(f);}
            out["finding_count"]=report["findings"].size();if(id.empty() && (size_t)(offset+limit)<report["findings"].size())out["next_offset"]=offset+limit;
            if(!id.empty() && out["findings"].empty())throw std::runtime_error("This finding is no longer present. Reassess the current review.");}
    }
    result=wxString::FromUTF8(out.dump());return true;
}

bool INDUCTIVE_PANEL::partsTool( const std::string& name, const nlohmann::json& args, wxString& result )
{
    INDUCTIVE_PARTS::STORE store( wxFileName( ChatsRoot() ).GetPath() + "/parts" );
    nlohmann::json out;
    if( name == "search_my_parts" )
    {
        out = nlohmann::json::array();
        for( auto entry : store.List( args.value( "query", "" ) ) )
        {
            entry.erase( "summary" );
            out.push_back( entry );
            if( out.size() == 50 ) break;
        }
    }
    else if( name == "get_my_part" ) out = store.Get( args.at( "part_id" ) );
    else if( name == "save_to_my_parts" )
    {
        auto it = m_attachments.find( args.value( "attachment_id", "" ) );
        if( it == m_attachments.end() ) throw std::runtime_error( "Reattach the file before saving it to My Parts." );
        if( INDUCTIVE_ATTACHMENTS::IsBundle( it->second ) )
            throw std::runtime_error( "Part downloads are imported into the project with import_attachment; "
                                      "My Parts does not keep their 3D models yet. Attach the download again to reuse it." );
        out = store.Add( it->second );
        sendParts();
    }
    else
    {
        if( currentProject() != m_chatProject ) throw std::runtime_error( "The project changed during this turn. Ask again in the current project." );
        out = usePart( args.at( "part_id" ) );
    }
    result = wxString::FromUTF8( out.dump() );
    return true;
}

void INDUCTIVE_PANEL::queueDatasheet( const std::string& id, const nlohmann::json& file )
{
    size_t total = 0;
    size_t count = m_turnDatasheets.size() + (m_turnDatasheets.count(id) ? 0 : 1);
    for( const auto& [key,source] : m_turnDatasheets )
        if( key != id ) total += source.at("url").get<std::string>().size() - std::string("data:application/pdf;base64,").size();
    total += file.at("url").get<std::string>().size() - std::string("data:application/pdf;base64,").size();
    for( size_t i = m_turnStart; i < m_history.size(); ++i )
    {
        auto entry = nlohmann::json::parse(m_history[i],nullptr,false);
        if( !entry.is_object() ) continue;
        for( const auto& part : entry.value("parts",nlohmann::json::array()) )
            if( part.contains("inlineData") ) { total += part["inlineData"].value("data","").size(); ++count; }
    }
    if( count > 6 ) throw std::runtime_error("Read up to six images and datasheets per turn.");
    if( total > INDUCTIVE_DATASHEETS::MAX_REQUEST_MEDIA ) throw std::runtime_error("Combined datasheets and images are too large. Read fewer documents per turn.");
    m_turnDatasheets[id] = file;
}

bool INDUCTIVE_PANEL::readDatasheetTool( const nlohmann::json& args, wxString& result )
{
    int count = 0;
    for( const char* key : {"datasheet_id","part_id","attachment_id"} ) if( args.contains(key) ) ++count;
    if( count != 1 ) throw std::runtime_error("Choose exactly one datasheet_id, part_id or attachment_id.");
    nlohmann::json file;
    std::string id;
    if( args.contains("attachment_id") )
    {
        id = args.at("attachment_id");
        auto it = m_attachments.find(id);
        if( it == m_attachments.end() ) throw std::runtime_error("Reattach this PDF or choose its saved My Parts entry.");
        file = it->second;
    }
    else
    {
        INDUCTIVE_PARTS::STORE store(wxFileName(ChatsRoot()).GetPath() + "/parts");
        id = args.contains("part_id") ? args.at("part_id").get<std::string>() : args.at("datasheet_id").get<std::string>();
        auto entry = store.Get(id);
        if( entry.value("archived",false) ) throw std::runtime_error("This entry was removed from My Parts.");
        if( args.contains("part_id") ) id = entry.value("datasheet_id","");
        if( id.empty() ) throw std::runtime_error("This part has no uploaded PDF. Link one in My Parts.");
        file = store.Get(id,true);
        if( file.value("archived",false) ) throw std::runtime_error("This datasheet was removed. Link another PDF in My Parts.");
    }
    auto summary = INDUCTIVE_DATASHEETS::Inspect(file);
    queueDatasheet(id,file);
    summary["source_id"] = id;
    summary["status"] = "Original PDF supplied to the model for this turn, including diagrams and tables.";
    summary["citation_format"] = "[filename.pdf, PDF p. N] (physical pages starting at 1)";
    result = wxString::FromUTF8(summary.dump());
    return true;
}

nlohmann::json INDUCTIVE_PANEL::usePart( const std::string& id )
{
    if( m_readyCheck )
    {
        wxString why;
        if( !m_readyCheck( why ) ) throw std::runtime_error( why.ToStdString() );
    }
    if( currentProject().IsEmpty() || !wxFileName::FileExists( currentProject() ) )
        throw std::runtime_error( "Save the project before using a part." );
    INDUCTIVE_PARTS::STORE store( wxFileName( ChatsRoot() ).GetPath() + "/parts" );
    auto entry = store.Get( id, true );
    if( entry["kind"] == "datasheet" ) throw std::runtime_error("Use Analyze datasheet to read a saved PDF.");
    if( entry.value( "archived", false ) ) throw std::runtime_error( "This part has been removed from My Parts." );
    nlohmann::json file = { { "name", entry["name"] }, { "text", entry["text"] } };
    wxString why;
    if( m_attachmentHandler && !m_attachmentHandler( file, false, why ) )
        throw std::runtime_error( why.ToStdString() );
    nlohmann::json result = { { "part_id", id }, { "name", entry["name"] } };
    if( entry["kind"] == "footprint" )
        result["footprint_id"] = ImportFootprintAttachment( currentProject(), file, id ).ToStdString();
    else if( entry["kind"] == "symbol_library" )
    {
        result.update( ImportSymbolAttachment( currentProject(), file, id ) );
        const std::string paired = entry.value( "footprint_id", "" );
        if( !paired.empty() )
        {
            auto fp = store.Get( paired, true );
            if( fp.value( "archived", false ) ) throw std::runtime_error( "The paired footprint was removed. Choose another in My Parts." );
            nlohmann::json source = { { "name", fp["name"] }, { "text", fp["text"] } };
            if( m_attachmentHandler && !m_attachmentHandler( source, false, why ) )
                throw std::runtime_error( why.ToStdString() );
            result["footprint_id"] = ImportFootprintAttachment( currentProject(), source, paired ).ToStdString();
        }
        const auto selected = entry.value( "symbol_name", "" );
        if( !selected.empty() ) result["selected_symbol_id"] = "Attached_" + id + ":" + selected;
    }
    else
    {
        if( !m_attachmentHandler || !m_attachmentHandler( file, true, why ) )
            throw std::runtime_error( why.empty() ? "Use schematic blocks in the schematic editor." : why.ToStdString() );
        result["message"] = why.ToStdString();
    }
    return result;
}

nlohmann::json INDUCTIVE_PANEL::ImportPartBundle( const wxString& aProject, const nlohmann::json& aFile,
                                                  const std::string& aId )
{
    const auto summary = INDUCTIVE_ATTACHMENTS::InspectBundle( aFile );
    if( !INDUCTIVE_PARTS::ValidId( aId ) ) throw std::runtime_error( "Invalid attachment id." );
    if( !wxFileName::FileExists( aProject ) || !wxFileName( aProject ).IsAbsolute() )
        throw std::runtime_error( "Save the project before importing a part." );
    const auto& b = aFile["bundle"];
    const wxString nickname = "Attached_" + wxString::FromUTF8( aId );
    // The server names the project library @PART_LIB@; it is ours to choose.
    auto local = [&]( std::string text )
    {
        const std::string token = "@PART_LIB@", lib = nickname.ToStdString();
        for( size_t at = text.find( token ); at != std::string::npos; at = text.find( token, at + lib.size() ) )
            text.replace( at, token.size(), lib );
        return text;
    };
    nlohmann::json result = { { "library", nickname.ToStdString() }, { "symbol_ids", nlohmann::json::array() },
                              { "footprint_ids", nlohmann::json::array() }, { "models", nlohmann::json::array() },
                              { "checks", summary["checks"] } };

    const auto footprints = b.value( "footprints", nlohmann::json::array() );
    if( !footprints.empty() )
    {
        auto& manager = Pgm().GetLibraryManager();
        auto  table = manager.Table( LIBRARY_TABLE_TYPE::FOOTPRINT, LIBRARY_TABLE_SCOPE::PROJECT );
        if( !table ) throw std::runtime_error( "No project footprint library table is available." );
        const wxString projectDir = wxFileName( aProject ).GetPathWithSep();
        const wxString dirname = nickname + ".pretty", modelDir = nickname + ".3dshapes";
        bool matchingRow = false;
        for( const auto& row : ( *table )->Rows() )
            if( row.Nickname() == nickname && row.URI() == "${KIPRJMOD}/" + dirname && row.Type() == "KiCad" ) matchingRow = true;
        if( !matchingRow && ( *table )->HasRow( nickname ) )
            throw std::runtime_error( "A project library already uses this name. Resolve its missing or conflicting asset first." );

        // Everything is written to a fresh folder, or must already match exactly.
        std::vector<std::pair<wxString, std::string>> writes;
        for( const auto& fp : footprints )
            writes.push_back( { projectDir + dirname + "/" + wxString::FromUTF8( fp["file"].get<std::string>() ) + ".kicad_mod",
                                local( fp["text"].get<std::string>() ) } );
        for( const auto& model : b.value( "models", nlohmann::json::array() ) )
        {
            wxMemoryBuffer decoded = wxBase64Decode( wxString::FromUTF8( model["data"].get<std::string>() ) );
            std::string data( (const char*) decoded.GetData(), decoded.GetDataLen() );
            const auto start = data.find_first_not_of( " \t\r\n" );
            if( data.empty() || data.size() > INDUCTIVE_ATTACHMENTS::MAX_MODEL || start == std::string::npos
                || data.compare( start, 13, "ISO-10303-21;" ) != 0 || data.find( '\0' ) != std::string::npos )
                throw std::runtime_error( "A 3D model in the part download is not a STEP file." );
            writes.push_back( { projectDir + modelDir + "/" + wxString::FromUTF8( model["file"].get<std::string>() ), data } );
            result["models"].push_back( ( "${KIPRJMOD}/" + modelDir + "/" ).ToStdString() + model["file"].get<std::string>() );
        }
        std::vector<wxString> created;
        try
        {
            for( const wxString& dir : { projectDir + dirname, projectDir + modelDir } )
            {
                if( std::filesystem::is_symlink( dir.ToStdString() ) ) throw std::runtime_error( "A project library folder is a link." );
                if( !wxDirExists( dir ) )
                {
                    if( !wxMkdir( dir, 0700 ) ) throw std::runtime_error( "Could not create the project part library." );
                    created.push_back( dir );
                }
            }
            for( const auto& [path, data] : writes )
            {
                if( wxFileExists( path ) )
                {
                    wxFFile in( path, "rb" );
                    std::string existing( in.IsOpened() ? (size_t) in.Length() : 0, '\0' );
                    if( !in.IsOpened() || in.Read( existing.data(), existing.size() ) != existing.size() || existing != data )
                        throw std::runtime_error( "An existing project library conflicts with this part download." );
                    continue;
                }
                wxTempFile out( path );
                if( !out.IsOpened() || !out.Write( data.data(), data.size() ) || !out.Commit() )
                    throw std::runtime_error( "Could not save the part download into the project." );
            }
            if( !matchingRow )
            {
                auto& row = ( *table )->InsertRow();
                row.SetNickname( nickname );
                row.SetURI( "${KIPRJMOD}/" + dirname );
                row.SetType( "KiCad" );
                row.SetDescription( wxString::FromUTF8( aFile["name"].get<std::string>() ) );
                if( !( *table )->Save() )
                {
                    ( *table )->Rows().pop_back();
                    throw std::runtime_error( "Could not save the project's footprint library table." );
                }
            }
        }
        catch( ... )
        {
            for( auto it = created.rbegin(); it != created.rend(); ++it ) wxFileName::Rmdir( *it, wxPATH_RMDIR_RECURSIVE );
            throw;
        }
        manager.ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT, { LIBRARY_TABLE_TYPE::FOOTPRINT } );
        for( const auto& fp : footprints )
            result["footprint_ids"].push_back( nickname.ToStdString() + ":" + fp["file"].get<std::string>() );
    }

    if( b.contains( "symbol_library" ) && b["symbol_library"].is_string() )
    {
        const auto symbols = ImportSymbolAttachment(
                aProject, { { "name", nickname.ToStdString() + ".kicad_sym" }, { "text", local( b["symbol_library"] ) } }, aId );
        result["symbol_ids"] = symbols["symbol_ids"];
    }
    result["next"] = "Use symbol_ids as lib_id in build_circuit or edit_circuit; their footprints are already assigned. "
                     "Resolve any error in checks before PCB layout.";
    return result;
}

nlohmann::json INDUCTIVE_PANEL::ImportSymbolAttachment( const wxString& project,
                                                       const nlohmann::json& file,
                                                       const std::string& id )
{
    auto summary = INDUCTIVE_ATTACHMENTS::Inspect( file );
    if( summary["kind"] != "symbol_library" || !INDUCTIVE_PARTS::ValidId( id ) )
        throw std::runtime_error( "Invalid personal symbol library." );
    if( !wxFileName::FileExists( project ) || !wxFileName( project ).IsAbsolute() )
        throw std::runtime_error( "Save the project before importing symbols." );
    auto& manager = Pgm().GetLibraryManager();
    auto table = manager.Table( LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_SCOPE::PROJECT );
    if( !table ) throw std::runtime_error( "No project symbol library table is available." );
    const wxString nickname = "Attached_" + wxString::FromUTF8( id );
    const wxString filename = nickname + ".kicad_sym";
    const wxString path = wxFileName( wxFileName( project ).GetPath(), filename ).GetFullPath();
    bool matchingRow = false;
    for( const auto& row : ( *table )->Rows() )
        if( row.Nickname() == nickname && row.URI() == "${KIPRJMOD}/" + filename && row.Type() == "KiCad" ) matchingRow = true;
    if( wxFileExists( path ) )
    {
        if( INDUCTIVE_PARTS::Read( path ) != file["text"].get<std::string>() || !matchingRow )
            throw std::runtime_error( "An existing project symbol library conflicts with this part." );
    }
    else
    {
        if( ( *table )->HasRow( nickname ) ) throw std::runtime_error( "A project symbol library already uses this name. Resolve its missing or conflicting asset first." );
        INDUCTIVE_PARTS::Write( path, file["text"].get<std::string>() );
        auto& row = ( *table )->InsertRow();
        row.SetNickname( nickname ); row.SetType( "KiCad" );
        row.SetURI( "${KIPRJMOD}/" + filename );
        row.SetDescription( wxString::FromUTF8( file["name"].get<std::string>() ) );
        auto saved = ( *table )->Save();
        if( !saved )
        {
            ( *table )->Rows().pop_back(); wxRemoveFile( path );
            throw std::runtime_error( "Could not save the project symbol library table." );
        }
    }
    manager.ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT, { LIBRARY_TABLE_TYPE::SYMBOL } );
    auto ids = nlohmann::json::array();
    for( const auto& symbol : summary["symbols"] )
        ids.push_back( nickname.ToStdString() + ":" + symbol.at( "name" ).get<std::string>() );
    return { { "symbol_ids", ids } };
}
