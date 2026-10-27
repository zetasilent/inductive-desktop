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

#ifndef INDUCTIVE_PANEL_H
#define INDUCTIVE_PANEL_H

#include <kicommon.h>
#include <wx/panel.h>
#include <wx/process.h>
#include <wx/timer.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class wxWebView;
class wxWebViewEvent;
class INDUCTIVE_NAV_PANEL;

/// A slash command an editor offers, e.g. "Gerbers" in the board editor.
struct INDUCTIVE_COMMAND
{
    wxString name;      ///< Shown in the palette and typed as /name.
    wxString hint;      ///< One-line description.
};

/// A tool the model may call to inspect or change the open document. What the model
/// reads about it -- its description and argument schema -- is the server's.
struct INDUCTIVE_TOOL
{
    wxString name;          ///< Function name, e.g. "set_symbol_value".

    /**
     * True if calling this changes the open document or writes a file.
     *
     * This is the whole of the mode policy.  Ask and Plan must not be able to
     * edit, and a sentence in the system prompt is not a control: text read off
     * the board can talk a model out of an instruction, so the mutating tools
     * are simply not declared in those modes, and the dispatcher refuses them a
     * second time at call time.  A capability the model was never handed cannot
     * be talked into existence.
     */
    bool mutates = false;
};

/**
 * Inductive's chat sidebar.
 *
 * The panel is self-contained: it loads a local HTML page into a wxWebView and
 * talks to Inductive's server itself, over libcurl, on a worker thread.  There
 * is no daemon, no loopback server and no spawned process of any kind.
 *
 * That is a deliberate reversal.  The previous design put a Node process behind
 * an HTTP+SSE surface, and the panel launched it with wxEXEC_ASYNC and then
 * dropped the pid -- so nothing could ever stop it.  It outlived the editor
 * indefinitely, holding a port and an API key.  Owning the request here means
 * the panel cannot leak a process, because it never creates one.
 *
 * The page/host bridge that earlier revisions rejected is usable again.  It was
 * rejected because wxWebView implements every host<->page mechanism by
 * evaluating a string, which needs 'unsafe-eval' in the page's CSP, and that was
 * too much to concede for a page that rendered model output *and* reached the
 * network.  This page no longer reaches the network at all: its CSP grants no
 * connect-src, so scripting it buys an attacker no way to send anything out.
 * Model output is inserted with textContent and is never parsed as HTML.
 */
class KICOMMON_API INDUCTIVE_PANEL : public wxPanel
{
public:
    INDUCTIVE_PANEL( wxWindow* aParent, wxWindowID aId = wxID_ANY );
    ~INDUCTIVE_PANEL() override;

    /// AUI pane name, shared by every editor that hosts the sidebar.
    static const wxString PaneName() { return wxS( "InductiveSidebar" ); }

    /// Directory holding sidebar.html.  $INDUCTIVE_WEB_DIR overrides the built-in path.
    static wxString WebDir();

    /// Repository root: the parent of WebDir().
    static wxString ProjectDir();

    /**
     * Notify the host that fresh board files exist somewhere on disk.
     *
     * Unused by the current design, and kept as an extension point rather than
     * because anything needs it.  The panel edits the document that is already
     * open, through the editor's own commit machinery, so there is nothing to
     * reload -- which is a better answer than reloading: an in-place edit keeps
     * the user's view, selection and undo history, and a reload discards all
     * three.  It would be wanted again only if the separate tscircuit pipeline,
     * which writes whole boards into `generated/`, were driven from here.
     */
    void SetGenerateHandler( std::function<void( const wxString& )> aHandler )
    {
        m_onGenerate = std::move( aHandler );
    }

    /**
     * Let the host draw a generated design into the document it has open.
     *
     * Installing this switches the panel out of the tool loop entirely: turns go
     * to the tscircuit pipeline instead, which spends one model request on the
     * whole circuit, compiles it, checks it, and writes KiCad files -- and this
     * handler is what puts the result on screen.  The editor that installs it is
     * the editor that gets the pipeline; the one that does not keeps the tools.
     *
     * Called on the UI thread with the directory the files were written to.
     * Returns false and fills aResult when the design could not be drawn.
     */
    void SetDrawHandler( std::function<bool( const wxString&, wxString& )> aHandler )
    {
        m_onDraw = std::move( aHandler );
    }

    /**
     * Ask the host whether the document can take a change right now.
     *
     * The editable check used to happen at the *end* of a turn, inside the draw
     * handler — so an engineer who clicked a symbol while waiting spent the whole
     * turn, the model request and the compile, and then got a refusal written for
     * a model rather than a person. Asking first costs nothing and fails in the
     * first second instead of the ninetieth.
     */
    void SetReadyCheck( std::function<bool( wxString& )> aCheck )
    {
        m_readyCheck = std::move( aCheck );
    }

    /**
     * Run when the user sends an Agent prompt or a slash command, before the ready
     * check: the editor goes back to its selection arrow, as Esc would. Sending a
     * request means the user is done with the tool they had picked up, and a wire
     * tool left selected used to refuse the whole turn.
     */
    void SetBeforeRequest( std::function<void()> aFn ) { m_beforeRequest = std::move( aFn ); }

    /**
     * Apply edit operations to the open document.
     *
     * Moving a symbol or clearing the sheet does not change the circuit, so the
     * pipeline answers those with a small list of operations rather than
     * regenerating a design the user did not ask to change.  aOps is that list as
     * a JSON array; the host validates it again before touching anything.
     */
    void SetOpsHandler( std::function<bool( const wxString&, wxString& )> aHandler )
    {
        m_onOps = std::move( aHandler );
    }

    /**
     * Supply a summary of the open document for the model's context.
     *
     * kicommon cannot link against eeschema or pcbnew, so each editor injects
     * its owndescription here.  Called once per turn, on the UI thread.
     */
    void SetContextProvider( std::function<wxString()> aProvider )
    {
        m_contextProvider = std::move( aProvider );
    }

    /**
     * Register the slash commands this editor offers, and the handler that runs
     * them.  The handler returns false if it does not recognise the command;
     * aResult carries the message shown to the user either way.
     */
    void SetCommands( std::vector<INDUCTIVE_COMMAND> aCommands,
                      std::function<bool( const wxString&, wxString& )> aHandler )
    {
        m_commands = std::move( aCommands );
        m_commandHandler = std::move( aHandler );
    }

    /**
     * Register the tools the model may call against the open document.
     *
     * The handler runs on the UI thread and returns a short result string, which
     * is fed back to the model.  Returning false marks the call as failed.
     */
    void SetTools( std::vector<INDUCTIVE_TOOL> aTools,
                   std::function<bool( const wxString&, const wxString&, wxString& )> aHandler )
    {
        m_tools = std::move( aTools );
        addAttachmentTools();
        m_toolHandler = std::move( aHandler );
    }

    /**
     * Called with true when a request (a chat message or a slash command) starts and
     * with false when it ends, on the UI thread. The editor folds the undo steps in
     * between into one, so one Cmd+Z takes back one request.
     */
    void SetRequestBoundaryHandler( std::function<void( bool )> aHandler )
    {
        m_requestBoundary = std::move( aHandler );
    }

    // The editor parses native files and inserts schematics. The panel owns uploads.
    void SetAttachmentHandler( std::function<bool( const nlohmann::json&, bool, wxString& )> aHandler )
    {
        m_attachmentHandler = std::move( aHandler );
    }

    void SetCircuitEditHandler( std::function<bool( const std::string&, const nlohmann::json&, wxString& )> handler )
    { m_circuitEditHandler = std::move( handler ); }

    /// The schematic editor collects its BOM ("collect") and records sourced parts ("apply").
    void SetSourcingHandler( std::function<nlohmann::json( const std::string&, const nlohmann::json& )> handler )
    { m_sourcingHandler = std::move( handler ); }

    void SetWorkflowSnapshot(std::function<nlohmann::json(bool)> handler)
    { m_workflowSnapshot=std::move(handler); }

    void SetReviewHandler(std::function<nlohmann::json(const std::string&,const nlohmann::json&)> handler)
    {m_reviewHandler=std::move(handler);}

    static nlohmann::json ImportSymbolAttachment( const wxString& aProject,
                                                  const nlohmann::json& aFile,
                                                  const std::string& aId );
    /// Install a server-checked part download: footprints, STEP models, then symbols.
    static nlohmann::json ImportPartBundle( const wxString& aProject, const nlohmann::json& aFile,
                                            const std::string& aId );
    static wxString ImportFootprintAttachment( const wxString& aProject,
                                               const nlohmann::json& aFile,
                                               const std::string& aId );

    /// (Re)load the sidebar page.
    void ReloadUI();

    /**
     * Attach the editor's navigation pane (search, customize, history, account).
     *
     * This panel stays the owner of everything the pane shows -- the chats, the
     * session, billing, the settings -- and the pane is a second view of it: what
     * it asks for goes through HandleNavMessage(), and the events it needs are
     * forwarded to it from emitEvent(). Null detaches.
     */
    void SetNavPanel( INDUCTIVE_NAV_PANEL* aNav ) { m_nav = aNav; }

    /// A message from the navigation pane's page. Only the kinds that page has
    /// any business sending are acted on; anything else is dropped.
    void HandleNavMessage( const std::string& aJson );

    /// The navigation pane's page has loaded: send it everything it shows.
    void SyncNav();

    bool IsWebViewReady() const { return m_webView != nullptr; }

    /// The sidebar's default thinking level (1-based; its INTEL_DEFAULT, "High").
    static constexpr int DEFAULT_INTELLIGENCE = 3;

private:
    std::function<nlohmann::json(bool)> m_workflowSnapshot;
    void workflowMessage(const nlohmann::json&);
    bool applyCircuitEdit( const nlohmann::json&, wxString&, bool aDirect = false );
    void notePublished( wxString& aResult );
    std::function<nlohmann::json(const std::string&,const nlohmann::json&)> m_reviewHandler;
    nlohmann::json reviewSnapshot();
    nlohmann::json reviewSources(const nlohmann::json&);
    nlohmann::json reviewInventory();
    void reviewMessage(const nlohmann::json&);
    bool reviewTool(const std::string&,const nlohmann::json&,wxString&);
    nlohmann::json m_reviewReport=nlohmann::json::object();
    std::function<bool( const std::string&, const nlohmann::json&, wxString& )> m_circuitEditHandler;
    void addAttachmentTools();
    bool partsTool( const std::string&, const nlohmann::json&, wxString& );
    nlohmann::json usePart( const std::string& );
    void sendParts();
    bool readDatasheetTool( const nlohmann::json&, wxString& );
    void queueDatasheet( const std::string&, const nlohmann::json& );
    std::map<std::string,nlohmann::json> m_turnDatasheets;
    nlohmann::json m_selectedPart;
    wxString m_selectedPartProject;
    bool attachmentTool( const std::string& aName, const nlohmann::json& aArgs, wxString& aResult );
    std::map<std::string, nlohmann::json> m_attachments;
    std::function<nlohmann::json( const std::string&, const nlohmann::json& )> m_sourcingHandler;
    /// source_bom answers fetched off the UI thread for this round, by call arguments.
    std::map<std::string, std::pair<bool, std::string>> m_prefetchedSourcing;
    /// Dispatch a round's tool calls and hand the results back, or finish the turn.
    void continueToolRound( const std::string& aParts, unsigned aTurn );
    /// Fetch this round's source_bom answers off the UI thread; false when there are none.
    bool sourceBomThenContinue( const std::string& aParts, unsigned aTurn );
    std::function<bool( const nlohmann::json&, bool, wxString& )> m_attachmentHandler;
    void onError( wxWebViewEvent& aEvent );

    /// A message posted by the page through window.inductiveHost.
    void onScriptMessage( wxWebViewEvent& aEvent );

    /// Act on one message from either page; aFromNav limits it to the nav's kinds.
    void handleMessage( const std::string& aJson, bool aFromNav );

    /// The editor window came to the front. If a sign-in was waiting on the
    /// browser, the user is back without finishing it: stop showing it as under way.
    void onFrameActivate( wxActivateEvent& aEvent );

    /// Start a turn: append the prompt to the conversation and run it.
    /// aAttachments is the page's attachment array; images in it become
    /// inlineData parts on this turn only.
    void ask( const wxString& aPrompt, const wxString& aMode,
              const nlohmann::json& aAttachments );
    /// The rest of ask(), once any part downloads have been opened by the server.
    void beginTurn( const wxString& aPrompt, const wxString& aMode, const nlohmann::json& aAttachments );
    /// Send part downloads to /v1/parts/unpack off the UI thread, then beginTurn.
    void openPartDownloads( const wxString& aPrompt, const wxString& aMode, const nlohmann::json& aAttachments );

    /**
     * Replace the image data in the turn that carried it with a short note.
     *
     * Images are megabytes, and every request in the conversation replays the
     * whole history verbatim -- so leaving them in would resend the same
     * screenshots on every later turn, slowly, for the rest of the session.  The
     * model sees them for the turn they were sent, including across its tool
     * rounds, which is when they are actually being asked about.
     */
    void dropImageData();

    /// True when the host installed a draw handler, i.e. this editor designs
    /// through the tscircuit pipeline rather than through model-driven tools.
    // Both editors use the live-document tool agent. CLI generation remains separate.
    bool usesPipeline() const { return false; }

    /// Run one turn through the pipeline: spawn the runner, stream its events.
    void runPipelineTurn( unsigned aTurn );

    /// Absolute path to a node binary, or empty if none can be found.
    static wxString NodePath();

    /// Read whatever the runner has written, and act on every complete line.
    void pumpPipeline();

    /// The runner exited: drain the last of it and close the turn out.
    void onPipelineEnd( wxProcessEvent& aEvent );

    /// The work of closing a finished pipeline turn out, from either the exit
    /// event or the pump that was running when the exit arrived.
    void finishPipeline( int aExitCode );

    /// Handle one event object from the runner, on the UI thread.  Catches
    /// everything: an exception escaping a wx handler terminates the editor.
    void onPipelineEvent( const nlohmann::json& aEvent );

    /// The body of onPipelineEvent(), inside its exception guard.
    void handlePipelineEvent( const nlohmann::json& aEvent );

    /// Tear down the running child process and its polling timer.
    void endPipeline();

    /// Issue one request for the current conversation.  Re-entered after tool calls.
    /// aTurn is the generation it belongs to, so a cancelled turn cannot adopt a new one.
    void runTurn( unsigned aTurn );

    /// Drop the oldest exchanges so the transcript resent each request stays bounded.
    void trimHistory();

    /// Run the tool calls the model asked for, appending their results.  Returns
    /// true if anything ran, meaning the conversation must go back to the model.
    ///
    /// aTurn is the generation these calls belong to.  A tool handler can pump
    /// the event loop, so Stop or a reset can dispatch between two calls of the
    /// same round; the loop re-checks and abandons rather than editing the
    /// document for a turn the user has already cancelled.
    bool executeToolCalls( const std::string& aCallsJson, unsigned aTurn );

    /// Abandon the running turn: its reply is discarded when it arrives.
    void stopTurn();

    /**
     * Push one event to the page by calling window.inductiveDeliver().
     *
     * The events carry the same shapes the old SSE stream used -- "text",
     * "done", "error", "cancelled" -- so the sidebar's renderer is unchanged.
     */
    void emitEvent( const std::string& aJson );

    /// Render an explanatory page instead of a blank panel when something is wrong.
    void ShowMessage( const wxString& aTitle, const wxString& aDetail );

    /// Tell the page which commands exist, once it has loaded.
    void onPageLoaded( wxWebViewEvent& aEvent );

    /// Push current cloud auth + entitlement snapshot to the page.
    void emitAuthStatus();

    /// GET /v1/me on a worker and emit an updated auth event with plan.
    void refreshEntitlementAsync();

    /// POST /v1/billing/checkout and open Stripe Checkout in the system browser.
    /// Open the plans page in the browser; checkout and Stripe happen there.
    void openPlansPage( const std::string& aPlan, const std::string& aAccount );

    /// POST /v1/billing/portal and open the Customer Portal in the system browser.
    void handleBillingPortal();

    /// Run an editor-supplied slash command.
    void runCommand( const wxString& aCommand );
    void beforeRequest();

    wxWebView*                             m_webView;
    std::function<void( const wxString& )> m_onGenerate;

    /// See SetDrawHandler() and SetOpsHandler().  Installed by eeschema only.
    std::function<bool( const wxString&, wxString& )> m_onDraw;
    std::function<bool( const wxString&, wxString& )> m_onOps;
    std::function<bool( wxString& )>                 m_readyCheck;
    std::function<void()>                            m_beforeRequest;

    /**
     * The running pipeline process, its output buffer, and the timer draining it.
     *
     * wxProcess does not push data at us, so a timer on the UI thread reads what
     * is available.  Everything about a pipeline turn therefore happens on the UI
     * thread, which is the reason this does not need the TURN_STATE dance the
     * HTTP path does.
     */
    wxProcess*                             m_pipeline = nullptr;
    long                                   m_pipelinePid = 0;
    wxTimer                                m_pipelineTimer;
    std::string                            m_pipelineOut;
    unsigned                               m_pipelineTurn = 0;

    /// Set while a pipeline turn is drawing, so the final event knows it worked.
    bool                                   m_pipelineDrew = false;
    bool                                   m_pipelineDrawAttempted = false;
    wxString                               m_pipelineDrawError;

    /**
     * True while pumpPipeline() is dispatching.
     *
     * An event can end up drawing a whole schematic, and committing that pumps
     * the event loop -- which fires the poll timer again, inside the handler that
     * is still running. Without this the same output could be dispatched twice,
     * and the second pass would be operating on a buffer the first one is halfway
     * through erasing.
     */
    bool                                   m_pipelinePumping = false;

    /// Set when the runner exits while pumpPipeline() is still dispatching, so
    /// the pump closes the turn out instead of the (re-entrant) exit handler.
    bool                                   m_pipelineEnded = false;
    int                                    m_pipelineExit = 0;

    /// Injected by the host editor; see SetContextProvider() and SetCommands().
    std::function<wxString()>                        m_contextProvider;
    std::vector<INDUCTIVE_COMMAND>                   m_commands;
    std::function<bool( const wxString&, wxString& )> m_commandHandler;

    /**
     * The conversation, as serialised "content" objects.
     *
     * Stored as JSON text rather than (role, text) pairs because tool calling
     * puts functionCall and functionResponse parts in the history, and the model
     * needs its own parts replayed verbatim to keep their signatures.
     */
    std::vector<std::string>                         m_history;

    /// One-based index of the history entry holding this turn's images, or 0 for none.
    size_t                                           m_imageTurn = 0;

    /// Mode of the turn in progress, so follow-up tool rounds keep its instruction.
    wxString                                         m_mode;

    /// How many tool rounds the current turn has taken, to bound a runaway loop.
    int                                              m_toolRounds;

    // ── chat history: saved per project, on this machine only ─────────────

    /// Called for every event sent to the page; records the turn in progress.
    void recordEvent( const std::string& aJson );
    /// Tell the editor a request starts or ends (see SetRequestBoundaryHandler).
    void requestBoundary( bool aBegin );
    /// When the model stops without a reply after running tools: what the tools did,
    /// from their own results in this turn's record.
    std::string toolStepsSummary() const;

    /**
     * After a turn that changed the schematic: ERC errors it left, if the reply did
     * not state them, as a note to show under it. Empty when there are none.
     */
    std::string ercNoteAfterTurn( const std::string& aParts );

    /// Start recording a turn: what the user sent, as the page shows it.
    void beginRecord( const wxString& aPrompt, int aImages );

    /// Write the current chat to disk (no-op until it has a finished turn).
    void saveChat();

    /// Forget the current chat's identity: the next turn starts a new one.
    void startNewChat();

    /// Send the page this project's chats, newest first.
    void sendHistoryList();

    /// The project's chats whose title or text contains aQuery, as a "history"
    /// event carrying the query, so the pane can tell results from the list.
    void searchHistory( const std::string& aQuery );

    /// Refresh the navigation pane's list, if there is one to refresh.
    void pushNavHistory();

    /// Send the Customize settings (instructions, default mode and level).
    void emitSettings();

    void openChat( const std::string& aId );
    void deleteChat( const std::string& aId );

    /// The open project changed under the panel: start afresh for the new one.
    void checkProject();

    /// Full path of the open KiCad project, or empty for none.
    wxString currentProject() const;

    /// "pcb" or "sch": chats are per editor, since each editor has its own tools.
    std::string editorKind() const;

    /**
     * "/redeem CODE": apply a promotion code to the signed-in account (POST
     * /v1/billing/redeem) and answer in the chat. False when the text is not that
     * command, so it goes on to the model as usual.
     */
    bool redeemPromo( const wxString& aText );
    std::string reportApp() const;

    /// The navigation pane, or null. Cleared by the pane when it goes first.
    INDUCTIVE_NAV_PANEL* m_nav = nullptr;

    /// search_parts results fetched on the network thread for the current round,
    /// keyed by the call's argument JSON: ok flag and the text for the model.
    std::map<std::string, std::pair<bool, std::string>> m_prefetchedParts;

    /// Browser sign-in: which attempt is the current one (an older attempt's
    /// timeout must not report over a newer one), and whether the pages are
    /// showing it as under way.
    unsigned             m_signInAttempt = 0;
    bool                 m_signInPending = false;
    /// When /redeem last opened Stripe checkout: the plan is read again on coming back.
    std::optional<std::chrono::steady_clock::time_point> m_checkoutOpened;
    /// The frame whose activation is watched; unbound again on destruction.
    wxWindow*            m_activateSource = nullptr;
    /// The last auth event, replayed to a navigation pane that loads after it.
    std::string          m_lastAuthEvent;

    std::string    m_chatId;
    wxString       m_chatProject;
    std::string    m_chatTitle;
    long long      m_chatCreated = 0;
    nlohmann::json m_chatTurns = nlohmann::json::array();
    bool           m_recording = false;
    nlohmann::json m_turnRecord;
    wxString       m_watchedProject;
    bool           m_projectKnown = false;
    wxTimer        m_projectTimer;

    /// The thinking level the sidebar last reported (1-based); it picks the model.
    int                                              m_intelligence = DEFAULT_INTELLIGENCE;

    /// The level of the turn in progress, taken when it starts, so every tool round
    /// of one turn runs on the same model even if the slider moves meanwhile.
    int                                              m_turnLevel = DEFAULT_INTELLIGENCE;

    /// The server's id for the turn in progress (POST /v1/generate/turn), sent with
    /// every request of the turn; empty until its first request opens it.
    std::string                                      m_serverTurnId;

    std::vector<INDUCTIVE_TOOL>                      m_tools;
    std::function<bool( const wxString&, const wxString&, wxString& )> m_toolHandler;
    std::function<void( bool )>            m_requestBoundary;

    /// One turn at a time; the page also disables its input while this is set.
    bool                                   m_busy;

    /**
     * The state a worker thread is allowed to look at.
     *
     * Held by shared_ptr and captured BY VALUE into every worker, so the worker
     * never dereferences the panel.  That is the whole point: `alive` alone was
     * not enough, because checking it and then reading a panel member are two
     * separate operations -- the panel can be destroyed between them, and the
     * second one reads freed memory.  Putting both values behind the same
     * shared_ptr removes the window rather than narrowing it.
     *
     * `turnId` is bumped to abandon a turn: a worker whose id no longer matches is
     * ignored, which is how Stop takes effect without cancelling the HTTP request.
     * Written only on the UI thread.
     */
    struct TURN_STATE
    {
        std::atomic<bool>     alive{ true };
        std::atomic<unsigned> turnId{ 0 };
    };

    /// Mutating tool calls made during the current turn, against MAX_TURN_MUTATIONS.
    int                                    m_mutationsThisTurn;

    /// Where the current turn began in m_history, so a cancelled or failed turn
    /// rolls back whole instead of dropping one arbitrary entry.
    size_t                                 m_turnStart;

    /// Shared with every in-flight worker; outlives the panel by design.
    std::shared_ptr<TURN_STATE>            m_state;
};

#endif  // INDUCTIVE_PANEL_H
