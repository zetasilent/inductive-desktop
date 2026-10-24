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

/**
 * The board editor's half of Inductive: what the model can see and change.
 *
 * Every tool here runs on the UI thread with the board loaded, dispatched from
 * INDUCTIVE_PANEL::executeToolCalls().  Three rules hold throughout, and the
 * reasons are worth stating once rather than repeating at each call site:
 *
 *  1. **Arguments are validated before they touch KiCad.**  They arrive as JSON
 *     a language model wrote.  INDUCTIVE_ARGS refuses anything that is not a
 *     finite number in range, and every refusal throws -- the panel catches it,
 *     shows the model the reason, and the model corrects itself on the next
 *     round.  Nothing here may pass a raw double into mmToIU(); see
 *     inductive_tool_args.h for why that is a memory-safety property and not a
 *     matter of taste.
 *
 *  2. **Every mutation goes through BOARD_COMMIT.**  Modify() is called *before*
 *     the object changes, so undo has the old state to restore; Push() is what
 *     writes the undo step, marks the board modified and invalidates the render
 *     cache.  Mutating an item without a commit leaves the user unable to undo
 *     the model's work, which is the one thing that makes an agent in an editor
 *     tolerable.
 *
 *  3. **A tool reports what it actually did.**  The string it returns is fed
 *     back to the model and shown in the sidebar, so it names the object and its
 *     new state ("R1 moved to 12.700, 8.400"). A tool that answers "ok" teaches
 *     the model nothing and leaves the user unable to check.
 */

#include "inductive_board_tools.h"
#include <widgets/inductive_attachments.h>
#include <pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>
#include <widgets/inductive_review.h>
#include "inductive_commit_guard.h"
#include "inductive_tool_args.h"
#include "inductive_board_rules.h"
#include "inductive_board_apply.h"
#include <kicad_curl/kicad_curl_easy.h>
#include <wildcards_and_files_ext.h>
#include <settings/settings_manager.h>
#include <pgm_base.h>
#include <widgets/inductive_cloud_auth.h>

#include <widgets/inductive_panel.h>

#include <board.h>
#include <board_commit.h>
#include <netlist_reader/pcb_netlist.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/board_netlist_updater.h>
#include <board_design_settings.h>
#include <project/net_settings.h>
#include <netclass.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_track.h>
#include <zone.h>
#include <zone_filler.h>
#include <pcb_shape.h>
#include <stroke_params.h>
#include <geometry/shape_poly_set.h>
#include <netinfo.h>
#include <padstack.h>
#include <layer_ids.h>

#include <footprint_library_adapter.h>
#include <kiid.h>
#include <lib_id.h>
#include <libraries/library_manager.h>
#include <project_pcb.h>
#include <refdes_utils.h>

#include <base_units.h>
#include <core/mirror.h>
#include <connectivity/connectivity_data.h>
#include <ratsnest/ratsnest_data.h>
#include <pcb_plotter.h>
#include <exporters/gendrill_excellon_writer.h>
#include <wx/zipstrm.h>
#include <wx/wfstream.h>
#include <wx/ffile.h>
#include <wx/dir.h>
#include <pcbplot.h>
#include "inductive_jlcpcb.h"
#include "inductive_board_items.h"
#include <widgets/appearance_controls.h>

#include <tools/drc_tool.h>
#include <connectivity/connectivity_algo.h>
#include <pcb_screen.h>
#include <tools/pcb_selection_tool.h>
#include <tools/pcb_selection.h>

#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <core/kicad_algo.h>
#include <pcb_marker.h>
#include <rc_item.h>
#include <widgets/report_severity.h>
#include <widgets/wx_progress_reporters.h>
#include <ki_exception.h>

#include <tool/actions.h>
#include <tool/tool_manager.h>

#include <wx/filename.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <thread>
#include <optional>
#include <fstream>
#include <atomic>
#include <chrono>
#include <set>
#include <functional>
#include <map>
#include <memory>
#include <vector>

using json = nlohmann::json;
namespace ARGS = INDUCTIVE_ARGS;

namespace
{

// ─────────────────────────────── units ───────────────────────────────

/**
 * Board internal units to millimetres.
 *
 * Takes int64_t rather than int deliberately.  BOX2I's GetWidth(), GetHeight()
 * and friends return VECTOR2I::extended_type -- int64_t -- while KiCad's own
 * EDA_IU_SCALE::IUTomm() takes an int, so passing a bounding-box dimension
 * straight into it narrows silently.  Dividing here instead is exact for every
 * value either type can hold, and means no call site has to remember.
 */
inline double ToMm( int64_t aIU )
{
    return static_cast<double>( aIU ) / pcbIUScale.IU_PER_MM;
}


/**
 * Millimetres to board internal units.
 *
 * A real check, not an assertion.  wxASSERT is live in this build -- wxDEBUG_LEVEL
 * defaults to 1 and nothing overrides it -- and at that level a failed assertion
 * opens a MODAL DIALOG.  From a tool handler, which already runs inside a
 * CallAfter continuation, that is the deadlock this whole surface is built to
 * avoid: a dialog the user cannot dismiss because the frame behind it is
 * disabled, on top of an agent turn they cannot cancel.
 *
 * So an out-of-range value throws instead.  The panel turns that into a failed
 * tool call the model reads and corrects, which is the outcome we want anyway.
 * It should be unreachable -- INDUCTIVE_ARGS bounds every value before it gets
 * here -- but "should be unreachable" is exactly the claim that needs a
 * backstop, because mmToIU() converts out-of-range doubles with undefined
 * behaviour and KiROUND's own NaN guard is dead code in this C++20 tree.
 */
inline int ToIU( double aMm )
{
    // Written negated so that NaN fails: every comparison against NaN is false.
    if( !( std::abs( aMm ) <= ARGS::MAX_COORD_MM ) )
    {
        throw ARGS::BAD_ARGUMENT( wxString::Format(
                _( "%g mm is outside the range this board can represent." ), aMm ) );
    }

    return pcbIUScale.mmToIU( aMm );
}


/// Format a length for the model and the sidebar: micron resolution, no trailing noise.
wxString Mm( int64_t aIU )
{
    return wxString::Format( wxS( "%.3f" ), ToMm( aIU ) );
}


/// Format a position as the model should read it back.
wxString Pos( const VECTOR2I& aPoint )
{
    return wxString::Format( wxS( "%.3f, %.3f" ), ToMm( aPoint.x ), ToMm( aPoint.y ) );
}


// ─────────────────────────── board lookups ───────────────────────────

/**
 * The board, or a refusal.
 *
 * Every tool starts here.  A frame without a board is not a state a user can
 * normally reach, but "normally" is doing a lot of work in an editor that can be
 * driven while a project is closing.
 */
BOARD* RequireBoard( PCB_EDIT_FRAME* aFrame )
{
    BOARD* board = aFrame ? aFrame->GetBoard() : nullptr;

    if( !board )
        throw ARGS::BAD_ARGUMENT( _( "No board is open." ) );

    return board;
}


/**
 * Find a footprint by reference designator.
 *
 * Matching is case-insensitive because models write "r1" as often as "R1".  On a
 * miss the message lists what is actually on the board, bounded -- a model that
 * is told "no R1; the board has C1, C2, U1" fixes itself on the next round,
 * where one that is told "not found" tends to try three more spellings.
 */
FOOTPRINT* RequireFootprint( BOARD* aBoard, const wxString& aReference )
{
    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        if( fp->GetReference().CmpNoCase( aReference ) == 0 )
            return fp;
    }

    wxString available;
    int      shown = 0;

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        if( shown++ >= 40 )
        {
            available << wxS( ", ..." );
            break;
        }

        available << ( shown > 1 ? wxS( ", " ) : wxS( "" ) ) << fp->GetReference();
    }

    if( available.IsEmpty() )
        available = _( "(the board has no footprints)" );

    throw ARGS::BAD_ARGUMENT( wxString::Format(
            _( "There is no footprint called '%s' on this board. Present: %s" ), aReference,
            available ) );
}


/**
 * Refuse to change the board while the editor is busy with something else.
 *
 * This is the gate that keeps the agent out of the editor's way, and it is not
 * ours: PCB_EDIT_FRAME::CanAcceptApiCommands() is what KiCad's own IPC API
 * consults before every mutating handler.  It covers a modal or quasi-modal
 * dialog being open, an interactive drawing or drag tool being mid-gesture, the
 * router running, a zone fill running, and a blocked undo stack.
 *
 * It matters more here than it does for the IPC API, because of how the panel
 * reaches this code.  Tool calls arrive through wxTheApp->CallAfter(), and
 * pending events are drained by whichever event loop is running -- *including
 * the nested loop inside a modal dialog*.  So "the user has Board Setup open"
 * is not an edge case, it is the ordinary case whenever they open a dialog
 * mid-conversation.  Editing the board underneath a dialog that is showing its
 * own copy of the same data is how an agent corrupts a document; and the user
 * cannot even press Stop, because the sidebar is behind the disabled frame.
 *
 * Read-only tools are deliberately *not* gated: answering a question about the
 * board while a dialog is open is harmless and useful.
 */
void RequireEditable( PCB_EDIT_FRAME* aFrame )
{
    if( !aFrame->CanAcceptApiCommands() )
    {
        throw ARGS::BAD_ARGUMENT(
                _( "The board editor is busy -- a dialog is open, or a tool is mid-operation. "
                   "Tell the user to finish or cancel it, then ask again." ) );
    }
}


/**
 * Refuse to move or delete something the user has locked.
 *
 * Locking is the one signal in the editor that means "I have decided about this
 * and I do not want it touched" -- a board-edge connector, a mounting hole
 * matched to an enclosure.  Silently overriding it is the fastest way for an
 * agent to lose an engineer's trust, so a locked item is a refusal the model can
 * report rather than an obstacle it can work around.
 */
void RefuseIfLocked( const FOOTPRINT* aFootprint, const wxString& aVerb )
{
    if( aFootprint->IsLocked() )
    {
        throw ARGS::BAD_ARGUMENT( wxString::Format(
                _( "%s is locked, so it will not be %s. Unlock it first if that is intended." ),
                aFootprint->GetReference(), aVerb ) );
    }
}


/// A footprint as one line of a listing.
wxString DescribeFootprint( BOARD* aBoard, FOOTPRINT* aFootprint )
{
    wxString line;

    line << aFootprint->GetReference() << wxS( "  " );

    if( !aFootprint->GetValue().IsEmpty() )
        line << aFootprint->GetValue() << wxS( "  " );

    line << wxS( "at " ) << Pos( aFootprint->GetPosition() ) << wxS( " mm" );

    const double angle = aFootprint->GetOrientationDegrees();

    if( angle != 0.0 )
        line << wxString::Format( wxS( ", rotated %.1f deg" ), angle );

    line << wxS( ", " ) << aBoard->GetLayerName( aFootprint->GetLayer() );

    if( aFootprint->IsLocked() )
        line << wxS( ", LOCKED" );

    line << wxS( "  [" ) << aFootprint->GetFPID().Format().wx_str() << wxS( "]" );

    return line;
}


// ────────────────────────── copper helpers ──────────────────────────

/**
 * Map a layer name the model typed onto a real layer of this board.
 *
 * A note for anyone porting code from KiCad 7 or 8: **the copper numbering
 * changed**.  In this tree F_Cu is 0, B_Cu is 2, In1_Cu is 4, and copper layers
 * are the *even* ids up to In30_Cu = 62.  Anything that hardcodes B_Cu as 31, or
 * walks copper with ++, is silently wrong here.
 *
 * Two checks, not one: a name can resolve to a perfectly valid copper id that is
 * not enabled on *this* board -- In4_Cu on a two-layer design -- and putting a
 * track there produces copper the user cannot see or fabricate.
 */
PCB_LAYER_ID RequireCopperLayer( BOARD* aBoard, const wxString& aName )
{
    const PCB_LAYER_ID layer = aBoard->GetLayerID( aName );

    wxString enabled;

    for( PCB_LAYER_ID candidate : LSET::AllCuMask( aBoard->GetCopperLayerCount() ).CuStack() )
    {
        if( aBoard->IsLayerEnabled( candidate ) )
        {
            enabled << ( enabled.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                    << aBoard->GetLayerName( candidate );
        }
    }

    if( layer == UNDEFINED_LAYER || !IsCopperLayer( layer ) )
    {
        throw ARGS::BAD_ARGUMENT( wxString::Format(
                _( "'%s' is not a copper layer on this board. Copper layers here: %s" ), aName,
                enabled ) );
    }

    if( !aBoard->IsLayerEnabled( layer ) )
    {
        throw ARGS::BAD_ARGUMENT( wxString::Format(
                _( "Layer '%s' exists but is not enabled on this board. Enabled copper: %s" ),
                aName, enabled ) );
    }

    return layer;
}


/**
 * Resolve a net name to its net.
 *
 * Netcodes are not stable across a connectivity rebuild, so nothing here ever
 * holds one between calls -- a net is resolved by name every time it is used.
 */
NETINFO_ITEM* RequireNet( BOARD* aBoard, const wxString& aName )
{
    if( NETINFO_ITEM* net = aBoard->FindNet( aName ) )
        return net;

    // Net names are stored escaped, and a model reading a name off a listing can
    // easily differ in case, or drop the sheet's "/" ("GND" for "/GND", as the
    // listings and the user write it), so a friendlier second pass is worth the scan.
    const wxString bare = aName.StartsWith( wxS( "/" ) ) ? aName.Mid( 1 ) : aName;

    for( NETINFO_ITEM* net : aBoard->GetNetInfo() )
    {
        if( !net )
            continue;

        const wxString name = net->GetNetname();

        if( name.CmpNoCase( aName ) == 0 || ( name.StartsWith( wxS( "/" ) ) && name.Mid( 1 ).CmpNoCase( bare ) == 0 ) )
            return net;
    }

    throw ARGS::BAD_ARGUMENT( wxString::Format(
            _( "There is no net called '%s'. Call list_nets to see the net names." ), aName ) );
}


// ──────────────────────────── libraries ────────────────────────────

/**
 * The footprint library adapter, or a refusal.
 *
 * Note for anyone porting code from KiCad 7/8/9: FP_LIB_TABLE does not exist in
 * this tree, and PROJECT::PcbFootprintLibs() is gone with it.  Libraries are
 * reached through the global LIBRARY_MANAGER behind this adapter.  Its argument
 * is ignored -- KiCad's own callers pass nullptr -- because it resolves the
 * singleton from Pgm().GetLibraryManager().
 */
FOOTPRINT_LIBRARY_ADAPTER* RequireLibraries()
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( nullptr );

    if( !adapter )
        throw ARGS::BAD_ARGUMENT( _( "The footprint libraries are not available." ) );

    return adapter;
}


/**
 * The next unused reference designator with a given prefix.
 *
 * "R", "R?", "REF**" and "R12" all reduce to the prefix, and the search then
 * takes the first free number.  A library footprint whose reference is the
 * placeholder "REF**" carries no useful prefix, so those become "U".
 */
wxString NextFreeReference( BOARD* aBoard, const wxString& aPrefixHint )
{
    wxString prefix = UTIL::GetRefDesPrefix( aPrefixHint );

    // GetRefDesPrefix() scans back for the first character that is neither a
    // digit nor '?', so for the library placeholder "REF**" it stops at the
    // trailing asterisk and hands back "REF**" -- not "REF". Comparing against
    // "REF" therefore never matches, and every part placed from a library that
    // uses the placeholder comes out named REF**1, REF**2. Strip the asterisks
    // before deciding.
    while( prefix.EndsWith( wxS( "*" ) ) )
        prefix.RemoveLast();

    if( prefix.IsEmpty() || prefix == wxS( "REF" ) )
        prefix = wxS( "U" );

    for( int n = 1; n < 100000; ++n )
    {
        const wxString candidate = wxString::Format( wxS( "%s%d" ), prefix, n );

        if( !aBoard->FindFootprintByReference( candidate ) )
            return candidate;
    }

    throw ARGS::BAD_ARGUMENT(
            wxString::Format( _( "Could not find a free reference designator for '%s'." ),
                              prefix ) );
}


/**
 * Run the design rule check and report it, shared by the tool and /drc.
 *
 * Markers are left on the canvas deliberately: the value of a check is not the
 * sentence the model writes about it, it is the engineer being able to click
 * through the violations themselves.
 */
bool RunDRC( PCB_EDIT_FRAME* aFrame, wxString& aResult )
{
                  BOARD* board = RequireBoard( aFrame );

                  // The engine is shared with the renderer's clearance cache and
                  // with the DRC dialog.  Constructing our own and swapping it in
                  // would pull that cache out from under BOARD_CONNECTED_ITEM.
                  std::shared_ptr<DRC_ENGINE> engine = board->GetDesignSettings().m_DRCEngine;

                  if( !engine )
                      throw ARGS::BAD_ARGUMENT( _( "The design rule engine is not available." ) );

                  if( DRC_TOOL* drcTool = aFrame->GetToolManager()->GetTool<DRC_TOOL>() )
                  {
                      if( drcTool->IsDRCRunning() )
                      {
                          throw ARGS::BAD_ARGUMENT(
                                  _( "A design rule check is already running." ) );
                      }

                      // The DRC dialog is MODELESS, so RequireEditable() cannot
                      // see it: the frame stays enabled and no interactive tool is
                      // active. Its tree model holds raw MARKER_BASE* pointers to
                      // every marker on the board, and the delete below frees all
                      // of them -- the dialog then dereferences freed memory on its
                      // next paint. Refusing is the only safe answer.
                      if( drcTool->GetDRCDialog() )
                      {
                          throw ARGS::BAD_ARGUMENT(
                                  _( "The DRC dialog is open and lists the current violations. "
                                     "Ask the user to close it, then run the check again." ) );
                      }
                  }

                  // The selection holds raw pointers, and DeleteMARKERs() raw
                  // `delete`s each marker without going through a commit -- so
                  // BOARD_COMMIT's own deselect-on-remove never runs. A marker the
                  // user clicked to read is then a dangling pointer in the
                  // selection overlay, dereferenced on the next repaint.
                  //
                  // This is exactly what DIALOG_DRC::deleteAllMarkers does first,
                  // for the same reason, with the same comment.
                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                  {
                      if( PCB_SELECTION_TOOL* sel = mgr->GetTool<PCB_SELECTION_TOOL>() )
                          sel->ClearSelection();
                  }

                  // Exclusions are the user's own judgement about violations they
                  // have decided not to care about, and they are stored on the
                  // markers -- which are all about to be deleted. Recording them
                  // here is only half the job; they are re-applied to the new
                  // markers after the run, below. Without that second half the
                  // exclusions are silently gone on the next save.
                  board->RecordDRCExclusions();
                  board->DeleteMARKERs( true, true );

                  try
                  {
                      engine->InitEngine( aFrame->GetDesignRulesPath() );
                  }
                  catch( const PARSE_ERROR& e )
                  {
                      // A syntax error in the user's own .kicad_dru file. Their
                      // problem to fix, but the model has to be able to say so
                      // rather than reporting a clean board.
                      //
                      // The markers -- and with them every exclusion the engineer
                      // had made -- were deleted above, and the re-apply that
                      // normally follows the run is further down. Throwing straight
                      // out of here would leave those exclusions recorded and never
                      // restored, so a fixed rules file would come back with every
                      // previously-dismissed violation live again. Put them back
                      // before reporting the failure.
                      aFrame->ResolveDRCExclusions( true );
                      aFrame->GetCanvas()->Refresh();

                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "The custom design rules do not compile: %s" ), e.What() ) );
                  }

                  engine->SetDrawingSheet( aFrame->GetCanvas()->GetDrawingSheet() );

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  engine->SetProgressReporter( nullptr );
                  engine->SetViolationHandler(
                          [&commit]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos,
                                     int aLayer,
                                     const std::function<void( PCB_MARKER* )>& aPathGenerator )
                          {
                              PCB_MARKER* marker = new PCB_MARKER( aItem, aPos, aLayer );

                              // Fills in the creepage and length paths.  Always
                              // called, even when the marker has none.
                              aPathGenerator( marker );
                              commit.Add( marker );
                          } );

                  // Blocks until finished.  The providers fan out onto KiCad's
                  // thread pool internally, but this call has to be on the UI
                  // thread because the handler above touches the view.
                  //
                  // Schematic parity is deliberately not tested: it needs a
                  // netlist from eeschema, and fetching one raises the schematic
                  // frame and can open dialogs -- which is the one thing a
                  // model-driven call must never do.
                  engine->RunTests( aFrame->GetUserUnits(), false, false );
                  engine->ClearViolationHandler();

                  // Not an edit: markers are a report about the board, so this
                  // must not become an undo step and must not mark the document
                  // dirty. Otherwise checking a board would make it look changed.
                  commit.Push( _( "DRC" ), SKIP_UNDO | SKIP_SET_DIRTY );
                  guard.Pushed();

                  // The other half of RecordDRCExclusions(): re-apply the user's
                  // exclusions to the markers this run just created, so a
                  // violation they had dismissed stays dismissed. DRC_TOOL does
                  // this at the same point for the same reason.
                  aFrame->ResolveDRCExclusions( false );

                  // An antenna module overhangs the board edge on purpose, its antenna clear
                  // of the copper, and its outline silkscreen with it; the fab clips that.
                  // Excluded with the reason, as the QA counts it, rather than left as
                  // "Silkscreen clipped by board edge" warnings on every ESP32 board.
                  int overhang = 0;

                  for( PCB_MARKER* marker : board->Markers() )
                  {
                      std::shared_ptr<RC_ITEM> item = marker->GetRCItem();

                      if( !item || marker->IsExcluded() || item->GetErrorCode() != DRCE_SILK_EDGE_CLEARANCE )
                          continue;

                      for( const KIID& id : { item->GetMainItemID(), item->GetAuxItemID() } )
                      {
                          BOARD_ITEM* silk = board->ResolveItem( id, true );
                          FOOTPRINT*  owner = silk ? silk->GetParentFootprint() : nullptr;

                          if( owner && !INDUCTIVE_BOARD_RULES::AntennaKeepouts( owner ).empty() )
                          {
                              marker->SetExcluded( true, _( "The antenna module overhangs the board edge by "
                                                            "design; the fab clips its outline silkscreen." ) );
                              ++overhang;
                              break;
                          }
                      }
                  }

                  // Into the project's settings, so the next save keeps them for KiCad's own
                  // DRC and kicad-cli (the DRC dialog does the same when it closes).
                  if( overhang )
                      board->RecordDRCExclusions();

                  aFrame->GetCanvas()->Refresh();

                  // Counted from the markers actually on the board, and grouped,
                  // so the model is told "4 clearance violations" rather than the
                  // same sentence four times.
                  BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();

                  std::map<wxString, int> errors;
                  std::map<wxString, int> warnings;
                  int                     errorCount   = 0;
                  int                     warningCount = 0;
                  wxString                detail;

                  int unroutedMarkers = 0;

                  for( PCB_MARKER* marker : board->Markers() )
                  {
                      std::shared_ptr<RC_ITEM> item = marker->GetRCItem();

                      if( !item )
                          continue;

                      // A marker excluded by the user is neither an error nor a
                      // warning: they have looked at it and decided.
                      if( marker->IsExcluded() )
                          continue;

                      // The marker's own severity wins where it has one. A custom
                      // rule in the user's .kicad_dru can declare its own, and
                      // reading only the board defaults would report their warning
                      // as an error -- telling an engineer their board fails on a
                      // rule they deliberately made advisory.
                      SEVERITY severity = marker->GetSeverity();

                      if( severity == RPT_SEVERITY_UNDEFINED )
                          severity = bds.GetSeverity( item->GetErrorCode() );

                      const int      code  = item->GetErrorCode();
                      const wxString label = item->GetErrorText( true );

                      // Unrouted connections get their own line below, from the
                      // ratsnest. Counting the markers here as well would report
                      // the same fact twice with two different numbers.
                      if( code == DRCE_UNCONNECTED_ITEMS )
                      {
                          ++unroutedMarkers;
                          continue;
                      }

                      if( severity == RPT_SEVERITY_ERROR )
                      {
                          errors[label]++;
                          ++errorCount;

                          // The first few in full -- what, between which items, where -- so
                          // the model can fix them and the user can find them.
                          if( errorCount <= 6 )
                          {
                              wxArrayString parts;

                              for( const KIID& id : { item->GetMainItemID(), item->GetAuxItemID() } )
                              {
                                  if( BOARD_ITEM* it = board->ResolveItem( id, true ) )
                                  {
                                      wxString what = it->GetItemDescription( aFrame, false );

                                      if( it->m_Uuid != niluuid )
                                          what << wxS( " [" ) << it->m_Uuid.AsString().Left( 8 ) << wxS( "]" );

                                      parts.Add( what );
                                  }
                              }

                              const VECTOR2I at = marker->GetPosition();
                              detail << wxString::Format( wxS( "  - %s: %s at %s, %s mm\n" ), item->GetErrorMessage( false ),
                                                          wxJoin( parts, '/', '\0' ), Mm( at.x ), Mm( at.y ) );
                          }
                      }
                      else if( severity == RPT_SEVERITY_WARNING )
                      {
                          warnings[label]++;
                          ++warningCount;
                      }
                  }

                  // false, not true: `true` filters to what is currently visible,
                  // so the answer would change with the user's layer visibility.
                  int unrouted = unroutedMarkers;

                  if( std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity() )
                      unrouted = (int) conn->GetUnconnectedCount( false );

                  wxString out;

                  if( !errorCount && !warningCount && !unrouted )
                  {
                      out << _( "DRC clean: no errors, no warnings, nothing unrouted.\n" );
                  }
                  else
                  {
                      out << wxString::Format( _( "DRC: %d error(s), %d warning(s), %d unrouted\n" ),
                                               errorCount, warningCount, unrouted );
                  }

                  // Markdown list lines: the chat joins plain lines into one run-on paragraph.
                  for( const auto& [label, count] : errors )
                      out << wxString::Format( wxS( "- %d %s: %s\n" ), count, count == 1 ? wxS( "error" ) : wxS( "errors" ), label );

                  out << detail;

                  for( const auto& [label, count] : warnings )
                      out << wxString::Format( wxS( "- %d %s: %s\n" ), count, count == 1 ? wxS( "warning" ) : wxS( "warnings" ), label );

                  if( unrouted )
                  {
                      out << wxS( "\n" ) << _( "Unrouted connections are ratsnest lines with no copper; "
                                "list_nets with unrouted_only shows which nets.\n" );
                  }

                  if( overhang )
                  {
                      out << wxS( "\n" ) << wxString::Format( _( "Excluded %d silkscreen finding(s): the antenna "
                                                                 "module's outline over the board edge, by design.\n" ),
                                                              overhang );
                  }

                  aResult = out;
                  return true;
              }


// ──────────────────────── placement and routing ────────────────────────
//
// The layout engine -- placement, routing, pours and teardrops, board setup and
// silkscreen placement -- runs on Inductive's servers, not in the app. A layout step
// sends the board as it is now (unsaved edits included), its project and rules and the
// currents the schematic stated; the server runs the step on its copy and sends the
// board back, and the change lands here as one undo step.

/// Drop the selection: a selected footprint is about to be moved or re-tracked under it.
static void ClearSelection( PCB_EDIT_FRAME* aFrame )
{
    if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
    {
        if( PCB_SELECTION_TOOL* sel = mgr->GetTool<PCB_SELECTION_TOOL>() )
            sel->ClearSelection();
    }
}


bool SyncSchematic( PCB_EDIT_FRAME* aFrame, wxString& aResult );


/// The currents the schematic recorded for its nets (<project>.inductive-currents.json,
/// beside the board), net name without its sheet path -> amps. Empty without one.
static std::map<wxString, double> StatedCurrents( BOARD* aBoard )
{
    std::map<wxString, double> amps;
    wxFileName                 file( aBoard->GetFileName() );

    if( !file.IsAbsolute() )
        return amps;

    file.SetFullName( file.GetName() + wxS( ".inductive-currents.json" ) );

    // Most designs state no currents. Opening a missing file makes wxWidgets put up an
    // error dialog in the middle of /layout, so look before opening.
    if( !file.FileExists() )
        return amps;

    wxFFile  in( file.GetFullPath(), "r" );
    wxString text;

    if( !in.IsOpened() || !in.ReadAll( &text ) )
        return amps;

    const json doc = json::parse( text.ToStdString( wxConvUTF8 ), nullptr, false );

    if( !doc.is_object() || !doc.contains( "nets" ) || !doc["nets"].is_object() )
        return amps;

    for( const auto& [net, value] : doc["nets"].items() )
    {
        if( value.is_number() && value.get<double>() >= 0.01 && value.get<double>() <= 50 )
            amps[wxString::FromUTF8( net ).AfterLast( '/' )] = value.get<double>();
    }

    return amps;
}


/// A whole file as bytes, or empty.
static std::string ReadFileText( const wxString& aPath )
{
    std::ifstream in( std::string( aPath.utf8_str() ), std::ios::binary );
    return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
}


/**
 * The board the server sent back, applied to the open one as one undo step
 * (inductive_board_apply.h), and the editor's layer controls rebuilt if its copper
 * layers changed.
 */
static void ApplyBoard( PCB_EDIT_FRAME* aFrame, BOARD* aFresh, const wxString& aTitle )
{
    BOARD* board = RequireBoard( aFrame );

    if( INDUCTIVE_BOARD_APPLY::ApplyLayers( board, aFresh ) )
    {
        aFrame->ReCreateLayerBox();
        aFrame->UpdateUserInterface();

        if( aFrame->GetAppearancePanel() )
            aFrame->GetAppearancePanel()->OnBoardChanged();

        aFrame->GetCanvas()->SyncLayersVisibility( board );
    }

    BOARD_COMMIT                         commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
    INDUCTIVE_BOARD_APPLY::StageItems( board, commit, aFresh );
    // The pours arrive filled (and cleaned up) and the teardrops fitted by the service:
    // no automatic refill or refit here.
    commit.Push( aTitle, ZONE_FILL_OP | SKIP_TEARDROPS );
    guard.Pushed();

    board->BuildConnectivity();
    aFrame->GetCanvas()->Refresh();
}


/**
 * Run a layout step on Inductive's servers and apply what comes back. aTitle names the
 * progress dialog (with Cancel) and the undo step. Throws, with the message for the
 * model, when the server refuses or cannot be reached; aResult is the step's report.
 */
static bool RemoteBoardJob( PCB_EDIT_FRAME* aFrame, const std::string& aOp, const json& aArgs,
                            const wxString& aTitle, wxString& aResult, json* aData = nullptr )
{
    BOARD* board = RequireBoard( aFrame );
    ClearSelection( aFrame );

    wxString name = wxFileName( board->GetFileName() ).GetName();

    if( name.IsEmpty() )
        name = wxS( "board" );

    // The board and project as the editor has them now, unsaved edits included.
    const wxString dir = wxFileName::CreateTempFileName( wxFileName::GetTempDir() + wxS( "/inductive-job" ) );
    wxRemoveFile( dir );

    if( !wxFileName::Mkdir( dir, 0700 ) )
        throw ARGS::BAD_ARGUMENT( _( "Could not prepare the board to send." ) );

    auto cleanup = [&]()
    {
        wxArrayString files;
        wxDir::GetAllFiles( dir, &files, wxEmptyString, wxDIR_FILES );

        for( const wxString& file : files )
            wxRemoveFile( file );

        wxFileName::Rmdir( dir );
    };

    json body;

    try
    {
        PCB_IO_KICAD_SEXPR io;
        io.SaveBoard( dir + "/" + name + ".kicad_pcb", board );
        Pgm().GetSettingsManager().SaveProjectCopy( dir + "/" + name + ".kicad_pro", &aFrame->Prj() );

        json files = { { std::string( ( name + ".kicad_pcb" ).utf8_str() ), ReadFileText( dir + "/" + name + ".kicad_pcb" ) },
                       { std::string( ( name + ".kicad_pro" ).utf8_str() ), ReadFileText( dir + "/" + name + ".kicad_pro" ) } };

        wxFileName rules( board->GetFileName() );
        rules.SetExt( FILEEXT::DesignRulesFileExtension );

        if( rules.IsAbsolute() && rules.FileExists() )
            files[std::string( ( name + ".kicad_dru" ).utf8_str() )] = ReadFileText( rules.GetFullPath() );

        json currents = json::object();

        for( const auto& [net, amps] : StatedCurrents( board ) )
            currents[std::string( net.utf8_str() )] = amps;

        body = { { "op", aOp }, { "args", aArgs }, { "name", std::string( name.utf8_str() ) },
                 { "currents", currents }, { "files", files } };
    }
    catch( const IO_ERROR& e )
    {
        cleanup();
        throw ARGS::BAD_ARGUMENT( _( "Could not prepare the board to send: " ) + e.What() );
    }

    // Sent from a worker; KiCad's progress dialog meanwhile, whose Cancel abandons it.
    std::atomic<bool>                 stop{ false }, done{ false };
    INDUCTIVE_CLOUD_AUTH::ApiResponse response;
    // Development: INDUCTIVE_LAYOUT_URL=http://127.0.0.1:PORT sends the job to a layout
    // worker running on this machine instead, without signing in.
    wxString local;
    const bool loopback = wxGetEnv( wxS( "INDUCTIVE_LAYOUT_URL" ), &local ) && local.StartsWith( wxS( "http://127.0.0.1:" ) );

    std::thread worker(
            [&]()
            {
                if( loopback )
                {
                    try
                    {
                        KICAD_CURL_EASY    curl;
                        std::ostringstream stream;
                        curl.SetURL( std::string( local.utf8_str() ) + "/v1/board/job" );
                        curl.SetHeader( "Content-Type", "application/json" );
                        curl.SetTimeout( 600 );
                        curl.SetOutputStream( &stream );
                        curl.SetPostFields( body.dump() );
                        curl.SetTransferCallback( [&]( size_t, size_t, size_t, size_t ) { return stop ? 1 : 0; }, 100000 );

                        if( curl.Perform() != 0 )
                            response.error = stop ? wxString( wxS( "Cancelled" ) ) : wxString( wxS( "Network error" ) );

                        response.status = curl.GetResponseStatusCode();
                        response.body = stream.str();
                    }
                    catch( const std::exception& e )
                    {
                        response.error = wxString::FromUTF8( e.what() );
                    }
                }
                else
                {
                    response = INDUCTIVE_CLOUD_AUTH::ApiRequest( wxS( "POST" ), wxS( "/v1/board/job" ), body.dump(), 600,
                                                                 [&]() { return !stop.load(); } );
                }

                done = true;
            } );

    {
        WX_PROGRESS_REPORTER reporter( aFrame, aTitle, 1, PR_CAN_ABORT );
        const auto           started = std::chrono::steady_clock::now();
        reporter.Report( aTitle );

        while( !done )
        {
            const double seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - started ).count();
            reporter.SetCurrentProgress( seconds / ( seconds + 15.0 ) );

            if( !reporter.KeepRefreshing() )
                stop = true;

            wxMilliSleep( 50 );
        }
    }

    worker.join();

    if( stop )
    {
        cleanup();
        aResult = _( "Cancelled: the board is unchanged. The user pressed Cancel: say so, and do not run this step "
                     "again unless they ask." );
        return false;
    }

    if( response.signedOut )
    {
        cleanup();
        throw ARGS::BAD_ARGUMENT( _( "Sign in to Inductive to lay out boards (account menu, Sign in)." ) );
    }

    if( !response.error.IsEmpty() )
    {
        cleanup();
        throw ARGS::BAD_ARGUMENT( _( "Inductive's layout service could not be reached: " ) + response.error
                                  + _( ". The board is unchanged; try again." ) );
    }

    const json out = json::parse( response.body, nullptr, false );

    if( response.status != 200 || !out.is_object() )
    {
        cleanup();
        const std::string message = out.is_object() ? out.value( "message", out.value( "error", std::string() ) )
                                                     : std::string();
        throw ARGS::BAD_ARGUMENT( message.empty() ? wxString::Format( _( "The layout service answered %d. The board "
                                                                         "is unchanged." ), response.status )
                                                  : wxString::FromUTF8( message ) );
    }

    if( out.contains( "error" ) && out["error"].is_string() )
    {
        cleanup();
        throw ARGS::BAD_ARGUMENT( wxString::FromUTF8( out["error"].get<std::string>() ) );
    }

    const std::string pcb = std::string( ( name + ".kicad_pcb" ).utf8_str() );

    if( out.contains( "files" ) && out["files"].contains( pcb ) && out["files"][pcb].is_string() )
    {
        const wxString returned = dir + "/returned.kicad_pcb";
        std::ofstream( std::string( returned.utf8_str() ), std::ios::binary ) << out["files"][pcb].get<std::string>();

        std::unique_ptr<BOARD> fresh;

        try
        {
            // Read on its own: given this project, a board takes over the project's settings.
            PCB_IO_KICAD_SEXPR io;
            fresh.reset( io.LoadBoard( returned, nullptr, nullptr, nullptr ) );
        }
        catch( const IO_ERROR& e )
        {
            cleanup();
            throw ARGS::BAD_ARGUMENT( _( "The layout service sent back a board that could not be read: " ) + e.What() );
        }

        INDUCTIVE_BOARD_APPLY::ApplyNetClasses( board, out.value( "net_classes", json() ) );
        ApplyBoard( aFrame, fresh.get(), aTitle );
    }

    cleanup();

    aResult = wxString::FromUTF8( out.value( "result", std::string() ) );

    if( aData )
        *aData = out.value( "data", json::object() );

    return out.value( "ok", false );
}


/**
 * /layout: sync the published schematic, place, route and run DRC, in that order,
 * with no model in the loop. Placing and routing run on the server as one step (it
 * retries a fresh board that will not route on a roomier one); sync and DRC run here.
 * Stops at the first step that does not complete, and says which.
 */
bool Layout( PCB_EDIT_FRAME* aFrame, wxString& aResult )
{
    wxString out;

    auto step = [&]( const wxString& aName, const std::function<bool( wxString& )>& aRun ) -> bool
    {
        wxString said;
        bool     ok = false;

        try
        {
            ok = aRun( said );
        }
        catch( const KI_PARAM_ERROR& e ) { said = e.What(); }
        catch( const std::exception& e ) { said = wxString::FromUTF8( e.what() ); }

        // Each step's own "what next" hint is the next step here.
        for( const wxChar* hint : { wxS( " Next: /place, then /route." ), wxS( " Next: /route." ),
                                    wxS( " Run /drc to confirm." ) } )
        {
            said.Replace( hint, wxEmptyString );
        }

        // One paragraph per step (the chat joins single lines into one), each step's own
        // detail lines folded into it, and no "DRC: DRC: ..." when the step names itself.
        wxArrayString lines = wxSplit( said.Trim().Trim( false ), '\n', '\0' );
        wxString      text;

        for( wxString line : lines )
        {
            line.Trim().Trim( false );

            if( line.StartsWith( wxS( "- " ) ) )
                line = line.Mid( 2 );

            while( line.Replace( wxS( "  " ), wxS( " " ) ) )
                ;

            if( line.IsEmpty() )
                continue;

            // A line that ends a sentence is followed by a space, not a "; " (".;").
            if( text.IsEmpty() )
                text = line;
            else if( text.EndsWith( wxS( "." ) ) || text.EndsWith( wxS( ":" ) ) )
                text << wxS( " " ) << line;
            else
                text << wxS( "; " ) << line;
        }

        if( !out.IsEmpty() )
            out << "\n\n";

        const bool named = text.StartsWith( aName + wxS( ":" ) ) || text.StartsWith( aName + wxS( " " ) );
        out << ( named ? text : aName + wxS( ": " ) + text );

        if( !ok )
            out << "\n\n" << wxString::Format( _( "Stopped at %s; the steps before it are kept (Undo reverses them)." ),
                                               aName.Lower() );

        return ok;
    };

    const bool synced = step( _( "Sync" ), [&]( wxString& r ) { return SyncSchematic( aFrame, r ); } );
    json       laid;
    wxString   refused;

    if( synced )
    {
        try
        {
            wxString said;
            RemoteBoardJob( aFrame, "layout", json::object(), _( "Laying Out the Board" ), said, &laid );

            if( !laid.contains( "placed" ) )
                refused = said;   // cancelled
        }
        catch( const KI_PARAM_ERROR& e ) { refused = e.What(); }
        catch( const std::exception& e ) { refused = wxString::FromUTF8( e.what() ); }
    }

    auto text = [&]( const char* aKey ) { return wxString::FromUTF8( laid.value( aKey, std::string() ) ); };
    const bool placed = laid.value( "placed", false ), routed = laid.value( "routed", false );

    const bool done =
            synced
            && step( _( "Place" ), [&]( wxString& r ) { r = refused.IsEmpty() ? text( "place" ) : refused; return placed; } )
            && step( _( "Route" ), [&]( wxString& r )
                     {
                         r = text( "route" );
                         if( !text( "retried" ).IsEmpty() )
                             r << wxS( " " ) << text( "retried" );
                         return routed;
                     } )
            && [&]()
               {
                   // Added before DRC, so the check covers it.
                   if( !text( "title" ).IsEmpty() )
                       out << "\n\n" << text( "title" );
                   return true;
               }()
            && step( _( "DRC" ), [&]( wxString& r ) { return RunDRC( aFrame, r ); } );

    aResult = out.Trim();
    return done;
}


// ───────────────────────────── fabrication ─────────────────────────────

/**
 * Why a board should not go to a fab yet, from its current DRC markers and ratsnest:
 * unrouted connections and DRC errors the user has not excluded, grouped and counted
 * the way run_drc reports them. Empty when it can go.
 */
/// The footprint both items of a DRC finding belong to, or null when they are not
/// parts of one footprint.
static const FOOTPRINT* WithinOneFootprint( BOARD* aBoard, const RC_ITEM& aItem )
{
    BOARD_ITEM* a = aBoard->ResolveItem( aItem.GetMainItemID(), true );
    BOARD_ITEM* b = aBoard->ResolveItem( aItem.GetAuxItemID(), true );
    const FOOTPRINT* fa = a ? a->GetParentFootprint() : nullptr;
    const FOOTPRINT* fb = b ? b->GetParentFootprint() : nullptr;
    return fa && fa == fb ? fa : nullptr;
}


wxString FabBlockersOf( BOARD* aBoard, wxString* aNote )
{
    BOARD_DESIGN_SETTINGS&  bds = aBoard->GetDesignSettings();
    std::map<wxString, int> errors;
    int                     errorCount = 0;
    std::set<wxString>      inside;   // footprints with findings between their own items

    for( PCB_MARKER* marker : aBoard->Markers() )
    {
        std::shared_ptr<RC_ITEM> item = marker->GetRCItem();

        if( !item || marker->IsExcluded() || item->GetErrorCode() == DRCE_UNCONNECTED_ITEMS )
            continue;

        SEVERITY severity = marker->GetSeverity();

        if( severity == RPT_SEVERITY_UNDEFINED )
            severity = bds.GetSeverity( item->GetErrorCode() );

        if( severity != RPT_SEVERITY_ERROR )
            continue;

        // Two items of one library footprint (a USB-C receptacle's pad beside its own
        // locating hole) are how the part is drawn: no routing or placement changes
        // them, and fabs build those footprints routinely. Said, not blocking.
        if( const FOOTPRINT* fp = WithinOneFootprint( aBoard, *item ) )
        {
            inside.insert( fp->GetReference() );
            continue;
        }

        errors[item->GetErrorText( true )]++;
        ++errorCount;
    }

    if( aNote && !inside.empty() )
    {
        wxArrayString refs;
        for( const wxString& r : inside ) refs.Add( r );
        *aNote = wxString::Format( _( " DRC also flags spacing inside the library footprint of %s (between "
                                      "that part's own pads and holes). That is how the part is drawn and "
                                      "does not stop the export; check it in the fab's preview." ),
                                   wxJoin( refs, ',' ) );
    }

    int unrouted = 0;

    if( std::shared_ptr<CONNECTIVITY_DATA> conn = aBoard->GetConnectivity() )
        unrouted = (int) conn->GetUnconnectedCount( false );

    if( !unrouted && !errorCount )
        return wxEmptyString;

    wxString out = _( "Not exported: this board fails its checks, and a fab would build it as it is." );

    if( unrouted )
        out << wxString::Format( _( " %d connection(s) are unrouted." ), unrouted );

    if( errorCount )
    {
        out << wxString::Format( _( " %d DRC error(s):" ), errorCount );

        for( const auto& [label, count] : errors )
            out << wxString::Format( wxS( " %s (%d);" ), label, count );
    }

    out << ( unrouted ? _( " Route what is left (/route)" ) : _( " Fix them" ) )
        << _( " -- where two parts or tracks are too close, move them and route again -- then /drc and "
              "export again. To export this board anyway, ask for it in the chat." );
    return out;
}


/// Run DRC, so the markers on the board match what is said, then FabBlockersOf().
static wxString CheckFabReady( PCB_EDIT_FRAME* aFrame, wxString* aNote = nullptr )
{
    wxString drc;
    RunDRC( aFrame, drc );
    return FabBlockersOf( RequireBoard( aFrame ), aNote );
}


/**
 * Plot every enabled layer as Gerbers into a "gerbers" folder beside the board.
 *
 * The destination is derived from the board's own path and is never taken from
 * the model.  That is the whole of the file-writing policy on this side: the
 * model chooses *when* to export, never *where*, so a tool that writes files can
 * never become a write-anywhere primitive however the arguments are phrased.
 */
bool ExportGerbers( BOARD* aBoard, wxString& aResult, const wxString& aSubdir = wxS( "gerbers" ),
                   bool aFabricationOnly = false )
{
    if( !aBoard || aBoard->GetFileName().IsEmpty() )
    {
        aResult = _( "Save the board before exporting Gerbers." );
        return false;
    }

    wxFileName outDir( aBoard->GetFileName() );
    outDir.SetFullName( wxEmptyString );
    for( const wxString& part : wxSplit( aSubdir, '/' ) )
        outDir.AppendDir( part );

    if( !outDir.DirExists() && !outDir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        aResult = _( "Could not create the output directory." );
        return false;
    }

    PCB_PLOT_PARAMS plotOpts = aBoard->GetPlotOptions();
    plotOpts.SetFormat( PLOT_FORMAT::GERBER );
    plotOpts.SetOutputDirectory( outDir.GetPath() );

    const LSEQ layers = aBoard->GetEnabledLayers().SeqStackupForPlotting();
    int        written = 0;

    for( PCB_LAYER_ID layer : layers )
    {
        // A fab house reads every file it is given; drawing and comment layers
        // in an order are at best noise.
        if( aFabricationOnly && !IsCopperLayer( layer )
            && layer != F_Mask && layer != B_Mask && layer != F_Paste && layer != B_Paste
            && layer != F_SilkS && layer != B_SilkS && layer != Edge_Cuts )
        {
            continue;
        }

        wxFileName fn( aBoard->GetFileName() );
        PCB_PLOTTER::BuildPlotFileName( &fn, outDir.GetPath(), aBoard->GetLayerName( layer ),
                                        FILEEXT::GerberFileExtension );

        PLOTTER* plotter = StartPlotBoard( aBoard, &plotOpts, layer, aBoard->GetLayerName( layer ),
                                           fn.GetFullPath(), wxEmptyString, wxEmptyString );

        if( plotter )
        {
            LSEQ one;
            one.push_back( layer );
            PlotBoardLayers( aBoard, plotter, one, plotOpts );
            plotter->EndPlot();
            ++written;
            delete plotter;
        }
    }

    if( !written )
    {
        aResult = _( "No layers were plotted." );
        return false;
    }

    aResult = wxString::Format( _( "%d Gerber files written to %s" ), written, outDir.GetPath() );
    return true;
}


/// CSV field, quoted when it needs to be.
static wxString Csv( const wxString& aText )
{
    if( !aText.Contains( wxS( "," ) ) && !aText.Contains( wxS( "\"" ) ) && !aText.Contains( wxS( "\n" ) ) )
        return aText;

    wxString quoted = aText;
    quoted.Replace( wxS( "\"" ), wxS( "\"\"" ) );
    return wxS( "\"" ) + quoted + wxS( "\"" );
}


/// The LCSC number on a footprint, under any of the names tools commonly use.
static wxString LcscOf( const FOOTPRINT* aFp )
{
    for( const wxChar* name : { wxS( "LCSC" ), wxS( "LCSC Part #" ), wxS( "LCSC Part" ),
                                wxS( "JLCPCB Part #" ), wxS( "lcsc" ) } )
    {
        if( const PCB_FIELD* field = aFp->GetField( name ) )
        {
            wxString text = field->GetText().Upper().Trim().Trim( false );

            if( !text.IsEmpty() )
                return text;
        }
    }

    return wxEmptyString;
}


/**
 * Everything JLCPCB asks for when ordering assembled boards, in a "jlcpcb"
 * folder beside the board: a zip of Gerbers and drill files, the BOM
 * (Comment, Designator, Footprint, LCSC Part #) and the placement file
 * (Designator, Mid X, Mid Y, Layer, Rotation). Like the Gerber export, the
 * destination comes from the board's own path, never from the model.
 */
bool ExportJlcpcbFiles( BOARD* aBoard, wxString& aResult )
{
    if( !aBoard || aBoard->GetFileName().IsEmpty() )
    {
        aResult = _( "Save the board before exporting for JLCPCB." );
        return false;
    }

    const wxFileName boardFile( aBoard->GetFileName() );
    const wxString   base = boardFile.GetName();

    wxString plotted;

    // A previous export's files would otherwise ride along in the new zip.
    {
        wxFileName stale( boardFile );
        stale.SetFullName( wxEmptyString );
        stale.AppendDir( wxS( "jlcpcb" ) );
        stale.AppendDir( wxS( "gerbers" ) );

        if( stale.DirExists() )
        {
            wxArrayString old;
            wxDir::GetAllFiles( stale.GetPath(), &old, wxEmptyString, wxDIR_FILES );

            for( const wxString& file : old )
                wxRemoveFile( file );
        }
    }

    if( !ExportGerbers( aBoard, plotted, wxS( "jlcpcb/gerbers" ), true ) )
    {
        aResult = plotted;
        return false;
    }

    wxFileName jlcDir( boardFile );
    jlcDir.SetFullName( wxEmptyString );
    jlcDir.AppendDir( wxS( "jlcpcb" ) );
    wxFileName gerberDir( jlcDir );
    gerberDir.AppendDir( wxS( "gerbers" ) );

    // Drill files: Excellon, metric, PTH and NPTH separate, as JLCPCB expects.
    EXCELLON_WRITER drill( aBoard );
    drill.SetFormat( true );
    drill.SetOptions( false, false, VECTOR2I( 0, 0 ), false );

    if( !drill.CreateDrillandMapFilesSet( gerberDir.GetPath(), true, false ) )
    {
        aResult = _( "The drill files could not be written." );
        return false;
    }

    // One zip of the fabrication files, the thing JLCPCB's upload takes.
    const wxString zipPath = jlcDir.GetPath() + wxFileName::GetPathSeparator() + base + wxS( "-gerbers.zip" );
    int            zipped = 0;
    {
        wxFFileOutputStream out( zipPath );
        wxZipOutputStream   zip( out );
        wxArrayString       files;
        wxDir::GetAllFiles( gerberDir.GetPath(), &files, wxEmptyString, wxDIR_FILES );
        files.Sort();

        for( const wxString& path : files )
        {
            wxFFileInputStream in( path );

            if( !in.IsOk() )
                continue;

            zip.PutNextEntry( wxFileName( path ).GetFullName() );
            zip.Write( in );
            ++zipped;
        }

        if( !zip.Close() || !out.Close() )
        {
            aResult = _( "The Gerber zip could not be written." );
            return false;
        }
    }

    // BOM: one line per distinct part, the way JLCPCB groups it.
    struct LINE { wxArrayString refs; wxString value, footprint, lcsc; };
    std::map<wxString, LINE> bom;
    wxArrayString            missing;
    const VECTOR2I           origin = aBoard->GetDesignSettings().GetAuxOrigin();
    wxString                 cpl = wxS( "Designator,Mid X,Mid Y,Layer,Rotation\n" );
    int                      placed = 0, corrected = 0, overridden = 0;

    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        const wxString ref = fp->GetReference();

        if( ref.IsEmpty() || ref.StartsWith( wxS( "#" ) ) || fp->IsDNP() )
            continue;

        const wxString lcsc = LcscOf( fp );

        if( !fp->IsExcludedFromBOM() )
        {
            LINE l;
            l.value = fp->GetValue();
            l.footprint = fp->GetFPID().GetLibItemName();
            l.lcsc = lcsc;
            LINE& line = bom.emplace( l.value + wxS( "\x1f" ) + l.footprint + wxS( "\x1f" ) + lcsc, l ).first->second;
            line.refs.Add( ref );

            if( lcsc.IsEmpty() )
                missing.Add( ref );
        }

        if( !fp->IsExcludedFromPosFiles() )
        {
            // Millimetres from the board's auxiliary origin, Y up, as KiCad's own
            // placement export writes them.
            const VECTOR2I pos = fp->GetPosition() - origin;
            const bool     bottom = fp->GetLayer() == B_Cu;

            // JLCPCB's library turns many packages from KiCad's zero rotation; a part's own
            // "JLCPCB Rotation Offset" field (kicad-jlcpcb-tools' name) wins over the table.
            double correction = 0;
            bool   own = false;

            for( const wxChar* name : { wxS( "JLCPCB Rotation Offset" ), wxS( "JLC Rotation Offset" ) } )
            {
                if( const PCB_FIELD* field = fp->GetField( name ) )
                {
                    wxString text = field->GetText();

                    if( text.Trim().Trim( false ).ToCDouble( &correction ) )
                    {
                        own = true;
                        break;
                    }

                    correction = 0;
                }
            }

            if( own )
            {
                ++overridden;
            }
            else if( auto table = INDUCTIVE_JLCPCB::RotationCorrection(
                             fp->GetFPID().GetUniStringLibItemName().ToStdString() ) )
            {
                correction = *table;
                ++corrected;
            }

            const double rotation = INDUCTIVE_JLCPCB::CplRotation( fp->GetOrientation().AsDegrees(), bottom,
                                                                    correction );

            cpl << Csv( ref ) << wxS( "," )
                << wxString::FromCDouble( pcbIUScale.IUTomm( pos.x ), 4 ) << wxS( "mm," )
                << wxString::FromCDouble( -pcbIUScale.IUTomm( pos.y ), 4 ) << wxS( "mm," )
                << ( bottom ? wxS( "Bottom" ) : wxS( "Top" ) ) << wxS( "," )
                << wxString::FromCDouble( rotation, 1 ) << wxS( "\n" );
            ++placed;
        }
    }

    wxString bomText = wxS( "Comment,Designator,Footprint,LCSC Part #\n" );

    for( auto& [key, line] : bom )
    {
        line.refs.Sort();
        bomText << Csv( line.value ) << wxS( "," ) << Csv( wxJoin( line.refs, ',' ) ) << wxS( "," )
                << Csv( line.footprint ) << wxS( "," ) << Csv( line.lcsc ) << wxS( "\n" );
    }

    auto write = [&]( const wxString& aName, const wxString& aText )
    {
        wxFFile file( jlcDir.GetPath() + wxFileName::GetPathSeparator() + aName, wxS( "w" ) );
        return file.IsOpened() && file.Write( aText, wxConvUTF8 ) && file.Close();
    };

    if( !write( base + wxS( "-bom.csv" ), bomText ) || !write( base + wxS( "-cpl.csv" ), cpl ) )
    {
        aResult = _( "The BOM or placement file could not be written." );
        return false;
    }

    aResult = wxString::Format( _( "JLCPCB files written to %s:\n"
                                   "- %s-gerbers.zip (%d Gerber and drill files)\n"
                                   "- %s-bom.csv (%d lines)\n"
                                   "- %s-cpl.csv (%d placements)\n" ),
                                jlcDir.GetPath(), base, zipped, base, (int) bom.size(), base, placed );

    if( !missing.empty() )
    {
        missing.Sort();
        aResult << wxString::Format(
                _( "%d part(s) have no LCSC number and JLCPCB will not assemble them: %s. "
                   "Choose parts in the schematic (ask \"find real parts for this design\"), "
                   "then /pcb, /sync and export again.\n" ),
                (int) missing.size(), wxJoin( missing, ',' ) );
    }

    if( corrected || overridden )
    {
        aResult << wxString::Format( _( "Rotations corrected for JLCPCB's library: %d by package (SOIC, QFN, "
                                        "SOT-23 and the like), %d by the part's own JLCPCB Rotation Offset "
                                        "field.\n" ),
                                     corrected, overridden );
    }

    aResult << _( "Check part rotations in JLCPCB's placement preview before ordering: its library "
                   "can draw one part differently from another in the same package. Fix one by "
                   "adding a \"JLCPCB Rotation Offset\" field (degrees) to that part." );
    return true;
}


// ─────────────────────────── the tool table ───────────────────────────

/// A tool's implementation.  Throws INDUCTIVE_ARGS::BAD_ARGUMENT to refuse.
using HANDLER = std::function<bool( PCB_EDIT_FRAME*, const json&, wxString& )>;

struct ENTRY
{
    INDUCTIVE_TOOL decl;
    HANDLER        run;
};


/// find_items' filters, as find_items and the "where" of the edit tools read them.
static INDUCTIVE_ITEMS::QUERY ParseItemQuery( BOARD* aBoard, const json& aArgs )
{
    INDUCTIVE_ITEMS::QUERY query;

    if( ARGS::Has( aArgs, "kinds" ) )
        for( const wxString& kind : ARGS::RequireStringArray( aArgs, "kinds", 12 ) )
            query.kinds.push_back( kind.Lower() );

    query.layer = ARGS::OptionalString( aArgs, "layer", wxEmptyString );
    query.net = ARGS::OptionalString( aArgs, "net", wxEmptyString );
    query.reference = ARGS::OptionalString( aArgs, "reference", wxEmptyString );
    query.text = ARGS::OptionalString( aArgs, "text", wxEmptyString );

    if( !query.layer.IsEmpty() && aBoard->GetLayerID( query.layer ) == UNDEFINED_LAYER )
        throw ARGS::BAD_ARGUMENT( wxString::Format( _( "There is no layer '%s' on this board." ), query.layer ) );

    if( ARGS::Has( aArgs, "area" ) )
    {
        const json& a = aArgs.at( "area" );
        BOX2I box( VECTOR2I( ToIU( ARGS::RequireCoordMm( a, "x0" ) ), ToIU( ARGS::RequireCoordMm( a, "y0" ) ) ) );
        box.Merge( VECTOR2I( ToIU( ARGS::RequireCoordMm( a, "x1" ) ), ToIU( ARGS::RequireCoordMm( a, "y1" ) ) ) );
        query.area = box;
    }

    return query;
}


/**
 * The items an edit tool acts on: its "ids", or every item its "where" matches (the
 * find_items filters, with at least a kind), up to aMax. One or the other.
 */
static std::vector<BOARD_ITEM*> ItemsToEdit( BOARD* aBoard, const json& aArgs, size_t aMax )
{
    std::vector<BOARD_ITEM*> items;
    const bool               byIds = ARGS::Has( aArgs, "ids" );
    const bool               byWhere = ARGS::Has( aArgs, "where" );

    if( byIds == byWhere )
        throw ARGS::BAD_ARGUMENT( _( "Give either 'ids' (from find_items) or 'where' (find_items' filters)." ) );

    if( byIds )
    {
        for( const wxString& id : ARGS::RequireStringArray( aArgs, "ids", 200 ) )
        {
            wxString    why;
            BOARD_ITEM* item = INDUCTIVE_ITEMS::Resolve( aBoard, id, &why );

            if( !item )
                throw ARGS::BAD_ARGUMENT( why );

            items.push_back( item );
        }

        return items;
    }

    const json& where = aArgs.at( "where" );

    if( !where.is_object() || !ARGS::Has( where, "kinds" ) )
        throw ARGS::BAD_ARGUMENT( _( "'where' needs at least 'kinds', e.g. {\"kinds\":[\"track\"],\"net\":\"GND\"}." ) );

    size_t total = 0;
    items = INDUCTIVE_ITEMS::Find( aBoard, ParseItemQuery( aBoard, where ), aMax, &total );

    if( total > aMax )
        throw ARGS::BAD_ARGUMENT( wxString::Format( _( "'where' matches %d items, more than %d in one call; narrow it." ),
                                                    (int) total, (int) aMax ) );

    if( items.empty() )
        throw ARGS::BAD_ARGUMENT( _( "Nothing matches 'where'; find_items with the same filters shows what is there." ) );

    return items;
}


/// A tool's "points": [x, y] pairs in mm (or {"x":..,"y":..} objects), bounded.
static std::vector<VECTOR2I> RequirePoints( const json& aArgs, size_t aMin, size_t aMax )
{
    const auto it = aArgs.find( "points" );

    if( it == aArgs.end() || !it->is_array() || it->size() < aMin || it->size() > aMax )
        throw ARGS::BAD_ARGUMENT( wxString::Format( _( "Give 'points' as %d to %d [x, y] pairs in mm." ),
                                                    (int) aMin, (int) aMax ) );

    std::vector<VECTOR2I> out;

    for( const json& p : *it )
    {
        double x = 0, y = 0;

        if( p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number() )
        {
            x = p[0].get<double>();
            y = p[1].get<double>();
        }
        else if( p.is_object() )
        {
            x = ARGS::RequireCoordMm( p, "x" );
            y = ARGS::RequireCoordMm( p, "y" );
        }
        else
        {
            throw ARGS::BAD_ARGUMENT( _( "Each point is [x, y] in mm." ) );
        }

        out.emplace_back( ToIU( x ), ToIU( y ) );
    }

    return out;
}


/// Shorthand for a JSON Schema property list, to keep the declarations readable.




/// The netlist updater's messages, with its errors and warnings counted: KiCad 10's
/// updater reports each one through its reporter but does not count them itself.
class COUNTING_REPORTER : public WX_STRING_REPORTER
{
public:
    REPORTER& Report( const wxString& aText, SEVERITY aSeverity = RPT_SEVERITY_UNDEFINED ) override
    {
        if( aSeverity == RPT_SEVERITY_ERROR )
            ++errors;
        else if( aSeverity == RPT_SEVERITY_WARNING )
            ++warnings;

        // What the user needs to read: the sync's problems. Its per-part progress lines
        // ("Updated R3 fields. Connected R3 pin 2 to /GND. ...") once filled the chat.
        if( aSeverity == RPT_SEVERITY_ERROR || aSeverity == RPT_SEVERITY_WARNING )
            problems << aText << wxS( "\n" );

        return WX_STRING_REPORTER::Report( aText, aSeverity );
    }

    void Clear() override
    {
        WX_STRING_REPORTER::Clear();
        problems.Clear();
        errors = warnings = 0;
    }

    int      errors = 0;
    int      warnings = 0;
    wxString problems;
};


bool SyncSchematic( PCB_EDIT_FRAME* aFrame, wxString& aResult )
{
    auto* board = RequireBoard( aFrame );
    wxFileName file( board->GetFileName() );
    if( !file.IsAbsolute() )
        throw ARGS::BAD_ARGUMENT( "Save this PCB beside its schematic with the same base name first." );
    file.SetExt( "inductive.net" );
    if( !file.FileExists() )
        throw ARGS::BAD_ARGUMENT( "No prepared schematic for this PCB. In the schematic editor run /pcb first, then /sync here. Expected " + file.GetFullPath() );
    NETLIST netlist;
    std::unique_ptr<NETLIST_READER> reader( NETLIST_READER::GetNetlistReader( &netlist, file.GetFullPath() ) );
    if( !reader ) throw ARGS::BAD_ARGUMENT( "The prepared netlist could not be read." );
    reader->LoadNetlist();
    if( netlist.IsEmpty() ) throw ARGS::BAD_ARGUMENT( "The prepared netlist has no components." );
    if( netlist.GetCount() > 500 ) throw ARGS::BAD_ARGUMENT( "This sync is limited to 500 components." );

    // The schematic editor may have just installed a project-local attachment library.
    Pgm().GetLibraryManager().ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT,
                                           { LIBRARY_TABLE_TYPE::FOOTPRINT } );
    auto* libs = RequireLibraries();
    // Resolve every footprint before touching the board, including those with a
    // user-chosen assignment. A miss must never produce a half-imported circuit.
    for( unsigned i = 0; i < netlist.GetCount(); ++i )
    {
        auto* part = netlist.GetComponent( i );
        const auto& id = part->GetFPID();
        if( !id.IsValid() || id.GetUniStringLibNickname().IsEmpty() )
            throw ARGS::BAD_ARGUMENT( "Missing footprint for " + part->GetReference() );
        if( !libs->IsLibraryLoaded( id.GetUniStringLibNickname() ) )
            libs->LoadOne( id.GetUniStringLibNickname() );
        std::unique_ptr<FOOTPRINT> fp( aFrame->LoadFootprint( id ) );
        if( !fp ) throw ARGS::BAD_ARGUMENT( "Footprint unavailable for " + part->GetReference() );
        for( unsigned j = 0; j < part->GetNetCount(); ++j )
            if( !fp->FindPadByNumber( part->GetNet( j ).GetPinName() ) )
                throw ARGS::BAD_ARGUMENT( "Footprint pad missing for " + part->GetReference()
                                         + "." + part->GetNet( j ).GetPinName() );
    }
    COUNTING_REPORTER reporter;
    BOARD_NETLIST_UPDATER updater( aFrame, board );
    updater.SetReporter( &reporter );
    updater.SetDeleteUnusedFootprints( false );
    updater.SetLookupByTimestamp( true );
    updater.SetReplaceFootprints( true );
    updater.SetTransferGroups( false );
    updater.SetApplyDesignBlockLayouts( false );
    updater.SetOverrideLocks( false );
    // Part data chosen in the schematic (LCSC, MPN, Manufacturer) must reach the
    // footprints, or the JLCPCB export has nothing to read. Fields already on a
    // footprint and absent from the schematic are kept.
    updater.SetUpdateFields( true );
    BOARD_NETLIST_UPDATER preflight( aFrame, board );
    preflight.SetReporter( &reporter );
    preflight.SetDeleteUnusedFootprints( false );
    preflight.SetLookupByTimestamp( true );
    preflight.SetReplaceFootprints( true );
    preflight.SetTransferGroups( false );
    preflight.SetApplyDesignBlockLayouts( false );
    preflight.SetOverrideLocks( false );
    preflight.SetUpdateFields( true );
    preflight.SetIsDryRun( true );
    preflight.UpdateNetlist( netlist );
    if( reporter.errors )
        throw ARGS::BAD_ARGUMENT( "PCB sync preflight failed:\n"
                                  + ( reporter.problems.IsEmpty() ? reporter.GetMessages() : reporter.problems ) );
    reporter.Clear();
    updater.SetIsDryRun( false );
    updater.UpdateNetlist( netlist );

    auto added = updater.GetAddedFootprints();
    if( !added.empty() )
    {
        // A staging tray, not a placement algorithm. Existing parts stay put;
        // new ones are laid out beside the board so they can be picked by hand.
        auto bounds = board->GetBoardEdgesBoundingBox();
        int x = bounds.GetWidth() > 0 ? bounds.GetRight() + ToIU( 15 ) : ToIU( 100 );
        int y = bounds.GetHeight() > 0 ? bounds.GetTop() : ToIU( 100 );
        BOARD_COMMIT commit( aFrame );
        INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
        for( auto* fp : added )
        {
            commit.Modify( fp );
            fp->ClearFlags( IS_NEW | IS_MOVING );
            // KiCad defers ratsnest participation until an imported part is placed.
            // Our staging commit completes that step without the interactive move tool.
            fp->SetAttributes( fp->GetAttributes() & ~FP_JUST_ADDED );
            auto box = fp->GetBoundingBox();
            fp->SetPosition( fp->GetPosition() + VECTOR2I( x - box.GetLeft(), y - box.GetTop() ) );
            y += box.GetHeight() + ToIU( 5 );
        }
        commit.Push( _( "Stage Imported Footprints" ) );
        guard.Pushed();
    }
    board->SynchronizeNetsAndNetClasses( false );
    board->BuildConnectivity();
    aFrame->Compile_Ratsnest( false );
    aFrame->GetCanvas()->Refresh();
    aFrame->GetToolManager()->PostAction( ACTIONS::zoomFitScreen );
    aResult = wxString::Format(
            "Synced %u schematic components; %zu new footprints staged beside the board. "
            "Existing footprints/copper preserved; no routing performed. %d sync error(s), %d warning(s). "
            "Next: /place, then /route.\n",
            netlist.GetCount(), added.size(), reporter.errors, reporter.warnings )
            + reporter.problems;
    return reporter.errors == 0;
}

std::vector<ENTRY> BuildTools()
{
    std::vector<ENTRY> tools;
    tools.push_back(
            { { wxS( "sync_schematic" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  return SyncSchematic( aFrame, aResult );
              } } );


    // ── inspection ────────────────────────────────────────────────────
    //
    // These exist because the alternative is a model that guesses.  The document
    // summary in the system prompt is a snapshot taken once per turn and lists
    // at most sixty parts; anything precise -- where something is, what it is
    // wired to, whether a net is routed -- has to be asked for.

    tools.push_back(
            { { wxS( "get_board_overview" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  BOARD*       board = RequireBoard( aFrame );
                  const BOX2I  bbox  = board->GetBoardEdgesBoundingBox();

                  wxString out;
                  out << wxString::Format( _( "%d footprints, %d nets, %d tracks, %d zones\n" ),
                                           (int) board->Footprints().size(),
                                           (int) board->GetNetCount(),
                                           (int) board->Tracks().size(),
                                           (int) board->Zones().size() );

                  if( bbox.GetWidth() > 0 && bbox.GetHeight() > 0 )
                  {
                      out << wxString::Format( _( "Board outline: %s x %s mm, from %s to %s\n" ),
                                               Mm( bbox.GetWidth() ), Mm( bbox.GetHeight() ),
                                               Pos( bbox.GetOrigin() ), Pos( bbox.GetEnd() ) );
                  }
                  else
                  {
                      out << _( "Board outline: none drawn yet (no Edge.Cuts shapes).\n" );
                  }

                  out << wxString::Format( _( "Copper layers: %d\n" ),
                                           board->GetCopperLayerCount() );

                  // The ratsnest is the honest measure of "is this board finished":
                  // a connection the router never made is still owed, however
                  // clean the design rules come back.
                  if( std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity() )
                  {
                      const int unrouted = conn->GetUnconnectedCount( false );

                      out << ( unrouted == 0
                                       ? _( "Routing: every connection is routed.\n" )
                                       : wxString::Format(
                                                 _( "Routing: %d connections still unrouted.\n" ),
                                                 unrouted ) );
                  }

                  BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();

                  out << wxString::Format( _( "Default track width %s mm, via %s mm\n" ),
                                           Mm( bds.GetCurrentTrackWidth() ),
                                           Mm( bds.GetCurrentViaSize() ) );

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "list_footprints" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*         board  = RequireBoard( aFrame );
                  const wxString filter = ARGS::OptionalString( aArgs, "filter" );

                  // Sorted by reference so repeated calls read the same way and a
                  // model can rely on the order when it plans several edits.
                  std::vector<FOOTPRINT*> matched;

                  for( FOOTPRINT* fp : board->Footprints() )
                  {
                      if( !filter.IsEmpty()
                          && !fp->GetReference().Lower().Contains( filter.Lower() )
                          && !fp->GetValue().Lower().Contains( filter.Lower() ) )
                      {
                          continue;
                      }

                      matched.push_back( fp );
                  }

                  std::sort( matched.begin(), matched.end(),
                             []( FOOTPRINT* a, FOOTPRINT* b )
                             {
                                 return a->GetReference().CmpNoCase( b->GetReference() ) < 0;
                             } );

                  if( matched.empty() )
                  {
                      aResult = filter.IsEmpty()
                                        ? _( "The board has no footprints." )
                                        : wxString::Format(
                                                  _( "No footprint matches '%s'." ), filter );
                      return true;
                  }

                  // The first line is the trace-row summary; see traceSummary()
                  // in the panel.
                  wxString out = wxString::Format( _( "%d footprints\n" ), (int) matched.size() );

                  const size_t limit = 300;
                  size_t       shown = 0;

                  for( FOOTPRINT* fp : matched )
                  {
                      if( shown++ >= limit )
                      {
                          out << wxString::Format( _( "...and %d more\n" ),
                                                   (int) ( matched.size() - limit ) );
                          break;
                      }

                      out << wxS( "  " ) << DescribeFootprint( board, fp ) << wxS( "\n" );
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_footprint" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* fp = RequireFootprint( board, ARGS::RequireString( aArgs, "reference" ) );

                  const BOX2I bbox = fp->GetBoundingBox( false );

                  wxString out;
                  out << DescribeFootprint( board, fp ) << wxS( "\n" );
                  out << wxString::Format( _( "Outline: %s x %s mm\n" ), Mm( bbox.GetWidth() ),
                                           Mm( bbox.GetHeight() ) );

                  // Pads() is a deque; copied so it can be sorted without
                  // reordering the footprint's own storage.
                  std::vector<PAD*> pads( fp->Pads().begin(), fp->Pads().end() );

                  std::sort( pads.begin(), pads.end(),
                             []( PAD* a, PAD* b )
                             {
                                 return a->GetNumber().CmpNoCase( b->GetNumber() ) < 0;
                             } );

                  out << wxString::Format( _( "%d pads:\n" ), (int) pads.size() );

                  for( PAD* pad : pads )
                  {
                      out << wxS( "  pad " ) << pad->GetNumber() << wxS( " at " )
                          << Pos( pad->GetPosition() ) << wxS( " mm  net " );

                      const wxString net = pad->GetNetname();

                      out << ( net.IsEmpty() ? _( "(none)" ) : net ) << wxS( "\n" );
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "list_nets" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  const bool unroutedOnly = ARGS::OptionalBool( aArgs, "unrouted_only", false );

                  std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity();

                  // Pad counts come from the pads themselves rather than from the
                  // net list: a net can exist in the file with nothing on it, and
                  // reporting those as real nets sends the model looking for pads
                  // that are not there.
                  std::map<int, int> padCount;

                  for( FOOTPRINT* fp : board->Footprints() )
                  {
                      for( PAD* pad : fp->Pads() )
                      {
                          if( pad->GetNetCode() > 0 )
                              padCount[pad->GetNetCode()]++;
                      }
                  }

                  wxString out;
                  int      listed = 0;

                  for( const auto& [netcode, count] : padCount )
                  {
                      NETINFO_ITEM* net = board->FindNet( netcode );

                      if( !net )
                          continue;

                      // The ratsnest edges for a net *are* its unrouted
                      // connections: an edge exists exactly where copper does
                      // not. Counting them is how "is this net finished" is
                      // answered without re-deriving connectivity ourselves.
                      int unrouted = 0;

                      if( conn )
                      {
                          if( RN_NET* rn = conn->GetRatsnestForNet( netcode ) )
                              unrouted = (int) rn->GetEdges().size();
                      }

                      if( unroutedOnly && unrouted == 0 )
                          continue;

                      out << wxS( "  " ) << net->GetNetname()
                          << wxString::Format( _( "  %d pads" ), count );

                      if( unrouted > 0 )
                          out << wxString::Format( _( ", %d unrouted" ), unrouted );
                      else
                          out << _( ", routed" );

                      out << wxS( "\n" );
                      ++listed;
                  }

                  aResult = ( listed ? wxString::Format( _( "%d nets\n" ), listed )
                                     : ( unroutedOnly ? _( "Every net is routed.\n" )
                                                      : _( "The board has no nets.\n" ) ) )
                            + out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_selection" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  PCB_SELECTION_TOOL* sel =
                          aFrame->GetToolManager()->GetTool<PCB_SELECTION_TOOL>();

                  if( !sel )
                      throw ARGS::BAD_ARGUMENT( _( "The selection is not available." ) );

                  const PCB_SELECTION& selection = sel->GetSelection();

                  if( selection.Empty() )
                  {
                      aResult = _( "Nothing is selected. Ask the user to click the item they "
                                   "mean, or to name it." );
                      return true;
                  }

                  wxString out = wxString::Format( _( "%d item(s) selected\n" ),
                                                   selection.Size() );

                  int shown = 0;

                  for( EDA_ITEM* item : selection )
                  {
                      if( shown++ >= 60 )
                      {
                          out << _( "  ...and more\n" );
                          break;
                      }

                      if( FOOTPRINT* fp = dynamic_cast<FOOTPRINT*>( item ) )
                      {
                          out << wxS( "  footprint " ) << DescribeFootprint( board, fp )
                              << wxS( "\n" );
                      }
                      else if( PCB_VIA* via = dynamic_cast<PCB_VIA*>( item ) )
                      {
                          out << wxS( "  via at " ) << Pos( via->GetPosition() ) << wxS( " mm, net " )
                              << via->GetNetname() << wxS( "\n" );
                      }
                      else if( PCB_TRACK* track = dynamic_cast<PCB_TRACK*>( item ) )
                      {
                          out << wxS( "  track on " ) << board->GetLayerName( track->GetLayer() )
                              << wxS( " from " ) << Pos( track->GetStart() ) << wxS( " to " )
                              << Pos( track->GetEnd() ) << wxS( " mm, net " )
                              << track->GetNetname() << wxS( "\n" );
                      }
                      else if( PAD* pad = dynamic_cast<PAD*>( item ) )
                      {
                          FOOTPRINT* parent = pad->GetParentFootprint();

                          out << wxS( "  pad " )
                              << ( parent ? parent->GetReference() : wxString( wxS( "?" ) ) )
                              << wxS( "." ) << pad->GetNumber() << wxS( " net " )
                              << pad->GetNetname() << wxS( "\n" );
                      }
                      else if( ZONE* zone = dynamic_cast<ZONE*>( item ) )
                      {
                          out << wxS( "  zone on " ) << board->GetLayerName( zone->GetLayer() )
                              << wxS( ", net " ) << zone->GetNetname() << wxS( "\n" );
                      }
                      else
                      {
                          out << wxS( "  " ) << item->GetFriendlyName() << wxS( "\n" );
                      }
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "show_on_canvas" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  const wxString reference = ARGS::OptionalString( aArgs, "reference" );
                  const wxString netName   = ARGS::OptionalString( aArgs, "net" );

                  wxString did;

                  if( !netName.IsEmpty() )
                  {
                      NETINFO_ITEM* net = RequireNet( board, netName );

                      board->SetHighLightNet( net->GetNetCode() );
                      board->HighLightON();
                      did << wxString::Format( _( "Highlighted net %s. " ), net->GetNetname() );
                  }
                  else
                  {
                      board->ResetNetHighLight();
                      board->HighLightOFF();
                  }

                  if( !reference.IsEmpty() )
                  {
                      FOOTPRINT* fp = RequireFootprint( board, reference );

                      // FocusOnItem scrolls and flashes the item.  It is a view
                      // operation only -- it does not select, so it cannot
                      // interfere with whatever the user had selected.
                      aFrame->FocusOnItem( fp );
                      did << wxString::Format( _( "Scrolled to %s at %s mm." ), fp->GetReference(),
                                               Pos( fp->GetPosition() ) );
                  }

                  aFrame->GetCanvas()->Refresh();

                  aResult = did.IsEmpty() ? _( "Cleared the net highlight." ) : did;
                  return true;
              } } );

    // ── the board itself ──────────────────────────────────────────────

    tools.push_back(
            { { wxS( "set_board_outline" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  const double width  = ARGS::RequireSizeMm( aArgs, "width" );
                  const double height = ARGS::RequireSizeMm( aArgs, "height" );
                  const double x      = ARGS::OptionalCoordMm( aArgs, "x", 0.0 );
                  const double y      = ARGS::OptionalCoordMm( aArgs, "y", 0.0 );

                  if( x + width > ARGS::MAX_COORD_MM || y + height > ARGS::MAX_COORD_MM )
                      throw ARGS::BAD_ARGUMENT( _( "That outline would fall outside the sheet." ) );

                  // Collected first, replaced in one commit: a board with the old
                  // outline deleted and the new one not yet drawn has no edge at
                  // all, and anything that reads the outline in between -- a
                  // zone fill, a DRC -- sees a board with no boundary.
                  std::vector<BOARD_ITEM*> oldEdges;

                  for( BOARD_ITEM* item : board->Drawings() )
                  {
                      if( item->GetLayer() == Edge_Cuts )
                          oldEdges.push_back( item );
                  }

                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                  {
                      if( PCB_SELECTION_TOOL* sel = mgr->GetTool<PCB_SELECTION_TOOL>() )
                          sel->ClearSelection();
                  }

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  for( BOARD_ITEM* item : oldEdges )
                      commit.Remove( item );

                  auto rect = std::make_unique<PCB_SHAPE>( board, SHAPE_T::RECTANGLE );

                  rect->SetLayer( Edge_Cuts );
                  rect->SetStart( VECTOR2I( ToIU( x ), ToIU( y ) ) );
                  rect->SetEnd( VECTOR2I( ToIU( x + width ), ToIU( y + height ) ) );
                  rect->SetStroke( STROKE_PARAMS( board->GetDesignSettings().GetLineThickness(
                                                          Edge_Cuts ),
                                                  LINE_STYLE::SOLID ) );

                  commit.Add( rect.release() );
                  commit.Push( _( "Set Board Outline" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format(
                          _( "Board outline set to %.3f x %.3f mm at %.3f, %.3f (replaced %d "
                             "existing Edge.Cuts shape(s))." ),
                          width, height, x, y, (int) oldEdges.size() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "set_design_rules" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  std::shared_ptr<NET_SETTINGS> settings =
                          board->GetDesignSettings().m_NetSettings;

                  if( !settings )
                      throw ARGS::BAD_ARGUMENT( _( "This board has no net settings." ) );

                  std::shared_ptr<NETCLASS> netclass = settings->GetDefaultNetclass();

                  if( !netclass )
                      throw ARGS::BAD_ARGUMENT( _( "This board has no default net class." ) );

                  // Read and validated in full before anything is written, so a
                  // bad via drill cannot leave the track width already changed.
                  const bool hasWidth = ARGS::Has( aArgs, "track_width" );
                  const bool hasClear = ARGS::Has( aArgs, "clearance" );
                  const bool hasVia   = ARGS::Has( aArgs, "via_diameter" );
                  const bool hasDrill = ARGS::Has( aArgs, "via_drill" );

                  if( !hasWidth && !hasClear && !hasVia && !hasDrill )
                      throw ARGS::BAD_ARGUMENT( _( "Give at least one rule to change." ) );

                  // Bounded to what a board fab can actually make. A 0.001 mm
                  // track is representable and meaningless, and the model has no
                  // way to know that unless it is told.
                  const double width = hasWidth ? ARGS::RequireNumber( aArgs, "track_width",
                                                                       0.05, 20.0 )
                                                : 0.0;
                  const double clear = hasClear ? ARGS::RequireNumber( aArgs, "clearance",
                                                                       0.05, 20.0 )
                                                : 0.0;
                  const double via = hasVia ? ARGS::RequireNumber( aArgs, "via_diameter", 0.1,
                                                                   10.0 )
                                            : 0.0;
                  const double drill = hasDrill ? ARGS::RequireNumber( aArgs, "via_drill", 0.05,
                                                                       10.0 )
                                                : 0.0;

                  // Checked against whichever value will be in force, not just
                  // against the one being set: changing only the drill on an
                  // existing via size has to be validated against that size.
                  const double effectiveVia =
                          hasVia ? via : ToMm( netclass->GetViaDiameter() );
                  const double effectiveDrill =
                          hasDrill ? drill : ToMm( netclass->GetViaDrill() );

                  if( effectiveDrill >= effectiveVia )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "A %.3f mm drill in a %.3f mm via leaves no copper ring." ),
                              effectiveDrill, effectiveVia ) );
                  }

                  wxString changed;

                  if( hasWidth )
                  {
                      netclass->SetTrackWidth( ToIU( width ) );
                      changed << wxString::Format( _( "track width %.3f mm" ), width );
                  }

                  if( hasClear )
                  {
                      netclass->SetClearance( ToIU( clear ) );
                      changed << ( changed.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                              << wxString::Format( _( "clearance %.3f mm" ), clear );
                  }

                  if( hasVia )
                  {
                      netclass->SetViaDiameter( ToIU( via ) );
                      changed << ( changed.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                              << wxString::Format( _( "via %.3f mm" ), via );
                  }

                  if( hasDrill )
                  {
                      netclass->SetViaDrill( ToIU( drill ) );
                      changed << ( changed.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                              << wxString::Format( _( "drill %.3f mm" ), drill );
                  }

                  // Net classes are project settings, not board items, so there is
                  // no BOARD_COMMIT for them and this is NOT undoable -- which is
                  // exactly how Board Setup behaves. Saying so is the honest part.
                  //
                  // Re-initialising the rule engine picks the new values up, but it
                  // parses the user's own .kicad_dru and throws on a syntax error
                  // in it.  By this point the net class has already changed, so
                  // letting that escape would report a failure for a change that
                  // did happen.  It is also not load-bearing: run_drc re-inits the
                  // engine itself and reports a bad rules file properly.
                  try
                  {
                      if( std::shared_ptr<DRC_ENGINE> engine =
                                  board->GetDesignSettings().m_DRCEngine )
                      {
                          engine->InitEngine( aFrame->GetDesignRulesPath() );
                      }
                  }
                  catch( ... )
                  {
                  }

                  // Netclass edits do not go through a commit, so nothing else
                  // bumps the board's modification counter -- and the render-time
                  // clearance cache in BOARD_CONNECTED_ITEM is keyed on it. Without
                  // this the canvas keeps drawing, and DRC keeps evaluating,
                  // against the clearances that were in force a moment ago.
                  board->IncrementTimeStamp();

                  aFrame->OnModify();
                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format(
                          _( "Default net class updated: %s. Existing copper keeps its old size; "
                             "re-run the design rule check to see what no longer complies. This "
                             "is a project setting, so Ctrl+Z will not undo it." ),
                          changed );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_silkscreen_text" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "add_silkscreen_text", aArgs, _( "Add Silkscreen Text" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "list_silkscreen_text" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );
                  json   out = json::array();

                  for( BOARD_ITEM* item : board->Drawings() )
                  {
                      if( item->Type() != PCB_TEXT_T || !INDUCTIVE_BOARD_RULES::IsSilk( item->GetLayer() ) )
                          continue;
                      auto* text = static_cast<PCB_TEXT*>( item );
                      out.push_back( { { "id", text->m_Uuid.AsString().ToStdString() },
                                       { "text", text->GetText().ToStdString() },
                                       { "side", item->GetLayer() == F_SilkS ? "top" : "bottom" },
                                       { "x_mm", ToMm( text->GetPosition().x ) }, { "y_mm", ToMm( text->GetPosition().y ) },
                                       { "size_mm", ToMm( text->GetTextSize().y ) },
                                       { "rotation", text->GetTextAngle().AsDegrees() } } );
                  }

                  aResult = out.empty() ? wxString( _( "The board has no silkscreen text of its own." ) )
                                        : wxString::FromUTF8( out.dump() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "edit_silkscreen_text" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "edit_silkscreen_text", aArgs, _( "Edit Silkscreen Text" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "delete_silkscreen_text" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );
                  std::set<KIID> ids;
                  for( const wxString& id : ARGS::RequireStringArray( aArgs, "ids", 64 ) ) ids.insert( KIID( id ) );

                  std::vector<BOARD_ITEM*> doomed;
                  for( BOARD_ITEM* item : board->Drawings() )
                      if( ids.count( item->m_Uuid ) && item->Type() == PCB_TEXT_T && INDUCTIVE_BOARD_RULES::IsSilk( item->GetLayer() ) )
                          doomed.push_back( item );

                  if( doomed.empty() )
                      throw ARGS::BAD_ARGUMENT( _( "None of those ids is a board silkscreen text." ) );

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  for( BOARD_ITEM* item : doomed ) commit.Remove( item );
                  commit.Push( _( "Delete Silkscreen Text" ) );
                  guard.Pushed();
                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "Deleted %d silkscreen text(s)." ), (int) doomed.size() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "label_connector_pins" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "label_connector_pins", aArgs, _( "Label Connector Pins" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "tidy_silkscreen" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "tidy_silkscreen", aArgs, _( "Tidy Silkscreen" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "set_net_rules" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );
                  BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();
                  std::shared_ptr<NET_SETTINGS> settings = bds.m_NetSettings;
                  std::shared_ptr<NETCLASS> def = settings ? settings->GetDefaultNetclass() : nullptr;

                  if( !def )
                      throw ARGS::BAD_ARGUMENT( _( "This board has no default net class." ) );

                  std::vector<wxString> names;
                  for( const wxString& n : ARGS::RequireStringArray( aArgs, "nets", 64 ) )
                      names.push_back( RequireNet( board, n )->GetNetname() );

                  const bool hasCurrent = ARGS::Has( aArgs, "current_a" );
                  const bool hasWidth = ARGS::Has( aArgs, "width_mm" );

                  if( hasCurrent == hasWidth )
                      throw ARGS::BAD_ARGUMENT( _( "Give either current_a or width_mm." ) );

                  const double rise = ARGS::Has( aArgs, "temp_rise_c" ) ? ARGS::RequireNumber( aArgs, "temp_rise_c", 1, 100 ) : 10.0;
                  const double oz = ARGS::Has( aArgs, "copper_oz" ) ? ARGS::RequireNumber( aArgs, "copper_oz", 0.5, 4 ) : 1.0;
                  double width = 0, amps = 0;

                  if( hasCurrent )
                  {
                      amps = ARGS::RequireNumber( aArgs, "current_a", 0.01, 50 );
                      width = INDUCTIVE_BOARD_RULES::Ipc2221WidthMm( amps, rise, oz );
                  }
                  else
                  {
                      width = ARGS::RequireNumber( aArgs, "width_mm", 0.1, 10 );
                  }

                  width = std::max( width, ToMm( def->GetTrackWidth() ) );

                  if( width > 6.0 )
                      throw ARGS::BAD_ARGUMENT( wxString::Format( _( "%.1f A needs a %.1f mm track; use a copper pour "
                                                                     "or heavier copper for that." ), amps, width ) );

                  wxString className = ARGS::OptionalString( aArgs, "class_name" );
                  if( className.IsEmpty() )
                      className = hasCurrent ? wxString::Format( wxS( "Current_%gA" ), amps ) : wxString::Format( wxS( "Width_%gmm" ), width );
                  if( className == NETCLASS::Default || className.length() > 40 )
                      throw ARGS::BAD_ARGUMENT( _( "Choose another net class name." ) );

                  INDUCTIVE_BOARD_RULES::AssignNetClass(
                          board, names, className, ToIU( width ),
                          hasCurrent ? wxString::Format( wxS( "%g A, IPC-2221, %g oz, %g C rise" ), amps, oz, rise )
                                     : wxString( wxS( "Set by Inductive" ) ),
                          ARGS::Has( aArgs, "clearance_mm" ) ? ToIU( ARGS::RequireNumber( aArgs, "clearance_mm", 0.1, 5 ) ) : 0 );
                  aFrame->OnModify();

                  wxArrayString list;
                  for( const wxString& n : names ) list.Add( n );
                  aResult = wxString::Format( _( "%s now in net class %s: %.2f mm track%s. Route again with rip_up "
                                                 "to widen existing tracks." ),
                                              wxJoin( list, ',' ), className, width,
                                              hasCurrent ? wxString::Format( _( " (IPC-2221 for %g A, %g oz copper, %g C rise)" ), amps, oz, rise )
                                                         : wxString() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_ground_pour" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "add_ground_pour", aArgs, _( "Add Ground Pour" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "fill_zones" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "fill_zones", aArgs, _( "Fill Zones" ), aResult );
              } } );

    // ── copper ────────────────────────────────────────────────────────

    tools.push_back(
            { { wxS( "add_track" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  // Every argument is resolved before anything is allocated, so a
                  // bad layer name cannot leave a half-built track to leak.
                  NETINFO_ITEM*      net   = RequireNet( board, ARGS::RequireString( aArgs, "net" ) );
                  const PCB_LAYER_ID layer = RequireCopperLayer(
                          board, ARGS::RequireString( aArgs, "layer" ) );

                  const VECTOR2I start( ToIU( ARGS::RequireCoordMm( aArgs, "x1" ) ),
                                        ToIU( ARGS::RequireCoordMm( aArgs, "y1" ) ) );
                  const VECTOR2I end( ToIU( ARGS::RequireCoordMm( aArgs, "x2" ) ),
                                      ToIU( ARGS::RequireCoordMm( aArgs, "y2" ) ) );

                  if( start == end )
                      throw ARGS::BAD_ARGUMENT( _( "The track would have zero length." ) );

                  const int width = ARGS::Has( aArgs, "width" )
                                            ? ToIU( ARGS::RequireSizeMm( aArgs, "width" ) )
                                            : board->GetDesignSettings().GetCurrentTrackWidth();

                  auto track = std::make_unique<PCB_TRACK>( board );

                  track->SetStart( start );
                  track->SetEnd( end );
                  track->SetWidth( width );

                  // Layer before net, always: SetNetCode() forces the code back to
                  // zero for an item that is not on a copper layer, so setting the
                  // net first would silently produce an unconnected track.
                  track->SetLayer( layer );
                  track->SetNet( net );

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  commit.Add( track.release() );
                  commit.Push( _( "Add Track" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format(
                          _( "Track on %s from %s to %s mm, %s mm wide, net %s." ),
                          board->GetLayerName( layer ), Pos( start ), Pos( end ), Mm( width ),
                          net->GetNetname() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_via" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*        board = RequireBoard( aFrame );
                  NETINFO_ITEM* net = RequireNet( board, ARGS::RequireString( aArgs, "net" ) );

                  const VECTOR2I at( ToIU( ARGS::RequireCoordMm( aArgs, "x" ) ),
                                     ToIU( ARGS::RequireCoordMm( aArgs, "y" ) ) );

                  const BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();

                  const int diameter = ARGS::Has( aArgs, "diameter" )
                                               ? ToIU( ARGS::RequireSizeMm( aArgs, "diameter" ) )
                                               : bds.GetCurrentViaSize();
                  const int drill = ARGS::Has( aArgs, "drill" )
                                            ? ToIU( ARGS::RequireSizeMm( aArgs, "drill" ) )
                                            : bds.GetCurrentViaDrill();

                  // A drill at or beyond the pad diameter leaves no annular ring
                  // at all -- an unmanufacturable via that DRC will flag but that
                  // is much cheaper to refuse here, in terms the model can act on.
                  if( drill >= diameter )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "A %s mm drill in a %s mm via leaves no copper ring. The drill "
                                 "must be smaller than the diameter." ),
                              Mm( drill ), Mm( diameter ) ) );
                  }

                  auto via = std::make_unique<PCB_VIA>( board );

                  via->SetPosition( at );
                  via->SetEnd( at );   // SetPosition only writes the start point

                  // The layer-qualified overload: PCB_VIA::SetWidth( int ) is a
                  // wxFAIL_MSG, because a via's pad can differ per layer.
                  via->SetWidth( PADSTACK::ALL_LAYERS, diameter );
                  via->SetDrill( drill );
                  via->SetNet( net );

                  // Before SetLayerPair(): SetViaType() calls SanitizeLayers(),
                  // which would undo the pair. The router comments this too.
                  via->SetViaType( VIATYPE::THROUGH );
                  via->SetLayerPair( F_Cu, B_Cu );

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  commit.Add( via.release() );
                  commit.Push( _( "Add Via" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format(
                          _( "Through via at %s mm on net %s, %s mm pad with a %s mm drill." ),
                          Pos( at ), net->GetNetname(), Mm( diameter ), Mm( drill ) );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "delete_tracks" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  const bool allNets = ARGS::OptionalBool( aArgs, "all_nets", false );

                  if( allNets == ARGS::Has( aArgs, "net" ) )
                  {
                      throw ARGS::BAD_ARGUMENT(
                              _( "Give either a net name or all_nets, not both and not neither." ) );
                  }

                  NETINFO_ITEM* net = nullptr;

                  if( !allNets )
                      net = RequireNet( board, ARGS::RequireString( aArgs, "net" ) );

                  std::vector<PCB_TRACK*> doomed;

                  for( PCB_TRACK* track : board->Tracks() )
                  {
                      if( allNets || track->GetNetCode() == net->GetNetCode() )
                          doomed.push_back( track );
                  }

                  if( doomed.empty() )
                  {
                      aResult = allNets ? _( "There is no routing on the board to remove." )
                                        : wxString::Format( _( "Net %s has no copper on it." ),
                                                            net->GetNetname() );
                      return true;
                  }

                  // Same reason as delete_footprints: the selection holds raw
                  // pointers, and removing a selected track without clearing it
                  // leaves the selection tool holding freed memory.
                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                  {
                      if( PCB_SELECTION_TOOL* sel = mgr->GetTool<PCB_SELECTION_TOOL>() )
                          sel->ClearSelection();
                  }

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  int segments = 0;
                  int vias     = 0;

                  for( PCB_TRACK* track : doomed )
                  {
                      if( track->Type() == PCB_VIA_T )
                          ++vias;
                      else
                          ++segments;

                      commit.Remove( track );
                  }

                  commit.Push( _( "Delete Tracks" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format(
                          _( "Removed %d track segment(s) and %d via(s) from %s." ), segments, vias,
                          allNets ? _( "the whole board" ) : net->GetNetname() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "auto_place" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "place", aArgs, _( "Placing Footprints" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "route" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "route", aArgs, _( "Routing" ), aResult );
              } } );

    // ── checking the work ─────────────────────────────────────────────

    tools.push_back(
            { { wxS( "run_drc" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  return RunDRC( aFrame, aResult );
              } } );

    // ── finding and adding parts ──────────────────────────────────────

    tools.push_back(
            { { wxS( "list_footprint_libraries" ), false },
              []( PCB_EDIT_FRAME*, const json&, wxString& aResult ) -> bool
              {
                  FOOTPRINT_LIBRARY_ADAPTER* libs = RequireLibraries();

                  std::vector<wxString> names;
                  for( const auto* row : Pgm().GetLibraryManager().Rows( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
                      names.push_back( row->Nickname() );

                  if( names.empty() )
                  {
                      aResult = _( "No footprint libraries are configured. The user needs to set "
                                   "them up in Preferences > Manage Footprint Libraries before "
                                   "parts can be placed." );
                      return true;
                  }

                  wxString out = wxString::Format( _( "%d footprint libraries\n" ),
                                                   (int) names.size() );

                  for( const wxString& name : names )
                  {
                      out << wxS( "  " ) << name;

                      // Whether a library is loaded decides whether searching it
                      // is instant or has to read it off disk first, and the
                      // model should be able to prefer the cheap ones.
                      if( !libs->IsLibraryLoaded( name ) )
                          out << _( "  (not loaded yet)" );

                      out << wxS( "\n" );
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "search_footprints" ), false },
              []( PCB_EDIT_FRAME*, const json& aArgs, wxString& aResult ) -> bool
              {
                  FOOTPRINT_LIBRARY_ADAPTER* libs = RequireLibraries();

                  const wxString library = ARGS::OptionalString( aArgs, "library" );
                  // An empty query with a library lists it: how a model asks what one holds.
                  const wxString query = ( library.IsEmpty() ? ARGS::RequireString( aArgs, "query" )
                                                             : ARGS::OptionalString( aArgs, "query" ) ).Lower();

                  std::vector<wxString> searchIn;

                  if( !library.IsEmpty() )
                  {
                      if( !Pgm().GetLibraryManager().GetRow( LIBRARY_TABLE_TYPE::FOOTPRINT, library ) )
                      {
                          throw ARGS::BAD_ARGUMENT( wxString::Format(
                                  _( "There is no footprint library called '%s'. Call "
                                     "list_footprint_libraries to see what is installed." ),
                                  library ) );
                      }

                      // Reading a library off disk is the one slow thing here, so
                      // it only happens for a library the model asked for by name.
                      if( !libs->IsLibraryLoaded( library ) )
                          libs->LoadOne( library );

                      searchIn.push_back( library );
                  }
                  else
                  {
                      // No library named: search only what is already in memory.
                      // Loading every installed library to answer a search would
                      // stall the editor for seconds with no way to cancel, which
                      // is exactly the freeze this tool surface must never cause.
                      for( const wxString& name : libs->GetLibraryNames() )
                      {
                          if( libs->IsLibraryLoaded( name ) )
                              searchIn.push_back( name );
                      }

                      if( searchIn.empty() )
                      {
                          aResult = _( "No footprint library is loaded yet, so there is nothing "
                                       "to search. Name a library with the 'library' argument -- "
                                       "list_footprint_libraries shows which exist." );
                          return true;
                      }
                  }

                  wxString   out;
                  int        hits  = 0;
                  const int  limit = 60;

                  for( const wxString& nickname : searchIn )
                  {
                      // GetFootprints(), not GetFootprintNames().  These look
                      // interchangeable and are not: enumeration deliberately
                      // destroys the plugin's parse cache once it has cloned the
                      // footprints into memory, so a library that reports itself
                      // LOADED has an empty plugin cache and GetFootprintNames()
                      // goes back to disk and re-parses every .kicad_mod in it.
                      // Across the installed set that is 15,000 files on the UI
                      // thread with no cancel -- exactly the freeze the
                      // already-loaded restriction above exists to prevent.
                      // GetFootprints() reads the clones under a shared lock and
                      // touches no disk.
                      for( FOOTPRINT* fp : libs->GetFootprints( nickname, true ) )
                      {
                          if( !fp )
                              continue;

                          const wxString fpName = fp->GetFPID().GetUniStringLibItemName();

                          if( !fpName.Lower().Contains( query ) )
                              continue;

                          if( ++hits > limit )
                              break;

                          out << wxS( "  " ) << nickname << wxS( ":" ) << fpName << wxS( "\n" );
                      }

                      if( hits > limit )
                          break;
                  }

                  if( !hits )
                  {
                      aResult = wxString::Format(
                              _( "Nothing matching '%s' in %d librar(ies)." ), query,
                              (int) searchIn.size() );
                      return true;
                  }

                  aResult = wxString::Format( _( "%d matches%s\n" ), std::min( hits, limit ),
                                              hits > limit ? _( " (more exist)" ) : wxString() )
                            + out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_mounting_holes" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "mounting_holes", aArgs, _( "Adding Mounting Holes" ), aResult );
              } } );

    tools.push_back(
            { { wxS( "place_footprint" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*                     board = RequireBoard( aFrame );
                  FOOTPRINT_LIBRARY_ADAPTER* libs  = RequireLibraries();

                  const wxString libIdText = ARGS::RequireString( aArgs, "lib_id" );

                  // Everything is read and validated before the footprint is
                  // loaded, so a bad argument cannot leave an orphaned clone.
                  const double x        = ARGS::RequireCoordMm( aArgs, "x" );
                  const double y        = ARGS::RequireCoordMm( aArgs, "y" );
                  const double rotation = ARGS::OptionalNumber( aArgs, "rotation", 0.0, -360.0,
                                                                360.0 );
                  const wxString wanted = ARGS::OptionalString( aArgs, "reference" );
                  const wxString value  = ARGS::OptionalString( aArgs, "value" );

                  LIB_ID libId;

                  if( libId.Parse( UTF8( libIdText.utf8_string() ) ) >= 0 || !libId.IsValid() )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "'%s' is not a valid footprint id. Use LIBRARY:FOOTPRINT, e.g. "
                                 "Resistor_SMD:R_0805_2012Metric." ),
                              libIdText ) );
                  }

                  const wxString nickname = libId.GetUniStringLibNickname();

                  if( nickname.IsEmpty() )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "'%s' does not name a library. Use LIBRARY:FOOTPRINT -- "
                                 "search_footprints returns ids in that form." ),
                              libIdText ) );
                  }

                  // The adapter answers nullptr, with no exception, for a library
                  // it has not enumerated yet -- so a lazily-unloaded library and
                  // a genuinely missing footprint look identical unless it is
                  // forced in first.
                  if( !libs->IsLibraryLoaded( nickname ) )
                      libs->LoadOne( nickname );

                  // Clones out of the library, clears the nets, and applies this
                  // board's own field and text styles.  We own it until Push().
                  FOOTPRINT* fp = aFrame->LoadFootprint( libId );

                  if( !fp )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "No footprint '%s' in library '%s'. Use search_footprints to "
                                 "find the exact name." ),
                              libId.GetUniStringLibItemName(), nickname ) );
                  }

                  // From here the footprint is ours to leak, so nothing below may
                  // throw before the commit takes ownership.  std::unique_ptr
                  // rather than care: the reference-designator search can throw.
                  std::unique_ptr<FOOTPRINT> owned( fp );

                  // The same preparation BOARD_EDITOR_CONTROL::PlaceFootprint does.
                  fp->SetLink( niluuid );        // not tied to a schematic symbol
                  fp->SetPath( KIID_PATH() );    // no hierarchical path
                  fp->SetParent( board );        // so design rules resolve against this board

                  for( PAD* pad : fp->Pads() )
                      pad->SetNetCode( 0 );      // library pads carry orphaned nets

                  // A library built from a board can hold a back-side footprint.
                  if( fp->IsFlipped() )
                      fp->Flip( fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );

                  fp->SetOrientationDegrees( rotation );
                  fp->SetPosition( VECTOR2I( ToIU( x ), ToIU( y ) ) );

                  // Mounting holes and fiducials belong to the board alone: no schematic
                  // symbol (so no parity error), not bought, not placed by the assembler.
                  const bool boardOnly = nickname == wxS( "MountingHole" ) || nickname == wxS( "Fiducial" );

                  // Their value is only the footprint's name (MountingHole_3.2mm_M3), drawn
                  // across the board otherwise; add_mounting_holes hides it the same way.
                  if( boardOnly )
                  {
                      fp->SetAttributes( fp->GetAttributes() | FP_BOARD_ONLY | FP_EXCLUDE_FROM_BOM
                                         | FP_EXCLUDE_FROM_POS_FILES );
                      fp->Value().SetVisible( false );
                  }

                  wxString reference = wanted;

                  if( reference.IsEmpty() || board->FindFootprintByReference( reference ) )
                      reference = NextFreeReference( board, nickname == wxS( "MountingHole" ) ? wxString( wxS( "H" ) )
                                                            : nickname == wxS( "Fiducial" ) ? wxString( wxS( "FID" ) )
                                                                                            : fp->GetReference() );

                  fp->SetReference( reference );

                  if( !value.IsEmpty() )
                      fp->SetValue( value );

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  commit.Add( owned.release() );   // ownership passes at Push()
                  commit.Push( _( "Place Footprint" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "Placed %s (%s) at %.3f, %.3f mm." ), reference,
                                              libIdText, x, y );
                  return true;
              } } );

    // ── moving things ─────────────────────────────────────────────────

    tools.push_back(
            { { wxS( "move_footprint" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* fp = RequireFootprint( board, ARGS::RequireString( aArgs, "reference" ) );

                  RefuseIfLocked( fp, _( "moved" ) );

                  const bool absolute = ARGS::Has( aArgs, "x" ) || ARGS::Has( aArgs, "y" );
                  const bool relative = ARGS::Has( aArgs, "dx" ) || ARGS::Has( aArgs, "dy" );

                  if( absolute && relative )
                  {
                      throw ARGS::BAD_ARGUMENT(
                              _( "Give either x/y or dx/dy, not both." ) );
                  }

                  if( !absolute && !relative )
                      throw ARGS::BAD_ARGUMENT( _( "Give x/y or dx/dy to say where to move it." ) );

                  const VECTOR2I from = fp->GetPosition();
                  VECTOR2I       to   = from;

                  if( absolute )
                  {
                      // Only the axis the model actually supplied is converted.
                      // Defaulting the other one to the current position and
                      // pushing that back through the bounds check would refuse a
                      // legitimate single-axis move of a part that already sits
                      // beyond the limit -- and would blame a coordinate the model
                      // never sent. The existing value is already valid; it is
                      // carried across untouched.
                      if( ARGS::Has( aArgs, "x" ) )
                          to.x = ToIU( ARGS::RequireCoordMm( aArgs, "x" ) );

                      if( ARGS::Has( aArgs, "y" ) )
                          to.y = ToIU( ARGS::RequireCoordMm( aArgs, "y" ) );
                  }
                  else
                  {
                      // Bounded as a coordinate rather than as a delta: the result
                      // has to land in range, and a legal delta from a legal
                      // position cannot leave it.
                      const double dx = ARGS::OptionalCoordMm( aArgs, "dx", 0.0 );
                      const double dy = ARGS::OptionalCoordMm( aArgs, "dy", 0.0 );

                      const double x = ToMm( from.x ) + dx;
                      const double y = ToMm( from.y ) + dy;

                      if( std::abs( x ) > ARGS::MAX_COORD_MM || std::abs( y ) > ARGS::MAX_COORD_MM )
                      {
                          throw ARGS::BAD_ARGUMENT( wxString::Format(
                                  _( "That would put %s at %.3f, %.3f mm, which is off any "
                                     "reasonable board." ),
                                  fp->GetReference(), x, y ) );
                      }

                      to.x = ToIU( x );
                      to.y = ToIU( y );
                  }

                  if( to == from )
                  {
                      aResult = wxString::Format( _( "%s is already at %s mm." ),
                                                  fp->GetReference(), Pos( from ) );
                      return true;
                  }

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Modify( fp );
                  fp->SetPosition( to );
                  commit.Push( _( "Move Footprint" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "%s moved from %s to %s mm." ),
                                              fp->GetReference(), Pos( from ), Pos( to ) );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "rotate_footprint" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* fp = RequireFootprint( board, ARGS::RequireString( aArgs, "reference" ) );

                  RefuseIfLocked( fp, _( "rotated" ) );

                  const double was = fp->GetOrientationDegrees();
                  double       now = was;

                  if( ARGS::Has( aArgs, "angle" ) )
                      now = ARGS::RequireNumber( aArgs, "angle", -3600.0, 3600.0 );
                  else if( ARGS::Has( aArgs, "by" ) )
                      now = was + ARGS::RequireNumber( aArgs, "by", -3600.0, 3600.0 );
                  else
                      throw ARGS::BAD_ARGUMENT( _( "Give 'angle' or 'by' to say how far to turn it." ) );

                  // Normalised so a model that keeps adding 90 does not walk the
                  // stored angle out to 3600 degrees over a session.
                  now = std::fmod( now, 360.0 );

                  if( now < 0 )
                      now += 360.0;

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Modify( fp );
                  fp->SetOrientationDegrees( now );
                  commit.Push( _( "Rotate Footprint" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "%s rotated from %.1f to %.1f degrees." ),
                                              fp->GetReference(), was, now );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "flip_footprint" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* fp = RequireFootprint( board, ARGS::RequireString( aArgs, "reference" ) );

                  RefuseIfLocked( fp, _( "flipped" ) );

                  const wxString wasLayer = board->GetLayerName( fp->GetLayer() );

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Modify( fp );

                  // Flipped about its own position, so the part stays where it is
                  // and only changes side.  Flipping about the board centre --
                  // which is what a bare Flip() on the board would mean -- would
                  // also relocate it, which is never what "flip R1" asks for.
                  fp->Flip( fp->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );
                  commit.Push( _( "Flip Footprint" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "%s flipped from %s to %s." ), fp->GetReference(),
                                              wasLayer,
                                              board->GetLayerName( fp->GetLayer() ) );
                  return true;
              } } );

    // ── fields and state ──────────────────────────────────────────────

    tools.push_back(
            { { wxS( "set_footprint_value" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* fp = RequireFootprint( board, ARGS::RequireString( aArgs, "reference" ) );

                  const wxString value = ARGS::RequireString( aArgs, "value" );
                  const wxString was   = fp->GetValue();

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Modify( fp );
                  fp->SetValue( value );
                  commit.Push( _( "Change Footprint Value" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "%s value changed from '%s' to '%s'." ),
                                              fp->GetReference(), was, value );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "delete_footprints" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  // Capped well below the panel's per-turn mutation budget: that
                  // budget counts calls, and this is the one tool where a single
                  // call can do unbounded damage.
                  const std::vector<wxString> refs =
                          ARGS::RequireStringArray( aArgs, "references", 50 );

                  // Everything is resolved and checked before anything is removed.
                  // A half-completed delete -- three parts gone, the fourth
                  // refused -- is worse than a clean refusal: the user has to
                  // work out what happened, and the model reports a failure while
                  // having already changed the board.
                  std::vector<FOOTPRINT*> victims;

                  for( const wxString& ref : refs )
                  {
                      FOOTPRINT* fp = RequireFootprint( board, ref );

                      RefuseIfLocked( fp, _( "deleted" ) );

                      if( std::find( victims.begin(), victims.end(), fp ) == victims.end() )
                          victims.push_back( fp );
                  }

                  // The selection holds raw pointers to board items.  Removing a
                  // selected footprint without clearing it leaves the selection
                  // tool holding freed memory, which is how an agent turns into a
                  // crash the moment the user happens to have clicked the part
                  // being deleted.  pcbnew's own delete clears it first too.
                  //
                  // Called directly rather than through TOOL_MANAGER::RunAction().
                  // RunAction() with aNow -- which is the default -- suspends the
                  // current coroutine and runs the action inline, pumping the
                  // event loop and re-entering us mid-commit.  KiCad's own
                  // developers warn about exactly this in tool_manager.h: "we need
                  // a way to guarantee that user events can't be processed in
                  // between API actions. Otherwise the COMMITs will get tangled up
                  // and we'll crash on bad pointers."  ClearSelection() is a plain
                  // public method that touches the selection and returns.
                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                  {
                      if( PCB_SELECTION_TOOL* sel = mgr->GetTool<PCB_SELECTION_TOOL>() )
                          sel->ClearSelection();
                  }

                  wxString removed;

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  for( FOOTPRINT* fp : victims )
                  {
                      removed << ( removed.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                              << fp->GetReference();
                      commit.Remove( fp );
                  }

                  // One Push for the lot, so undo restores the whole operation
                  // rather than making the user press it once per part.
                  commit.Push( _( "Delete Footprints" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "Deleted %d footprint(s): %s" ),
                                              (int) victims.size(), removed );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "align_footprints" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  const std::vector<wxString> refs =
                          ARGS::RequireStringArray( aArgs, "references", 100 );

                  const std::vector<wxString> edges = { wxS( "left" ),   wxS( "right" ),
                                                        wxS( "top" ),    wxS( "bottom" ),
                                                        wxS( "horizontal_centre" ),
                                                        wxS( "vertical_centre" ) };

                  const int edge = ARGS::RequireChoice( aArgs, "edge", edges );

                  if( refs.size() < 2 )
                      throw ARGS::BAD_ARGUMENT( _( "Give at least two footprints to align." ) );

                  std::vector<FOOTPRINT*> parts;

                  for( const wxString& ref : refs )
                  {
                      FOOTPRINT* fp = RequireFootprint( board, ref );

                      RefuseIfLocked( fp, _( "moved" ) );

                      if( std::find( parts.begin(), parts.end(), fp ) == parts.end() )
                          parts.push_back( fp );
                  }

                  // Aligned to the extreme of the group rather than to the first
                  // one named: "align these left" means a common left edge, and
                  // which part happened to be listed first is not information.
                  // Bounding boxes are used, not origins -- an origin can sit
                  // anywhere inside a footprint, so aligning origins leaves the
                  // visible edges ragged, which is the opposite of the request.
                  //
                  // int64 throughout: BOX2I's accessors return extended_type, and
                  // on a full-size page two coordinates summed for the centre case
                  // overflow a signed 32-bit int.
                  bool    first  = true;
                  int64_t target = 0;
                  int64_t sum    = 0;   // for the centre cases, a true mean

                  for( FOOTPRINT* fp : parts )
                  {
                      const BOX2I box = fp->GetBoundingBox( false );
                      int64_t     v   = 0;

                      switch( edge )
                      {
                      case 0: v = box.GetLeft();     break;
                      case 1: v = box.GetRight();    break;
                      case 2: v = box.GetTop();      break;
                      case 3: v = box.GetBottom();   break;
                      case 4: v = box.GetCenter().y; break;   // a common horizontal line
                      case 5: v = box.GetCenter().x; break;   // a common vertical line
                      }

                      sum += v;

                      if( first )
                      {
                          target = v;
                          first  = false;
                      }
                      else if( edge == 0 || edge == 2 )
                      {
                          target = std::min( target, v );      // left / top: the smallest
                      }
                      else if( edge == 1 || edge == 3 )
                      {
                          target = std::max( target, v );      // right / bottom: the largest
                      }
                  }

                  // The centre cases are a real mean over the group. Folding
                  // pairwise as ( target + v ) / 2 is an exponential moving
                  // average that weights the last part half and the first part
                  // almost nothing -- so the answer changed with the order the
                  // model happened to list them, which is not information.
                  if( edge == 4 || edge == 5 )
                      target = sum / (int64_t) parts.size();

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  int          moved = 0;

                  for( FOOTPRINT* fp : parts )
                  {
                      const BOX2I box = fp->GetBoundingBox( false );
                      VECTOR2I    shift( 0, 0 );

                      // Computed in int64 and narrowed once, at the end, after the
                      // subtraction has brought it back into range.
                      switch( edge )
                      {
                      case 0: shift.x = (int) ( target - box.GetLeft() );     break;
                      case 1: shift.x = (int) ( target - box.GetRight() );    break;
                      case 2: shift.y = (int) ( target - box.GetTop() );      break;
                      case 3: shift.y = (int) ( target - box.GetBottom() );   break;
                      case 4: shift.y = (int) ( target - box.GetCenter().y ); break;
                      case 5: shift.x = (int) ( target - box.GetCenter().x ); break;
                      }

                      if( shift.x == 0 && shift.y == 0 )
                          continue;

                      commit.Modify( fp );
                      fp->SetPosition( fp->GetPosition() + shift );
                      ++moved;
                  }

                  commit.Push( _( "Align Footprints" ) );
                  guard.Pushed();
                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "Aligned %d of %d footprints on their %s." ),
                                              moved, (int) parts.size(), edges[edge] );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "measure_distance" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* a = RequireFootprint( board, ARGS::RequireString( aArgs, "from" ) );
                  FOOTPRINT* b = RequireFootprint( board, ARGS::RequireString( aArgs, "to" ) );

                  if( a == b )
                      throw ARGS::BAD_ARGUMENT( _( "Those are the same footprint." ) );

                  const VECTOR2I pa = a->GetPosition();
                  const VECTOR2I pb = b->GetPosition();
                  const BOX2I    ba = a->GetBoundingBox( false );
                  const BOX2I    bb = b->GetBoundingBox( false );

                  // Separation per axis: negative means the outlines overlap on
                  // that axis, which is what makes this answer "will it fit".
                  const int64_t gapX = std::max<int64_t>( ba.GetLeft(), bb.GetLeft() )
                                       - std::min<int64_t>( ba.GetRight(), bb.GetRight() );
                  const int64_t gapY = std::max<int64_t>( ba.GetTop(), bb.GetTop() )
                                       - std::min<int64_t>( ba.GetBottom(), bb.GetBottom() );

                  // Widened before the subtraction: two positions at opposite
                  // ends of the coordinate range differ by more than an int holds.
                  const double dx = ToMm( (int64_t) pb.x - (int64_t) pa.x );
                  const double dy = ToMm( (int64_t) pb.y - (int64_t) pa.y );

                  wxString out;
                  out << wxString::Format( _( "%s to %s: centres %.3f mm apart (dx %.3f, dy %.3f)\n" ),
                                           a->GetReference(), b->GetReference(),
                                           std::sqrt( dx * dx + dy * dy ), dx, dy );

                  if( gapX < 0 && gapY < 0 )
                  {
                      out << wxString::Format(
                              _( "Their outlines OVERLAP by %s mm horizontally and %s mm "
                                 "vertically.\n" ),
                              Mm( -gapX ), Mm( -gapY ) );
                  }
                  else
                  {
                      out << wxString::Format( _( "Outline gap: %s mm horizontally, %s mm "
                                                  "vertically.\n" ),
                                               Mm( std::max<int64_t>( gapX, 0 ) ),
                                               Mm( std::max<int64_t>( gapY, 0 ) ) );
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "save_board" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  BOARD* board = RequireBoard( aFrame );

                  const wxString filename = board->GetFileName();

                  // An unnamed board is refused rather than saved.  SavePcbFile()
                  // with an empty path does not prompt -- the header comment
                  // claiming it does is stale -- it builds an unwritable path and
                  // raises a modal error dialog, which is the one outcome a tool
                  // handler must never produce.  Refusing is both safer and more
                  // honest: choosing where a new board lives is the user's call.
                  if( filename.IsEmpty() )
                  {
                      throw ARGS::BAD_ARGUMENT(
                              _( "This board has never been saved, so there is no file to save "
                                 "it to. Ask the user to save it once with File > Save As." ) );
                  }

                  if( !aFrame->SavePcbFile( filename ) )
                  {
                      aResult = wxString::Format( _( "Could not save %s." ), filename );
                      return false;
                  }

                  aResult = wxString::Format( _( "Board saved to %s." ), filename );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "export_gerbers" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  wxString note;

                  if( !ARGS::OptionalBool( aArgs, "allow_errors", false ) )
                  {
                      aResult = CheckFabReady( aFrame, &note );

                      if( !aResult.IsEmpty() )
                          return false;
                  }

                  const bool ok = ExportGerbers( RequireBoard( aFrame ), aResult );
                  aResult << note;
                  return ok;
              } } );

    tools.push_back(
            { { wxS( "export_jlcpcb" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  wxString note;

                  if( !ARGS::OptionalBool( aArgs, "allow_errors", false ) )
                  {
                      aResult = CheckFabReady( aFrame, &note );

                      if( !aResult.IsEmpty() )
                          return false;
                  }

                  const bool ok = ExportJlcpcbFiles( RequireBoard( aFrame ), aResult );
                  aResult << note;
                  return ok;
              } } );

    tools.push_back(
            { { wxS( "set_footprint_locked" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*     board = RequireBoard( aFrame );
                  FOOTPRINT* fp = RequireFootprint( board, ARGS::RequireString( aArgs, "reference" ) );

                  if( !ARGS::Has( aArgs, "locked" ) )
                      throw ARGS::BAD_ARGUMENT( _( "Say whether to lock or unlock it." ) );

                  const bool locked = ARGS::OptionalBool( aArgs, "locked", true );

                  BOARD_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Modify( fp );
                  fp->SetLocked( locked );
                  commit.Push( locked ? _( "Lock Footprint" ) : _( "Unlock Footprint" ) );
                  guard.Pushed();

                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( locked ? _( "%s is now locked." )
                                                     : _( "%s is now unlocked." ),
                                              fp->GetReference() );
                  return true;
              } } );

    // ── layers ────────────────────────────────────────────────────────

    tools.push_back(
            { { wxS( "set_layer_count" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return RemoteBoardJob( aFrame, "set_layer_count", aArgs, _( "Set Layer Count" ), aResult );
              } } );

    // ── any item ──────────────────────────────────────────────────────
    // What the dedicated tools do not cover: every item, every property the Properties
    // panel shows, by id. One undo step per call.

    tools.push_back(
            { { wxS( "find_items" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*                       board = RequireBoard( aFrame );
                  const INDUCTIVE_ITEMS::QUERY query = ParseItemQuery( board, aArgs );

                  // A bigger limit is clamped, not refused: the call still answers.
                  const int limit = (int) std::clamp( ARGS::OptionalNumber( aArgs, "limit", 60, 1, 1e6 ), 1.0, 300.0 );
                  size_t    total = 0;
                  const std::vector<BOARD_ITEM*> items = INDUCTIVE_ITEMS::Find( board, query, (size_t) limit, &total );

                  if( items.empty() )
                  {
                      aResult = _( "No item matches." );
                      return true;
                  }

                  wxString out = wxString::Format( _( "%d item(s)" ), (int) total );

                  if( total > items.size() )
                      out << wxString::Format( _( ", the first %d shown; narrow by kinds, layer, net or area" ),
                                               (int) items.size() );

                  out << wxS( ":\n" );

                  for( BOARD_ITEM* item : items )
                      out << INDUCTIVE_ITEMS::Describe( board, item ) << wxS( "\n" );

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_item_properties" ), false },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*      board = RequireBoard( aFrame );
                  wxString    why;
                  BOARD_ITEM* item = INDUCTIVE_ITEMS::Resolve( board, ARGS::RequireString( aArgs, "id" ), &why );

                  if( !item )
                      throw ARGS::BAD_ARGUMENT( why );

                  aResult = INDUCTIVE_ITEMS::Describe( board, item ) + wxS( "\n" )
                            + INDUCTIVE_ITEMS::Properties( item, INDUCTIVE_ITEMS::Context( board ) );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "set_item_properties" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*                   board = RequireBoard( aFrame );
                  std::vector<BOARD_ITEM*> items = ItemsToEdit( board, aArgs, 1000 );

                  const auto it = aArgs.find( "properties" );

                  if( it == aArgs.end() || !it->is_object() || it->empty() )
                      throw ARGS::BAD_ARGUMENT( _( "Give 'properties' as an object of name -> value." ) );

                  std::vector<std::pair<wxString, wxString>> values;

                  for( const auto& [name, value] : it->items() )
                  {
                      wxString text;

                      if( value.is_string() )
                          text = wxString::FromUTF8( value.get<std::string>() );
                      else if( value.is_boolean() )
                          text = value.get<bool>() ? wxS( "true" ) : wxS( "false" );
                      else if( value.is_number() )
                          text = wxString::FromCDouble( value.get<double>(), 6 );
                      else if( value.is_null() )
                          text = wxS( "none" );
                      else
                          throw ARGS::BAD_ARGUMENT( wxString::Format( _( "'%s' needs a text, number or true/false value." ),
                                                                      wxString::FromUTF8( name ) ) );

                      ARGS::RejectDangerousText( text, "properties" );
                      values.emplace_back( wxString::FromUTF8( name ), text );
                  }

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );

                  INDUCTIVE_ITEMS::EDIT_REPORT report =
                          INDUCTIVE_ITEMS::SetProperties( board, commit, items, values, aFrame );

                  if( report.changed )
                  {
                      commit.Push( _( "Edit Properties" ) );
                      guard.Pushed();
                  }

                  wxString out;

                  if( report.changed && report.copper && !board->Zones().empty() )
                  {
                      std::vector<ZONE*> zones( board->Zones().begin(), board->Zones().end() );
                      BOARD_COMMIT                         refill( aFrame );
                      INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> refillGuard( refill );
                      ZONE_FILLER filler( board, &refill );
                      filler.Fill( zones );
                      refill.Push( _( "Fill Zones" ), ZONE_FILL_OP | APPEND_UNDO );
                      refillGuard.Pushed();
                      out << _( "Pours refilled.\n" );
                  }

                  aFrame->GetCanvas()->Refresh();

                  // Every line for a few items; a count for a bulk edit, so the result stays readable.
                  if( report.changed > 20 )
                      out.Prepend( wxString::Format( _( "%d change(s) on %d item(s); the first:\n%s...\n" ), report.changed,
                                                     (int) items.size(),
                                                     report.changes.Left( report.changes.find( '\n' ) ) ) );
                  else
                      out.Prepend( report.changes );

                  if( !report.problems.IsEmpty() )
                      out << _( "Not changed:\n" ) << report.problems;

                  if( !report.changed && report.problems.IsEmpty() )
                      out = _( "Nothing to change." );

                  aResult = out.Trim();
                  return report.changed > 0;
              } } );

    tools.push_back(
            { { wxS( "delete_items" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*                   board = RequireBoard( aFrame );
                  std::vector<BOARD_ITEM*> items = ItemsToEdit( board, aArgs, 500 );

                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                      if( PCB_SELECTION_TOOL* sel = mgr->GetTool<PCB_SELECTION_TOOL>() )
                          sel->ClearSelection( true );

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  INDUCTIVE_ITEMS::EDIT_REPORT report = INDUCTIVE_ITEMS::Delete( commit, items );

                  if( report.changed )
                  {
                      commit.Push( _( "Delete Items" ) );
                      guard.Pushed();
                  }

                  aFrame->GetCanvas()->Refresh();

                  wxString out = report.changed > 20 ? wxString::Format( _( "Deleted %d item(s).\n" ), report.changed )
                                                     : report.changes;

                  if( !report.problems.IsEmpty() )
                      out << _( "Not deleted:\n" ) << report.problems;

                  aResult = out.Trim();
                  return report.changed > 0;
              } } );


    tools.push_back(
            { { wxS( "add_shape" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*             board = RequireBoard( aFrame );
                  const wxString     layerName = ARGS::RequireString( aArgs, "layer" );
                  const PCB_LAYER_ID layer = board->GetLayerID( layerName );

                  if( layer == UNDEFINED_LAYER || !board->IsLayerEnabled( layer ) )
                      throw ARGS::BAD_ARGUMENT( wxString::Format( _( "There is no layer '%s' on this board." ), layerName ) );

                  const int kind = ARGS::RequireChoice( aArgs, "kind", { "line", "rect", "circle", "arc", "polygon" } );
                  const size_t need[] = { 2, 2, 2, 3, 3 };
                  const std::vector<VECTOR2I> at = RequirePoints( aArgs, need[kind], kind == 4 ? 500 : need[kind] );
                  const int width = ARGS::Has( aArgs, "width" )
                                            ? ToIU( ARGS::RequireNumber( aArgs, "width", 0, 20 ) )
                                            : board->GetDesignSettings().GetLineThickness( layer );

                  auto shape = INDUCTIVE_ITEMS::MakeShape( board, layer, (INDUCTIVE_ITEMS::SHAPE_KIND) kind, at, width,
                                                           ARGS::OptionalBool( aArgs, "filled", false ) );
                  const wxString line = INDUCTIVE_ITEMS::Describe( board, shape.get() );

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Add( shape.release() );
                  commit.Push( _( "Add Shape" ) );
                  guard.Pushed();
                  aFrame->GetCanvas()->Refresh();

                  aResult = _( "Added " ) + line;

                  if( layer == Edge_Cuts && kind != 0 && kind != 3 )
                      aResult << _( "\nA closed shape inside the outline is a cutout; run_drc confirms the outline still closes." );

                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_zone" ), true },
              []( PCB_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  BOARD*                     board = RequireBoard( aFrame );
                  INDUCTIVE_ITEMS::ZONE_SPEC spec;

                  for( const wxString& name : ARGS::RequireStringArray( aArgs, "layers", 32 ) )
                  {
                      const PCB_LAYER_ID layer = board->GetLayerID( name );

                      if( layer == UNDEFINED_LAYER || !board->IsLayerEnabled( layer ) )
                          throw ARGS::BAD_ARGUMENT( wxString::Format( _( "There is no layer '%s' on this board." ), name ) );

                      spec.layers.set( layer );
                  }

                  spec.outline = RequirePoints( aArgs, 3, 500 );
                  spec.name = ARGS::OptionalString( aArgs, "name", wxEmptyString );
                  spec.keepout = ARGS::Has( aArgs, "keepout" );

                  if( spec.keepout )
                  {
                      const json& k = aArgs.at( "keepout" );
                      spec.noTracks = ARGS::OptionalBool( k, "tracks", true );
                      spec.noVias = ARGS::OptionalBool( k, "vias", true );
                      spec.noPads = ARGS::OptionalBool( k, "pads", false );
                      spec.noCopper = ARGS::OptionalBool( k, "copper", true );
                      spec.noFootprints = ARGS::OptionalBool( k, "footprints", false );
                  }
                  else
                  {
                      if( !( spec.layers & LSET::AllCuMask() ).any() )
                          throw ARGS::BAD_ARGUMENT( _( "A copper pour needs a copper layer; for anything else set keepout." ) );

                      const wxString net = ARGS::OptionalString( aArgs, "net", wxEmptyString );
                      std::optional<int> code = INDUCTIVE_ITEMS::Context( board ).netCode( net );

                      if( !code )
                          throw ARGS::BAD_ARGUMENT( wxString::Format( _( "There is no net '%s' on this board." ), net ) );

                      spec.net = *code;
                      spec.priority = ARGS::OptionalInt( aArgs, "priority", 0, 0, 1000 );

                      if( ARGS::Has( aArgs, "clearance" ) )
                          spec.clearance = ToIU( ARGS::RequireNumber( aArgs, "clearance", 0, 20 ) );

                      if( ARGS::Has( aArgs, "min_width" ) )
                          spec.minWidth = ToIU( ARGS::RequireNumber( aArgs, "min_width", 0.05, 20 ) );

                      if( ARGS::Has( aArgs, "pad_connection" ) )
                      {
                          const int c = ARGS::RequireChoice( aArgs, "pad_connection", { "solid", "thermal", "none" } );
                          spec.pads = c == 0 ? ZONE_CONNECTION::FULL : c == 1 ? ZONE_CONNECTION::THERMAL : ZONE_CONNECTION::NONE;
                      }
                  }

                  std::unique_ptr<ZONE> zone = INDUCTIVE_ITEMS::MakeZone( board, spec );
                  ZONE* added = zone.get();

                  BOARD_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> guard( commit );
                  commit.Add( zone.release() );
                  commit.Push( spec.keepout ? _( "Add Rule Area" ) : _( "Add Zone" ) );
                  guard.Pushed();

                  // Every pour, so priorities and the new keep-out take effect; same undo step.
                  if( !spec.keepout || spec.noCopper )
                  {
                      std::vector<ZONE*> zones( board->Zones().begin(), board->Zones().end() );
                      BOARD_COMMIT                         refill( aFrame );
                      INDUCTIVE_COMMIT_GUARD<BOARD_COMMIT> refillGuard( refill );
                      ZONE_FILLER filler( board, &refill );
                      filler.Fill( zones );
                      refill.Push( _( "Fill Zones" ), ZONE_FILL_OP | APPEND_UNDO );
                      refillGuard.Pushed();
                  }

                  aFrame->GetCanvas()->Refresh();

                  aResult = _( "Added " ) + INDUCTIVE_ITEMS::Describe( board, added );
                  return true;
              } } );

    return tools;
}


}  // namespace


bool ExportJlcpcb( BOARD* aBoard, wxString& aResult )
{
    return ExportJlcpcbFiles( aBoard, aResult );
}


wxString FabBlockers( BOARD* aBoard, wxString* aNote )
{
    return FabBlockersOf( aBoard, aNote );
}


json InductiveBoardWorkflowSnapshot(BOARD* board, bool checkpoint)
{

        STRING_FORMATTER f;PCB_IO_KICAD_SEXPR io;io.FormatBoardToFormatter(&f,board);
        json snapshot={{"fingerprint",INDUCTIVE_REVIEW::Hash(f.GetString())},{"document",board->GetFileName().ToStdString()}};
        if(checkpoint) snapshot["files"]=json::array({{{"path",board->GetFileName().ToStdString()},{"content",f.GetString()}}});
        return snapshot;
}

void InductiveInstallBoardTools( PCB_EDIT_FRAME* aFrame, INDUCTIVE_PANEL* aPanel )
{
    if( !aFrame || !aPanel )
        return;

    aPanel->SetWorkflowSnapshot([aFrame](bool checkpoint) -> json { RequireEditable(aFrame);return InductiveBoardWorkflowSnapshot(RequireBoard(aFrame),checkpoint); });
    // One request, one undo step (see INDUCTIVE_UNDO_GROUP).
    auto undoGroup = std::make_shared<INDUCTIVE_UNDO_GROUP>();
    aPanel->SetRequestBoundaryHandler( [aFrame, undoGroup]( bool aBegin ) {
        if( aBegin )
            undoGroup->Begin( aFrame );
        else
            undoGroup->End( aFrame, _( "Inductive Request" ) );
    } );
    aPanel->SetAttachmentHandler( []( const json& file, bool import, wxString& result ) {
        if( INDUCTIVE_ATTACHMENTS::Extension( file.at( "name" ).get<std::string>() ) == ".kicad_sch" )
        {
            if( !import ) return true;
            result = "Attach and import this schematic in the schematic editor, then prepare_pcb and sync.";
            return false;
        }
        if( INDUCTIVE_ATTACHMENTS::Extension( file.at( "name" ).get<std::string>() ) == ".kicad_sym" )
            return true; // Native symbol parsing happens in the schematic editor.
        try
        {
            PCB_IO_KICAD_SEXPR io;
            std::unique_ptr<BOARD_ITEM> parsed( io.Parse( wxString::FromUTF8( file.at( "text" ).get<std::string>() ) ) );
            if( !parsed || parsed->Type() != PCB_FOOTPRINT_T )
                throw std::runtime_error( "The attached file is not a valid footprint." );
            return true;
        }
        catch( const IO_ERROR& e ) { result = e.What(); return false; }
    } );

    // ── what the model is told it is looking at ───────────────────────
    //
    // Taken once per turn.  It is deliberately a summary and not an inventory:
    // the inspection tools exist for detail, and a thousand-line context on
    // every request would crowd out the conversation and be stale by the second
    // tool call anyway.  The panel fences this text off as untrusted data --
    // reference designators and silkscreen strings come from the board, not
    // from us.
    // Sending a request puts the editor back on its selection arrow, as Esc would: a
    // route or move in progress made the board editor refuse the request as busy. The
    // router takes two cancels (the first ends the track in progress), so this repeats
    // while a tool is on the stack. A modal dialog is left for the user to close.
    aPanel->SetBeforeRequest(
            [aFrame]()
            {
                if( !aFrame->GetToolManager() || !aFrame->IsEnabled() )
                    return;

                for( int i = 0; i < 4 && !aFrame->ToolStackIsEmpty(); ++i )
                    aFrame->GetToolManager()->RunAction( ACTIONS::selectionTool );  // re-entry safe: before the request, outside any commit
            } );

    aPanel->SetContextProvider(
            [aFrame]() -> wxString
            {
                BOARD* board = aFrame->GetBoard();

                if( !board )
                    return wxEmptyString;

                wxString out;
                out << wxS( "Editor: board (pcbnew)\n" );
                out << wxString::Format( wxS( "Copper layers: %d\n" ),
                                         board->GetCopperLayerCount() );
                out << wxString::Format( wxS( "Footprints: %d, nets: %d, tracks: %d, zones: %d\n" ),
                                         (int) board->Footprints().size(),
                                         (int) board->GetNetCount(),
                                         (int) board->Tracks().size(),
                                         (int) board->Zones().size() );

                const BOX2I bbox = board->GetBoardEdgesBoundingBox();

                if( bbox.GetWidth() > 0 && bbox.GetHeight() > 0 )
                {
                    out << wxString::Format( wxS( "Board outline: %.2f x %.2f mm\n" ),
                                             ToMm( bbox.GetWidth() ), ToMm( bbox.GetHeight() ) );
                }
                else
                {
                    out << wxS( "Board outline: none drawn yet\n" );
                }

                if( std::shared_ptr<CONNECTIVITY_DATA> conn = board->GetConnectivity() )
                {
                    const int unrouted = conn->GetUnconnectedCount( false );

                    out << ( unrouted == 0
                                     ? wxString( wxS( "Routing: complete\n" ) )
                                     : wxString::Format( wxS( "Routing: %d unrouted\n" ),
                                                         unrouted ) );
                }

                const int limit = 60;
                int       i     = 0;

                for( FOOTPRINT* fp : board->Footprints() )
                {
                    if( i++ >= limit )
                    {
                        out << wxString::Format( wxS( "  ...and %d more\n" ),
                                                 (int) board->Footprints().size() - limit );
                        break;
                    }

                    out << wxS( "  " ) << fp->GetReference() << wxS( " " ) << fp->GetValue()
                        << wxS( " [" ) << fp->GetFPID().Format().wx_str() << wxS( "]\n" );
                }

                return out;
            } );

    // ── slash commands ────────────────────────────────────────────────
    //
    // These run against the document directly and spend no model quota, which
    // makes them the reliable half of the product: they work when the day's
    // requests are gone.  The sidebar builds its "/" palette from this list, so
    // adding one here is all it takes for it to appear.
    aPanel->SetCommands(
            { { wxS( "Sync" ), _( "Sync footprints and nets from the prepared schematic" ) },
              { wxS( "Place" ), _( "Place the footprints on the board" ) },
              { wxS( "Route" ), _( "Route every unrouted connection" ) },
              { wxS( "DRC" ), _( "Run the design rule check" ) },
              { wxS( "Layout" ), _( "Sync, place, route and check the board in one step" ) },
              { wxS( "Gerbers" ), _( "Plot the board to Gerber files" ) },
              { wxS( "JLCPCB" ), _( "Gerbers, BOM and placement files for a JLCPCB order" ) } },
            [aFrame]( const wxString& aCommand, wxString& aResult ) -> bool
            {
                // The schematic side writes a placed and routed .kicad_pcb every
                // time it builds a circuit. This opens it the way File > Open
                // would -- deliberately, rather than merging it into whatever is
                // already loaded: a half-merged board with two net lists is a far
                // worse thing to hand an engineer than a clean file.
                if( aCommand.CmpNoCase( wxS( "sync" ) ) == 0 )
                {
                    RequireEditable( aFrame );
                    return SyncSchematic( aFrame, aResult );
                }

                // A command changes the document exactly as a tool does, so it
                // takes the same gate -- routing underneath an open dialog is no
                // safer for being triggered by a slash than by the model.
                if( aCommand.CmpNoCase( wxS( "place" ) ) == 0 )
                {
                    RequireEditable( aFrame );
                    return RemoteBoardJob( aFrame, "place", json::object(), _( "Placing Footprints" ), aResult );
                }

                if( aCommand.CmpNoCase( wxS( "route" ) ) == 0 )
                {
                    RequireEditable( aFrame );
                    return RemoteBoardJob( aFrame, "route", json::object(), _( "Routing" ), aResult );
                }

                if( aCommand.CmpNoCase( wxS( "drc" ) ) == 0 )
                {
                    RequireEditable( aFrame );
                    return RunDRC( aFrame, aResult );
                }

                if( aCommand.CmpNoCase( wxS( "layout" ) ) == 0 )
                {
                    RequireEditable( aFrame );
                    return Layout( aFrame, aResult );
                }

                if( aCommand.CmpNoCase( wxS( "gerbers" ) ) == 0 || aCommand.CmpNoCase( wxS( "jlcpcb" ) ) == 0 )
                {
                    RequireEditable( aFrame );
                    wxString note;
                    aResult = CheckFabReady( aFrame, &note );

                    if( !aResult.IsEmpty() )
                        return false;

                    const bool ok = aCommand.CmpNoCase( wxS( "gerbers" ) ) == 0
                                            ? ExportGerbers( aFrame->GetBoard(), aResult )
                                            : ExportJlcpcbFiles( aFrame->GetBoard(), aResult );
                    aResult << note;
                    return ok;
                }

                return false;
            } );

    // Built once and captured: the declarations are constant, and rebuilding the
    // table on every turn would allocate three dozen wxStrings for nothing.
    static const std::vector<ENTRY> tools = BuildTools();

    std::vector<INDUCTIVE_TOOL> declarations;
    declarations.reserve( tools.size() );

    for( const ENTRY& entry : tools )
        declarations.push_back( entry.decl );

    aPanel->SetTools( declarations,
                      [aFrame]( const wxString& aName, const wxString& aArgs,
                                wxString& aResult ) -> bool
                      {
                          for( const ENTRY& entry : tools )
                          {
                              if( entry.decl.name != aName )
                                  continue;

                              // Gated here rather than inside each handler on
                              // purpose.  A per-tool call is a rule someone has
                              // to remember; a dispatcher check is a rule the
                              // next tool inherits whether or not its author
                              // read this file.
                              if( entry.decl.mutates )
                                  RequireEditable( aFrame );

                              // Parsed here rather than in each handler so that a
                              // malformed blob is one refusal with one message,
                              // and no handler can forget to check.
                              const json args = ARGS::ParseObject( aArgs );

                              return entry.run( aFrame, args, aResult );
                          }

                          // Unreachable: the panel checks the name against the
                          // declarations before dispatching.  Kept because "the
                          // caller checks" is exactly the assumption that stops
                          // being true when someone adds a second caller.
                          aResult = wxString::Format( _( "Unknown tool '%s'." ), aName );
                          return false;
                      } );
}
