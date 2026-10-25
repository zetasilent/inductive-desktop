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
 * The schematic editor's half of Inductive.
 *
 * The same three rules as the board side -- validate before touching KiCad,
 * mutate only through a guarded SCH_COMMIT, and report what actually happened --
 * plus three that are specific to schematics and each of which is a silent
 * failure rather than an error:
 *
 *  1. **A wire is a wire only because of its layer.**  SCH_LINE defaults to
 *     LAYER_NOTES, and its constructor quietly maps anything it does not
 *     recognise back to LAYER_NOTES.  A "wire" drawn on that layer looks
 *     identical on screen at default colours and connects nothing whatsoever.
 *     Every line this file creates is asserted with IsWire() before it is
 *     committed.
 *
 *  2. **Connectivity is exact integer equality.**  The connection graph keys a
 *     std::map on raw VECTOR2I coordinates; one internal unit -- 100 nm -- of
 *     disagreement and the net simply does not form.  So every endpoint is
 *     snapped to the schematic grid before use, and pin positions are read from
 *     the symbol rather than computed.
 *
 *  3. **Modify() needs the screen.**  SCH_COMMIT drops an entry whose screen is
 *     null, with no diagnostic in a release build, and the right screen is the
 *     sheet path's, which is not always the one on display.
 */

#include "inductive_sch_tools.h"
#include "inductive_commit_guard.h"
#include "inductive_tool_args.h"
#include "inductive_sch_items.h"
#include "inductive_design_checks.h"

#include <widgets/inductive_panel.h>
#include <widgets/inductive_attachments.h>
#include <widgets/inductive_review.h>
#include <wildcards_and_files_ext.h>
#include <fstream>
#include <kiface_base.h>
#include <wx/stdpaths.h>
#include <sch_sheet_pin.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>

#include <sch_edit_frame.h>
#include <sch_commit.h>
#include <sch_sheet.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_parser.h>
#include <richio.h>
#include <sch_field.h>
#include <tool/actions.h>
#include <wx/ffile.h>
#include <wx/base64.h>
#include <plotters/plotters_pslike.h>
#include <sch_plotter.h>
#include <sch_symbol.h>
#include <sch_line.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_no_connect.h>
#include <pgm_base.h>
#include <libraries/library_manager.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <wx/dir.h>
#include <climits>
#include <regex>
#include <wx/regex.h>
#include <set>
#include <geometry/geometry_utils.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_pin.h>
#include <sch_connection.h>
#include <schematic.h>
#include <lib_symbol.h>
#include <lib_id.h>
#include <libraries/symbol_library_adapter.h>
#include <project_sch.h>
#include <sch_marker.h>
#include <sch_draw_panel.h>
#include <sch_view.h>
#include <connection_graph.h>
#include <erc/erc.h>
#include <erc/erc_item.h>
#include <erc/erc_settings.h>
#include <dialogs/dialog_erc.h>
#include <rc_item.h>
#include <marker_base.h>
#include <kiway.h>
#include <project.h>
#include <widgets/report_severity.h>
#include <reporter.h>
#include <tools/sch_selection_tool.h>
#include <tools/sch_selection.h>

#include <base_units.h>
#include <layer_ids.h>

#include <tool/tool_manager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_utils.h>
#include <map>
#include <functional>
#include <memory>
#include <vector>

using json = nlohmann::json;
namespace ARGS = INDUCTIVE_ARGS;

wxString InductiveSearchSymbols( const wxString& aQuery );

bool InductiveSymbolOnBoard( const SCH_SYMBOL* aSymbol )
{
    // A power symbol (GND, +5V, PWR_FLAG) is a net, not a part: the stock library marks
    // PWR_FLAG in_bom and on_board, and KiCad's own netlister skips it all the same.
    return !aSymbol->IsPower() && !aSymbol->GetExcludedFromBoard();
}

namespace
{

// ─────────────────────────────── units ───────────────────────────────

/// Schematic internal units to millimetres.  Note this is a different scale from
/// the board's: eeschema IU is 100 nm, pcbnew's is 1 nm.
inline double ToMm( int64_t aIU )
{
    return static_cast<double>( aIU ) / schIUScale.IU_PER_MM;
}


inline int ToIU( double aMm )
{
    // See the board-side ToIU(): a wxASSERT here would open a modal dialog in a
    // release build, which from a tool handler is a deadlock. Negated so NaN fails.
    if( !( std::abs( aMm ) <= ARGS::MAX_COORD_MM ) )
    {
        throw ARGS::BAD_ARGUMENT( wxString::Format(
                _( "%g mm is outside the range this schematic can represent." ), aMm ) );
    }

    return schIUScale.mmToIU( aMm );
}


/**
 * The schematic grid, in internal units: 1.27 mm, the classic 50 mil pitch.
 *
 * Every symbol in the standard libraries places its pins on this pitch, and the
 * connection graph matches endpoints by exact integer equality -- so a wire that
 * ends 100 nm away from a pin is not "nearly connected", it is unconnected, and
 * it looks correct on screen.  Snapping is not a nicety here; it is the
 * difference between a netlist and a picture of one.
 */
constexpr int SCH_GRID_IU = 12700;


/// The nearest grid intersection to a point.
VECTOR2I SnapToGrid( const VECTOR2I& aPoint )
{
    auto snap = []( int v )
    {
        // Rounds to nearest, including for negatives, without relying on the
        // implementation-defined sign of integer division's remainder.
        const int half = SCH_GRID_IU / 2;
        return ( v >= 0 ? ( v + half ) / SCH_GRID_IU : ( v - half ) / SCH_GRID_IU ) * SCH_GRID_IU;
    };

    return VECTOR2I( snap( aPoint.x ), snap( aPoint.y ) );
}


/**
 * A human name for a sheet, for results that need to say where they acted.
 *
 * The read tools report the whole hierarchy while the write tools act on the
 * sheet currently displayed.  On a multi-sheet design those are not the same
 * place, and a model that read a pin coordinate off sheet 2 will happily ask for
 * a wire that then lands on sheet 1 -- at the right coordinates, on the wrong
 * page, connected to nothing. Naming the sheet in both directions is what makes
 * that visible instead of silent.
 */
wxString SheetName( const SCH_SHEET_PATH& aPath )
{
    const wxString name = aPath.PathHumanReadable();
    return name.IsEmpty() ? wxString( wxS( "/" ) ) : name;
}


wxString Pos( const VECTOR2I& aPoint )
{
    return wxString::Format( wxS( "%.3f, %.3f" ), ToMm( aPoint.x ), ToMm( aPoint.y ) );
}


// ────────────────────────── schematic lookups ──────────────────────────

SCHEMATIC* RequireSchematic( SCH_EDIT_FRAME* aFrame )
{
    SCHEMATIC* schematic = aFrame ? &aFrame->Schematic() : nullptr;

    if( !schematic )
        throw ARGS::BAD_ARGUMENT( _( "No schematic is open." ) );

    return schematic;
}


/**
 * Refuse to change the schematic while the editor is busy.
 *
 * SCH_EDIT_FRAME does not override CanAcceptApiCommands() the way the board
 * editor does, so the inherited implementation only notices a disabled frame --
 * a modal dialog -- and not an interactive tool mid-gesture.  The tool-stack
 * check covers the second case: a user halfway through dragging a wire has a
 * tool on the stack, and editing the document underneath them corrupts both
 * their gesture and our commit.
 */
void RequireEditable( SCH_EDIT_FRAME* aFrame )
{
    if( !aFrame->CanAcceptApiCommands() )
    {
        throw ARGS::BAD_ARGUMENT(
                _( "The schematic editor is busy -- a dialog is open. Tell the user to close "
                   "it, then ask again." ) );
    }

    if( !aFrame->ToolStackIsEmpty() )
    {
        throw ARGS::BAD_ARGUMENT(
                _( "The user is in the middle of an editing tool in the schematic. Tell them to "
                   "finish or press Escape, then ask again." ) );
    }
}


/**
 * The symbol library adapter, or a refusal.
 *
 * As on the board side, SYMBOL_LIB_TABLE is gone from this tree; libraries are
 * reached through the global library manager behind this adapter, and its
 * argument is ignored.
 */
SYMBOL_LIBRARY_ADAPTER* RequireSymbolLibraries()
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( nullptr );

    if( !adapter )
        throw ARGS::BAD_ARGUMENT( _( "The symbol libraries are not available." ) );

    return adapter;
}


/**
 * Every symbol in the design, with the sheet path each one was found on.
 *
 * The path matters for more than bookkeeping: SCH_COMMIT needs the path's screen
 * rather than the displayed one, and a symbol's reference is per-instance, so
 * reading it requires knowing which instance is meant.
 */
struct SYMBOL_REF
{
    SCH_SYMBOL*    symbol = nullptr;
    SCH_SHEET_PATH path;
};


std::vector<SYMBOL_REF> AllSymbols( SCH_EDIT_FRAME* aFrame )
{
    std::vector<SYMBOL_REF> found;

    for( const SCH_SHEET_PATH& sheet : RequireSchematic( aFrame )->Hierarchy() )
    {
        SCH_SCREEN* screen = sheet.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            if( SCH_SYMBOL* symbol = dynamic_cast<SCH_SYMBOL*>( item ) )
                found.push_back( { symbol, sheet } );
        }
    }

    return found;
}


/**
 * Find a symbol by reference designator.
 *
 * A multi-unit package is several SCH_SYMBOL objects sharing one reference, so
 * this returns the first unit and the caller is told when there are more.  Code
 * that stops at the first match and edits it is the standard way to change unit
 * A of a quad op-amp and leave B, C and D alone.
 */
SYMBOL_REF RequireSymbol( SCH_EDIT_FRAME* aFrame, const wxString& aReference,
                          int* aUnitCount = nullptr )
{
    std::vector<SYMBOL_REF> matches;

    for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
    {
        if( entry.symbol->GetRef( &entry.path, false ).CmpNoCase( aReference ) == 0 )
            matches.push_back( entry );
    }

    if( aUnitCount )
        *aUnitCount = (int) matches.size();

    if( !matches.empty() )
        return matches.front();

    wxString available;
    int      shown = 0;

    for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
    {
        if( shown++ >= 40 )
        {
            available << wxS( ", ..." );
            break;
        }

        available << ( shown > 1 ? wxS( ", " ) : wxS( "" ) )
                  << entry.symbol->GetRef( &entry.path, false );
    }

    if( available.IsEmpty() )
        available = _( "(the schematic has no symbols)" );

    throw ARGS::BAD_ARGUMENT( wxString::Format(
            _( "There is no symbol called '%s' in this schematic. Present: %s" ), aReference,
            available ) );
}


wxString DescribeSymbol( const SYMBOL_REF& aEntry, bool aMultiSheet = false )
{
    SCH_SYMBOL* symbol = aEntry.symbol;

    wxString line;
    line << symbol->GetRef( &aEntry.path, true ) << wxS( "  " ) << symbol->GetValue( true,
                                                                                     &aEntry.path,
                                                                                     false );
    line << wxS( "  at " ) << Pos( symbol->GetPosition() ) << wxS( " mm" );

    const wxString footprint = symbol->GetFootprintFieldText( true, &aEntry.path, false );

    line << wxS( "  footprint: " ) << ( footprint.IsEmpty() ? _( "(none set)" ) : footprint );
    line << wxS( "  [" ) << symbol->GetLibId().Format().wx_str() << wxS( "]" );

    // Only on a hierarchical design: on the common single-sheet case it is noise,
    // and on a multi-sheet one it is the difference between a coordinate that
    // means something and one that does not.
    if( aMultiSheet )
        line << wxS( "  on sheet " ) << SheetName( aEntry.path );

    return line;
}


/**
 * Run the electrical rule check and report it, shared by the tool and /erc.
 *
 * ERC is materially cheaper than the board's DRC -- no geometry, no thread pool
 * -- so running it synchronously on the UI thread is far less of an imposition
 * here, and it means the markers are on the canvas by the time the model
 * answers.  As with DRC, the value is not the sentence the model writes; it is
 * the engineer being able to click through the violations themselves.
 */
bool RunERC( SCH_EDIT_FRAME* aFrame, wxString& aResult )
{
    SCHEMATIC* schematic = RequireSchematic( aFrame );

    // The ERC dialog is MODELESS, so RequireEditable() cannot see it -- the frame
    // stays enabled and no interactive tool is active. Its tree model holds raw
    // MARKER_BASE* pointers to every marker, and the delete below frees all of
    // them; left to itself the dialog dereferences freed memory on its next paint.
    //
    // Found by window name, not through SCH_EDIT_FRAME::GetErcDialog(), which is a
    // lazy *constructor*: it news up a dialog when none exists, so it never returns
    // null. Testing it therefore refused every single run -- and allocated a dialog
    // nobody asked for on the way. (pcbnew's DRC_TOOL::GetDRCDialog() genuinely is a
    // plain getter, which is why the DRC twin of this guard is correct as written.)
    // Finding it by name is how eeschema itself locates this dialog; see
    // annotate.cpp.
    wxWindow*   ercWindow = wxWindow::FindWindowByName( DIALOG_ERC_WINDOW_NAME );
    DIALOG_ERC* ercDialog = ercWindow ? static_cast<DIALOG_ERC*>( ercWindow ) : nullptr;

    // Markers are about to be deleted and the selection holds raw pointers.
    // Called directly rather than through TOOL_MANAGER::RunAction(), which would
    // suspend the coroutine and pump the event loop underneath us.
    if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
    {
        if( SCH_SELECTION_TOOL* sel = mgr->GetTool<SCH_SELECTION_TOOL>() )
            sel->ClearSelection();
    }

    // The user's own decisions about violations they do not care about.
    schematic->RecordERCExclusions();

    SCH_SCREENS screens( schematic->Root() );
    screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, true );

    // KiCad 10 loads a symbol library on first use, and ERC reports every symbol of one not
    // loaded yet as "library not found": 22 warnings on a project just opened, none once the
    // model had searched the libraries. The libraries this sheet uses are loaded first.
    if( SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &aFrame->Prj() ) )
    {
        std::set<wxString> used;

        for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
            used.insert( entry.symbol->GetLibId().GetUniStringLibNickname() );

        for( const wxString& nickname : used )
        {
            if( !nickname.IsEmpty() && !adapter->IsLibraryLoaded( nickname ) )
                adapter->LoadOne( nickname );
        }
    }

    ERC_TESTER tester( schematic, false );

    // aEditFrame must be this frame, not nullptr: RunTests only rebuilds
    // connectivity when it is given one, and checking a stale connection graph
    // produces confident, wrong answers.
    tester.RunTests( aFrame->GetCanvas()->GetView()->GetDrawingSheet(), aFrame,
                     aFrame->Kiway().KiFACE( KIWAY::FACE_CVPCB ), &aFrame->Prj(), nullptr );

    // Rebuild the open dialog's tree from the markers that exist now.
    //
    // This is what makes running ERC underneath it safe rather than merely lucky:
    // everything from DeleteAllMarkers() to here is synchronous -- RunTests() is
    // handed a null PROGRESS_REPORTER, so nothing pumps the event loop -- which
    // means no paint or hover can observe the freed pointers before UpdateData()
    // replaces them. The dialog the user is reading refreshes in place, instead of
    // the turn stopping to ask them to close a window.
    if( ercDialog )
        ercDialog->UpdateData();

    SHEETLIST_ERC_ITEMS_PROVIDER provider( schematic );
    provider.SetSeverities( RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING );

    ERC_SETTINGS& settings = schematic->ErcSettings();

    std::map<wxString, int> errors;
    std::map<wxString, int> warnings;
    int                     errorCount   = 0;
    int                     warningCount = 0;
    int                     strapped     = 0;
    std::set<wxString>      undriven;    // "U1 pin 8 (VCC) on net VCC"

    for( int i = 0; i < provider.GetCount(); ++i )
    {
        std::shared_ptr<ERC_ITEM> item = provider.GetERCItem( i );

        if( !item )
            continue;

        const SEVERITY severity = settings.GetSeverity( item->GetErrorCode() );
        const wxString label    = item->GetErrorText( true );

        // A bidirectional pin strapped to a PWR_FLAG-driven supply or ground
        // (an address or mode pin tied low) trips KiCad's conservative default.
        if( item->GetErrorCode() == ERCE_PIN_TO_PIN_WARNING )
        {
            auto* a = dynamic_cast<SCH_PIN*>( schematic->ResolveItem( item->GetMainItemID(), nullptr, true ) );
            auto* b = dynamic_cast<SCH_PIN*>( schematic->ResolveItem( item->GetAuxItemID(), nullptr, true ) );
            auto flag = []( SCH_PIN* aPin )
            {
                auto* parent = aPin ? dynamic_cast<SCH_SYMBOL*>( aPin->GetParentSymbol() ) : nullptr;
                return parent && parent->GetLibId().GetLibItemName() == "PWR_FLAG";
            };
            auto bidi = []( SCH_PIN* aPin ) { return aPin && aPin->GetType() == ELECTRICAL_PINTYPE::PT_BIDI; };
            if( ( flag( a ) && bidi( b ) ) || ( flag( b ) && bidi( a ) ) )
                ++strapped;
        }

        // Name the undriven supply: the bare message sent a model searching the sheet
        // and trying parts for a dozen rounds without knowing which net it was.
        if( item->GetErrorCode() == ERCE_POWERPIN_NOT_DRIVEN && undriven.size() < 4 )
        {
            SCH_SHEET_PATH path;
            if( auto* pin = dynamic_cast<SCH_PIN*>( schematic->ResolveItem( item->GetMainItemID(), &path, true ) ) )
            {
                auto*           parent = dynamic_cast<SCH_SYMBOL*>( pin->GetParentSymbol() );
                SCH_CONNECTION* connection = pin->Connection( &path );
                undriven.insert( wxString::Format( wxS( "%s pin %s (%s) on net %s" ),
                                                   parent ? parent->GetRef( &path, false ) : wxString( wxS( "?" ) ),
                                                   pin->GetNumber(), pin->GetShownName(),
                                                   connection ? connection->Name( true ) : wxString( wxS( "?" ) ) ) );
            }
        }

        if( severity == RPT_SEVERITY_ERROR )
        {
            errors[label]++;
            ++errorCount;
        }
        else if( severity == RPT_SEVERITY_WARNING )
        {
            warnings[label]++;
            ++warningCount;
        }
    }

    aFrame->GetCanvas()->Refresh();

    wxString out;

    if( !errorCount && !warningCount )
    {
        out << _( "ERC clean: no errors, no warnings.\n" );
    }
    else
    {
        out << wxString::Format( _( "ERC: %d error(s), %d warning(s)\n" ), errorCount,
                                 warningCount );
    }

    // Markdown list lines: the chat joins plain lines into one run-on paragraph.
    for( const auto& [label, count] : errors )
        out << wxString::Format( wxS( "- %d %s: %s\n" ), count, count == 1 ? wxS( "error" ) : wxS( "errors" ), label );

    for( const auto& [label, count] : warnings )
        out << wxString::Format( wxS( "- %d %s: %s\n" ), count, count == 1 ? wxS( "warning" ) : wxS( "warnings" ), label );

    if( !undriven.empty() )
    {
        wxArrayString list;
        for( const wxString& entry : undriven )
            list.Add( entry );
        out << wxS( "\n" ) << _( "Nothing drives the supply at " ) << wxJoin( list, ';' )
            << _( ". If a battery or connector feeds that net, directly or through a switch, diode, fuse, "
                  "ferrite, inductor, resistor or MOSFET, flag it with edit_circuit power_flags (it checks "
                  "that chain). Otherwise a regulator or supply connection is missing. Never add a test "
                  "point or other part only to silence this.\n" );
    }

    if( strapped )
    {
        out << wxS( "\n" ) << wxString::Format(
                _( "Note: %d of these are a bidirectional pin tied straight to a flagged supply or "
                   "ground (an address or mode strap). That is normal practice and KiCad's default "
                   "is conservative; keep the strap unless that pin can actively drive.\n" ),
                strapped );
    }

    // ERC_TESTER does not check annotation the way the ERC dialog does, so an
    // unannotated schematic produces confidently misleading results. Saying so
    // is cheaper than letting the model report them as real.
    int unannotated = 0;

    for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
    {
        const wxString ref = entry.symbol->GetRef( &entry.path, false );

        if( ref.IsEmpty() || ref.EndsWith( wxS( "?" ) ) )
            ++unannotated;
    }

    if( unannotated )
    {
        out << wxS( "\n" ) << wxString::Format(
                _( "Note: %d symbol(s) are not annotated, which makes these results unreliable. "
                   "Run annotate_schematic first.\n" ),
                unannotated );
    }

    out << INDUCTIVE_CHECKS::Report( INDUCTIVE_CHECKS::Collect( schematic ) );

    aResult = out;
    return true;
}



/// Write a hidden part field (LCSC, MPN, Manufacturer), reusing one of any case.
/// The caller has already registered the symbol with its commit.
static bool PutPartField( SCH_SYMBOL* aSymbol, const wxString& aName, const wxString& aValue )
{
    if( aValue.IsEmpty() )
        return false;

    SCH_FIELD* field = aSymbol->FindFieldCaseInsensitive( aName );

    if( !field )
    {
        field = aSymbol->AddField( SCH_FIELD( aSymbol, FIELD_T::USER, aName ) );
        field->SetPosition( aSymbol->GetPosition() );
        field->SetVisible( false );
    }

    field->SetText( aValue );
    return true;
}


/**
 * The live BOM grouped as a buyer reads it, for /v1/parts/source: references,
 * value, footprint and symbol per line, an LCSC number when every reference
 * already shares one, and the highest rail voltage any of its pins touches.
 */
json BomForSourcing( SCHEMATIC& aSchematic )
{
    auto hierarchy = aSchematic.BuildSheetListSortedByPageNumbers();
    for( auto& path : hierarchy ) path.UpdateAllScreenReferences();
    aSchematic.ConnectionGraph()->Recalculate( hierarchy, true );

    struct ENTRY { SCH_SYMBOL* symbol; SCH_SHEET_PATH path; };
    std::vector<ENTRY> all;
    for( auto& path : hierarchy )
        for( auto* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            all.push_back( { static_cast<SCH_SYMBOL*>( item ), path } );

    struct GROUP { std::set<wxString> refs; wxString value, footprint, lib, lcsc; bool mixed = false; double rail = -1; };
    std::map<wxString, GROUP> groups;

    for( const auto& entry : all )
    {
        const wxString ref = entry.symbol->GetRef( &entry.path, false );

        if( entry.symbol->GetExcludedFromBOM() || entry.symbol->IsPower() || ref.StartsWith( wxS( "#" ) ) )
            continue;

        const wxString value = entry.symbol->GetValue( true, &entry.path, false );
        const wxString footprint = entry.symbol->GetFootprintFieldText( true, &entry.path, false );
        const wxString lib = entry.symbol->GetLibId().Format().wx_str();
        const SCH_FIELD* lcscField = entry.symbol->FindFieldCaseInsensitive( wxS( "LCSC" ) );
        const wxString lcsc = lcscField ? lcscField->GetShownText( &entry.path, false ) : wxString();

        GROUP& g = groups[value + wxS( "\x1f" ) + footprint + wxS( "\x1f" ) + lib];

        if( g.refs.empty() )
        {
            g.value = value; g.footprint = footprint; g.lib = lib; g.lcsc = lcsc;
        }
        else if( !g.refs.count( ref ) && g.lcsc != lcsc )
        {
            g.mixed = true;
        }

        g.refs.insert( ref );

        for( SCH_PIN* pin : entry.symbol->GetPins( &entry.path ) )
            if( SCH_CONNECTION* connection = pin->Connection( &entry.path ) )
                g.rail = std::max( g.rail, INDUCTIVE_CHECKS::detail::RailVoltage( connection->Name( true ) ) );
    }

    json lines = json::array();

    for( const auto& [key, g] : groups )
    {
        json refs = json::array();
        for( const wxString& r : g.refs ) refs.push_back( r.ToStdString() );
        json line = { { "refs", refs }, { "value", g.value.ToStdString() },
                      { "footprint", g.footprint.ToStdString() }, { "lib_id", g.lib.ToStdString() } };
        if( !g.mixed && !g.lcsc.IsEmpty() ) line["lcsc"] = g.lcsc.ToStdString();
        if( g.rail >= 0 ) line["rail_v"] = g.rail;
        lines.push_back( line );
    }

    return lines;
}


/// Record every confidently picked line of a /v1/parts/source answer. aModify
/// registers each symbol with the caller's commit before it changes.
json ApplySourcedPartsTo( SCHEMATIC& aSchematic, const json& aResponse, bool aOverwrite,
                          const std::function<void( SCH_SYMBOL*, SCH_SCREEN* )>& aModify )
{
    struct ENTRY { SCH_SYMBOL* symbol; SCH_SHEET_PATH path; };
    std::map<wxString, std::vector<ENTRY>> byRef;

    for( auto& path : aSchematic.BuildSheetListSortedByPageNumbers() )
        for( auto* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            byRef[symbol->GetRef( &path, false )].push_back( { symbol, path } );
        }

    json recorded = json::array(), kept = json::array();
    wxRegEx lcscPattern( wxS( "^C[0-9]{1,10}$" ) );

    for( const auto& line : aResponse.value( "lines", json::array() ) )
    {
        if( !line.is_object() || line.value( "status", "" ) != "picked" || !line.contains( "pick" ) || !line["pick"].is_object() )
            continue;

        const wxString lcsc = wxString::FromUTF8( line["pick"].value( "lcsc", "" ) ).Upper();
        const wxString mpn = wxString::FromUTF8( line["pick"].value( "mpn", "" ) ).Left( 80 );

        if( !lcscPattern.Matches( lcsc ) )
            continue;

        for( const auto& refJson : line.value( "refs", json::array() ) )
        {
            if( !refJson.is_string() ) continue;
            const wxString ref = wxString::FromUTF8( refJson.get<std::string>() );
            auto it = byRef.find( ref );
            if( it == byRef.end() ) continue;

            bool done = false;
            for( const ENTRY& entry : it->second )
            {
                const SCH_FIELD* existing = entry.symbol->FindFieldCaseInsensitive( wxS( "LCSC" ) );
                if( existing && !existing->GetText().IsEmpty() && !aOverwrite )
                {
                    if( !done ) kept.push_back( ref.ToStdString() );
                    done = true;
                    continue;
                }
                aModify( entry.symbol, entry.path.LastScreen() );
                PutPartField( entry.symbol, wxS( "LCSC" ), lcsc );
                PutPartField( entry.symbol, wxS( "MPN" ), mpn );
                if( !done ) recorded.push_back( ref.ToStdString() + " " + lcsc.ToStdString() );
                done = true;
            }
        }
    }

    return { { "recorded", recorded }, { "kept_existing", kept } };
}


/// The editor's half: one undo step for everything recorded.
static json ApplySourcedParts( SCH_EDIT_FRAME* aFrame, const json& aResponse, bool aOverwrite )
{
    RequireEditable( aFrame );
    SCH_COMMIT                         commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
    json out = ApplySourcedPartsTo( aFrame->Schematic(), aResponse, aOverwrite,
            [&]( SCH_SYMBOL* aSymbol, SCH_SCREEN* aScreen ) { commit.Modify( aSymbol, aScreen ); } );

    if( !out["recorded"].empty() )
    {
        commit.Push( _( "Source BOM" ) );
        guard.Pushed();
    }

    return out;
}


// Native circuit construction. All validation happens before the commit: a bad
// pin, missing footprint or duplicate reference must leave the sheet untouched.
LIB_SYMBOL* LoadDesignSymbol( const wxString& aText, LIB_ID& aId )
{
    if( aId.Parse( UTF8( aText.utf8_string() ) ) >= 0 || !aId.IsValid()
        || aId.GetUniStringLibNickname().IsEmpty() )
        throw ARGS::BAD_ARGUMENT( "Use an exact LIBRARY:SYMBOL id." );

    auto* libs = RequireSymbolLibraries();
    if( !Pgm().GetLibraryManager().GetRow( LIBRARY_TABLE_TYPE::SYMBOL,
                                             aId.GetUniStringLibNickname() ) )
        throw ARGS::BAD_ARGUMENT( "Symbol library not found: " + aText );
    if( !libs->IsLibraryLoaded( aId.GetUniStringLibNickname() ) )
        libs->LoadOne( aId.GetUniStringLibNickname() );
    auto* symbol = libs->LoadSymbol( aId );
    if( !symbol )
        throw ARGS::BAD_ARGUMENT( "Symbol not found: " + aText );
    return symbol;
}

/**
 * True when a part's value names another part than its library symbol: both look like
 * part numbers and neither covers the other (a lowercase x in a KiCad symbol name, as in
 * STM32F103C8Tx, stands for any character). Passives, connectors and switches carry
 * plain values. An ESP32-C3-MINI-1 value on the WROOM-02 symbol and footprint would
 * make the BOM buy the wrong module.
 */
static bool NamesAnotherPart( const LIB_ID& aSymbol, const wxString& aValue )
{
    const wxString lib = aSymbol.GetUniStringLibNickname();
    if( lib == wxS( "Device" ) || lib == wxS( "power" ) || lib.StartsWith( wxS( "Connector" ) )
        || lib.StartsWith( wxS( "Switch" ) ) || lib.StartsWith( wxS( "Mechanical" ) ) )
        return false;
    auto fold = []( const wxString& aText, bool aWild )
    {
        wxString out;
        for( wxUniChar c : aText )
            if( wxIsalnum( c ) ) out += aWild && c == 'x' ? wxUniChar( '?' ) : wxUniChar( wxTolower( c ) );
        return out;
    };
    auto partLike = []( const wxString& s )
    {
        bool alpha = false, digit = false;
        for( wxUniChar c : s ) { alpha |= wxIsalpha( c ) != 0; digit |= wxIsdigit( c ) != 0; }
        return s.length() >= 5 && alpha && digit;
    };
    const wxString value = fold( aValue, false ), name = fold( aSymbol.GetUniStringLibItemName(), true );
    if( !partLike( value ) || !partLike( name ) )
        return false;
    return !wxMatchWild( wxS( "*" ) + name + wxS( "*" ), value, false ) && !name.Contains( value );
}

/// One installed symbol in SymbolIndex().
struct INDEXED_SYMBOL
{
    wxString library, name, description, keywords;
};

/// The quoted text after a KiCad property name on one line, unescaped.
static wxString QuotedAfter( const std::string& aLine, size_t aFrom )
{
    std::string out;
    for( size_t i = aFrom; i < aLine.size() && aLine[i] != '"'; ++i )
    {
        if( aLine[i] == '\\' && i + 1 < aLine.size() ) ++i;
        out += aLine[i];
    }
    return wxString::FromUTF8( out );
}

/**
 * Every symbol in every installed .kicad_sym library: name, description, keywords.
 * A line scan of the files (KiCad writes top-level symbols one tab in), not a load:
 * about a second for KiCad's 225 libraries, once per session, where loading them all
 * would stall the editor far longer. Searching only the libraries already in memory
 * found nothing for AMS1117, BME280 or ESP32-C3 on a fresh editor.
 */
static const std::vector<INDEXED_SYMBOL>& SymbolIndex()
{
    static std::vector<INDEXED_SYMBOL> index;
    static bool                        built = false;

    if( built )
        return index;

    built = true;
    auto& manager = Pgm().GetLibraryManager();
    const std::string name = "\t(symbol \"", description = "\t\t(property \"Description\" \"",
                      keywords = "\t\t(property \"ki_keywords\" \"";

    for( auto* row : manager.Rows( LIBRARY_TABLE_TYPE::SYMBOL ) )
    {
        auto uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::SYMBOL, row->Nickname(), true );
        if( !uri || !uri->EndsWith( wxS( ".kicad_sym" ) ) ) continue;
        std::ifstream in( uri->ToStdString() );
        std::string line;
        bool        inSymbol = false;

        while( std::getline( in, line ) )
        {
            if( line.compare( 0, name.size(), name ) == 0 )
            {
                index.push_back( { row->Nickname(), QuotedAfter( line, name.size() ) } );
                inSymbol = true;
            }
            else if( inSymbol && line.compare( 0, description.size(), description ) == 0 )
                index.back().description = QuotedAfter( line, description.size() );
            else if( inSymbol && line.compare( 0, keywords.size(), keywords ) == 0 )
                index.back().keywords = QuotedAfter( line, keywords.size() );
        }
    }

    return index;
}

/// Symbols matching every word of a query: by name first, then description or keywords.
static wxString SearchAllSymbols( const wxString& aQuery )
{
    // USB-C, USB_C and usbc are one word to whoever types them.
    auto fold = []( wxString aText )
    {
        aText.MakeLower();
        aText.Replace( wxS( "-" ), wxEmptyString );
        aText.Replace( wxS( "_" ), wxEmptyString );
        return aText;
    };
    wxArrayString words = wxSplit( aQuery.Lower().Trim().Trim( false ), ' ' );
    for( wxString& word : words ) word = fold( word );
    std::vector<std::pair<int, const INDEXED_SYMBOL*>> hits;

    for( const INDEXED_SYMBOL& symbol : SymbolIndex() )
    {
        const wxString name = fold( symbol.name ), about = fold( symbol.description + " " + symbol.keywords );
        int score = 0;
        bool all = true;

        for( const wxString& word : words )
        {
            if( word.IsEmpty() ) continue;
            if( name == word ) score += 6;
            else if( name.Contains( word ) ) score += 4;
            else if( about.Contains( word ) || symbol.library.Lower().Contains( word ) ) score += 1;
            else all = false;
        }

        if( all && score > 0 )
            hits.emplace_back( score, &symbol );
    }

    std::stable_sort( hits.begin(), hits.end(), []( const auto& a, const auto& b )
                      { return a.first != b.first ? a.first > b.first : a.second->name.length() < b.second->name.length(); } );

    if( hits.empty() )
        return wxString::Format( _( "Nothing matching '%s' in %zu installed symbols. Try a part family "
                                    "(AMS1117, LM358), a function (LDO, USB-C) or a library name." ),
                                 aQuery, SymbolIndex().size() );

    const size_t limit = 40;
    wxString     out = wxString::Format( _( "%zu matches%s across all installed libraries\n" ),
                                         std::min( hits.size(), limit ), hits.size() > limit ? _( " (best shown)" ) : wxString() );

    for( size_t i = 0; i < hits.size() && i < limit; ++i )
    {
        const INDEXED_SYMBOL& s = *hits[i].second;
        out << wxS( "  " ) << s.library << wxS( ":" ) << s.name;
        if( !s.description.IsEmpty() )
            out << wxS( " -- " ) << ( s.description.length() > 90 ? s.description.Left( 87 ) + wxS( "..." ) : s.description );
        out << wxS( "\n" );
    }

    return out;
}

/// Every installed footprint as LIBRARY:NAME, from the directory listings; built once.
static const std::vector<wxString>& FootprintIndex()
{
    static std::vector<wxString> index;
    static bool                  built = false;
    if( built ) return index;
    built = true;
    auto& manager = Pgm().GetLibraryManager();
    for( auto* row : manager.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
    {
        auto uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::FOOTPRINT, row->Nickname(), true );
        if( !uri ) continue;
        wxDir dir( *uri );
        if( !dir.IsOpened() ) continue;
        wxString name;
        for( bool more = dir.GetFirst( &name, "*.kicad_mod", wxDIR_FILES ); more; more = dir.GetNext( &name ) )
            index.push_back( row->Nickname() + ":" + wxFileName( name ).GetName() );
    }
    return index;
}

/// Installed footprints a symbol's own footprint filters accept, shortest first: what
/// get_symbol_definition offers when the symbol has no default, so the model need not
/// spend a round searching for each switch and connector.
static wxString SuggestedFootprints( const wxArrayString& aFilters, size_t aLimit = 5 )
{
    std::vector<wxString> hits;
    for( const wxString& id : FootprintIndex() )
    {
        const wxString name = id.AfterFirst( ':' );
        for( const wxString& filter : aFilters )
            if( filter.Contains( ':' ) ? wxMatchWild( filter, id, false ) : wxMatchWild( filter, name, false ) )
            {
                hits.push_back( id );
                break;
            }
    }
    std::stable_sort( hits.begin(), hits.end(), []( const wxString& a, const wxString& b ) { return a.length() < b.length(); } );
    wxString out;
    for( size_t i = 0; i < hits.size() && i < aLimit; ++i ) out << ( i ? ", " : "" ) << hits[i];
    return out;
}

/// Installed footprints closest to an id that does not exist: the parts of its name
/// (1x02, P2.54mm, Vertical) found in theirs. " Closest installed: ..." or empty.
static wxString NearestFootprints( const wxString& aNickname, const wxString& aItem )
{
    std::vector<wxString> tokens;
    for( const wxString& t : wxSplit( aItem.Lower(), '_' ) )
        for( const wxString& u : wxSplit( t, '-' ) )
            if( u.length() >= 2 ) tokens.push_back( u );
    if( tokens.empty() ) return wxEmptyString;
    std::vector<std::pair<int, wxString>> scored;
    auto& manager = Pgm().GetLibraryManager();
    for( auto* row : manager.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
    {
        auto uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::FOOTPRINT, row->Nickname(), true );
        if( !uri ) continue;
        wxDir dir( *uri );
        if( !dir.IsOpened() ) continue;
        wxString name;
        for( bool more = dir.GetFirst( &name, "*.kicad_mod", wxDIR_FILES ); more; more = dir.GetNext( &name ) )
        {
            const wxString item = wxFileName( name ).GetName(), lower = item.Lower();
            int score = row->Nickname() == aNickname ? 1 : 0;
            for( const wxString& t : tokens ) score += lower.Contains( t ) ? 2 : 0;
            if( score >= std::max<int>( 4, tokens.size() ) ) scored.emplace_back( score, row->Nickname() + ":" + item );
        }
    }
    std::sort( scored.begin(), scored.end(), []( const auto& a, const auto& b )
               { return a.first != b.first ? a.first > b.first : a.second.length() < b.second.length(); } );
    wxString best;
    for( size_t i = 0; i < scored.size() && i < 5; ++i ) best << ( i ? ", " : "" ) << scored[i].second;
    return best.IsEmpty() ? wxString() : " Closest installed: " + best + ".";
}

wxString FootprintFile( const wxString& aText )
{
    LIB_ID id;
    if( id.Parse( UTF8( aText.utf8_string() ) ) >= 0 || !id.IsValid()
        || id.GetUniStringLibNickname().IsEmpty()
        || id.GetUniStringLibItemName().Contains( "/" )
        || id.GetUniStringLibItemName().Contains( "\\" ) )
        throw ARGS::BAD_ARGUMENT( "Use an exact LIBRARY:FOOTPRINT id." );
    auto uri = Pgm().GetLibraryManager().GetFullURI(
            LIBRARY_TABLE_TYPE::FOOTPRINT, id.GetUniStringLibNickname(), true );
    if( !uri )
        throw ARGS::BAD_ARGUMENT( "Footprint library not found: " + aText
                                  + NearestFootprints( id.GetUniStringLibNickname(), id.GetUniStringLibItemName() ) );
    wxFileName file( *uri, id.GetUniStringLibItemName() + ".kicad_mod" );
    if( !file.FileExists() )
        throw ARGS::BAD_ARGUMENT( "Footprint not found in the installed library: " + aText
                                  + NearestFootprints( id.GetUniStringLibNickname(), id.GetUniStringLibItemName() ) );
    return file.GetFullPath();
}

/**
 * Footprints in aLibrary with a pad for every number in aPins, those matching the
 * symbol's own footprint filters first: what a refusal suggests instead. An audio
 * jack's symbol (pins S, SN, R, RN, T, TN) and a footprint numbered otherwise were
 * refused with nothing to go on, and the model spent rounds guessing.
 */
static std::vector<std::tuple<int, int, wxString>> CompatibleFootprintList( const wxString& aLibrary, const wxString& aChosen,
                                                                           const std::set<wxString>& aPins,
                                                                           const wxArrayString& aFilters )
{
    std::vector<std::tuple<int, int, wxString>> found;   // (not filtered, -shared name prefix, id)
    auto uri = Pgm().GetLibraryManager().GetFullURI( LIBRARY_TABLE_TYPE::FOOTPRINT, aLibrary, true );
    if( !uri ) return found;
    wxDir dir( *uri );
    if( !dir.IsOpened() ) return found;
    static const std::regex pad( R"re(\n\s*\(pad "([^"]*)")re" );
    wxString name;
    for( bool more = dir.GetFirst( &name, "*.kicad_mod", wxDIR_FILES ); more; more = dir.GetNext( &name ) )
    {
        wxFFile file( wxFileName( *uri, name ).GetFullPath(), "r" );
        wxString content;
        if( !file.IsOpened() || !file.ReadAll( &content ) ) continue;
        const std::string text = content.ToStdString( wxConvUTF8 );
        std::set<wxString> pads;
        for( auto it = std::sregex_iterator( text.begin(), text.end(), pad ); it != std::sregex_iterator(); ++it )
            pads.insert( wxString::FromUTF8( ( *it )[1].str() ) );
        if( !std::includes( pads.begin(), pads.end(), aPins.begin(), aPins.end() ) ) continue;
        const wxString item = wxFileName( name ).GetName();
        bool filtered = false;
        for( const wxString& filter : aFilters )
            filtered = filtered || wxMatchWild( filter.Lower(), item.Lower(), false );
        // The same family as the one chosen first: Jack_3.5mm_ before Jack_6.35mm_.
        size_t shared = 0;
        while( shared < item.length() && shared < aChosen.length() && item[shared] == aChosen[shared] ) ++shared;
        found.emplace_back( filtered ? 0 : 1, -(int) shared, aLibrary + ":" + item );
    }
    std::sort( found.begin(), found.end() );
    return found;
}

static wxString CompatibleFootprints( const wxString& aLibrary, const wxString& aChosen,
                                      const std::set<wxString>& aPins, const wxArrayString& aFilters )
{
    const auto found = CompatibleFootprintList( aLibrary, aChosen, aPins, aFilters );
    wxString   out;
    for( size_t i = 0; i < found.size() && i < 6; ++i ) out << ( i ? ", " : "" ) << std::get<2>( found[i] );
    return out;
}

/// Symbols in aLibrary whose pin numbers are all pads of the footprint, covering the
/// most of them first: the other way out of a mismatch. (No 3.5 mm footprint has all
/// six pins of AudioJack3_Switch; the PJ320D's S, T, R1, R2 are AudioJack4's.)
static wxString MatchingSymbols( const wxString& aLibrary, const std::set<wxString>& aPads )
{
    std::vector<std::pair<int, wxString>> found;
    try
    {
        for( LIB_SYMBOL* symbol : RequireSymbolLibraries()->GetSymbols( aLibrary ) )
        {
            if( !symbol || symbol->IsPower() ) continue;
            std::unique_ptr<LIB_SYMBOL> flat = symbol->Flatten();
            std::set<wxString> numbers;
            for( SCH_PIN* pin : flat->GetPins() ) numbers.insert( pin->GetNumber() );
            if( numbers.size() < 2 || !std::includes( aPads.begin(), aPads.end(), numbers.begin(), numbers.end() ) )
                continue;
            found.emplace_back( -(int) numbers.size(), aLibrary + ":" + symbol->GetName() );
        }
    }
    catch( ... )
    {
        return wxString();
    }
    std::sort( found.begin(), found.end() );
    wxString out;
    for( size_t i = 0; i < found.size() && i < 4; ++i ) out << ( i ? ", " : "" ) << found[i].second;
    return out;
}

void ValidateFootprint( SCH_SYMBOL* aSymbol, SCH_SHEET_PATH& aSheet, const wxString& aText )
{
    wxFFile file( FootprintFile( aText ), "r" );
    wxString content;
    if( !file.IsOpened() || !file.ReadAll( &content ) )
        throw ARGS::BAD_ARGUMENT( "Could not read footprint: " + aText );
    // Read actual pad expressions, never pad-shaped text in a graphic or property.
    auto summary = INDUCTIVE_ATTACHMENTS::Inspect( {
        { "name", wxFileName( FootprintFile( aText ) ).GetFullName().ToStdString() },
        { "text", content.ToStdString() } } );
    std::set<wxString> numbers;
    for( const auto& number : summary["pads"] )
        if( !number.get<std::string>().empty() ) numbers.insert( wxString::FromUTF8( number.get<std::string>() ) );
    std::set<wxString> pins;
    wxArrayString      missing;
    for( SCH_PIN* pin : aSymbol->GetPins( &aSheet ) )
    {
        pins.insert( pin->GetNumber() );
        if( !numbers.count( pin->GetNumber() ) && missing.Index( pin->GetNumber() ) == wxNOT_FOUND )
            missing.Add( pin->GetNumber() );
    }
    if( missing.IsEmpty() )
        return;
    wxArrayString padList;
    for( const wxString& n : numbers ) padList.Add( n );
    wxArrayString filters;
    if( aSymbol->GetLibSymbolRef() )
        filters = aSymbol->GetLibSymbolRef()->GetFPFilters();
    LIB_ID id;
    id.Parse( UTF8( aText.utf8_string() ) );
    const wxString compatible = CompatibleFootprints( id.GetUniStringLibNickname(), id.GetUniStringLibItemName(), pins,
                                                      filters );
    const wxString symbols = MatchingSymbols( aSymbol->GetLibId().GetUniStringLibNickname(), numbers );
    wxString       next;
    if( !symbols.IsEmpty() )
        next << "Symbols whose pins are this footprint's pads: " << symbols << ". ";
    if( !compatible.IsEmpty() )
        next << "Footprints in " << id.GetUniStringLibNickname() << " with every pin of this symbol: " << compatible
             << ".";
    if( next.IsEmpty() )
        next = "Choose a compatible package, or a symbol whose pin numbers match these pads.";
    throw ARGS::BAD_ARGUMENT( wxString::Format( "Footprint %s has no pad %s for %s (its pads: %s). %s", aText,
                                                wxJoin( missing, ',' ), aSymbol->GetRef( &aSheet, false ),
                                                wxJoin( padList, ',' ), next.Trim() ) );
}

/**
 * A plan's footprint as given, or -- when its pads are numbered otherwise than the symbol's
 * pins -- the first footprint of the same library and family that has every pin and that the
 * symbol's own footprint filters accept, with a note saying so. A model chose a 1-6 numbered
 * 3.5 mm jack for an S, R, T symbol; the refusal led to a partial build, two refused edits
 * and a rebuild, 33 calls. Refused as ValidateFootprint refuses when there is no such one.
 */
static wxString FitFootprint( SCH_SYMBOL* aSymbol, SCH_SHEET_PATH& aSheet, const wxString& aText, wxString& aNote )
{
    try
    {
        ValidateFootprint( aSymbol, aSheet, aText );
        return aText;
    }
    catch( const ARGS::BAD_ARGUMENT& )
    {
        LIB_ID id;
        if( id.Parse( UTF8( aText.utf8_string() ) ) >= 0 || !aSymbol->GetLibSymbolRef() )
            throw;

        std::set<wxString> pins;
        for( SCH_PIN* pin : aSymbol->GetPins( &aSheet ) )
            pins.insert( pin->GetNumber() );

        const wxArrayString filters = aSymbol->GetLibSymbolRef()->GetFPFilters();
        const auto          found = CompatibleFootprintList( id.GetUniStringLibNickname(), id.GetUniStringLibItemName(),
                                                             pins, filters );

        // What a footprint file says about itself: its pads, and smd or through_hole.
        auto describe = []( const wxString& aId, std::set<wxString>& aPads ) -> wxString
        {
            wxFFile  file( FootprintFile( aId ), "r" );
            wxString content;

            if( !file.IsOpened() || !file.ReadAll( &content ) )
                return wxString();

            static const std::regex pad( R"re(\n\s*\(pad "([^"]*)")re" );
            const std::string       text = content.ToStdString( wxConvUTF8 );

            for( auto it = std::sregex_iterator( text.begin(), text.end(), pad ); it != std::sregex_iterator(); ++it )
                if( !( *it )[1].str().empty() )
                    aPads.insert( wxString::FromUTF8( ( *it )[1].str() ) );

            return content.Contains( wxS( "(attr smd" ) ) ? wxString( wxS( "smd" ) )
                   : content.Contains( wxS( "(attr through_hole" ) ) ? wxString( wxS( "through_hole" ) ) : wxString();
        };
        auto orientation = []( const wxString& aId )
        {
            const wxString lower = aId.Lower();
            return lower.Contains( wxS( "vertical" ) ) ? 1 : lower.Contains( wxS( "horizontal" ) ) ? 2 : 0;
        };

        std::set<wxString> chosenPads;
        const wxString     chosenAttr = describe( aText, chosenPads );

        // Only one the symbol says fits, of the chosen one's family (Jack_3.5mm_...); then
        // mounted the same way, facing the same way, with the fewest pads left over.
        wxString other;
        std::tuple<int, int, size_t, int> best{ INT_MAX, INT_MAX, SIZE_MAX, 0 };

        for( size_t i = 0; i < found.size() && i < 24; ++i )
        {
            const auto& [unfiltered, shared, candidate] = found[i];

            if( unfiltered != 0 || -shared < 6 )
                continue;

            std::set<wxString> pads;
            const wxString     attr = describe( candidate, pads );
            const std::tuple<int, int, size_t, int> score{ attr != chosenAttr, orientation( candidate ) != orientation( aText ),
                                                           pads.size() - std::min( pads.size(), pins.size() ), shared };

            if( score < best )
            {
                best = score;
                other = candidate;
            }
        }

        if( other.IsEmpty() )
            throw;

        ValidateFootprint( aSymbol, aSheet, other );

        wxArrayString pinList;
        for( const wxString& pin : pins ) pinList.Add( pin );

        aNote << wxString::Format( "%s: footprint %s has no pads for the symbol's pins (%s), so %s is used, the "
                                   "first in %s that has them; give another with set_symbol_footprint if the "
                                   "part must differ.\n",
                                   aSymbol->GetRef( &aSheet, false ), aText, wxJoin( pinList, ',' ), other,
                                   id.GetUniStringLibNickname() );
        return other;
    }
}

wxString StandardFootprint( const wxString& aLibId )
{
    const std::map<wxString, wxString> defaults = {
        { "Device:R", "Resistor_SMD:R_0805_2012Metric" },
        { "Device:C", "Capacitor_SMD:C_0805_2012Metric" },
        { "Device:LED", "LED_SMD:LED_0805_2012Metric" },
        // JLCPCB's basic tactile switch (TS-1187A-B-A-B, C318884) and the USB-C
        // receptacle it stocks most (HRO TYPE-C-31-M-12, C165948): the BOM sourcer
        // finds the part each footprint is drawn for.
        { "Switch:SW_Push", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A" },
        { "Connector:USB_C_Receptacle_USB2.0_16P", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12" },
        { "Connector_Generic:Conn_01x02", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical" },
        { "Connector_Generic:Conn_01x03", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical" },
        { "Device:R_Small", "Resistor_SMD:R_0805_2012Metric" },
        { "Device:C_Small", "Capacitor_SMD:C_0805_2012Metric" },
        { "Device:LED_Small", "LED_SMD:LED_0805_2012Metric" },
        { "Device:C_Polarized", "Capacitor_SMD:CP_Elec_6.3x5.4" },
        { "Device:D", "Diode_SMD:D_SOD-123" },
        { "Device:D_Schottky", "Diode_SMD:D_SOD-123" },
        // A coin cell and an on/off slide switch, both stocked by LCSC (MYOUNG
        // BS-07-A1BJ001, C2979167; SHOU HAN MSK12C02, C431540, common on pad 2):
        // a simple battery board needs no footprint searches at all.
        { "Device:Battery_Cell", "Battery:BatteryHolder_MYOUNG_BS-07-A1BJ001_CR2032" },
        { "Switch:SW_SPDT", "Button_Switch_SMD:SW_SPDT_Shouhan_MSK12C02" },
        { "Switch:SW_SPST", "Button_Switch_SMD:SW_SPDT_Shouhan_MSK12C02" }
    };
    auto it = defaults.find( aLibId );
    return it == defaults.end() ? wxString() : it->second;
}

wxString DefaultFootprint( SCH_SYMBOL* aSymbol, SCH_SHEET_PATH& aSheet, const wxString& aLibId )
{
    auto fp = aSymbol->GetFootprintFieldText( true, &aSheet, false );
    return fp.IsEmpty() ? StandardFootprint( aLibId ) : fp;
}

/// "U1.3" -> every drawn copy of that pin. A multi-unit package draws a pin once
/// per unit that carries it, so one number can have several copies.
using DESIGN_PINS = std::map<wxString, std::vector<SCH_PIN*>>;
/// "U1.GND" -> the pin numbers with that name.
using PIN_NAMES = std::map<wxString, std::vector<wxString>>;

/// A pin name as a person or model would write it: library markup removed
/// (~{CS} -> CS, IO_{0} -> IO0), upper case.
wxString NormalPinName( wxString aName )
{
    for( const wxChar* markup : { wxS( "~{" ), wxS( "_{" ), wxS( "^{" ), wxS( "}" ), wxS( "~" ) } )
        aName.Replace( markup, wxEmptyString );

    return aName.Upper().Trim().Trim( false );
}

/// Index a pin under its full name and each half of a combined name
/// ("DI/IO0" answers to DI and IO0); a name shared by pins resolves only as
/// ResolvePin allows.
void IndexPinName( PIN_NAMES& aNames, const wxString& aRef, const wxString& aShown, const wxString& aKey )
{
    const wxString full = NormalPinName( aShown );

    if( full.IsEmpty() )
        return;

    std::set<wxString> forms = { full };

    for( const wxString& part : wxSplit( full, '/' ) )
        if( !part.IsEmpty() )
            forms.insert( part );

    for( const wxString& form : forms )
        aNames[aRef + "." + form].push_back( aKey );
}

/// Ground and power names that legitimately repeat on one package.
bool IsSharedSupplyName( const wxString& aName )
{
    static wxRegEx supply( "^(GND|VSS|VSSA|VSSD|AGND|DGND|PGND|GNDA|GNDD|GNDPWR|EP|EPAD|PAD|THERMAL_PAD|SHIELD|VDD|VCC|VDDA|VDDIO|VBUS|3V3|5V|IOVDD|DVDD)[0-9_]*$", wxRE_ICASE );
    return supply.Matches( aName );
}

/**
 * The pin numbers a plan's "REF.PIN" names. A number wins; otherwise the pin's
 * name. Models often know a part by its pin names rather than its numbering,
 * and guessing a number is the commonest way a plan goes wrong. A name that
 * several pins share joins them all only when they are supply or ground pins --
 * VDD and GND repeat by design; anything else is ambiguous and refused.
 */
std::vector<wxString> ResolvePin( const wxString& aKey, const DESIGN_PINS& aPins, const PIN_NAMES& aNames )
{
    if( aPins.count( aKey ) )
        return { aKey };

    const int dot = aKey.Find( '.', true );

    if( dot == wxNOT_FOUND )
        throw ARGS::BAD_ARGUMENT( "Pins are written REF.PIN, e.g. R1.1 or U1.GND: " + aKey );

    const wxString ref = aKey.Left( dot );
    const wxString pinName = aKey.Mid( dot + 1 );

    // "~" is how a library says a pin has no name; gate and op-amp pins are
    // reached by number only.
    if( pinName.IsEmpty() || pinName == wxS( "~" ) )
        throw ARGS::BAD_ARGUMENT( "No pin " + aKey + ". Unnamed pins are written by number." );

    const auto hit = aNames.find( ref + "." + NormalPinName( pinName ) );

    if( hit == aNames.end() )
        throw ARGS::BAD_ARGUMENT( "No pin " + aKey + ". Read get_symbol_definition first." );

    if( hit->second.size() > 1 )
    {
        // Joined when they are supply pins, or one signal repeated within a
        // single unit (USB-C's two D+ contacts). The same name on different
        // units is different circuitry -- op-amp A's + is not op-amp B's +.
        bool supply = IsSharedSupplyName( pinName );
        bool repeated = true;
        const SCH_PIN* first = aPins.at( hit->second.front() ).front();

        for( const wxString& number : hit->second )
        {
            const SCH_PIN* pin = aPins.at( number ).front();
            const auto     type = pin->GetType();
            supply = supply || type == ELECTRICAL_PINTYPE::PT_POWER_IN || type == ELECTRICAL_PINTYPE::PT_POWER_OUT;
            repeated = repeated && pin->GetUnit() == first->GetUnit() && type == first->GetType();
        }

        if( !supply && !repeated )
        {
            wxArrayString numbers;
            for( const wxString& n : hit->second ) numbers.Add( n.AfterLast( '.' ) );
            throw ARGS::BAD_ARGUMENT( "Pin name " + aKey + " is ambiguous (pins " + wxJoin( numbers, ',' )
                                     + "). Use the pin number." );
        }
    }

    return hit->second;
}

/// The builder's own rule for a net name: KiCad's supply names (+5V, +3V3, -12V)
/// start with a sign, everything else with a letter or _.
static bool IsLegalNetName( const wxString& aName )
{
    static const wxRegEx safeName( "^([A-Za-z_]|[+-][A-Za-z0-9])[A-Za-z0-9_+.-]*$" );
    return aName.length() <= 40 && safeName.Matches( aName );
}

/**
 * KiCad's own spellings of a net, as a model copies them from a pin name or a
 * netlist, made legal: ~{SHDN} (an overbar) becomes SHDN_N, Net-(D1-K) (an unnamed
 * net) D1-K, /VIN (a sheet path) VIN, 3V3 +3V3, a space _. Refusing them cost a
 * round each. A name that is already legal, or cannot be made so, is returned as is.
 */
static wxString LegalNetName( const wxString& aName )
{
    if( IsLegalNetName( aName ) )
        return aName;

    wxString n = aName;
    n.Trim().Trim( false );

    if( n.Contains( wxS( "/" ) ) )
        n = n.AfterLast( '/' );

    static const wxRegEx unnamed( wxS( "^(unconnected-)?Net-\\((.*)\\)$" ) );
    if( unnamed.Matches( n ) )
        n = unnamed.GetMatch( n, 2 );

    static const wxRegEx overbar( wxS( "~\\{([^}]*)\\}" ) );
    overbar.ReplaceAll( &n, wxS( "\\1_N" ) );

    wxString clean;
    for( wxUniChar c : n )
        clean += ( wxIsalnum( c ) && c < 128 ) || c == '_' || c == '+' || c == '-' || c == '.' ? c : wxUniChar( '_' );
    while( clean.Replace( wxS( "__" ), wxS( "_" ) ) ) {}

    static const wxRegEx rail( wxS( "^[0-9]+V[0-9]*$" ), wxRE_ICASE );
    if( !clean.IsEmpty() && wxIsdigit( clean[0] ) )
        clean = ( rail.Matches( clean ) ? wxS( "+" ) : wxS( "N" ) ) + clean;

    clean = clean.Left( 40 );
    return IsLegalNetName( clean ) ? clean : aName;
}

// A short wire extends OUT of each pin. Its endpoint receives a local net label.
// Label connectivity avoids invented wire coordinates and accidental crossings.
/// The wire stub a pin gets: three grid steps straight out from it, on the sheet.
static VECTOR2I StubDelta( const SCH_PIN* aPin )
{
    VECTOR2I delta;

    switch( aPin->GetOrientation() )
    {
    case PIN_ORIENTATION::PIN_RIGHT: delta = VECTOR2I( -SCH_GRID_IU * 3, 0 ); break;
    case PIN_ORIENTATION::PIN_LEFT:  delta = VECTOR2I( SCH_GRID_IU * 3, 0 ); break;
    case PIN_ORIENTATION::PIN_UP:    delta = VECTOR2I( 0, SCH_GRID_IU * 3 ); break;
    default:                         delta = VECTOR2I( 0, -SCH_GRID_IU * 3 ); break;
    }

    auto* parent = static_cast<const SCH_SYMBOL*>( aPin->GetParentSymbol() );
    return parent->GetTransform().TransformCoordinate( delta );
}


/**
 * Ground and supply nets drawn with power symbols, a ground symbol or a supply arrow, as an
 * engineer draws them; every other net with labels. Made per sheet (MakePowerStyle): a net
 * the sheet already labels keeps its labels, so an edit cannot split one net in two.
 */
struct POWER_STYLE
{
    /// Whether this net gets power symbols. aPowered: the net has a power pin or is flagged
    /// as supplied from outside, so a power symbol (itself a power input) changes no ERC result.
    std::function<bool( const wxString& aNet, bool aPowered )> wants;
    /// A power symbol for the net with its pin at aAt, pointing along aAway.
    std::function<std::unique_ptr<SCH_SYMBOL>( const wxString& aNet, const VECTOR2I& aAt, const VECTOR2I& aAway )> make;
    /// The net was drawn with labels.
    std::function<void( const wxString& aNet )> labelled;
};

std::map<const SCH_PIN*, wxString> PinNets( const json& aNets, const DESIGN_PINS& aPins, const PIN_NAMES& aNames );


void AddNetStubs( const json& aNets, const DESIGN_PINS& aPins, const PIN_NAMES& aNames,
                  std::set<wxString>& aAssigned, std::vector<std::unique_ptr<SCH_ITEM>>& aItems,
                  const POWER_STYLE* aPower = nullptr )
{
    if( !aNets.is_array() || aNets.size() > 600 )
        throw ARGS::BAD_ARGUMENT( "nets must be an array of at most 600 nets." );
    std::set<wxString> netNames;
    struct BRANCH { VECTOR2I start, end; wxString net; };
    std::vector<BRANCH> branches;
    const std::map<const SCH_PIN*, wxString> netOf =
            aPower ? PinNets( aNets, aPins, aNames ) : std::map<const SCH_PIN*, wxString>();

    // A wire of one net, refused where it would touch another net's.
    auto wire = [&]( const VECTOR2I& aStart, const VECTOR2I& aEnd, const wxString& aName )
    {
        for( const auto& branch : branches )
            if( branch.net != aName
                && ( IsPointOnSegment( branch.start, branch.end, aStart )
                     || IsPointOnSegment( branch.start, branch.end, aEnd )
                     || IsPointOnSegment( aStart, aEnd, branch.start )
                     || IsPointOnSegment( aStart, aEnd, branch.end ) ) )
                throw ARGS::BAD_ARGUMENT( "Wire branches would short " + aName + " and "
                                         + branch.net + ". Space the symbols farther apart, "
                                         "or omit x and y to let the builder lay them out." );
        branches.push_back( { aStart, aEnd, aName } );
        auto line = std::make_unique<SCH_LINE>( aStart, LAYER_WIRE );
        line->SetEndPoint( aEnd );
        aItems.push_back( std::move( line ) );
    };

    for( const auto& net : aNets )
    {
        const wxString name = ARGS::RequireString( net, "name" );
        if( !IsLegalNetName( name ) )
            throw ARGS::BAD_ARGUMENT( "Net name '" + name + "' is not allowed: use at most 40 letters, digits and "
                                      "_ + . -, starting with a letter, _ or a sign (+5V)." );
        if( !netNames.insert( name ).second )
            throw ARGS::BAD_ARGUMENT( "Net " + name + " is listed twice. Give each net one entry with all of its pins." );

        struct STUB { const SCH_PIN* pin; VECTOR2I start, end, delta; };
        std::vector<STUB>             stubs;
        std::set<std::pair<int, int>> drawn;   // stacked pins share a point; one stub each
        bool powered = net.contains( "externally_powered" ) && net["externally_powered"].is_boolean()
                       && net["externally_powered"].get<bool>();

        for( const wxString& member : ARGS::RequireStringArray( net, "pins", 256 ) )
        {
            for( const wxString& key : ResolvePin( member, aPins, aNames ) )
            {
                if( !aAssigned.insert( key ).second )
                    throw ARGS::BAD_ARGUMENT( "Pin appears on multiple nets: " + key );
                for( SCH_PIN* pin : aPins.at( key ) )
                {
                    if( pin->GetType() == ELECTRICAL_PINTYPE::PT_NC )
                        throw ARGS::BAD_ARGUMENT( "Pin " + key + " is specified as no-connect by its library." );
                    powered = powered || pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_IN
                              || pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_OUT;
                    if( !drawn.insert( { pin->GetPosition().x, pin->GetPosition().y } ).second )
                        continue;
                    const VECTOR2I delta = StubDelta( pin );
                    stubs.push_back( { pin, pin->GetPosition(), pin->GetPosition() + delta, delta } );
                }
            }
        }

        for( const STUB& stub : stubs )
            wire( stub.start, stub.end, name );

        if( !aPower || !aPower->wants( name, powered ) )
        {
            for( const STUB& stub : stubs )
            {
                auto label = std::make_unique<SCH_LABEL>( stub.end, name );
                label->SetTextSize( VECTOR2I( SCH_GRID_IU, SCH_GRID_IU ) );
                // Keep labels outside the body, with the text running away from pins: along
                // the stub, so the labels of a row of pins on a top or bottom edge stand
                // side by side instead of writing over each other.
                label->SetSpinStyle( stub.delta.x < 0   ? SPIN_STYLE::LEFT
                                     : stub.delta.x > 0 ? SPIN_STYLE::RIGHT
                                     : stub.delta.y < 0 ? SPIN_STYLE::UP
                                                        : SPIN_STYLE::BOTTOM );
                aItems.push_back( std::move( label ) );
            }

            if( aPower )
                aPower->labelled( name );

            continue;
        }

        // Neighbouring pins of one part on one edge (an IC's row of supply pins) are joined at
        // their stub ends and share one symbol, as drawn by hand; a pin alone gets its own.
        auto along = []( const STUB& aStub ) { return aStub.delta.x == 0 ? aStub.end.x : aStub.end.y; };
        auto across = []( const STUB& aStub ) { return aStub.delta.x == 0 ? aStub.end.y : aStub.end.x; };
        std::vector<bool> used( stubs.size(), false );

        for( size_t i = 0; i < stubs.size(); ++i )
        {
            if( used[i] )
                continue;

            const SYMBOL* parent = stubs[i].pin->GetParentSymbol();
            std::vector<size_t> line;

            for( size_t j = i; j < stubs.size(); ++j )
                if( !used[j] && stubs[j].pin->GetParentSymbol() == parent && stubs[j].delta == stubs[i].delta
                    && across( stubs[j] ) == across( stubs[i] ) )
                    line.push_back( j );

            std::sort( line.begin(), line.end(), [&]( size_t a, size_t b ) { return along( stubs[a] ) < along( stubs[b] ); } );

            // Joined only across a gap of at most four grid steps with no other pin of the part
            // ending in it.
            auto joins = [&]( const STUB& aFrom, const STUB& aTo )
            {
                if( along( aTo ) - along( aFrom ) > SCH_GRID_IU * 4 )
                    return false;

                for( const SCH_PIN* other : static_cast<const SCH_SYMBOL*>( parent )->GetPins( nullptr ) )
                {
                    const VECTOR2I delta = StubDelta( other );
                    const STUB     probe{ other, other->GetPosition(), other->GetPosition() + delta, delta };

                    if( delta != aFrom.delta || across( probe ) != across( aFrom ) )
                        continue;

                    auto it = netOf.find( other );

                    if( along( probe ) > along( aFrom ) && along( probe ) < along( aTo )
                        && ( it == netOf.end() || it->second != name ) )
                        return false;
                }

                return true;
            };

            std::vector<std::vector<size_t>> runs;

            for( size_t k : line )
            {
                if( runs.empty() || !joins( stubs[runs.back().back()], stubs[k] ) )
                    runs.emplace_back();

                runs.back().push_back( k );
                used[k] = true;
            }

            for( const std::vector<size_t>& run : runs )
            {
                for( size_t k = 0; k + 1 < run.size(); ++k )
                    wire( stubs[run[k]].end, stubs[run[k + 1]].end, name );

                const size_t hub = run[run.size() / 2];

                if( std::unique_ptr<SCH_SYMBOL> symbol = aPower->make( name, stubs[hub].end, stubs[hub].delta ) )
                    aItems.push_back( std::move( symbol ) );

                // A dot wherever three or more meet: a stub, the joining wires, the symbol.
                for( size_t k = 0; k < run.size(); ++k )
                {
                    const int meeting = 1 + ( k > 0 ) + ( k + 1 < run.size() ) + ( run[k] == hub );

                    if( meeting >= 3 )
                        aItems.push_back( std::make_unique<SCH_JUNCTION>( stubs[run[k]].end ) );
                }
            }
        }
    }
}


/// Rail names an engineer draws with a supply symbol: +5V, +3.3V, 3V3, V3V3, V5, VCC, VBUS, VDD_IO.
static bool SupplyName( const wxString& aNet )
{
    static const wxRegEx volts( wxS( "^\\+?(V?[0-9]+(\\.[0-9]+)?V[0-9]*|V[0-9]+)$" ) );
    static const wxRegEx named( wxS( "^\\+?(VCC|VDD|VBUS|VBAT|VSYS|VIN|VMAIN|VSUP|VDDA|AVDD|VCCA|VDDIO)(_[A-Z0-9_.]+)?$" ) );
    const wxString       name = aNet.Upper();
    return volts.Matches( name ) || named.Matches( name );
}


/// The power style for a sheet: what it already draws with labels or power symbols, and the
/// next free #PWR references across the schematic.
static POWER_STYLE MakePowerStyle( SCHEMATIC* aSchematic, const SCH_SHEET_PATH& aSheet )
{
    struct STATE
    {
        std::set<wxString> labels, powers, refs;
        int                next = 1;
    };

    auto state = std::make_shared<STATE>();

    for( const SCH_SHEET_PATH& path : aSchematic->BuildSheetListSortedByPageNumbers() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items() )
        {
            if( auto* label = dynamic_cast<SCH_LABEL_BASE*>( item ) )
            {
                state->labels.insert( label->GetText() );
            }
            else if( item->Type() == SCH_SYMBOL_T )
            {
                auto* symbol = static_cast<SCH_SYMBOL*>( item );
                state->refs.insert( symbol->GetRef( &path, false ) );

                if( symbol->IsPower() )
                    state->powers.insert( symbol->GetValue( false, &path, false ) );
            }
        }
    }

    auto library = []( const wxString& aNet, LIB_ID& aId ) -> LIB_SYMBOL*
    {
        const bool            ground = INDUCTIVE_CHECKS::detail::IsGround( aNet );
        std::vector<wxString> names = ground ? std::vector<wxString>{ wxS( "power:GND" ) }
                                             : std::vector<wxString>{ wxS( "power:" ) + aNet, wxS( "power:VCC" ) };

        for( const wxString& name : names )
        {
            try
            {
                LIB_SYMBOL* symbol = LoadDesignSymbol( name, aId );

                if( symbol && symbol->IsPower() && symbol->GetPinCount() == 1 )
                    return symbol;
            }
            catch( const ARGS::BAD_ARGUMENT& )
            {
            }
        }

        return nullptr;
    };

    POWER_STYLE style;

    style.wants = [state, library]( const wxString& aNet, bool aPowered )
    {
        if( state->labels.count( aNet ) )
            return false;

        if( state->powers.count( aNet ) )
            return true;

        LIB_ID id;
        return aPowered && ( INDUCTIVE_CHECKS::detail::IsGround( aNet ) || SupplyName( aNet ) ) && library( aNet, id );
    };

    style.labelled = [state]( const wxString& aNet )
    {
        state->labels.insert( aNet );
    };

    style.make = [state, library, aSheet]( const wxString& aNet, const VECTOR2I& aAt,
                                           const VECTOR2I& aAway ) -> std::unique_ptr<SCH_SYMBOL>
    {
        LIB_ID      id;
        LIB_SYMBOL* lib = library( aNet, id );

        if( !lib )
            return nullptr;

        auto symbol = std::make_unique<SCH_SYMBOL>( *lib, id, &aSheet, 1, 0, aAt );
        wxString ref;

        do
        {
            ref = wxString::Format( wxS( "#PWR%03d" ), state->next++ );
        } while( state->refs.count( ref ) );

        state->refs.insert( ref );
        state->powers.insert( aNet );
        symbol->SetRef( &aSheet, ref );
        symbol->SetValueFieldText( aNet );

        // Turned to point away from the pin it hangs on, its own pin on the stub's end.
        int    best = SYM_ORIENT_0;
        double reach = -1e18;

        for( int orientation : { SYM_ORIENT_0, SYM_ORIENT_90, SYM_ORIENT_180, SYM_ORIENT_270 } )
        {
            symbol->SetOrientation( orientation );
            symbol->Move( aAt - symbol->GetPins( nullptr ).front()->GetPosition() );
            const VECTOR2I out = symbol->GetBodyBoundingBox().Centre() - aAt;
            const double   along = (double) out.x * aAway.x + (double) out.y * aAway.y;

            if( along > reach )
            {
                reach = along;
                best = orientation;
            }
        }

        symbol->SetOrientation( best );
        symbol->Move( aAt - symbol->GetPins( nullptr ).front()->GetPosition() );

        // The ground symbol says what it is; its name only where there is more than one ground.
        SCH_FIELD* value = symbol->GetField( FIELD_T::VALUE );

        if( aNet.Upper() == wxS( "GND" ) )
            value->SetVisible( false );

        // The name runs on past the symbol along the stub, as a label does, so a row of pins
        // on an IC's top edge does not have names written across each other. Centred, then
        // measured, so KiCad's flipping of justification on turned symbols does not matter;
        // on a symbol turned 90 or 270 a stored angle displays the other way.
        if( value->IsVisible() )
        {
            const bool vertical = aAway.x == 0;
            const bool turned = symbol->GetTransform().y1 != 0;
            value->SetTextAngle( vertical != turned ? ANGLE_VERTICAL : ANGLE_HORIZONTAL );
            value->SetHorizJustify( GR_TEXT_H_ALIGN_CENTER );
            value->SetVertJustify( GR_TEXT_V_ALIGN_CENTER );
            value->SetPosition( aAt );

            const BOX2I text = value->GetBoundingBox();
            const int   half = ( vertical ? text.GetHeight() : text.GetWidth() ) / 2;
            const BOX2I body = symbol->GetBodyBoundingBox();
            const int   gap = schIUScale.MilsToIU( 25 );
            VECTOR2I    at = aAt;

            if( aAway.y < 0 )      at.y = body.GetTop() - gap - half;
            else if( aAway.y > 0 ) at.y = body.GetBottom() + gap + half;
            else if( aAway.x < 0 ) at.x = body.GetLeft() - gap - half;
            else                   at.x = body.GetRight() + gap + half;

            // Beside the stub's line, as KiCad sets a label's text above its wire: centred on
            // it, a name leaned into the next pin's label 2.54 mm away.
            const int beside = schIUScale.MilsToIU( 8 );

            if( vertical )
                at.x = aAt.x - beside - text.GetWidth() / 2;
            else
                at.y = aAt.y - beside - text.GetHeight() / 2;

            value->SetPosition( at );
        }

        return symbol;
    };

    return style;
}

/// A symbol's footprint on the sheet including the wire stubs and labels the
/// builder will draw from its pins, plus a gap to its neighbours. Each label runs
/// out along its stub; given the nets, only connected pins get one, as long as its
/// net's name, else every pin one as long as the longest.
BOX2I LayoutBox( SCH_SYMBOL* aSymbol, int aLongestNet, const std::map<const SCH_PIN*, wxString>* aNetOf = nullptr )
{
    BOX2I box = aSymbol->GetBoundingBox();
    box.Merge( aSymbol->GetBodyAndPinsBoundingBox() );

    for( SCH_PIN* pin : aSymbol->GetPins( nullptr ) )
    {
        int chars = aLongestNet;

        if( aNetOf )
        {
            auto it = aNetOf->find( pin );

            if( it == aNetOf->end() )
                continue;

            chars = (int) it->second.length();
        }

        const VECTOR2I delta = StubDelta( pin );
        const VECTOR2I end = pin->GetPosition() + delta;
        const int      length = SCH_GRID_IU * ( 2 + chars * 3 / 4 );
        const VECTOR2I tip = end + VECTOR2I( ( delta.x > 0 ) - ( delta.x < 0 ), ( delta.y > 0 ) - ( delta.y < 0 ) ) * length;
        BOX2I          text( end, VECTOR2I( 0, 0 ) );
        text.Merge( tip );
        text.Inflate( SCH_GRID_IU );   // the text's height, across the stub
        box.Merge( text );
    }

    box.Inflate( SCH_GRID_IU * 2 );
    return box;
}


/// Each pin's net, from the plan; pins it does not name are absent.
std::map<const SCH_PIN*, wxString> PinNets( const json& aNets, const DESIGN_PINS& aPins, const PIN_NAMES& aNames )
{
    std::map<const SCH_PIN*, wxString> netOf;

    if( !aNets.is_array() )
        return netOf;

    for( const auto& net : aNets )
    {
        if( !net.contains( "name" ) || !net["name"].is_string() || !net.contains( "pins" ) || !net["pins"].is_array() )
            continue;

        const wxString name = wxString::FromUTF8( net["name"].get<std::string>() );

        for( const auto& member : net["pins"] )
        {
            if( !member.is_string() )
                continue;

            try
            {
                for( const wxString& key : ResolvePin( wxString::FromUTF8( member.get<std::string>() ), aPins, aNames ) )
                    for( const SCH_PIN* pin : aPins.at( key ) )
                        netOf[pin] = name;
            }
            catch( ... )
            {
                // A bad pin is the net stubs' to report.
            }
        }
    }

    return netOf;
}


/**
 * Lay symbols out as an engineer groups them: each IC or connector with the parts that
 * serve it underneath -- its decoupling capacitors, its crystal and load capacitors, its
 * pull-ups, an LED and the resistor driving it, a series resistor by the connector it
 * terminates -- and the groups in rows across the page, below whatever the sheet already
 * holds. Plan order is kept between groups. Models place symbols badly in two dimensions;
 * a plan that leaves positions out, or whose positions would collide, is laid out here
 * instead of failing. Rows stop above the drawing sheet's title block. A circuit that does
 * not fit an empty sheet gets a larger one (A3, A2, A1), as an engineer would choose.
 */
void AutoLayout( SCH_SCREEN* aScreen, const std::vector<SCH_SYMBOL*>& aSymbols, int aLongestNet,
                 const DESIGN_PINS& aPins, const std::map<const SCH_PIN*, wxString>& aNetOf )
{
    using namespace INDUCTIVE_CHECKS::detail;

    // ── the parts, their units and nets ──
    struct PART
    {
        wxString                                  ref;
        std::vector<SCH_SYMBOL*>                  units;
        std::map<wxString, std::vector<const SCH_PIN*>> nets;   // net -> this part's pins on it
        std::set<wxString>                        numbers;
        bool                                      anchor = false;
        PART*                                     group = nullptr;
    };

    std::map<const SCH_SYMBOL*, wxString> refOf;

    for( const auto& [key, pins] : aPins )
        for( const SCH_PIN* pin : pins )
            refOf[static_cast<const SCH_SYMBOL*>( pin->GetParentSymbol() )] = key.BeforeLast( '.' );

    std::vector<std::unique_ptr<PART>> parts;
    std::map<wxString, PART*>          byRef;
    std::map<const SCH_SYMBOL*, PART*> partOf;

    for( SCH_SYMBOL* symbol : aSymbols )
    {
        const wxString ref = refOf.count( symbol ) ? refOf[symbol] : wxString::Format( wxS( "#%zu" ), parts.size() );
        PART*&         part = byRef[ref];

        if( !part )
        {
            parts.push_back( std::make_unique<PART>() );
            part = parts.back().get();
            part->ref = ref;
        }

        part->units.push_back( symbol );
        partOf[symbol] = part;

        for( SCH_PIN* pin : symbol->GetPins( nullptr ) )
        {
            part->numbers.insert( pin->GetNumber() );

            if( auto it = aNetOf.find( pin ); it != aNetOf.end() )
                part->nets[it->second].push_back( pin );
        }
    }

    std::map<wxString, std::vector<PART*>> onNet;   // net -> parts with a pin on it

    for( auto& part : parts )
        for( const auto& [net, pins] : part->nets )
            onNet[net].push_back( part.get() );

    auto rail = [&]( const wxString& aNet )
    {
        if( IsGround( aNet ) )
            return false;

        if( RailVoltage( aNet ) > 0 || onNet[aNet].size() >= 8 )
            return true;

        static const wxRegEx supply( wxS( "^\\+?(VCC|VDD|VIN|VBAT|VSYS|VBUS|VMAIN|VSUP)" ) );

        if( supply.Matches( Base( aNet ) ) )
            return true;

        for( PART* part : onNet[aNet] )
            for( const SCH_PIN* pin : part->nets[aNet] )
                if( pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_IN || pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_OUT )
                    return true;

        return false;
    };
    auto signals = [&]( PART* aPart )
    {
        std::vector<wxString> nets;

        for( const auto& [net, pins] : aPart->nets )
            if( !IsGround( net ) && !rail( net ) )
                nets.push_back( net );

        return nets;
    };
    auto sourcing = []( const PART* aPart )
    {
        for( const auto& [net, pins] : aPart->nets )
            for( const SCH_PIN* pin : pins )
                if( pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_OUT )
                    return true;

        return false;
    };

    // Passives, crystals, diodes, transistors and switches serve a part; ICs, modules and
    // connectors are served.
    static const std::set<wxString> SERVING = { wxS( "R" ), wxS( "C" ), wxS( "L" ), wxS( "D" ), wxS( "LED" ),
                                                wxS( "FB" ), wxS( "Y" ), wxS( "X" ), wxS( "RV" ), wxS( "F" ),
                                                wxS( "Q" ), wxS( "SW" ), wxS( "TP" ), wxS( "JP" ) };

    std::vector<PART*> anchors, serving;

    for( auto& part : parts )
    {
        part->anchor = !SERVING.count( Prefix( part->ref ) ) || part->numbers.size() > 4;
        ( part->anchor ? anchors : serving ).push_back( part.get() );
    }

    // 1. On a signal net with an IC or connector: with the smallest of them, so a series
    //    resistor between a connector and an MCU sits by the connector.
    for( PART* part : serving )
    {
        for( const wxString& net : signals( part ) )
            for( PART* other : onNet[net] )
                if( other->anchor && ( !part->group || other->numbers.size() < part->group->numbers.size() ) )
                    part->group = other;
    }

    // 2. Chains -- an LED behind its resistor, a crystal's capacitor -- follow their partner.
    for( bool changed = true; changed; )
    {
        changed = false;

        for( PART* part : serving )
        {
            if( part->group )
                continue;

            for( const wxString& net : signals( part ) )
            {
                for( PART* other : onNet[net] )
                {
                    if( other != part && !other->anchor && other->group )
                    {
                        part->group = other->group;
                        changed = true;
                        break;
                    }
                }

                if( part->group )
                    break;
            }
        }
    }

    // 3. On rails only: bulk capacitors by the regulator (or else the connector) on their
    //    rail, decoupling shared out among the ICs by how many supply pins each has there.
    std::map<std::pair<PART*, wxString>, int> given;

    for( PART* part : serving )
    {
        if( part->group )
            continue;

        const wxString value = part->units[0]->GetField( FIELD_T::VALUE )->GetText();
        const bool     bulk = Prefix( part->ref ) != wxS( "C" ) || Farads( value ) >= 4.7e-6;
        double         best = -1e9;
        wxString       bestRail;

        for( const auto& [net, pins] : part->nets )
        {
            if( !rail( net ) )
                continue;

            for( PART* other : onNet[net] )
            {
                if( !other->anchor )
                    continue;

                const wxString prefix = Prefix( other->ref );
                const bool     connector = prefix == wxS( "J" ) || prefix == wxS( "P" ) || prefix == wxS( "CN" ) || prefix == wxS( "BT" );
                const double   pins = (double) other->nets[net].size();
                const double   score = bulk ? ( sourcing( other ) ? 1000 : connector ? 500 : 0 ) + pins
                                            : ( connector ? -500 : 0 ) + pins - 2.0 * given[{ other, net }];

                if( score > best )
                {
                    best = score;
                    part->group = other;
                    bestRail = net;
                }
            }
        }

        if( part->group )
            ++given[{ part->group, bestRail }];
    }

    // ── groups: the served part's units in a row, the parts serving it in rows beneath ──
    struct PLACED
    {
        SCH_SYMBOL* symbol;
        BOX2I       box;      // where its layout box is now
        VECTOR2I    offset;   // where that box goes, from the group's top left
    };
    struct GROUP
    {
        std::vector<PLACED> symbols;
        VECTOR2I            size;
    };

    auto natural = []( const PART* a, const PART* b )
    {
        static const std::vector<wxString> ORDER = { wxS( "Y" ), wxS( "X" ), wxS( "C" ), wxS( "R" ), wxS( "L" ), wxS( "FB" ) };
        auto rank = [&]( const PART* p )
        {
            auto it = std::find( ORDER.begin(), ORDER.end(), Prefix( p->ref ) );
            return (int) ( it - ORDER.begin() );
        };
        if( rank( a ) != rank( b ) )
            return rank( a ) < rank( b );
        if( Prefix( a->ref ) != Prefix( b->ref ) )
            return Prefix( a->ref ) < Prefix( b->ref );
        long na = 0, nb = 0;
        a->ref.Mid( Prefix( a->ref ).length() ).ToLong( &na );
        b->ref.Mid( Prefix( b->ref ).length() ).ToLong( &nb );
        return na < nb;
    };

    auto makeGroup = [&]( std::vector<SCH_SYMBOL*> aHead, std::vector<PART*> aMembers )
    {
        GROUP group;
        int   x = 0, y = 0, row = 0, width = 0;

        for( SCH_SYMBOL* symbol : aHead )
        {
            const BOX2I box = LayoutBox( symbol, aLongestNet, &aNetOf );
            group.symbols.push_back( { symbol, box, VECTOR2I( x, 0 ) } );
            x += box.GetWidth();
            row = std::max( row, (int) box.GetHeight() );
        }

        width = std::max( x, ToIU( 60 ) );
        y = row;
        x = 0;
        row = 0;
        std::sort( aMembers.begin(), aMembers.end(), natural );

        for( PART* member : aMembers )
        {
            for( SCH_SYMBOL* symbol : member->units )
            {
                const BOX2I box = LayoutBox( symbol, aLongestNet, &aNetOf );

                if( x > 0 && x + box.GetWidth() > width )
                {
                    x = 0;
                    y += row;
                    row = 0;
                }

                group.symbols.push_back( { symbol, box, VECTOR2I( x, y ) } );
                x += box.GetWidth();
                row = std::max( row, (int) box.GetHeight() );
            }
        }

        int right = 0;

        for( const PLACED& placed : group.symbols )
            right = std::max( right, placed.offset.x + (int) placed.box.GetWidth() );

        group.size = VECTOR2I( right, y + row );
        return group;
    };

    std::vector<GROUP> groups;
    std::vector<PART*> loose;

    for( PART* anchor : anchors )
    {
        std::vector<PART*> members;

        for( PART* part : serving )
            if( part->group == anchor )
                members.push_back( part );

        groups.push_back( makeGroup( anchor->units, members ) );
    }

    for( PART* part : serving )
        if( !part->group )
            loose.push_back( part );

    if( !loose.empty() )
        groups.push_back( makeGroup( {}, loose ) );

    // ── the groups in rows across the page ──
    BOX2I existing;
    bool  any = false;
    for( SCH_ITEM* item : aScreen->Items() )
    {
        if( any ) existing.Merge( item->GetBoundingBox() );
        else { existing = item->GetBoundingBox(); any = true; }
    }

    const int margin = ToIU( 25.4 );
    const int titleBlock = ToIU( 45 );   // KiCad's default title block, bottom right

    // Lay out on a page, or only measure; true when everything ends above the title block.
    auto rows = [&]( const PAGE_INFO& aPage, bool aMove )
    {
        const int width = std::max( ToIU( 180 ), aPage.GetWidthIU( schIUScale.IU_PER_MILS ) - 2 * margin );
        const int bottom = aPage.GetHeightIU( schIUScale.IU_PER_MILS ) - margin - titleBlock;
        const int left = margin;
        int x = left, y = any ? existing.GetBottom() + ToIU( 15.24 ) : margin, row = 0;

        for( const GROUP& group : groups )
        {
            if( x > left && x + group.size.x > left + width )
            {
                x = left;
                y += row;
                row = 0;
            }

            if( aMove )
                for( const PLACED& placed : group.symbols )
                    placed.symbol->Move( SnapToGrid( VECTOR2I( x, y ) + placed.offset - placed.box.GetOrigin() ) );

            x += group.size.x + ToIU( 5.08 );
            row = std::max( row, group.size.y + ToIU( 5.08 ) );
        }

        return y + row <= bottom;
    };

    PAGE_INFO page = aScreen->GetPageSettings();

    if( !any && !rows( page, false ) )
    {
        for( PAGE_SIZE_TYPE size : { PAGE_SIZE_TYPE::A3, PAGE_SIZE_TYPE::A2, PAGE_SIZE_TYPE::A1 } )
        {
            PAGE_INFO bigger( size );
            if( bigger.GetWidthMils() <= page.GetWidthMils() )
                continue;
            page = bigger;
            if( rows( page, false ) )
                break;
        }
        aScreen->SetPageSettings( page );
    }

    rows( page, true );
}

/// A resistor value as written on a schematic -- 4k7, 13.5k, 100R, 0R1, 1M, 10 kOhm -- in
/// ohms; -1 when it is not one.
static double ParseOhms( const wxString& aValue )
{
    wxString v = aValue.Upper();
    v.Replace( wxS( " " ), wxS( "" ) );
    for( const wxString& unit : { wxString( wxS( "OHMS" ) ), wxString( wxS( "OHM" ) ), wxString( wxUniChar( 0x3A9 ) ),
                                  wxString( wxUniChar( 0x2126 ) ) } )
        v.Replace( unit, wxS( "" ) );
    static const wxRegEx plain( wxS( "^([0-9]+)(\\.[0-9]+)?([RKM]?)$" ) );
    static const wxRegEx infix( wxS( "^([0-9]+)([RKM])([0-9]+)$" ) );
    double whole = 0, frac = 0;
    wxString multiplier;
    if( plain.Matches( v ) )
    {
        plain.GetMatch( v, 1 ).ToCDouble( &whole );
        if( !plain.GetMatch( v, 2 ).IsEmpty() ) ( wxS( "0" ) + plain.GetMatch( v, 2 ) ).ToCDouble( &frac );
        multiplier = plain.GetMatch( v, 3 );
    }
    else if( infix.Matches( v ) )
    {
        infix.GetMatch( v, 1 ).ToCDouble( &whole );
        ( wxS( "0." ) + infix.GetMatch( v, 3 ) ).ToCDouble( &frac );
        multiplier = infix.GetMatch( v, 2 );
    }
    else
    {
        return -1;
    }
    const double scale = multiplier == "K" ? 1e3 : multiplier == "M" ? 1e6 : 1;
    return ( whole + frac ) * scale;
}

/**
 * A note when a resistor's value is in neither E24 nor E96: a 13.5k feedback resistor
 * cannot be bought, sourcing swaps it and the output moves. Names the nearest E96
 * values either side. Empty for standard values, zero ohms and anything unparsed.
 */
static wxString OffSeriesNote( const wxString& aRef, const wxString& aValue )
{
    static const double E24[] = { 1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                  3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1 };
    // IEC 60063.
    static const std::vector<double> e96 = {
        1.00, 1.02, 1.05, 1.07, 1.10, 1.13, 1.15, 1.18, 1.21, 1.24, 1.27, 1.30,
        1.33, 1.37, 1.40, 1.43, 1.47, 1.50, 1.54, 1.58, 1.62, 1.65, 1.69, 1.74,
        1.78, 1.82, 1.87, 1.91, 1.96, 2.00, 2.05, 2.10, 2.15, 2.21, 2.26, 2.32,
        2.37, 2.43, 2.49, 2.55, 2.61, 2.67, 2.74, 2.80, 2.87, 2.94, 3.01, 3.09,
        3.16, 3.24, 3.32, 3.40, 3.48, 3.57, 3.65, 3.74, 3.83, 3.92, 4.02, 4.12,
        4.22, 4.32, 4.42, 4.53, 4.64, 4.75, 4.87, 4.99, 5.11, 5.23, 5.36, 5.49,
        5.62, 5.76, 5.90, 6.04, 6.19, 6.34, 6.49, 6.65, 6.81, 6.98, 7.15, 7.32,
        7.50, 7.68, 7.87, 8.06, 8.25, 8.45, 8.66, 8.87, 9.09, 9.31, 9.53, 9.76 };
    const double ohms = ParseOhms( aValue );
    if( ohms <= 0 )
        return wxString();
    const double decade = std::pow( 10.0, std::floor( std::log10( ohms ) + 1e-9 ) );
    const double m = ohms / decade;
    auto closeTo = []( double a, double b ) { return std::abs( a - b ) <= 0.002 * b; };
    for( double e : E24 ) if( closeTo( m, e ) || closeTo( m, e * 10 ) ) return wxString();
    for( double e : e96 ) if( closeTo( m, e ) || closeTo( m, e * 10 ) ) return wxString();
    double below = e96.front(), above = 10.0;
    for( double e : e96 ) { if( e <= m ) below = e; if( e > m ) { above = e; break; } }
    auto show = []( double aOhms )
    {
        if( aOhms >= 1e6 ) return wxString::Format( wxS( "%gM" ), std::round( aOhms / 1e4 ) / 100 );
        if( aOhms >= 1e3 ) return wxString::Format( wxS( "%gk" ), std::round( aOhms / 10 ) / 100 );
        return wxString::Format( wxS( "%g" ), std::round( aOhms * 100 ) / 100 );
    };
    return wxString::Format( "Check %s: %s is not a standard resistor value (E24/E96); the nearest E96 values are "
                             "%s and %s. Pick one and recompute what depends on it.\n",
                             aRef, aValue, show( below * decade ), show( above * decade ) );
}

/// A part that brings a supply onto the board: a connector or a battery.
static bool IsSupplySource( SCH_SYMBOL* aSymbol )
{
    const LIB_ID& libId = aSymbol->GetLibId();
    return libId.GetUniStringLibNickname().StartsWith( "Connector" )
           || ( libId.GetUniStringLibNickname() == "Device" && libId.GetUniStringLibItemName().StartsWith( "Battery" ) );
}

/**
 * The nets a supply reaches from aFed, the nets a connector or battery is on.
 *
 * A supply behind a series part from a connector -- a barrel jack, then a reverse-
 * polarity diode, then the regulator's input -- is just as external: nothing on the
 * board drives it, and unflagged, ERC calls the regulator's input undriven (adding
 * a flag by hand afterwards failed six ways and used up a turn). Found through
 * diodes, fuses, ferrites, inductors, resistors, power switches (a coin cell, an
 * on/off slide switch, then the chip: the second time it cost a whole turn), jumpers
 * and MOSFETs, three deep, never past a net a power output drives.
 *
 * @param aKeysOf  each part's pin keys (U1.8).
 * @param aNetOfPin  the net of each connected pin key.
 */
static std::set<wxString> SpreadSupply( std::set<wxString> aFed, const std::map<SCH_SYMBOL*, std::vector<wxString>>& aKeysOf,
                                        const std::map<wxString, wxString>& aNetOfPin, const std::set<wxString>& aDriven,
                                        const DESIGN_PINS& aPins, const SCH_SHEET_PATH& aSheet )
{
    for( int hop = 0; hop < 3; ++hop )
    {
        std::set<wxString> reached = aFed;
        for( const auto& [symbol, keys] : aKeysOf )
        {
            wxString prefix;
            for( wxUniChar c : symbol->GetRef( &aSheet ) )
            {
                if( !wxIsalpha( c ) ) break;
                prefix += c;
            }
            const bool series = ( prefix == "D" || prefix == "F" || prefix == "FB" || prefix == "L" || prefix == "R" )
                                && !symbol->GetLibId().GetUniStringLibItemName().StartsWith( "LED" );
            // SPST or SPDT, or a solder jumper: whichever contact is fed reaches the others. A MOSFET in
            // the supply (reverse-polarity protection, a high-side switch) passes
            // it between drain and source, never to or from its gate.
            const bool toggle = ( prefix == "SW" || prefix == "JP" || symbol->GetLibId().GetUniStringLibNickname() == "Switch"
                                  || symbol->GetLibId().GetUniStringLibNickname() == "Jumper" )
                                && ( keys.size() == 2 || keys.size() == 3 );
            auto pinName = [&]( const wxString& aKey ) { return aPins.at( aKey ).front()->GetShownName().Upper(); };
            std::set<wxString> pinNames;
            for( const wxString& key : keys )
                pinNames.insert( pinName( key ) );
            const bool fet = prefix == "Q" && pinNames == std::set<wxString>{ "D", "G", "S" };
            if( !( series && keys.size() == 2 ) && !toggle && !fet )
                continue;
            auto conducts = [&]( const wxString& aKey ) { return !fet || pinName( aKey ) != "G"; };
            for( const wxString& fromKey : keys )
            {
                auto from = aNetOfPin.find( fromKey );
                if( !conducts( fromKey ) || from == aNetOfPin.end() || !aFed.count( from->second ) )
                    continue;
                for( const wxString& toKey : keys )
                {
                    auto to = aNetOfPin.find( toKey );
                    if( toKey != fromKey && conducts( toKey ) && to != aNetOfPin.end() && !aDriven.count( to->second ) )
                        reached.insert( to->second );
                }
            }
        }
        aFed = reached;
    }
    return aFed;
}

std::vector<std::unique_ptr<SCH_ITEM>> ConstructCircuit( SCHEMATIC* schematic, const json& aPlan, wxString& aResult,
                                                         const POWER_STYLE* aPower = nullptr )
{
    // Net names in KiCad's own spelling are made legal first, and the result says so.
    json     legal = aPlan;
    wxString renamed;
    if( legal.contains( "nets" ) && legal["nets"].is_array() )
    {
        for( auto& net : legal["nets"] )
        {
            if( !net.is_object() || !net.contains( "name" ) || !net["name"].is_string() )
                continue;
            const wxString was = wxString::FromUTF8( net["name"].get<std::string>() );
            const wxString now = LegalNetName( was );
            if( now != was )
            {
                net["name"] = now.ToStdString( wxConvUTF8 );
                renamed << ( renamed.IsEmpty() ? wxS( "" ) : wxS( ", " ) ) << was << wxS( " -> " ) << now;
            }
        }
    }
    const json& aArgs = legal;
    auto& sheet = schematic->CurrentSheet();
    auto* screen = sheet.LastScreen();
    if( !screen ) throw ARGS::BAD_ARGUMENT( "No sheet is open." );
    const auto& parts = aArgs.at( "components" );
    const auto& nets = aArgs.at( "nets" );
    // One style for a whole edit, so its power symbols' references do not repeat.
    const POWER_STYLE power = aPower ? *aPower : MakePowerStyle( schematic, sheet );
    // Sized for a real board in one call (a 64-LED driver is 148 parts); the per-turn
    // mutation budget counts calls, and the build is one undo step either way.
    if( !parts.is_array() || parts.empty() || parts.size() > 400 )
        throw ARGS::BAD_ARGUMENT( "components must contain 1 to 400 parts; build a larger design in parts with edit_circuit add." );
    std::set<wxString> references;
    for( const auto& path : schematic->Hierarchy() )
        for( auto* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            references.insert( static_cast<SCH_SYMBOL*>( item )->GetRef( &path, false ).Upper() );
    std::vector<std::unique_ptr<SCH_ITEM>> items;
    std::vector<SCH_SYMBOL*> placed;          // every unit, in plan order
    DESIGN_PINS pins;
    PIN_NAMES names;
    std::set<wxString> assigned;
    wxRegEx referencePattern( "^[A-Za-z]+[0-9]+$" );
    wxString summary;
    wxString mismatched;
    bool positioned = true;
    for( const auto& part : parts )
    {
        const wxString ref = ARGS::RequireString( part, "reference" );
        if( !referencePattern.Matches( ref ) || !references.insert( ref.Upper() ).second )
            throw ARGS::BAD_ARGUMENT( "Reference is invalid or already exists: " + ref );
        const wxString name = ARGS::RequireString( part, "lib_id" );
        LIB_ID id;
        auto* lib = LoadDesignSymbol( name, id );
        const bool hasPosition = part.contains( "x" ) && part.contains( "y" );
        positioned = positioned && hasPosition;
        VECTOR2I at = hasPosition ? SnapToGrid( VECTOR2I( ToIU( ARGS::RequireCoordMm( part, "x" ) ),
                                                          ToIU( ARGS::RequireCoordMm( part, "y" ) ) ) )
                                  : VECTOR2I( 0, 0 );
        auto value = ARGS::OptionalString( part, "value" );
        auto footprint = ARGS::OptionalString( part, "footprint" );
        const int units = std::max( 1, lib->GetUnitCount() );
        // Fixed resistors only: pots are sold as 5k and 50k.
        if( id.GetUniStringLibNickname() == "Device"
            && ( id.GetUniStringLibItemName() == "R" || id.GetUniStringLibItemName().StartsWith( "R_Small" )
                 || id.GetUniStringLibItemName().StartsWith( "R_US" ) ) )
            mismatched << OffSeriesNote( ref, value );
        if( !value.IsEmpty() && NamesAnotherPart( id, value ) )
            mismatched << wxString::Format( "Check %s: its value %s names a different part than its symbol %s "
                                            "(whose footprint it gets), and the BOM would list %s. Set the "
                                            "value to the part used, or tell the user what was substituted.\n",
                                            ref, value, name, value );

        // Every unit of a package: an op-amp's A and B halves and its supply unit
        // are all needed for a complete, ERC-clean part. Later units sit to the
        // right of the first until layout decides otherwise.
        for( int unit = 1; unit <= units; ++unit )
        {
            // Body style 1: a De Morgan alternate would draw every pin twice.
            auto symbol = std::make_unique<SCH_SYMBOL>( *lib, id, &sheet, unit, 1, at );
            symbol->SetRef( &sheet, ref );
            symbol->SetUnitSelection( &sheet, unit );
            if( !value.IsEmpty() ) symbol->SetValueFieldText( value );
            if( unit == 1 && footprint.IsEmpty() ) footprint = DefaultFootprint( symbol.get(), sheet, name );
            if( footprint.IsEmpty() )
                throw ARGS::BAD_ARGUMENT( "Choose a footprint for " + ref + " using search_footprints." );
            footprint = FitFootprint( symbol.get(), sheet, footprint, mismatched );
            symbol->SetFootprintFieldText( footprint );
            if( unit > 1 && !placed.empty() )
            {
                const BOX2I previous = placed.back()->GetBodyAndPinsBoundingBox();
                symbol->Move( SnapToGrid( VECTOR2I( previous.GetRight() - symbol->GetBodyAndPinsBoundingBox().GetLeft()
                                                    + ToIU( 15.24 ), 0 ) ) );
            }
            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                const wxString key = ref + "." + pin->GetNumber();
                auto& copies = pins[key];
                if( copies.empty() )
                    IndexPinName( names, ref, pin->GetShownName(), key );
                copies.push_back( pin );
            }
            placed.push_back( symbol.get() );
            items.push_back( std::move( symbol ) );
        }
        summary << ref << " " << footprint << ( units > 1 ? wxString::Format( " (%d units)", units ) : wxString() ) << "\n";
    }

    // Positions the plan gave are kept when they are clear of each other and of
    // the existing sheet; otherwise every new symbol is laid out.
    int longestNet = 0;
    if( nets.is_array() )
        for( const auto& net : nets )
            if( net.contains( "name" ) && net["name"].is_string() )
                longestNet = std::max( longestNet, (int) net["name"].get<std::string>().size() );
    bool layout = !positioned;
    for( size_t i = 0; i < placed.size() && !layout; ++i )
    {
        const BOX2I a = LayoutBox( placed[i], longestNet );
        for( size_t j = i + 1; j < placed.size() && !layout; ++j )
            layout = a.Intersects( placed[j]->GetBodyAndPinsBoundingBox() );
        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
            layout = layout || a.Intersects( item->GetBoundingBox() );
    }
    const std::map<const SCH_PIN*, wxString> netOf = PinNets( nets, pins, names );
    if( layout )
        AutoLayout( screen, placed, longestNet, pins, netOf );

    // Positions the plan gave can still put two nets' stubs on top of each other: lay
    // the symbols out instead of refusing (refused twice in a row, a build cost two
    // rounds), and say so.
    const size_t             itemsBefore = items.size();
    const std::set<wxString> assignedBefore = assigned;
    try
    {
        AddNetStubs( nets, pins, names, assigned, items, &power );
    }
    catch( const ARGS::BAD_ARGUMENT& e )
    {
        if( layout || !wxString::FromUTF8( e.what() ).StartsWith( "Wire branches would short" ) )
            throw;
        items.resize( itemsBefore );
        assigned = assignedBefore;
        AutoLayout( screen, placed, longestNet, pins, netOf );
        layout = true;
        AddNetStubs( nets, pins, names, assigned, items, &power );
    }
    if( aArgs.contains( "no_connect" ) && !aArgs.at( "no_connect" ).empty() )
    {
        for( const wxString& member : ARGS::RequireStringArray( aArgs, "no_connect", 256 ) )
        {
            for( const wxString& key : ResolvePin( member, pins, names ) )
            {
                if( !assigned.insert( key ).second )
                    throw ARGS::BAD_ARGUMENT( "Invalid, duplicate or already connected no_connect pin: " + key );
                for( SCH_PIN* pin : pins.at( key ) )
                    items.push_back( std::make_unique<SCH_NO_CONNECT>( pin->GetPosition() ) );
            }
        }
    }
    // A pin the library itself marks no-connect needs no instruction: flag it.
    for( const auto& [key, copies] : pins )
    {
        if( assigned.count( key ) || copies.front()->GetType() != ELECTRICAL_PINTYPE::PT_NC )
            continue;
        assigned.insert( key );
        for( SCH_PIN* pin : copies )
            items.push_back( std::make_unique<SCH_NO_CONNECT>( pin->GetPosition() ) );
    }
    for( const auto& [key, copies] : pins )
        if( !assigned.count( key ) )
            throw ARGS::BAD_ARGUMENT( "Pin " + key + " (" + copies.front()->GetShownName()
                                     + ") has no net. Connect it or explicitly mark no_connect." );
    // A flag is only legal on an external supply: one fed through a connector or
    // a battery, never one that would silence arbitrary ERC. A net with such a
    // source, power inputs and nothing driving it is that supply whether or not
    // the plan said so (a header feeding a regulator's input), and is flagged too.
    auto isSource = IsSupplySource;
    // A supply behind a series part from a connector is just as external (SpreadSupply).
    std::set<wxString> fed;
    {
        std::map<wxString, wxString> netOfPin;
        std::set<wxString>           driven;
        for( const auto& net : nets )
        {
            const wxString name = ARGS::RequireString( net, "name" );
            bool source = false;
            for( const wxString& member : ARGS::RequireStringArray( net, "pins", 256 ) )
            {
                for( const wxString& key : ResolvePin( member, pins, names ) )
                {
                    netOfPin[key] = name;
                    SCH_PIN* pin = pins.at( key ).front();
                    source |= isSource( static_cast<SCH_SYMBOL*>( pin->GetParentSymbol() ) );
                    if( pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_OUT )
                        driven.insert( name );
                }
            }
            if( source && !driven.count( name ) )
                fed.insert( name );
        }
        std::map<SCH_SYMBOL*, std::vector<wxString>> keysOf;
        for( const auto& [key, copies] : pins )
            keysOf[static_cast<SCH_SYMBOL*>( copies.front()->GetParentSymbol() )].push_back( key );
        fed = SpreadSupply( fed, keysOf, netOfPin, driven, pins, sheet );
    }
    int flags = 0;
    wxArrayString inferred;
    for( const auto& net : nets )
    {
        bool external = ARGS::OptionalBool( net, "externally_powered", false );
        bool internal = ARGS::OptionalBool( net, "internal_regulator", false );
        bool connector = fed.count( ARGS::RequireString( net, "name" ) ) > 0, output = false, powerIn = false;
        std::set<wxString> chips; // non-passive, non-connector parts on the net
        for( const wxString& member : ARGS::RequireStringArray( net, "pins", 256 ) )
        {
            for( const wxString& key : ResolvePin( member, pins, names ) )
            {
                auto* pin = pins.at( key ).front();
                auto* symbol = static_cast<SCH_SYMBOL*>( pin->GetParentSymbol() );
                bool isConnector = isSource( symbol );
                connector |= isConnector;
                output |= pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_OUT;
                powerIn |= pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_IN;
                bool passive = true;
                for( SCH_PIN* other : symbol->GetPins( &sheet ) )
                    passive &= other->GetType() == ELECTRICAL_PINTYPE::PT_PASSIVE;
                if( !passive && !isConnector )
                    chips.insert( symbol->GetRef( &sheet ) );
            }
        }
        if( !external && !internal )
        {
            if( !connector || output || !powerIn ) continue;
            external = true;
            inferred.Add( ARGS::RequireString( net, "name" ) );
        }
        if( external && ( !connector || output ) )
            throw ARGS::BAD_ARGUMENT( "externally_powered needs a connector or battery (on the net, or behind a series "
                                     "diode, fuse, ferrite, inductor, resistor, switch or MOSFET) and no internal power output: "
                                     + ARGS::RequireString( net, "name" ) );
        // A chip's own regulator or charge-pump output that its library marks
        // as a power input (DRV8833 VINT, many VCAP pins): only that chip and
        // its capacitors may share the net, so a missing supply is not hidden.
        if( internal && ( external || connector || output || !powerIn || chips.size() != 1 ) )
            throw ARGS::BAD_ARGUMENT( "internal_regulator is only for one chip's own regulator output pin "
                                     "marked power input, with just its capacitors on the net: "
                                     + ARGS::RequireString( net, "name" ) );
        LIB_ID id;
        auto* lib = LoadDesignSymbol( "power:PWR_FLAG", id );
        // Give the flag its own labelled branch, away from symbol pin bodies.
        VECTOR2I at( ToIU( 40 + flags * 20 ), ToIU( 25 ) );
        if( layout )
        {
            BOX2I all = placed.front()->GetBoundingBox();
            for( SCH_SYMBOL* symbol : placed ) all.Merge( symbol->GetBoundingBox() );
            at = VECTOR2I( all.GetLeft() + ToIU( flags * 20 ), all.GetTop() - ToIU( 12.7 ) );
        }
        auto flag = std::make_unique<SCH_SYMBOL>( *lib, id, &sheet, 1, 0, SnapToGrid( at ) );
        wxString ref;
        do { ref = wxString::Format( "#FLG%03d", ++flags ); }
        while( references.count( ref ) );
        flag->SetRef( &sheet, ref );
        const wxString flagged = ARGS::RequireString( net, "name" );
        const VECTOR2I flagPin = flag->GetPins( &sheet ).front()->GetPosition();
        items.push_back( std::move( flag ) );
        // On a net drawn with power symbols, the flag stands on one of its own; else a label.
        if( std::unique_ptr<SCH_SYMBOL> symbol = power.wants( flagged, true ) ? power.make( flagged, flagPin, VECTOR2I( 0, 1 ) ) : nullptr )
        {
            items.push_back( std::move( symbol ) );
        }
        else
        {
            auto label = std::make_unique<SCH_LABEL>( flagPin, flagged );
            label->SetTextSize( VECTOR2I( SCH_GRID_IU, SCH_GRID_IU ) );
            items.push_back( std::move( label ) );
        }
    }
    aResult = wxString::Format( "Created %zu components and %zu nets; footprints verified%s. Run ERC next.\n",
                               parts.size(), nets.size(), layout ? "; symbols laid out automatically" : "" )
              + ( inferred.IsEmpty() ? wxString()
                                     : "Supplied from a connector or battery, so flagged as external supplies: "
                                       + wxJoin( inferred, ',' ) + ".\n" )
              + mismatched + summary;
    if( !renamed.IsEmpty() )
        aResult << "Net names made legal: " << renamed << ".\n";
    return items;
}

/**
 * Currents stated for nets while designing, recorded beside the project in
 * <project>.inductive-currents.json for the PCB editor, which sizes those nets'
 * tracks (IPC-2221) before it routes. /layout has no model in the loop: a user's
 * "up to 3 A" never reached the board, and the load ran on 0.5 mm necked to 0.2 mm.
 * Merged into what the file already holds. Returns a sentence for the tool result.
 */
static wxString RecordNetCurrents( const PROJECT& aProject, const std::map<wxString, double>& aAmps )
{
    if( aAmps.empty() )
        return wxString();
    if( aProject.GetProjectFullName().IsEmpty() )
        return " Currents not recorded for the PCB: save the project first.";
    wxFileName file( aProject.GetProjectFullName() );
    file.SetFullName( file.GetName() + wxS( ".inductive-currents.json" ) );
    json doc = json::object();
    if( file.FileExists() )   // a missing file opened makes wxWidgets put up an error dialog
    {
        wxFFile in( file.GetFullPath(), "r" );
        wxString text;
        if( in.IsOpened() && in.ReadAll( &text ) )
            doc = json::parse( text.ToStdString( wxConvUTF8 ), nullptr, false );
        if( !doc.is_object() )
            doc = json::object();
    }
    if( !doc.contains( "nets" ) || !doc["nets"].is_object() )
        doc["nets"] = json::object();
    doc["version"] = 1;
    wxString said;
    for( const auto& [net, amps] : aAmps )
    {
        doc["nets"][net.ToStdString( wxConvUTF8 )] = amps;
        said << ( said.IsEmpty() ? wxS( "" ) : wxS( ", " ) ) << net << wxString::Format( wxS( " %g A" ), amps );
    }
    const wxString temp = file.GetFullPath() + wxS( ".tmp" );
    {
        wxFFile out( temp, "w" );
        if( !out.IsOpened() || !out.Write( wxString::FromUTF8( doc.dump( 2 ) + "\n" ) ) )
            return " Currents not recorded for the PCB: the project folder is not writable.";
    }
    if( !wxRenameFile( temp, file.GetFullPath(), true ) )
        return " Currents not recorded for the PCB: the project folder is not writable.";
    return " Currents recorded for the PCB, which sizes these tracks when it routes: " + said + ".";
}

/// The currents a plan's nets state (current_a), validated.
static std::map<wxString, double> PlanCurrents( const json& aNets )
{
    std::map<wxString, double> amps;
    if( !aNets.is_array() )
        return amps;
    for( const auto& net : aNets )
        if( net.is_object() && net.contains( "current_a" ) )
            amps[LegalNetName( ARGS::RequireString( net, "name" ) )] = ARGS::RequireNumber( net, "current_a", 0.01, 50 );
    return amps;
}

/**
 * Wires shorter than 0.1 mm with an end on nothing, removed after a change to the
 * sheet: repeated edits in design 17 left four 0.04 mm stubs, each an ERC warning.
 * One end on nothing means removing it changes no connection. Appended to the undo
 * step of the change that left them. Returns a sentence for the result, or nothing.
 */
static wxString RemoveTinyDanglingWires( SCH_EDIT_FRAME* aFrame )
{
    SCH_SHEET_PATH& path = aFrame->GetCurrentSheet();
    SCH_SCREEN*     screen = path.LastScreen();
    if( !screen )
        return wxString();
    screen->TestDanglingEnds( &path );
    std::vector<SCH_LINE*> tiny;
    for( SCH_ITEM* item : screen->Items().OfType( SCH_LINE_T ) )
    {
        auto* line = static_cast<SCH_LINE*>( item );
        if( line->IsWire() && line->GetLength() < schIUScale.mmToIU( 0.1 )
            && ( line->IsStartDangling() || line->IsEndDangling() ) )
            tiny.push_back( line );
    }
    if( tiny.empty() )
        return wxString();
    SCH_COMMIT                         commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
    for( SCH_LINE* line : tiny )
        commit.Remove( line, screen );
    commit.Push( _( "Remove Wire Stubs" ), APPEND_UNDO );
    guard.Pushed();
    return wxString::Format( " Removed %zu wire stub(s) shorter than 0.1 mm that ended on nothing.", tiny.size() );
}

bool BuildCircuit( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult )
{
    auto* schematic = RequireSchematic( aFrame );
    const auto amps = PlanCurrents( aArgs.contains( "nets" ) ? aArgs.at( "nets" ) : json() );
    auto items = ConstructCircuit( schematic, aArgs, aResult );
    SCH_COMMIT commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
    for( auto& item : items ) commit.Add( item.release(), schematic->CurrentSheet().LastScreen() );
    commit.Push( _( "Build Circuit" ) );
    guard.Pushed();
    aResult << RecordNetCurrents( aFrame->Prj(), amps );

    // What an engineer would catch, in the same result, so the turn that built it fixes it.
    aFrame->RecalculateConnections( nullptr, NO_CLEANUP );
    aResult << INDUCTIVE_CHECKS::Report( INDUCTIVE_CHECKS::Collect( schematic ) );
    aFrame->GetCanvas()->Refresh();
    aFrame->GetToolManager()->PostAction( ACTIONS::zoomFitScreen );
    return true;
}


wxString SchematicNetlist( SCH_EDIT_FRAME* aFrame )
{
    auto* schematic = RequireSchematic( aFrame );
    aFrame->RecalculateConnections( nullptr, NO_CLEANUP );
    NETLIST_EXPORTER_KICAD exporter( schematic );
    STRING_FORMATTER output;
    exporter.Format( &output, GNL_ALL | GNL_OPT_KICAD );
    return wxString::FromUTF8( output.GetString() );
}

bool PreparePCB( SCH_EDIT_FRAME* aFrame, wxString& aResult )
{
    auto* schematic = RequireSchematic( aFrame );
    wxFileName file( schematic->RootScreen()->GetFileName() );
    if( !file.IsAbsolute() || !file.FileExists() )
        throw ARGS::BAD_ARGUMENT( "Save the schematic in its project first, then prepare the PCB." );
    for( const auto& entry : AllSymbols( aFrame ) )
    {
        if( !InductiveSymbolOnBoard( entry.symbol ) ) continue;
        auto fp = entry.symbol->GetFootprintFieldText( true, &entry.path, false );
        if( fp.IsEmpty() ) throw ARGS::BAD_ARGUMENT( "Assign a footprint to " + entry.symbol->GetRef( &entry.path, false ) );
        auto sheet = entry.path;
        ValidateFootprint( entry.symbol, sheet, fp );
    }
    file.SetExt( "inductive.net" );
    wxString text = SchematicNetlist( aFrame );
    wxFFile output( file.GetFullPath() + ".tmp", "w" );
    if( !output.IsOpened() || !output.Write( text ) || !output.Close()
        || !wxRenameFile( file.GetFullPath() + ".tmp", file.GetFullPath(), true ) )
        throw ARGS::BAD_ARGUMENT( "Could not publish the schematic netlist." );
    aResult = "Published the current schematic and verified footprints. Next: /pcb opens its board (made "
              "if missing), and there /layout syncs, places, routes and checks it in one step.";
    return true;
}

/**
 * Open a board in the PCB editor. Inside the project manager that is KiCad's own
 * OnOpenPcbnew(). A standalone schematic editor is Inductive Schematic.app, and
 * KiCad's lookup there wants a "pcbnew" that this bundle names Inductive PCB.app:
 * open its sibling instead, which also hands the file to a copy already running.
 */
static bool LaunchPcbEditor( SCH_EDIT_FRAME* aFrame, const wxString& aBoard )
{
    if( !Kiface().IsSingle() )
    {
        aFrame->OnOpenPcbnew();
        return true;
    }

#ifdef __WXMAC__
    // .../Contents/Applications/Inductive Schematic.app/Contents/MacOS/<exe>
    wxFileName apps;
    apps.AssignDir( wxFileName( wxStandardPaths::Get().GetExecutablePath() ).GetPath() );
    apps.RemoveLastDir();
    apps.RemoveLastDir();
    apps.RemoveLastDir();
    const wxString pcb = apps.GetPathWithSep() + wxS( "Inductive PCB.app" );

    if( wxDirExists( pcb ) )
    {
        // A board already open has KiCad's lock file beside it. Handing it over again
        // makes the PCB editor warn "already open... interleaved saves", so just bring
        // the editor forward; it picks up the published netlist with /layout or /sync.
        wxFileName lock( aBoard );
        lock.SetName( FILEEXT::LockFilePrefix + lock.GetName() );
        lock.SetExt( lock.GetExt() + wxS( "." ) + FILEEXT::LockFileExtension );

        if( lock.FileExists() )
        {
            const wxChar* argv[] = { wxS( "/usr/bin/open" ), wxS( "-a" ), pcb.wc_str(), nullptr };
            return wxExecute( const_cast<wxChar**>( argv ), wxEXEC_ASYNC ) > 0;
        }

        const wxChar* argv[] = { wxS( "/usr/bin/open" ), wxS( "-a" ), pcb.wc_str(), aBoard.wc_str(), nullptr };
        return wxExecute( const_cast<wxChar**>( argv ), wxEXEC_ASYNC ) > 0;
    }
#elif defined( __WXMSW__ )
    // <install>\bin\inductive-schematic.exe, beside inductive-pcb.exe
    wxFileName pcb( wxStandardPaths::Get().GetExecutablePath() );
    pcb.SetName( wxS( "inductive-pcb" ) );

    if( pcb.FileExists() )
    {
        // Already open (KiCad's lock file is beside it): a second editor would warn about
        // interleaved saves, and the open one picks up the netlist with /layout or /sync.
        wxFileName lock( aBoard );
        lock.SetName( FILEEXT::LockFilePrefix + lock.GetName() );
        lock.SetExt( lock.GetExt() + wxS( "." ) + FILEEXT::LockFileExtension );

        if( lock.FileExists() )
            return true;

        const wxString path = pcb.GetFullPath();
        const wxChar*  argv[] = { path.wc_str(), aBoard.wc_str(), nullptr };
        return wxExecute( const_cast<wxChar**>( argv ), wxEXEC_ASYNC ) > 0;
    }
#endif

    return false;
}

/**
 * Publish the schematic, then open its board in the PCB editor. A project with no
 * board gets an empty two-layer one beside the schematic first, rather than the PCB
 * editor asking whether to create it; the placer draws its outline.
 */
bool OpenPCB( SCH_EDIT_FRAME* aFrame, wxString& aResult )
{
    if( !PreparePCB( aFrame, aResult ) )
        return false;

    // The board OnOpenPcbnew() opens: the project's, named like the schematic.
    wxFileName board( RequireSchematic( aFrame )->RootScreen()->GetFileName() );
    if( !aFrame->Prj().GetProjectFullName().IsEmpty() )
        board = wxFileName( aFrame->Prj().GetProjectFullName() );
    board.SetExt( FILEEXT::PcbFileExtension );
    wxFileName legacy( board );
    legacy.SetExt( FILEEXT::LegacyPcbFileExtension );

    bool created = false;
    if( !board.FileExists() && !legacy.FileExists() )
    {
        wxFFile out( board.GetFullPath(), "w" );
        if( !out.IsOpened() || !out.Write( ARGS::EMPTY_BOARD ) || !out.Close() )
            throw ARGS::BAD_ARGUMENT( "Could not create the board file " + board.GetFullPath() );
        created = true;
    }

    const bool opened = LaunchPcbEditor( aFrame, board.GetFullPath() );
    // prepare_pcb's own reply already says what /layout and /sync do there.
    aResult = wxString( "Published the current schematic and verified footprints. " )
              + ( created ? "Created an empty two-layer board " + board.GetFullName() + ". " : wxString() )
              + ( opened ? "Opened " + board.GetFullName() + " in the PCB editor." : "Open " + board.GetFullPath() + " in Inductive PCB." )
              + " There, /layout syncs, places, routes and runs DRC in one step; /sync only imports the parts.";
    return true;
}

// ─────────────────────────── the tool table ───────────────────────────

using HANDLER = std::function<bool( SCH_EDIT_FRAME*, const json&, wxString& )>;

struct ENTRY
{
    INDUCTIVE_TOOL decl;
    HANDLER        run;
};

/// find_items' filters, as find_items and the "where" of the edit tools read them.
static INDUCTIVE_SCH_ITEMS::QUERY ParseSchQuery( const json& aArgs )
{
    INDUCTIVE_SCH_ITEMS::QUERY query;

    if( ARGS::Has( aArgs, "kinds" ) )
        for( const wxString& kind : ARGS::RequireStringArray( aArgs, "kinds", 15 ) )
            query.kinds.push_back( kind.Lower() );

    query.sheet = ARGS::OptionalString( aArgs, "sheet", wxEmptyString );
    query.net = ARGS::OptionalString( aArgs, "net", wxEmptyString );
    query.reference = ARGS::OptionalString( aArgs, "reference", wxEmptyString );
    query.text = ARGS::OptionalString( aArgs, "text", wxEmptyString );

    if( ARGS::Has( aArgs, "area" ) )
    {
        const json& a = aArgs.at( "area" );
        BOX2I box( VECTOR2I( ToIU( ARGS::RequireCoordMm( a, "x0" ) ), ToIU( ARGS::RequireCoordMm( a, "y0" ) ) ) );
        box.Merge( VECTOR2I( ToIU( ARGS::RequireCoordMm( a, "x1" ) ), ToIU( ARGS::RequireCoordMm( a, "y1" ) ) ) );
        query.area = box;
    }

    return query;
}


/// An edit tool's items: its "ids", or every item its "where" matches (kinds required), up to aMax.
static std::vector<INDUCTIVE_SCH_ITEMS::PLACED> SchItemsToEdit( SCHEMATIC* aSchematic, const json& aArgs, size_t aMax )
{
    std::vector<INDUCTIVE_SCH_ITEMS::PLACED> items;
    const bool byIds = ARGS::Has( aArgs, "ids" );

    if( byIds == ARGS::Has( aArgs, "where" ) )
        throw ARGS::BAD_ARGUMENT( _( "Give either 'ids' (from find_items) or 'where' (find_items' filters)." ) );

    if( byIds )
    {
        for( const wxString& id : ARGS::RequireStringArray( aArgs, "ids", 200 ) )
        {
            wxString why;
            auto     placed = INDUCTIVE_SCH_ITEMS::Resolve( aSchematic, id, &why );

            if( !placed )
                throw ARGS::BAD_ARGUMENT( why );

            items.push_back( *placed );
        }

        return items;
    }

    const json& where = aArgs.at( "where" );

    if( !where.is_object() || !ARGS::Has( where, "kinds" ) )
        throw ARGS::BAD_ARGUMENT( _( "'where' needs at least 'kinds', e.g. {\"kinds\":[\"symbol\"],\"reference\":\"R1\"}." ) );

    size_t total = 0;
    items = INDUCTIVE_SCH_ITEMS::Find( aSchematic, ParseSchQuery( where ), aMax, &total );

    if( total > aMax )
        throw ARGS::BAD_ARGUMENT( wxString::Format( _( "'where' matches %d items, more than %d in one call; narrow it." ),
                                                    (int) total, (int) aMax ) );

    if( items.empty() )
        throw ARGS::BAD_ARGUMENT( _( "Nothing matches 'where'; find_items with the same filters shows what is there." ) );

    return items;
}




std::vector<ENTRY> BuildTools()
{
    std::vector<ENTRY> tools;

    tools.push_back(
            { { wxS( "get_reference_circuit" ), false },
              []( SCH_EDIT_FRAME*, const json& aArgs, wxString& aResult ) -> bool
              {
                  const auto name = ARGS::RequireString( aArgs, "design" );
                  if( name != "led" && name != "buck" && name != "motor" )
                      throw ARGS::BAD_ARGUMENT( "Unknown reference circuit." );
                  wxFFile file( INDUCTIVE_PANEL::ProjectDir() + "/examples/circuits/" + name + ".json", "r" );
                  if( !file.IsOpened() || !file.ReadAll( &aResult ) )
                      throw ARGS::BAD_ARGUMENT( "Reference circuit is not installed." );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "clear_schematic" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  auto* screen = RequireSchematic( aFrame )->GetCurrentScreen();
                  std::vector<SCH_ITEM*> items;
                  for( auto* item : screen->Items() ) items.push_back( item );
                  SCH_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  for( auto* item : items ) commit.Remove( item, screen );
                  commit.Push( _( "Clear Schematic" ) );
                  guard.Pushed();
                  aFrame->GetCanvas()->Refresh();
                  aResult = "Cleared the current sheet. Undo restores it.";
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_symbol_definition" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {

                  wxString out;
                  for( const wxString& name : ARGS::RequireStringArray( aArgs, "ids", 16 ) )
                  {
                      LIB_ID id;
                      auto* lib = LoadDesignSymbol( name, id );
                      auto flat = lib->Flatten();
                      auto fp = flat->GetFootprintField().GetText();
                      if( fp.IsEmpty() ) fp = StandardFootprint( name );
                      out << name << "  footprint: " << fp
                          << "  units: " << flat->GetUnitCount() << "\n";
                      if( fp.IsEmpty() )
                      {
                          const wxString suggested = SuggestedFootprints( flat->GetFPFilters() );
                          out << "  no default footprint; "
                              << ( suggested.IsEmpty() ? wxString( "choose one with search_footprints" )
                                                       : "fitting installed footprints: " + suggested )
                              << "\n";
                      }
                      for( auto* pin : flat->GetPins() )
                          out << "  pin " << pin->GetNumber() << " " << pin->GetName()
                              << " (" << pin->GetCanonicalElectricalTypeName() << ")\n";
                  }
                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "search_footprints" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {

                  auto& manager = Pgm().GetLibraryManager();
                  auto library = ARGS::OptionalString( aArgs, "library" );
                  // An empty query with a library lists it: how a model asks what one holds.
                  auto query = ( library.IsEmpty() ? ARGS::RequireString( aArgs, "query" )
                                                   : ARGS::OptionalString( aArgs, "query" ) ).Lower();
                  wxString out;
                  int hits = 0;
                  for( auto* row : manager.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
                  {
                      if( !library.IsEmpty() && row->Nickname() != library ) continue;
                      auto uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::FOOTPRINT, row->Nickname(), true );
                      if( !uri ) continue;
                      wxDir dir( *uri );
                      if( !dir.IsOpened() ) continue;
                      wxString name;
                      bool more = dir.GetFirst( &name, "*.kicad_mod", wxDIR_FILES );
                      while( more && hits < 60 )
                      {
                          if( name.Lower().Contains( query ) )
                          {
                              out << row->Nickname() << ":" << wxFileName( name ).GetName() << "\n";
                              ++hits;
                          }
                          more = dir.GetNext( &name );
                      }
                      if( hits >= 60 ) break;
                  }
                  aResult = wxString::Format( "%d installed footprint matches\n", hits ) + out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "build_circuit" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return BuildCircuit( aFrame, aArgs, aResult );
              } } );

    tools.push_back(
            { { wxS( "set_net_current" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC* schematic = RequireSchematic( aFrame );
                  const double amps = ARGS::RequireNumber( aArgs, "current_a", 0.01, 50 );
                  std::set<wxString> known;
                  aFrame->RecalculateConnections( nullptr, NO_CLEANUP );
                  for( const auto& [key, subgraphs] : schematic->ConnectionGraph()->GetNetMap() )
                      known.insert( key.Name.AfterLast( '/' ) );
                  std::map<wxString, double> record;
                  for( const wxString& net : ARGS::RequireStringArray( aArgs, "nets", 32 ) )
                  {
                      if( !known.count( net.AfterLast( '/' ) ) )
                          throw ARGS::BAD_ARGUMENT( "No net named " + net + " on the schematic." );
                      record[net.AfterLast( '/' )] = amps;
                  }
                  aResult = RecordNetCurrents( aFrame->Prj(), record ).Trim( false );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "connect_pins" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {

                  auto& sheet = aFrame->Schematic().CurrentSheet();
                  DESIGN_PINS pins;
                  PIN_NAMES   names;
                  for( auto* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                  {
                      auto* symbol = static_cast<SCH_SYMBOL*>( item );
                      for( auto* pin : symbol->GetPins( &sheet ) )
                      {
                          const wxString key = symbol->GetRef( &sheet, false ) + "." + pin->GetNumber();
                          if( pins[key].empty() )
                              IndexPinName( names, symbol->GetRef( &sheet, false ), pin->GetShownName(), key );
                          pins[key].push_back( pin );
                      }
                  }
                  aFrame->RecalculateConnections( nullptr, NO_CLEANUP );
                  for( const auto& net : aArgs.at( "nets" ) )
                  {
                      auto name = ARGS::RequireString( net, "name" );
                      for( const auto& member : ARGS::RequireStringArray( net, "pins", 256 ) )
                      for( const auto& key : ResolvePin( member, pins, names ) )
                      {
                          auto* connection = pins.at( key ).front()->Connection( &sheet );
                          auto* driver = connection ? connection->Driver() : nullptr;
                          if( driver && dynamic_cast<SCH_LABEL_BASE*>( driver )
                              && connection->Name( true ) != name )
                              throw ARGS::BAD_ARGUMENT( key + " already belongs to "
                                  + connection->Name( true ) + ". This tool cannot rename or disconnect existing nets." );
                      }
                  }
                  std::vector<std::unique_ptr<SCH_ITEM>> items;
                  std::set<wxString> assigned;
                  const POWER_STYLE power = MakePowerStyle( &aFrame->Schematic(), sheet );
                  AddNetStubs( aArgs.at( "nets" ), pins, names, assigned, items, &power );
                  SCH_COMMIT commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  for( auto& item : items ) commit.Add( item.release(), sheet.LastScreen() );
                  commit.Push( _( "Connect Circuit Pins" ) );
                  guard.Pushed();
                  aResult = "Connected pins with named nets. Run ERC to verify.";
                  return true;
              } } );

    tools.push_back(
            { { wxS( "prepare_pcb" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return PreparePCB( aFrame, aResult );
              } } );

    tools.push_back(
            { { wxS( "open_pcb" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  return OpenPCB( aFrame, aResult );
              } } );

    tools.push_back(
            { { wxS( "get_netlist" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  aResult = SchematicNetlist( aFrame );
                  return true;
              } } );


    tools.push_back(
            { { wxS( "get_selection" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  auto* selection = aFrame->GetToolManager()->GetTool<SCH_SELECTION_TOOL>();
                  if( !selection ) throw ARGS::BAD_ARGUMENT( _( "The selection is unavailable." ) );
                  std::vector<SCH_ITEM*> items;
                  for( EDA_ITEM* item : selection->GetSelection() )
                      if( auto* schItem = dynamic_cast<SCH_ITEM*>( item ) ) items.push_back( schItem );
                  aResult = wxString::FromUTF8( InductiveSchematicSelection( aFrame->Schematic(), items ).dump() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_schematic_overview" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  SCHEMATIC* schematic = RequireSchematic( aFrame );

                  const std::vector<SYMBOL_REF> symbols = AllSymbols( aFrame );

                  int noFootprint = 0;
                  int unannotated = 0;

                  for( const SYMBOL_REF& entry : symbols )
                  {
                      if( InductiveSymbolOnBoard( entry.symbol )
                          && entry.symbol->GetFootprintFieldText( true, &entry.path, false ).IsEmpty() )
                          ++noFootprint;

                      const wxString ref = entry.symbol->GetRef( &entry.path, false );

                      if( ref.IsEmpty() || ref.EndsWith( wxS( "?" ) ) )
                          ++unannotated;
                  }

                  wxString out;
                  out << wxString::Format( _( "%d symbols across %d sheet(s)\n" ),
                                           (int) symbols.size(),
                                           (int) schematic->Hierarchy().size() );

                  if( noFootprint )
                  {
                      // The single most common reason a schematic will not become
                      // a board, so it is worth saying before being asked.
                      out << wxString::Format(
                              _( "%d symbol(s) have no footprint set -- those cannot be placed on "
                                 "a board until they do.\n" ),
                              noFootprint );
                  }

                  if( unannotated )
                  {
                      out << wxString::Format( _( "%d symbol(s) are not annotated (still R?).\n" ),
                                               unannotated );
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "list_symbols" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  const wxString filter = ARGS::OptionalString( aArgs, "filter" ).Lower();

                  std::vector<SYMBOL_REF> matched;

                  for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
                  {
                      if( !filter.IsEmpty()
                          && !entry.symbol->GetRef( &entry.path, false ).Lower().Contains( filter )
                          && !entry.symbol->GetValue( true, &entry.path, false )
                                      .Lower()
                                      .Contains( filter ) )
                      {
                          continue;
                      }

                      matched.push_back( entry );
                  }

                  std::sort( matched.begin(), matched.end(),
                             []( const SYMBOL_REF& a, const SYMBOL_REF& b )
                             {
                                 return a.symbol->GetRef( &a.path, true ).CmpNoCase(
                                                b.symbol->GetRef( &b.path, true ) )
                                        < 0;
                             } );

                  if( matched.empty() )
                  {
                      aResult = filter.IsEmpty()
                                        ? _( "The schematic has no symbols." )
                                        : wxString::Format( _( "No symbol matches '%s'." ),
                                                            filter );
                      return true;
                  }

                  const bool multiSheet = aFrame->Schematic().Hierarchy().size() > 1;

                  wxString out = wxString::Format( _( "%d symbols\n" ), (int) matched.size() );

                  const size_t limit = 300;
                  size_t       shown = 0;

                  for( const SYMBOL_REF& entry : matched )
                  {
                      if( shown++ >= limit )
                      {
                          out << wxString::Format( _( "...and %d more\n" ),
                                                   (int) ( matched.size() - limit ) );
                          break;
                      }

                      out << wxS( "  " ) << DescribeSymbol( entry, multiSheet ) << wxS( "\n" );
                  }

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_symbol" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  int              units = 0;
                  const SYMBOL_REF entry =
                          RequireSymbol( aFrame, ARGS::RequireString( aArgs, "reference" ),
                                         &units );

                  wxString out;
                  out << DescribeSymbol( entry,
                                         aFrame->Schematic().Hierarchy().size() > 1 )
                      << wxS( "\n" );

                  if( units > 1 )
                  {
                      out << wxString::Format(
                              _( "This is a multi-unit package: %d units share this reference. "
                                 "Changing one unit does not change the others.\n" ),
                              units );
                  }

                  std::vector<SCH_PIN*> pins = entry.symbol->GetPins( &entry.path );

                  out << wxString::Format( _( "%d pins:\n" ), (int) pins.size() );

                  for( SCH_PIN* pin : pins )
                  {
                      out << wxS( "  pin " ) << pin->GetNumber() << wxS( "  " ) << pin->GetName()
                          << wxS( "  at " ) << Pos( pin->GetPosition() ) << wxS( " mm\n" );
                  }

                  aResult = out;
                  return true;
              } } );

    // ── changing symbols ──────────────────────────────────────────────

    tools.push_back(
            { { wxS( "set_symbol_value" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  const SYMBOL_REF entry =
                          RequireSymbol( aFrame, ARGS::RequireString( aArgs, "reference" ) );
                  const wxString value = ARGS::RequireString( aArgs, "value" );
                  const wxString was = entry.symbol->GetValue( true, &entry.path, false );

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  // The screen from the sheet path, not GetScreen(): a commit
                  // entry with a null screen is dropped silently in a release
                  // build, and GetScreen() is only the sheet on display.
                  commit.Modify( entry.symbol, entry.path.LastScreen() );
                  entry.symbol->SetValueFieldText( value );
                  commit.Push( _( "Change Symbol Value" ) );
                  guard.Pushed();

                  aResult = wxString::Format( _( "%s value changed from '%s' to '%s'." ),
                                              entry.symbol->GetRef( &entry.path, true ), was,
                                              value );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "set_symbol_footprint" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  const SYMBOL_REF entry =
                          RequireSymbol( aFrame, ARGS::RequireString( aArgs, "reference" ) );
                  const wxString footprint = ARGS::RequireString( aArgs, "footprint" );

                  if( !footprint.Contains( wxS( ":" ) ) )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "'%s' is not a footprint id. It must be LIBRARY:FOOTPRINT, e.g. "
                                 "Resistor_SMD:R_0805_2012Metric." ),
                              footprint ) );
                  }

                  auto sheet = entry.path;
                  ValidateFootprint( entry.symbol, sheet, footprint );

                  const wxString was = entry.symbol->GetFootprintFieldText( true, &entry.path,
                                                                            false );

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  commit.Modify( entry.symbol, entry.path.LastScreen() );
                  entry.symbol->SetFootprintFieldText( footprint );
                  commit.Push( _( "Set Symbol Footprint" ) );
                  guard.Pushed();

                  aResult = wxString::Format( _( "%s footprint set to '%s'%s." ),
                                              entry.symbol->GetRef( &entry.path, true ), footprint,
                                              was.IsEmpty()
                                                      ? wxString()
                                                      : wxString::Format( _( " (was '%s')" ),
                                                                          was ) );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "set_part_number" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  const SYMBOL_REF entry =
                          RequireSymbol( aFrame, ARGS::RequireString( aArgs, "reference" ) );
                  const wxString lcsc = ARGS::OptionalString( aArgs, "lcsc" ).Upper().Trim().Trim( false );
                  const wxString mpn = ARGS::OptionalString( aArgs, "mpn" ).Trim().Trim( false );
                  const wxString maker = ARGS::OptionalString( aArgs, "manufacturer" ).Trim().Trim( false );

                  if( lcsc.IsEmpty() && mpn.IsEmpty() )
                      throw ARGS::BAD_ARGUMENT( _( "Give an lcsc number or an mpn." ) );

                  if( !lcsc.IsEmpty() && !wxRegEx( wxS( "^C[0-9]{1,10}$" ) ).Matches( lcsc ) )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "'%s' is not an LCSC part number; they look like C2906982." ),
                              lcsc ) );
                  }

                  for( const wxString& text : { mpn, maker } )
                  {
                      if( text.length() > 80 )
                          throw ARGS::BAD_ARGUMENT( _( "Part numbers and manufacturer names are at most 80 characters." ) );
                  }

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  commit.Modify( entry.symbol, entry.path.LastScreen() );

                  wxArrayString changed;

                  // Hidden fields, named the way JLCPCB's BOM tooling and most
                  // KiCad plugins read them. An existing field of any case is reused.
                  auto put = [&]( const wxString& aName, const wxString& aValue )
                  {
                      if( PutPartField( entry.symbol, aName, aValue ) )
                          changed.Add( aName + wxS( " " ) + aValue );
                  };

                  put( wxS( "LCSC" ), lcsc );
                  put( wxS( "MPN" ), mpn );
                  put( wxS( "Manufacturer" ), maker );

                  commit.Push( _( "Set Part Number" ) );
                  guard.Pushed();

                  aResult = wxString::Format( _( "%s now records %s." ),
                                              entry.symbol->GetRef( &entry.path, true ),
                                              wxJoin( changed, ',' ) );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "move_symbol" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  const SYMBOL_REF entry =
                          RequireSymbol( aFrame, ARGS::RequireString( aArgs, "reference" ) );

                  const bool absolute = ARGS::Has( aArgs, "x" ) || ARGS::Has( aArgs, "y" );
                  const bool relative = ARGS::Has( aArgs, "dx" ) || ARGS::Has( aArgs, "dy" );

                  if( absolute && relative )
                      throw ARGS::BAD_ARGUMENT( _( "Give either x/y or dx/dy, not both." ) );

                  if( !absolute && !relative )
                      throw ARGS::BAD_ARGUMENT( _( "Give x/y or dx/dy to say where to move it." ) );

                  const VECTOR2I from = entry.symbol->GetPosition();
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
                      const double x = ToMm( from.x ) + ARGS::OptionalCoordMm( aArgs, "dx", 0.0 );
                      const double y = ToMm( from.y ) + ARGS::OptionalCoordMm( aArgs, "dy", 0.0 );

                      if( std::abs( x ) > ARGS::MAX_COORD_MM || std::abs( y ) > ARGS::MAX_COORD_MM )
                      {
                          throw ARGS::BAD_ARGUMENT(
                                  _( "That would move it off any reasonable sheet." ) );
                      }

                      to.x = ToIU( x );
                      to.y = ToIU( y );
                  }

                  // Snapped because every pin on the symbol moves with it, and a
                  // symbol placed off-grid puts its pins off-grid, where no wire
                  // drawn on the grid can ever reach them.
                  to = SnapToGrid( to );

                  if( to == from )
                  {
                      aResult = wxString::Format( _( "%s is already at %s mm." ),
                                                  entry.symbol->GetRef( &entry.path, true ),
                                                  Pos( from ) );
                      return true;
                  }

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  commit.Modify( entry.symbol, entry.path.LastScreen() );
                  entry.symbol->SetPosition( to );
                  commit.Push( _( "Move Symbol" ) );
                  guard.Pushed();

                  aResult = wxString::Format(
                          _( "%s moved from %s to %s mm. Any wires that were attached are now "
                             "dangling." ),
                          entry.symbol->GetRef( &entry.path, true ), Pos( from ), Pos( to ) );
                  return true;
              } } );

    // ── adding and removing ───────────────────────────────────────────

    tools.push_back(
            { { wxS( "search_symbols" ), false },
              []( SCH_EDIT_FRAME*, const json& aArgs, wxString& aResult ) -> bool
              {
                  SYMBOL_LIBRARY_ADAPTER* libs = RequireSymbolLibraries();

                  const wxString query   = ARGS::RequireString( aArgs, "query" ).Lower();
                  const wxString library = ARGS::OptionalString( aArgs, "library" );

                  if( library.IsEmpty() )
                  {
                      aResult = SearchAllSymbols( query );
                      return true;
                  }

                  std::vector<wxString> searchIn;

                  if( !library.IsEmpty() )
                  {
                      if( !Pgm().GetLibraryManager().GetRow( LIBRARY_TABLE_TYPE::SYMBOL, library ) )
                      {
                          throw ARGS::BAD_ARGUMENT( wxString::Format(
                                  _( "There is no symbol library called '%s'." ), library ) );
                      }

                      if( !libs->IsLibraryLoaded( library ) )
                          libs->LoadOne( library );

                      searchIn.push_back( library );
                  }
                  else
                  {
                      // Only what is already in memory: loading every installed
                      // library to answer a search would stall the editor for
                      // seconds with no way to cancel.
                      for( const wxString& name : libs->GetLibraryNames() )
                      {
                          if( libs->IsLibraryLoaded( name ) )
                              searchIn.push_back( name );
                      }

                      if( searchIn.empty() )
                      {
                          aResult = _( "No symbol library is loaded yet. Name one with the "
                                       "'library' argument -- 'Device' holds the common passives." );
                          return true;
                      }
                  }

                  wxString  out;
                  int       hits  = 0;
                  const int limit = 60;

                  for( const wxString& nickname : searchIn )
                  {
                      // GetSymbols(), not GetSymbolNames() -- see the board-side
                      // search_footprints for why the names variant goes to disk.
                      for( LIB_SYMBOL* symbol : libs->GetSymbols( nickname ) )
                      {
                          if( !symbol )
                              continue;

                          const wxString name = symbol->GetName();

                          if( !name.Lower().Contains( query ) )
                              continue;

                          if( ++hits > limit )
                              break;

                          out << wxS( "  " ) << nickname << wxS( ":" ) << name << wxS( "\n" );
                      }

                      if( hits > limit )
                          break;
                  }

                  if( !hits )
                  {
                      aResult = wxString::Format( _( "Nothing matching '%s' in %d librar(ies)." ),
                                                  query, (int) searchIn.size() );
                      return true;
                  }

                  aResult = wxString::Format( _( "%d matches%s\n" ), std::min( hits, limit ),
                                              hits > limit ? _( " (more exist)" ) : wxString() )
                            + out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "place_symbol" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC*              schematic = RequireSchematic( aFrame );
                  SYMBOL_LIBRARY_ADAPTER* libs      = RequireSymbolLibraries();

                  const wxString libIdText = ARGS::RequireString( aArgs, "lib_id" );
                  const double   x         = ARGS::RequireCoordMm( aArgs, "x" );
                  const double   y         = ARGS::RequireCoordMm( aArgs, "y" );
                  const wxString value     = ARGS::OptionalString( aArgs, "value" );
                  const wxString reference = ARGS::OptionalString( aArgs, "reference" );

                  LIB_ID libId;

                  // Parse() returns -1 on SUCCESS and the offending character
                  // index otherwise, so the natural-looking `if( Parse(...) )`
                  // rejects every valid id.
                  if( libId.Parse( UTF8( libIdText.utf8_string() ) ) >= 0 || !libId.IsValid() )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "'%s' is not a valid symbol id. Use LIBRARY:SYMBOL, e.g. "
                                 "Device:R." ),
                              libIdText ) );
                  }

                  const wxString nickname = libId.GetUniStringLibNickname();

                  if( nickname.IsEmpty() )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "'%s' does not name a library. Use LIBRARY:SYMBOL." ),
                              libIdText ) );
                  }

                  if( !libs->IsLibraryLoaded( nickname ) )
                      libs->LoadOne( nickname );

                  LIB_SYMBOL* libSymbol = libs->LoadSymbol( libId );

                  if( !libSymbol )
                  {
                      throw ARGS::BAD_ARGUMENT( wxString::Format(
                              _( "No symbol '%s' in library '%s'. Use search_symbols to find the "
                                 "exact name." ),
                              libId.GetUniStringLibItemName(), nickname ) );
                  }

                  SCH_SHEET_PATH& sheet  = schematic->CurrentSheet();
                  SCH_SCREEN*     screen = sheet.LastScreen();

                  if( !screen )
                      throw ARGS::BAD_ARGUMENT( _( "The current sheet has no screen." ) );

                  // Snapped, because the symbol's pins move with it: a symbol
                  // placed off-grid puts every one of its pins off-grid, where no
                  // wire drawn on the grid can ever connect to them.
                  const VECTOR2I at = SnapToGrid( VECTOR2I( ToIU( x ), ToIU( y ) ) );

                  // The constructor flattens the library symbol and takes
                  // ownership of the copy; the library's own object stays owned by
                  // the library cache and must not be deleted or held.
                  auto symbol = std::make_unique<SCH_SYMBOL>( *libSymbol, libId, &sheet, 1, 0, at );

                  if( !value.IsEmpty() )
                      symbol->SetValueFieldText( value );

                  if( !reference.IsEmpty() )
                      symbol->SetRef( &sheet, reference );

                  SCH_SYMBOL* raw = symbol.get();

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  commit.Add( symbol.release(), screen );
                  commit.Push( _( "Place Symbol" ) );
                  guard.Pushed();

                  const wxString placed = raw->GetRef( &sheet, true );

                  aResult = wxString::Format(
                          _( "Placed %s (%s) at %s mm on sheet '%s'.%s" ),
                          placed.IsEmpty() ? libIdText : placed, libIdText, Pos( at ),
                          SheetName( sheet ),
                          reference.IsEmpty()
                                  ? _( " Run annotate_schematic to give it a reference." )
                                  : wxString() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_wire" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC*      schematic = RequireSchematic( aFrame );
                  SCH_SHEET_PATH& sheet     = schematic->CurrentSheet();
                  SCH_SCREEN*     screen    = sheet.LastScreen();

                  if( !screen )
                      throw ARGS::BAD_ARGUMENT( _( "The current sheet has no screen." ) );

                  const VECTOR2I start = SnapToGrid(
                          VECTOR2I( ToIU( ARGS::RequireCoordMm( aArgs, "x1" ) ),
                                    ToIU( ARGS::RequireCoordMm( aArgs, "y1" ) ) ) );
                  const VECTOR2I end = SnapToGrid(
                          VECTOR2I( ToIU( ARGS::RequireCoordMm( aArgs, "x2" ) ),
                                    ToIU( ARGS::RequireCoordMm( aArgs, "y2" ) ) ) );

                  if( start == end )
                  {
                      throw ARGS::BAD_ARGUMENT(
                              _( "Both ends snap to the same grid point, so the wire would have "
                                 "zero length." ) );
                  }

                  // LAYER_WIRE explicitly.  The default is LAYER_NOTES, which
                  // draws an identical-looking line that is a graphic annotation
                  // and connects nothing at all.
                  auto wire = std::make_unique<SCH_LINE>( start, LAYER_WIRE );
                  wire->SetEndPoint( end );

                  // Asserted rather than assumed: the constructor silently maps
                  // any layer it does not recognise back to LAYER_NOTES, and the
                  // failure is invisible on screen.
                  if( !wire->IsWire() )
                  {
                      throw ARGS::BAD_ARGUMENT(
                              _( "Internal error: the line was not created as a wire." ) );
                  }

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  // Appended to the screen first, then declared to the commit with
                  // Added() rather than Add().  The junction test below asks the
                  // screen what meets at a point, so the wire has to be visible to
                  // it before the question is worth asking -- and doing it this way
                  // keeps the wire and its junctions in a single undo step.
                  SCH_LINE* raw = wire.release();
                  aFrame->AddToScreen( raw, screen );
                  commit.Added( raw, screen );

                  // Without this, a wire that ends on the middle of another wire
                  // draws a perfect T and connects nothing. KiCad requires an
                  // explicit junction wherever more than two connectable things
                  // meet, and the absence of one is invisible on the canvas -- the
                  // netlist is simply wrong. This is the single most likely way for
                  // agent-drawn wiring to look right and be wrong.
                  // Both ends, plus every existing connection point the new wire
                  // passes THROUGH. The mid-span case is the one that matters most
                  // and is easiest to miss: a wire routed straight past a pin, or
                  // across the end of another wire, needs a junction there too --
                  // and without it the two are drawn touching and are electrically
                  // separate. This mirrors what the interactive wire tool does.
                  std::vector<VECTOR2I> candidates = { start, end };

                  for( const VECTOR2I& point : screen->GetConnections() )
                  {
                      if( point != start && point != end
                          && IsPointOnSegment( start, end, point ) )
                      {
                          candidates.push_back( point );
                      }
                  }

                  int junctions = 0;

                  for( const VECTOR2I& point : candidates )
                  {
                      if( !screen->IsExplicitJunctionNeeded( point ) )
                          continue;

                      auto junction = std::make_unique<SCH_JUNCTION>( point );

                      if( screen->GetBus( point ) )
                          junction->SetLayer( LAYER_BUS_JUNCTION );

                      SCH_JUNCTION* rawJunction = junction.release();
                      aFrame->AddToScreen( rawJunction, screen );
                      commit.Added( rawJunction, screen );
                      ++junctions;
                  }

                  commit.Push( _( "Add Wire" ) );
                  guard.Pushed();

                  aResult = wxString::Format( _( "Wire from %s to %s mm on sheet '%s'.%s" ),
                                              Pos( start ), Pos( end ), SheetName( sheet ),
                                              junctions
                                                      ? wxString::Format(
                                                                _( " Added %d junction(s) where "
                                                                   "it meets existing wires." ),
                                                                junctions )
                                                      : wxString() );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "delete_symbols" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  const std::vector<wxString> refs =
                          ARGS::RequireStringArray( aArgs, "references", 50 );

                  // Resolved in full before anything is removed: a half-completed
                  // delete leaves the user working out what happened while the
                  // model reports a failure.
                  std::vector<SYMBOL_REF> victims;
                  int                     multiUnit = 0;

                  for( const wxString& ref : refs )
                  {
                      int              units = 0;
                      const SYMBOL_REF entry = RequireSymbol( aFrame, ref, &units );

                      if( units > 1 )
                          ++multiUnit;

                      victims.push_back( entry );
                  }

                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                  {
                      if( SCH_SELECTION_TOOL* sel = mgr->GetTool<SCH_SELECTION_TOOL>() )
                          sel->ClearSelection();
                  }

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  wxString removed;

                  for( const SYMBOL_REF& entry : victims )
                  {
                      removed << ( removed.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
                              << entry.symbol->GetRef( &entry.path, true );
                      commit.Remove( entry.symbol, entry.path.LastScreen() );
                  }

                  commit.Push( _( "Delete Symbols" ) );
                  guard.Pushed();

                  aResult = wxString::Format( _( "Deleted %d symbol(s): %s." ),
                                              (int) victims.size(), removed );

                  if( multiUnit )
                  {
                      // Deleting one unit of a package leaves its siblings sharing
                      // the reference, which ERC reports as a missing unit.
                      aResult << wxString::Format(
                              _( " Note: %d of these belong to multi-unit packages, and only one "
                                 "unit was removed -- ERC will report the rest as incomplete." ),
                              multiUnit );
                  }

                  return true;
              } } );

    // ── checking the work ─────────────────────────────────────────────

    tools.push_back(
            { { wxS( "run_erc" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  return RunERC( aFrame, aResult );
              } } );

    tools.push_back(
            { { wxS( "annotate_schematic" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json&, wxString& aResult ) -> bool
              {
                  RequireSchematic( aFrame );

                  int before = 0;

                  for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
                  {
                      const wxString ref = entry.symbol->GetRef( &entry.path, false );

                      if( ref.IsEmpty() || ref.EndsWith( wxS( "?" ) ) )
                          ++before;
                  }

                  if( !before )
                  {
                      aResult = _( "Every symbol is already annotated; nothing to do." );
                      return true;
                  }

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

                  WX_STRING_REPORTER reporter;

                  // aResetAnnotation is false, deliberately and importantly.  True
                  // renumbers *everything*: every existing R1 and C3 can change,
                  // which breaks the symbol-to-footprint linkage on any board
                  // already made from this schematic and invalidates every
                  // reference the user has been told about. Filling the gaps is
                  // what "annotate" means when an agent says it.
                  //
                  // SYMBOL_FILTER_NON_POWER because power symbols are annotated
                  // separately by the netlist and ERC code themselves; putting
                  // them through this path produces junk designators.
                  aFrame->AnnotateSymbols( &commit, ANNOTATE_ALL, SORT_BY_X_POSITION,
                                           INCREMENTAL_BY_REF, true /* recursive */, 0,
                                           false /* aResetAnnotation */,
                                           true /* aRegroupUnits */,
                                           false /* aRepairTimestamps */, reporter,
                                           SYMBOL_FILTER_NON_POWER );

                  commit.Push( _( "Annotate Schematic" ) );
                  guard.Pushed();

                  int after = 0;

                  for( const SYMBOL_REF& entry : AllSymbols( aFrame ) )
                  {
                      const wxString ref = entry.symbol->GetRef( &entry.path, false );

                      if( ref.IsEmpty() || ref.EndsWith( wxS( "?" ) ) )
                          ++after;
                  }

                  aResult = wxString::Format(
                          _( "Annotated %d symbol(s); %d still without a reference." ),
                          before - after, after );
                  return true;
              } } );

    // ── any item ──────────────────────────────────────────────────────
    // What the dedicated tools do not cover, on any sheet, by id. One undo step per call.

    tools.push_back(
            { { wxS( "find_items" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC*                       schematic = RequireSchematic( aFrame );
                  const INDUCTIVE_SCH_ITEMS::QUERY query = ParseSchQuery( aArgs );

                  const int limit = (int) std::clamp( ARGS::OptionalNumber( aArgs, "limit", 60, 1, 1e6 ), 1.0, 300.0 );
                  size_t    total = 0;
                  const auto found = INDUCTIVE_SCH_ITEMS::Find( schematic, query, (size_t) limit, &total );

                  if( found.empty() )
                  {
                      aResult = _( "No item matches." );
                      return true;
                  }

                  const bool multi = schematic->Hierarchy().size() > 1;
                  wxString   out = wxString::Format( _( "%d item(s)" ), (int) total );

                  if( total > found.size() )
                      out << wxString::Format( _( ", the first %d shown; narrow by kinds, sheet, net or area" ),
                                               (int) found.size() );

                  out << wxS( ":\n" );

                  for( const auto& placed : found )
                      out << INDUCTIVE_SCH_ITEMS::Describe( placed, multi ) << wxS( "\n" );

                  aResult = out;
                  return true;
              } } );

    tools.push_back(
            { { wxS( "get_item_properties" ), false },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC* schematic = RequireSchematic( aFrame );
                  wxString   why;
                  auto       placed = INDUCTIVE_SCH_ITEMS::Resolve( schematic, ARGS::RequireString( aArgs, "id" ), &why );

                  if( !placed )
                      throw ARGS::BAD_ARGUMENT( why );

                  aResult = INDUCTIVE_SCH_ITEMS::Describe( *placed, schematic->Hierarchy().size() > 1 ) + wxS( "\n" )
                            + INDUCTIVE_SCH_ITEMS::Properties( placed->item );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "set_item_properties" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC* schematic = RequireSchematic( aFrame );
                  std::vector<INDUCTIVE_SCH_ITEMS::PLACED> items = SchItemsToEdit( schematic, aArgs, 1000 );

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
                      else
                          throw ARGS::BAD_ARGUMENT( wxString::Format( _( "'%s' needs a text, number or true/false value." ),
                                                                      wxString::FromUTF8( name ) ) );

                      ARGS::RejectDangerousText( text, "properties" );
                      values.emplace_back( wxString::FromUTF8( name ), text );
                  }

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  auto report = INDUCTIVE_SCH_ITEMS::SetProperties(
                          [&]( SCH_ITEM* aItem, SCH_SCREEN* aScreen ) { commit.Modify( aItem, aScreen ); }, items,
                          values, aFrame, schematic->CurrentSheet() );

                  if( report.changed )
                  {
                      commit.Push( _( "Edit Properties" ) );
                      guard.Pushed();
                  }

                  aFrame->GetCanvas()->Refresh();

                  wxString out = report.changes;

                  if( !report.problems.IsEmpty() )
                      out << _( "Not changed:\n" ) << report.problems;

                  if( !report.changed && report.problems.IsEmpty() )
                      out = _( "Nothing to change." );

                  aResult = out.Trim();
                  return report.changed > 0;
              } } );

    tools.push_back(
            { { wxS( "delete_items" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC* schematic = RequireSchematic( aFrame );
                  std::vector<INDUCTIVE_SCH_ITEMS::PLACED> items = SchItemsToEdit( schematic, aArgs, 500 );

                  if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
                      if( SCH_SELECTION_TOOL* sel = mgr->GetTool<SCH_SELECTION_TOOL>() )
                          sel->ClearSelection();

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  auto report = INDUCTIVE_SCH_ITEMS::Delete(
                          [&]( SCH_ITEM* aItem, SCH_SCREEN* aScreen ) { commit.Remove( aItem, aScreen ); }, items );

                  if( report.changed )
                  {
                      commit.Push( _( "Delete Items" ) );
                      guard.Pushed();
                  }

                  aFrame->GetCanvas()->Refresh();

                  wxString out = report.changes;

                  if( !report.problems.IsEmpty() )
                      out << _( "Not deleted:\n" ) << report.problems;

                  aResult = out.Trim();
                  return report.changed > 0;
              } } );

    tools.push_back(
            { { wxS( "add_label" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC*     schematic = RequireSchematic( aFrame );
                  const wxString text = ARGS::RequireString( aArgs, "text" );
                  const int      kind = ARGS::OptionalChoice( aArgs, "kind", { "local", "global", "hierarchical" }, 0 );

                  ARGS::RejectDangerousText( text, "text" );

                  if( text.Contains( wxS( " " ) ) )
                      throw ARGS::BAD_ARGUMENT( _( "A net name has no spaces; use _ instead." ) );

                  VECTOR2I    at;
                  SPIN_STYLE  spin = SPIN_STYLE::RIGHT;
                  SCH_SCREEN* screen = schematic->CurrentSheet().LastScreen();
                  wxString    where;

                  if( ARGS::Has( aArgs, "pin" ) )
                  {
                      wxString why;
                      auto     pin = INDUCTIVE_SCH_ITEMS::FindPin( schematic, ARGS::RequireString( aArgs, "pin" ), &why );

                      if( !pin )
                          throw ARGS::BAD_ARGUMENT( why );

                      if( pin->path != schematic->CurrentSheet() )
                          throw ARGS::BAD_ARGUMENT( wxString::Format( _( "%s is on sheet '%s'; labels go on the sheet displayed." ),
                                                                      ARGS::RequireString( aArgs, "pin" ), SheetName( pin->path ) ) );

                      at = pin->pin->GetPosition();
                      spin = INDUCTIVE_SCH_ITEMS::AwayFrom( *pin );
                      screen = pin->screen;
                      where = ARGS::RequireString( aArgs, "pin" );
                  }
                  else
                  {
                      at = SnapToGrid( VECTOR2I( ToIU( ARGS::RequireCoordMm( aArgs, "x" ) ),
                                                 ToIU( ARGS::RequireCoordMm( aArgs, "y" ) ) ) );
                      where = Pos( at ) + wxS( " mm" );
                  }

                  if( !screen )
                      throw ARGS::BAD_ARGUMENT( _( "The current sheet has no screen." ) );

                  SCH_LABEL_BASE* label = nullptr;

                  if( kind == 1 )
                      label = new SCH_GLOBALLABEL( at, text );
                  else if( kind == 2 )
                      label = new SCH_HIERLABEL( at, text );
                  else
                      label = new SCH_LABEL( at, text );

                  label->SetSpinStyle( spin );

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  aFrame->AddToScreen( label, screen );
                  commit.Added( label, screen );
                  commit.Push( _( "Add Label" ) );
                  guard.Pushed();
                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "Added %s label %s at %s." ),
                                              kind == 1 ? _( "global" ) : kind == 2 ? _( "hierarchical" ) : _( "local" ),
                                              text, where );
                  return true;
              } } );

    tools.push_back(
            { { wxS( "add_no_connect" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC*            schematic = RequireSchematic( aFrame );
                  std::vector<wxString> pins = ARGS::RequireStringArray( aArgs, "pins", 100 );

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  wxArrayString                      done, refused;

                  for( const wxString& name : pins )
                  {
                      wxString why;
                      auto     pin = INDUCTIVE_SCH_ITEMS::FindPin( schematic, name, &why );

                      if( !pin )
                      {
                          refused.Add( why );
                          continue;
                      }

                      if( pin->screen->GetItem( pin->pin->GetPosition(), 0, SCH_NO_CONNECT_T ) )
                      {
                          done.Add( name );
                          continue;
                      }

                      auto* flag = new SCH_NO_CONNECT( pin->pin->GetPosition() );
                      aFrame->AddToScreen( flag, pin->screen );
                      commit.Added( flag, pin->screen );
                      done.Add( name );
                  }

                  if( !done.IsEmpty() )
                  {
                      commit.Push( _( "Add No-connect Flags" ) );
                      guard.Pushed();
                  }

                  aFrame->GetCanvas()->Refresh();

                  aResult = done.IsEmpty() ? wxString()
                                           : wxString::Format( _( "No-connect on %s." ), wxJoin( done, ',', '\0' ) );

                  if( !refused.IsEmpty() )
                      aResult << ( aResult.IsEmpty() ? wxS( "" ) : wxS( "\n" ) ) << _( "Not placed: " )
                              << wxJoin( refused, ';', '\0' );

                  return !done.IsEmpty();
              } } );

    tools.push_back(
            { { wxS( "add_text" ), true },
              []( SCH_EDIT_FRAME* aFrame, const json& aArgs, wxString& aResult ) -> bool
              {
                  SCHEMATIC*     schematic = RequireSchematic( aFrame );
                  SCH_SCREEN*    screen = schematic->CurrentSheet().LastScreen();
                  const wxString text = ARGS::RequireString( aArgs, "text" );

                  ARGS::RejectDangerousText( text, "text" );

                  if( !screen )
                      throw ARGS::BAD_ARGUMENT( _( "The current sheet has no screen." ) );

                  const VECTOR2I at( ToIU( ARGS::RequireCoordMm( aArgs, "x" ) ), ToIU( ARGS::RequireCoordMm( aArgs, "y" ) ) );
                  const int      size = ToIU( ARGS::OptionalNumber( aArgs, "size", 1.27, 0.5, 20 ) );

                  auto* note = new SCH_TEXT( at, text );
                  note->SetTextSize( VECTOR2I( size, size ) );

                  SCH_COMMIT                         commit( aFrame );
                  INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                  aFrame->AddToScreen( note, screen );
                  commit.Added( note, screen );
                  commit.Push( _( "Add Text" ) );
                  guard.Pushed();
                  aFrame->GetCanvas()->Refresh();

                  aResult = wxString::Format( _( "Added the note at %s mm on sheet '%s'." ), Pos( at ),
                                              SheetName( schematic->CurrentSheet() ) );
                  return true;
              } } );

    return tools;
}


}  // namespace


wxString InductiveSearchSymbols( const wxString& aQuery )
{
    return SearchAllSymbols( aQuery );
}

wxString InductiveSuggestedFootprints( const wxArrayString& aFilters )
{
    return SuggestedFootprints( aFilters );
}


// ───────────────────── drawing a generated design ─────────────────────
//
// The schematic the user sees is produced by the pipeline, not placed symbol by
// symbol: one model request writes the whole circuit, tscircuit compiles it, and
// the exporter writes a .kicad_sch.  What is left is to put that file's contents
// into the sheet the user has open -- their project, their undo stack, one step.
//
// The mechanics mirror eeschema's own paste, because paste is the tested path for
// "items from another document, into this one":
//
//   - parse into a throwaway sheet, so a malformed file cannot half-apply,
//   - re-key every item, because the generated UUIDs mean nothing here,
//   - attach each symbol's library definition from the file's own lib_symbols,
//     since the destination project has never heard of tscircuit's libraries,
//   - give each symbol an instance on *this* sheet path, or it has no reference
//     and ERC treats it as unannotated.

/** What the exporter calls the schematic it writes. */
static const wxString GENERATED_SCHEMATIC = wxS( "inductive_demo.kicad_sch" );


/**
 * Replace the contents of the open sheet with a generated schematic.
 *
 * Returns false and fills aResult with something worth showing the user; on
 * success aResult is the one-line trace row.
 */
static bool DrawGeneratedSchematic( SCH_EDIT_FRAME* aFrame, const wxString& aDirectory,
                                    wxString& aResult )
{
    SCHEMATIC* schematic = aFrame ? &aFrame->Schematic() : nullptr;

    if( !schematic )
    {
        aResult = _( "No schematic is open." );
        return false;
    }

    const wxFileName file( aDirectory, GENERATED_SCHEMATIC );

    if( !file.FileExists() )
    {
        aResult = wxString::Format( _( "The generated schematic is missing: %s" ),
                                    file.GetFullPath() );
        return false;
    }

    wxString content;

    {
        wxFFile handle;

        if( !handle.Open( file.GetFullPath(), wxS( "rb" ) )
            || !handle.ReadAll( &content, wxConvUTF8 ) )
        {
            aResult = wxString::Format( _( "Could not read %s" ), file.GetFullPath() );
            return false;
        }
    }

    // Parsed into a sheet of its own first. A file that turns out to be broken
    // then costs nothing: the user's schematic has not been touched yet.
    SCH_SHEET   staging;
    SCH_SCREEN* stagingScreen = new SCH_SCREEN( schematic );

    staging.SetScreen( stagingScreen );   // the sheet owns it from here

    try
    {
        STRING_LINE_READER reader( content.utf8_string(), file.GetFullPath() );

        // The parser directly, not SCH_IO_KICAD_SEXPR::LoadContent(): that one
        // parses a *clipboard fragment* -- a bare list of items -- and chokes on
        // the `(kicad_sch` that opens a real file. And not LoadSchematicFile()
        // either, which would take the live SCHEMATIC through a full project
        // load. This parses the document exactly as opening it would, into a
        // sheet that is ours.
        SCH_IO_KICAD_SEXPR_PARSER parser( &reader );

        parser.ParseSchematic( &staging );
    }
    catch( const IO_ERROR& e )
    {
        aResult = wxString::Format( _( "The generated schematic could not be read: %s" ),
                                    e.What() );
        return false;
    }

    std::vector<SCH_ITEM*> incoming;
    std::vector<SCH_ITEM*> unwanted;

    for( SCH_ITEM* item : stagingScreen->Items() )
    {
        // The exporter writes a flat sheet. A sub-sheet would need a file of its
        // own on disk and a hierarchy update; refusing one is better than half
        // importing it.
        if( item->Type() == SCH_SHEET_T )
            unwanted.push_back( item );
        else
            incoming.push_back( item );
    }

    if( incoming.empty() )
    {
        aResult = _( "The generated schematic was empty." );
        return false;
    }

    SCH_SHEET_PATH& sheetPath = schematic->CurrentSheet();
    SCH_SCREEN*     screen    = sheetPath.LastScreen();

    if( !screen )
    {
        aResult = _( "The current sheet has no screen." );
        return false;
    }

    // The library definitions travel inside the file, which is what lets a
    // generated symbol render in a project whose library table has never heard
    // of it. Copied now, while the staging screen still owns them.
    std::map<wxString, LIB_SYMBOL*> libSymbols = stagingScreen->GetLibSymbols();

    // Hand the items over without freeing them; the staging screen is only a
    // parser destination, and everything in it is about to belong to the real one.
    stagingScreen->Clear( false );

    for( SCH_ITEM* item : unwanted )
        delete item;

    if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
    {
        if( SCH_SELECTION_TOOL* selection = mgr->GetTool<SCH_SELECTION_TOOL>() )
            selection->ClearSelection();
    }

    SCH_COMMIT                         commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

    // Out with what was drawn before. The circuit is regenerated whole, so a
    // partial replacement would leave orphans of a design that no longer exists.
    std::vector<SCH_ITEM*> existing;

    for( SCH_ITEM* item : screen->Items() )
        existing.push_back( item );

    for( SCH_ITEM* item : existing )
        commit.Remove( item, screen );

    int symbols = 0;

    for( SCH_ITEM* item : incoming )
    {
        if( item->IsConnectable() )
            item->SetConnectivityDirty();

        // The generated file's UUIDs describe a document that is not this one.
        const_cast<KIID&>( item->m_Uuid ) = KIID();

        if( item->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            auto definition = libSymbols.find( symbol->GetSchSymbolLibraryName() );

            if( definition != libSymbols.end() && definition->second )
                symbol->SetLibSymbol( new LIB_SYMBOL( *definition->second ) );

            // The reference the generator assigned, re-hung on this sheet's path.
            // Instance data is keyed by (project, sheet path), and the generator's
            // key names a document that does not exist here -- so the old entries
            // are dropped rather than left for KiCad to save back out as orphans.
            wxString reference;
            int      unit = symbol->GetUnit();

            if( !symbol->GetInstances().empty() )
            {
                reference = symbol->GetInstances().front().m_Reference;
                unit      = symbol->GetInstances().front().m_Unit;
            }

            if( reference.IsEmpty() )
            {
                if( SCH_FIELD* field = symbol->GetField( FIELD_T::REFERENCE ) )
                    reference = field->GetText();
            }

            std::vector<KIID_PATH> stale;

            for( const SCH_SYMBOL_INSTANCE& instance : symbol->GetInstances() )
                stale.push_back( instance.m_Path );

            for( const KIID_PATH& path : stale )
                symbol->RemoveInstance( path );

            SCH_SYMBOL_INSTANCE instance;
            instance.m_Path        = sheetPath.Path();
            instance.m_Reference   = reference;
            instance.m_Unit        = unit;
            instance.m_ProjectName = aFrame->Prj().GetProjectName();

            symbol->AddHierarchicalReference( instance );
            symbol->SetUnit( unit );

            // Sets the field text and the designator prefix as well, which
            // AddHierarchicalReference on its own does not.
            symbol->SetRef( &sheetPath, reference );

            for( SCH_PIN* pin : symbol->GetPins() )
            {
                const_cast<KIID&>( pin->m_Uuid ) = KIID();
                pin->SetConnectivityDirty();
            }

            ++symbols;
        }

        commit.Add( item, screen );
    }

    commit.Push( _( "Draw Schematic" ) );
    guard.Pushed();

    // A generated sheet does not land where the user happens to be looking, and a
    // correct schematic off the edge of the viewport reads as nothing happening.
    if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
        mgr->PostAction( ACTIONS::zoomFitScreen );

    aResult = wxString::Format( _( "%d symbol%s" ), symbols, symbols == 1 ? wxS( "" ) : wxS( "s" ) );
    return true;
}


// ───────────────────── rearranging what is drawn ─────────────────────
//
// Moving a symbol and clearing the sheet do not change the circuit, so the
// pipeline answers those with operations rather than regenerating a design the
// user did not ask to change. The list arrives already validated by the runner;
// it is validated again here, because the only argument check that counts is the
// one on the side that touches the document.

/** Apply the runner's edit operations. Returns false with a reason in aResult. */
static bool ApplySchematicOps( SCH_EDIT_FRAME* aFrame, const wxString& aOpsJson,
                               wxString& aResult )
{
    SCHEMATIC* schematic = aFrame ? &aFrame->Schematic() : nullptr;

    if( !schematic )
    {
        aResult = _( "No schematic is open." );
        return false;
    }

    const json ops = json::parse( std::string( aOpsJson.utf8_str() ), nullptr, false );

    if( ops.is_discarded() || !ops.is_array() || ops.empty() )
    {
        aResult = _( "No changes to apply." );
        return false;
    }

    SCH_SHEET_PATH& sheetPath = schematic->CurrentSheet();
    SCH_SCREEN*     screen    = sheetPath.LastScreen();

    if( !screen )
    {
        aResult = _( "The current sheet has no screen." );
        return false;
    }

    if( TOOL_MANAGER* mgr = aFrame->GetToolManager() )
    {
        if( SCH_SELECTION_TOOL* selection = mgr->GetTool<SCH_SELECTION_TOOL>() )
            selection->ClearSelection();
    }

    SCH_COMMIT                         commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );

    int moved = 0;
    int removed = 0;
    bool cleared = false;

    try
    {
        for( const auto& op : ops )
        {
            if( !op.is_object() )
                continue;

            const std::string name = op.value( "op", std::string() );

            if( name == "delete_all" )
            {
                std::vector<SCH_ITEM*> everything;

                for( SCH_ITEM* item : screen->Items() )
                    everything.push_back( item );

                for( SCH_ITEM* item : everything )
                    commit.Remove( item, screen );

                cleared = true;
                continue;
            }

            if( name == "delete_symbols" )
            {
                const auto refs = op.find( "refs" );

                if( refs == op.end() || !refs->is_array() )
                    continue;

                for( const auto& ref : *refs )
                {
                    if( !ref.is_string() )
                        continue;

                    const SYMBOL_REF entry =
                            RequireSymbol( aFrame, wxString::FromUTF8( ref.get<std::string>() ) );

                    commit.Remove( entry.symbol, entry.path.LastScreen() );
                    ++removed;
                }

                continue;
            }

            if( name == "move_symbol" )
            {
                const SYMBOL_REF entry = RequireSymbol(
                        aFrame, wxString::FromUTF8( op.value( "ref", std::string() ) ) );

                const VECTOR2I from = entry.symbol->GetPosition();
                VECTOR2I       to   = from;

                if( op.contains( "x_mm" ) && op.contains( "y_mm" ) )
                {
                    to.x = ToIU( op.value( "x_mm", 0.0 ) );
                    to.y = ToIU( op.value( "y_mm", 0.0 ) );
                }
                else
                {
                    to.x = ToIU( ToMm( from.x ) + op.value( "dx_mm", 0.0 ) );
                    to.y = ToIU( ToMm( from.y ) + op.value( "dy_mm", 0.0 ) );
                }

                // Snapped, because every pin moves with the symbol and a symbol
                // off-grid puts its pins where no wire can reach them.
                commit.Modify( entry.symbol, entry.path.LastScreen() );
                entry.symbol->SetPosition( SnapToGrid( to ) );
                ++moved;
            }
        }
    }
    catch( const ARGS::BAD_ARGUMENT& e )
    {
        // The guard reverts whatever was staged when it goes out of scope.
        aResult = wxString::FromUTF8( e.what() );
        return false;
    }

    commit.Push( cleared ? _( "Clear Schematic" ) : _( "Edit Schematic" ) );
    guard.Pushed();

    wxString detail;

    if( cleared )
        detail = _( "cleared the sheet" );

    if( moved )
    {
        detail << ( detail.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
               << wxString::Format( _( "moved %d symbol%s" ), moved,
                                    moved == 1 ? wxS( "" ) : wxS( "s" ) );
    }

    if( removed )
    {
        detail << ( detail.IsEmpty() ? wxS( "" ) : wxS( ", " ) )
               << wxString::Format( _( "deleted %d symbol%s" ), removed,
                                    removed == 1 ? wxS( "" ) : wxS( "s" ) );
    }

    aResult = detail.IsEmpty() ? _( "nothing to change" ) : detail;
    return true;
}


// Also used by the headless QA runner, so integration tests exercise the exact
// symbol, footprint and wiring construction used in the editor.
std::vector<std::unique_ptr<SCH_ITEM>> InductiveBuildCircuitItems(
        SCHEMATIC* aSchematic, const nlohmann::json& aArgs, wxString& aResult )
{
    return ConstructCircuit( aSchematic, aArgs, aResult );
}

// Parse without loading child paths or changing the live document.
std::unique_ptr<SCH_SHEET> InductiveReadAttachedSchematic( SCHEMATIC* aSchematic,
                                                         const json& aFile )
{
    auto summary = INDUCTIVE_ATTACHMENTS::Inspect( aFile );
    if( summary["kind"] != "schematic" ) throw std::runtime_error( "Expected a schematic." );
    if( summary["child_sheets"].get<int>() )
        throw std::runtime_error( "This schematic references child sheets. Attach a standalone sheet; importing a multi-file hierarchy is not supported yet." );
    auto sheet = std::make_unique<SCH_SHEET>();
    sheet->SetScreen( new SCH_SCREEN( aSchematic ) );
    STRING_LINE_READER reader( aFile.at( "text" ).get<std::string>(),
                               wxString::FromUTF8( aFile.at( "name" ).get<std::string>() ) );
    SCH_IO_KICAD_SEXPR_PARSER parser( &reader );
    parser.ParseSchematic( sheet.get() );
    auto* screen = sheet->GetScreen();
    const auto& definitions = screen->GetLibSymbols();
    int items = 0;
    for( SCH_ITEM* item : screen->Items() )
    {
        ++items;
        if( item->Type() == SCH_SHEET_T || item->Type() == SCH_GROUP_T )
            throw std::runtime_error( "Nested sheets or grouped schematic items are not supported by attachment import yet." );
        if( item->Type() == SCH_SYMBOL_T )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            auto definition = definitions.find( symbol->GetSchSymbolLibraryName() );
            if( definition == definitions.end() || !definition->second )
                throw std::runtime_error( "An attached symbol is missing its embedded library definition." );
            symbol->SetLibSymbol( new LIB_SYMBOL( *definition->second ) );
        }
    }
    if( !items ) throw std::runtime_error( "The attached schematic is empty." );
    return sheet;
}

std::unique_ptr<SCH_SHEET> InductivePrepareAttachedSchematic( SCHEMATIC& schematic, const json& aFile,
                                                              wxString& aResult )
{
    auto sheet = InductiveReadAttachedSchematic( &schematic, aFile );
    auto* screen = sheet->GetScreen();
    auto& parentPath = schematic.CurrentSheet();
    auto* parentScreen = parentPath.LastScreen();
    if( !parentScreen ) throw std::runtime_error( "No current schematic sheet." );
    if( !wxFileName( parentScreen->GetFileName() ).IsAbsolute() )
        throw std::runtime_error( "Save the current schematic sheet before importing a circuit." );
    const_cast<KIID&>( sheet->m_Uuid ) = KIID();
    screen->AssignNewUuid();
    sheet->SetParent( parentPath.Last() );
    const wxString stem = "Attached_" + sheet->m_Uuid.AsString();
    const wxString projectDir = schematic.Project().GetProjectPath();
    const wxString fullPath = wxFileName( projectDir, stem + ".kicad_sch" ).GetFullPath();
    if( wxFileExists( fullPath ) ) throw std::runtime_error( "Import destination already exists." );
    // Sheet file paths are relative to the parent sheet, which may itself be in a subfolder.
    wxFileName relative( fullPath );
    relative.MakeRelativeTo( wxFileName( parentScreen->GetFileName() ).GetPath() );
    sheet->SetFileName( relative.GetFullPath() );
    sheet->SetName( wxFileName( wxString::FromUTF8( aFile.at( "name" ).get<std::string>() ) ).GetName()
                    + "_" + sheet->m_Uuid.AsString().Left( 8 ) );
    screen->SetFileName( fullPath );
    VECTOR2I position( schIUScale.mmToIU( 50.8 ), schIUScale.mmToIU( 50.8 ) );
    for( SCH_ITEM* item : parentScreen->Items() )
        position.x = std::max( position.x, item->GetBoundingBox().GetRight() + schIUScale.mmToIU( 12.7 ) );
    position.x = ( ( position.x + SCH_GRID_IU - 1 ) / SCH_GRID_IU ) * SCH_GRID_IU;
    sheet->SetPosition( position );
    sheet->SetSize( VECTOR2I( schIUScale.mmToIU( 50.8 ), schIUScale.mmToIU( 25.4 ) ) );
    SCH_SHEET_PATH path = parentPath;
    path.push_back( sheet.get() );
    SCH_SHEET_INSTANCE instance;
    instance.m_Path = parentPath.Path();
    instance.m_ProjectName = schematic.Project().GetProjectName();
    int page = 1;
    auto hierarchy = schematic.Hierarchy();
    while( hierarchy.PageNumberExists( wxString::Format( "%d", page ) ) ) ++page;
    instance.m_PageNumber = wxString::Format( "%d", page );
    std::vector<KIID_PATH> oldSheetPaths;
    for( const auto& old : sheet->GetInstances() ) oldSheetPaths.push_back( old.m_Path );
    for( const auto& old : oldSheetPaths ) sheet->RemoveInstance( old );
    sheet->AddInstance( instance );
    std::set<wxString> used;
    for( auto& existingPath : hierarchy )
        for( SCH_ITEM* item : existingPath.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            used.insert( static_cast<SCH_SYMBOL*>( item )->GetRef( &existingPath, false ) );
    std::map<wxString, wxString> references;
    std::set<wxString> ports;
    int symbols = 0;
    for( SCH_ITEM* item : screen->Items() )
    {
        const_cast<KIID&>( item->m_Uuid ) = KIID();
        item->SetConnectivityDirty();
        if( item->Type() == SCH_HIER_LABEL_T )
        {
            auto* label = static_cast<SCH_HIERLABEL*>( item );
            if( ports.insert( label->GetText() ).second )
            {
                const VECTOR2I pinPos( position.x, position.y + schIUScale.mmToIU( 5.08 * ports.size() ) );
                auto* pin = new SCH_SHEET_PIN( sheet.get(), pinPos, label->GetText() );
                pin->SetShape( label->GetShape() );
                pin->SetSide( SHEET_SIDE::LEFT );
                sheet->AddPin( pin );
            }
        }
        if( item->Type() != SCH_SYMBOL_T ) continue;
        auto* symbol = static_cast<SCH_SYMBOL*>( item );
        wxString oldRef = symbol->GetField( FIELD_T::REFERENCE )->GetText();
        int unit = symbol->GetUnit();
        if( !symbol->GetInstances().empty() )
        {
            oldRef = symbol->GetInstances().front().m_Reference;
            unit = symbol->GetInstances().front().m_Unit;
        }
        const wxString refKey = oldRef.empty() || oldRef.Contains( "?" )
                                    ? oldRef + symbol->m_Uuid.AsString() : oldRef;
        if( !references.count( refKey ) )
        {
            wxString prefix;
            for( wxUniChar c : oldRef )
            {
                if( wxIsalpha( c ) || c == '#' ) prefix += c;
                else break;
            }
            if( prefix.empty() ) prefix = "U";
            wxString ref = oldRef;
            int number = 1;
            while( ref.empty() || ref.Contains( "?" ) || used.count( ref ) )
                ref = prefix + wxString::Format( "%d", number++ );
            references[refKey] = ref;
            used.insert( ref );
        }
        std::vector<KIID_PATH> stale;
        for( const auto& old : symbol->GetInstances() ) stale.push_back( old.m_Path );
        for( const auto& old : stale ) symbol->RemoveInstance( old );
        SCH_SYMBOL_INSTANCE si;
        si.m_Path = path.Path(); si.m_ProjectName = schematic.Project().GetProjectName();
        si.m_Reference = references[refKey]; si.m_Unit = unit;
        symbol->AddHierarchicalReference( si );
        symbol->SetUnit( unit ); symbol->SetRef( &path, si.m_Reference );
        for( SCH_PIN* pin : symbol->GetPins() )
        { const_cast<KIID&>( pin->m_Uuid ) = KIID(); pin->SetConnectivityDirty(); }
        ++symbols;
    }
    sheet->SetSize( VECTOR2I( schIUScale.mmToIU( 50.8 ),
                             schIUScale.mmToIU( std::max( 25.4, 5.08 * ( ports.size() + 1 ) ) ) ) );
    sheet->AutoplaceFields( nullptr, AUTOPLACE_AUTO );
    aResult = wxString::Format( "Imported %d symbols on a new child sheet with %d sheet pins. "
              "Existing circuitry preserved; conflicting references renumbered. Global labels and power nets "
              "retain their original connectivity. Inspect footprint availability and run ERC. "
              "The project-local child file remains after undo.", symbols, (int) ports.size() );
    return sheet;
}

static bool ImportAttachedSchematic( SCH_EDIT_FRAME* aFrame, const json& aFile, wxString& aResult )
{
    RequireEditable( aFrame );
    auto& schematic = aFrame->Schematic();
    auto sheet = InductivePrepareAttachedSchematic( schematic, aFile, aResult );
    auto* parentScreen = schematic.CurrentSheet().LastScreen();
    const wxString path = sheet->GetScreen()->GetFileName();
    SCH_COMMIT commit( aFrame );
    INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
    auto* imported = sheet.release();
    parentScreen->Append( imported );
    commit.Added( imported, parentScreen );
    schematic.RefreshHierarchy();
    try
    {
        SCH_IO_KICAD_SEXPR io;
        io.SaveSchematicFile( path, imported, &schematic );
        commit.Push( _( "Import Attached Schematic" ) );
        guard.Pushed();
    }
    catch( ... )
    {
        if( wxFileExists( path ) ) wxRemoveFile( path );
        throw;
    }
    aFrame->UpdateHierarchyNavigator();
    aFrame->GetCanvas()->Refresh();
    return true;
}

// A circuit edit is prepared and checked without touching the live document.
struct INDUCTIVE_CIRCUIT_EDIT
{
    struct CHANGE { KIID originalId; std::unique_ptr<SCH_ITEM> replacement; };
    std::vector<CHANGE> changes;
    std::vector<KIID> removals;
    std::vector<std::unique_ptr<SCH_ITEM>> additions;
    std::string before;
    json summary;
};

static std::string CircuitSnapshot( SCHEMATIC& schematic, bool includeActiveSheet = true )
{
    SCH_IO_KICAD_SEXPR io;
    std::string snapshot = schematic.Project().GetProjectFullName().ToStdString();
    if( includeActiveSheet ) snapshot += schematic.CurrentSheet().Path().AsString().ToStdString();
    std::set<SCH_SCREEN*> screens;
    for( const auto& path : schematic.Hierarchy() )
    {
        if( !screens.insert( path.LastScreen() ).second ) continue;
        STRING_FORMATTER formatter;
        io.FormatSchematicToFormatter( &formatter, path.Last(), &schematic );
        snapshot += path.LastScreen()->GetFileName().ToStdString() + formatter.GetString();
    }
    return snapshot;
}

static std::unique_ptr<SCHEMATIC> CloneCircuit( SCHEMATIC& source )
{
    auto clone = std::make_unique<SCHEMATIC>( &source.Project() );
    SCH_IO_KICAD_SEXPR io;
    std::map<SCH_SCREEN*, SCH_SCREEN*> screens;
    std::function<std::unique_ptr<SCH_SHEET>( SCH_SHEET* )> copy = [&]( SCH_SHEET* sheet ) {
        auto result = std::make_unique<SCH_SHEET>( *sheet );
        // The copy constructor shares screens: replace before doing any work.
        if( screens.count( sheet->GetScreen() ) )
        {
            result->SetScreen( screens.at( sheet->GetScreen() ) );
            return result;
        }
        auto parsed = std::make_unique<SCH_SHEET>();
        parsed->SetScreen( new SCH_SCREEN( clone.get() ) );
        STRING_FORMATTER formatter;
        io.FormatSchematicToFormatter( &formatter, sheet, &source );
        STRING_LINE_READER reader( formatter.GetString(), "Circuit edit preview" );
        SCH_IO_KICAD_SEXPR_PARSER parser( &reader );
        parser.ParseSchematic( parsed.get() );
        auto* screen = parsed->GetScreen();
        screen->SetFileName( sheet->GetScreen()->GetFileName() );
        result->SetScreen( screen );
        screens[sheet->GetScreen()] = screen;
        const auto& definitions = screen->GetLibSymbols();
        for( auto* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            auto it = definitions.find( symbol->GetSchSymbolLibraryName() );
            if( it == definitions.end() ) throw std::runtime_error( "A live symbol is missing its library definition." );
            symbol->SetLibSymbol( new LIB_SYMBOL( *it->second ) );
        }
        for( auto* item : screen->Items().OfType( SCH_SHEET_T ) )
        {
            auto* child = static_cast<SCH_SHEET*>( item );
            SCH_SHEET* original = nullptr;
            for( auto* old : sheet->GetScreen()->Items().OfType( SCH_SHEET_T ) )
                if( old->m_Uuid == child->m_Uuid ) original = static_cast<SCH_SHEET*>( old );
            if( !original || !original->GetScreen() ) throw std::runtime_error( "A child sheet is missing. Open it before editing." );
            auto copied = copy( original );
            child->SetScreen( copied->GetScreen() );
        }
        return result;
    };
    std::vector<std::unique_ptr<SCH_SHEET>> owners;
    std::vector<SCH_SHEET*> roots;
    for( auto* root : source.GetTopLevelSheets() )
    { owners.push_back( copy( root ) ); roots.push_back( owners.back().get() ); }
    clone->SetTopLevelSheets( roots );
    for( auto& owner : owners ) owner.release();
    for( auto& path : clone->Hierarchy() )
        if( path.Path() == source.CurrentSheet().Path() ) clone->SetCurrentSheet( path );
    if( clone->CurrentSheet().empty() ) throw std::runtime_error( "Cannot stage the current sheet." );
    return clone;
}

static std::map<std::string, std::string> CircuitPinNets( SCHEMATIC& schematic )
{
    auto hierarchy = schematic.BuildSheetListSortedByPageNumbers();
    for( auto& path : hierarchy ) path.UpdateAllScreenReferences();
    schematic.ConnectionGraph()->Recalculate( hierarchy, true );
    std::map<std::string, std::string> result;
    for( auto& path : hierarchy )
        for( auto* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            // Parts' pins only: a power symbol or PWR_FLAG marks a net, it is not on the board.
            if( symbol->IsPower() )
                continue;
            for( auto* pin : symbol->GetPins( &path ) )
            {
                auto* connection = pin->Connection( &path );
                const std::string key = path.Path().AsString().ToStdString() + ":"
                                        + symbol->GetRef( &path, false ).ToStdString() + "." + pin->GetNumber().ToStdString();
                result[key] = connection ? connection->Name().ToStdString() : "";
            }
        }
    return result;
}

json InductiveReviewSnapshot(SCHEMATIC& schematic)
{
    using INDUCTIVE_REVIEW::Hash;
    auto hierarchy=schematic.BuildSheetListSortedByPageNumbers();
    for(auto& path:hierarchy) path.UpdateAllScreenReferences();
    schematic.ConnectionGraph()->Recalculate(hierarchy,true);
    std::map<std::string,json> grouped; std::map<std::string,std::set<int>> units;
    size_t pins=0;
    for(auto& path:hierarchy) for(auto* item:path.LastScreen()->Items().OfType(SCH_SYMBOL_T))
    {
        auto* symbol=static_cast<SCH_SYMBOL*>(item); const auto ref=symbol->GetRef(&path,false).ToStdString();
        const auto value=symbol->GetValue(true,&path,false).ToStdString();
        const auto footprint=symbol->GetFootprintFieldText(true,&path,false).ToStdString();
        const auto lib=symbol->GetLibId().Format().wx_str().ToStdString();
        std::set<std::string> partNumbers;
        for(const auto& field:symbol->GetFields()) {
            const auto name=INDUCTIVE_REVIEW::MatchKey(field.GetName().ToStdString());
            if(name=="MPN" || name=="MANUFACTURER PART NUMBER" || name=="MANUFACTURER_PART_NUMBER" || name=="MFR PART NUMBER") {
                const auto text=field.GetShownText(&path,false).ToStdString();
                if(!INDUCTIVE_REVIEW::MatchKey(text).empty())partNumbers.insert(text);
            }
        }
        const std::string mpn=partNumbers.size()==1?*partNumbers.begin():"";
        if(!grouped.count(ref)) grouped[ref]={{"reference",ref},{"value",value},{"lib_id",lib},{"footprint",footprint},
            {"mpn",mpn},{"mpn_ambiguous",partNumbers.size()>1},
            {"excluded",symbol->GetExcludedFromBoard()},{"ambiguous",ref.empty() || ref.find('?')!=std::string::npos},
            {"instances",json::array()},{"pins",json::array()},{"pads",json::array()},{"footprint_error",""}};
        auto& c=grouped[ref];
        if(!units[ref].insert(symbol->GetUnit()).second || c["value"]!=value || c["footprint"]!=footprint || c["lib_id"]!=lib || c["mpn"]!=mpn || partNumbers.size()>1) c["ambiguous"]=true;
        c["instances"].push_back({{"sheet",path.Path().AsString().ToStdString()},{"sheet_name",SheetName(path).ToStdString()},{"uuid",symbol->m_Uuid.AsString().ToStdString()},{"unit",symbol->GetUnit()}});
        for(auto* pin:symbol->GetPins(&path))
        {
            if(++pins>10000) throw std::runtime_error("This review supports up to 10,000 schematic pins. Split the project for review.");
            auto* connection=pin->Connection(&path);
            const std::string net=connection && connection->IsNet()?connection->Name().ToStdString():"";
            const auto number=pin->GetNumber().ToStdString(); bool existing=false;
            for(const auto& old:c["pins"]) if(old["number"]==number) {existing=true;if(old["net"]!=net)c["ambiguous"]=true;}
            if(!existing) c["pins"].push_back({{"number",number},{"name",pin->GetName().ToStdString()},{"net",net}});
        }
        if(grouped.size()>1000) throw std::runtime_error("This review supports up to 1,000 physical components.");
    }
    json components=json::array();
    for(auto& [ref,c]:grouped)
    {
        auto& instances=c["instances"];std::sort(instances.begin(),instances.end(),[](const json& a,const json& b){return a.dump()<b.dump();});
        auto& pinlist=c["pins"];std::sort(pinlist.begin(),pinlist.end(),[](const json& a,const json& b){return a["number"]<b["number"];});
        json ids=json::array();for(const auto& i:instances) ids.push_back(json::array({i["sheet"],i["uuid"]}));
        c["id"]=Hash(ids.dump());
        auto identity=c;identity.erase("pads");identity.erase("footprint_error");
        for(auto& p:identity["pins"])p.erase("net");
        c["identity"]=Hash(identity.dump());
        if(!c["excluded"].get<bool>())
        {
            try
            {
                if(c["footprint"].get<std::string>().empty()) throw std::runtime_error("No footprint is assigned.");
                const auto path=FootprintFile(wxString::FromUTF8(c["footprint"].get<std::string>()));
                wxFFile file(path,"rb");wxString text;
                if(!file.IsOpened() || file.Length()>1024*1024 || !file.ReadAll(&text,wxConvUTF8)) throw std::runtime_error("Footprint source is missing or larger than 1 MiB.");
                auto summary=INDUCTIVE_ATTACHMENTS::Inspect({{"name",wxFileName(path).GetFullName().ToStdString()},{"text",text.ToStdString()}});
                c["pads"]=summary["pads"]; c["footprint_sha256"]=Hash(text.ToStdString());
            }
            catch(const KI_PARAM_ERROR& e) {c["footprint_error"]=e.What().ToStdString();}
            catch(const IO_ERROR& e) {c["footprint_error"]=e.What().ToStdString();}
            catch(const std::exception& e) {c["footprint_error"]=e.what();}
        }
        components.push_back(c);
    }
    // Full hierarchy serialization invalidates a review even for manual edits outside modeled pins.
    SCH_IO_KICAD_SEXPR io; std::string serialized;std::set<SCH_SCREEN*> screens;
    for(const auto& path:hierarchy) if(screens.insert(path.LastScreen()).second)
    {STRING_FORMATTER f;io.FormatSchematicToFormatter(&f,path.Last(),&schematic);serialized+=f.GetString();}
    return {{"project",schematic.Project().GetProjectFullName().ToStdString()},{"fingerprint",Hash(serialized+components.dump())},{"components",components}};
}

// Render the actual native staged geometry, never an AI reconstruction. Both
// images share a viewport so changes can be compared without moving the camera.
static json CircuitVisual(SCHEMATIC& before, SCHEMATIC& after, const json& diff)
{
    BOX2I bounds;bool first=true;
    for(auto* schematic:{&before,&after})for(auto* item:schematic->GetCurrentScreen()->Items()) {
        const auto box=item->GetBoundingBox();
        if(first){bounds=box;first=false;}else bounds.Merge(box);
    }
    if(first)throw std::runtime_error("No schematic geometry to preview.");
    bounds.Inflate(schIUScale.mmToIU(5));
    PAGE_INFO page(PAGE_SIZE_TYPE::User);page.SetWidthMM(schIUScale.IUTomm(bounds.GetWidth()));page.SetHeightMM(schIUScale.IUTomm(bounds.GetHeight()));
    json visual={{"renderer","KiCad SVG"},{"width",page.GetWidthMils()},{"height",page.GetHeightMils()}};
    std::set<std::string> changed;for(const auto& row:diff)changed.insert(row.at("reference"));
    for(auto* schematic:{&before,&after}) {
        const bool isBefore=schematic==&before;
        const wxString temporary=wxFileName::CreateTempFileName("inductive-preview-");
        struct REMOVE {wxString path;~REMOVE(){wxRemoveFile(path);}} cleanup{temporary};
        SCH_RENDER_SETTINGS settings;settings.SetDefaultPenWidth(schIUScale.mmToIU(0.15));
        SVG_PLOTTER plotter;plotter.SetRenderSettings(&settings);plotter.SetPageSettings(page);
        plotter.SetColorMode(false);plotter.SetViewport(bounds.GetOrigin(),schIUScale.IU_PER_MILS/10,1.0,false);
        plotter.SetCreator("Inductive native change preview");
        if(!plotter.OpenFile(temporary))throw std::runtime_error("Could not render schematic preview.");
        plotter.StartPlot(schematic->CurrentSheet().GetPageNumber());
        SCH_PLOT_OPTS options;options.m_plotAll=false;options.m_plotDrawingSheet=false;options.m_blackAndWhite=true;
        schematic->GetCurrentScreen()->Plot(&plotter,options);
        plotter.EndPlot();
        wxFFile input(temporary,"rb");wxString text;
        if(!input.IsOpened() || input.Length()>900*1024 || !input.ReadAll(&text,wxConvUTF8))
            throw std::runtime_error("This sheet is too large for an inline image. Use the native diff and canvas.");
        const auto bytes=text.ToUTF8();json markers=json::array();
        for(auto* item:schematic->GetCurrentScreen()->Items().OfType(SCH_SYMBOL_T)) {
            auto* symbol=static_cast<SCH_SYMBOL*>(item);auto& path=schematic->CurrentSheet();
            const auto ref=symbol->GetRef(&path,false).ToStdString();if(!changed.count(ref))continue;
            auto box=symbol->GetBoundingBox();box.Inflate(schIUScale.mmToIU(1));
            const double width=page.GetWidthMils()*schIUScale.IU_PER_MILS,height=page.GetHeightMils()*schIUScale.IU_PER_MILS;
            markers.push_back({{"reference",ref},{"x",(box.GetX()-bounds.GetX())/width},{"y",(box.GetY()-bounds.GetY())/height},
                {"width",box.GetWidth()/width},{"height",box.GetHeight()/height}});
        }
        visual[isBefore?"before":"after"]={{"image","data:image/svg+xml;base64,"+wxBase64Encode(bytes.data(),bytes.length()).ToStdString()},{"markers",markers}};
    }
    return visual;
}

static void ApplyEditToStage( SCHEMATIC& staged, const INDUCTIVE_CIRCUIT_EDIT& plan )
{
    auto* screen = staged.CurrentSheet().LastScreen();
    auto find = [&]( const KIID& id ) -> SCH_ITEM* {
        for( auto* item : screen->Items() ) if( item->m_Uuid == id ) return item;
        throw std::runtime_error( "Circuit preview could not find an item." );
    };
    for( const auto& original : plan.removals )
    { auto* item = find( original ); screen->Remove( item ); delete item; }
    for( const auto& change : plan.changes )
    {
        auto* original = find( change.originalId );
        screen->Remove( original ); delete original;
        auto* replacement = static_cast<SCH_ITEM*>( change.replacement->Clone() );
        screen->Append( replacement );
    }
    for( const auto& item : plan.additions ) screen->Append( static_cast<SCH_ITEM*>( item->Clone() ) );
    staged.RefreshHierarchy();
}

json InductiveSchematicSelection( SCHEMATIC& schematic, const std::vector<SCH_ITEM*>& selected )
{
    auto& sheet = schematic.CurrentSheet();
    json result = {{"sheet", sheet.Path().AsString().ToStdString()},
                   {"sheet_name", SheetName( sheet ).ToStdString()},
                   {"items", json::array()}, {"selected_count", selected.size()}, {"truncated", false}};
    std::set<SCH_ITEM*> live;
    for( auto* item : sheet.LastScreen()->Items() ) live.insert( item );
    std::set<KIID> seen;
    for( auto* item : selected )
    {
        if( !item ) continue;
        auto* symbol = dynamic_cast<SCH_SYMBOL*>( item );
        if( !symbol && ( item->Type() == SCH_FIELD_T || item->Type() == SCH_PIN_T ) )
            symbol = dynamic_cast<SCH_SYMBOL*>( item->GetParent() );
        SCH_ITEM* owner = symbol ? static_cast<SCH_ITEM*>( symbol ) : item;
        if( !live.count( owner ) || !seen.insert( owner->m_Uuid ).second ) continue;
        if( result["items"].size() >= 60 ) { result["truncated"] = true; break; }
        json entry = {{"uuid", owner->m_Uuid.AsString().ToStdString()},
                      {"x", ToMm( owner->GetPosition().x )}, {"y", ToMm( owner->GetPosition().y )}};
        if( symbol )
        {
            entry["kind"] = "symbol";
            entry["reference"] = symbol->GetRef( &sheet, false ).ToStdString();
            entry["value"] = symbol->GetValue( true, &sheet, false ).ToStdString();
            entry["lib_id"] = symbol->GetLibId().Format().wx_str().ToStdString();
            entry["footprint"] = symbol->GetFootprintFieldText( true, &sheet, false ).ToStdString();
        }
        else
        {
            entry["kind"] = "other";
            if( auto* line = dynamic_cast<SCH_LINE*>( item ); line && line->IsWire() )
            {
                entry["kind"] = "wire";
                entry["end_x"] = ToMm( line->GetEndPoint().x );
                entry["end_y"] = ToMm( line->GetEndPoint().y );
            }
        }
        result["items"].push_back( entry );
    }
    if( result["items"].empty() ) result["message"] = "Nothing is selected on the active sheet. Ask the user to select a component or name it.";
    return result;
}

std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT> InductivePreviewCircuitEdit( SCHEMATIC& schematic, const json& aArgs, bool aVisual )
{
    if( !aArgs.is_object() ) throw std::runtime_error( "Circuit edit must be an object." );
    json args = aArgs;
    auto plan = std::make_shared<INDUCTIVE_CIRCUIT_EDIT>();
    plan->before = CircuitSnapshot( schematic );
    plan->summary = { { "title", args.value( "title", "Change circuit" ) },
                      { "changes", json::array() }, { "diff", json::array() }, { "sheet", SheetName( schematic.CurrentSheet() ).ToStdString() },
                      { "pcb", "PCB placement and copper are unchanged. Publish and sync separately after reviewing this schematic edit." } };
    auto& path = schematic.CurrentSheet();
    auto* screen = path.LastScreen();
    if( !screen ) throw std::runtime_error( "No current sheet." );
    const POWER_STYLE power = MakePowerStyle( &schematic, path );
    std::map<wxString, SCH_SYMBOL*> symbols;
    // A reference on more than one sheet, or one unit twice, is ambiguous. A multi-unit
    // part's units (a dual pot's A and B) are one component: its value, footprint,
    // removal and free pins can be edited together (design 16's pot could not be).
    std::set<wxString> repeated;
    std::map<wxString, std::vector<SCH_SYMBOL*>> unitsOf;   // on the current sheet
    {
        std::map<wxString, std::set<std::pair<wxString, int>>> seen;   // ref -> (sheet, unit)
        std::map<wxString, std::set<wxString>> sheetsOf;
        for( const auto& p : schematic.Hierarchy() )
            for( auto* item : p.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            {
                auto* symbol = static_cast<SCH_SYMBOL*>( item );
                const auto ref = symbol->GetRef( &p, false );
                const wxString sheet = p.Path().AsString();
                if( !seen[ref].insert( { sheet, symbol->GetUnitSelection( &p ) } ).second ) repeated.insert( ref );
                sheetsOf[ref].insert( sheet );
                if( sheetsOf[ref].size() > 1 ) repeated.insert( ref );
                if( !symbols.count( ref ) || p.Path() == path.Path() ) symbols[ref] = symbol;
                if( p.Path() == path.Path() ) unitsOf[ref].push_back( symbol );
            }
    }
    // An addition naming an existing pin ("wire the new sensor to U1.GPIO4")
    // connects that pin: it is not a pin of the new parts.
    if( args.contains( "add" ) && args["add"].is_object() && args["add"].contains( "nets" ) && args["add"]["nets"].is_array() )
    {
        std::set<std::string> added;
        for( const auto& part : args["add"].value( "components", json::array() ) )
            if( part.is_object() && part.contains( "reference" ) && part["reference"].is_string() )
                added.insert( part["reference"].get<std::string>() );
        if( !args.contains( "connects" ) ) args["connects"] = json::array();
        json kept = json::array();
        for( auto net : args["add"]["nets"] )
        {
            if( !net.is_object() || !net.contains( "pins" ) || !net["pins"].is_array() || !net.contains( "name" ) ) { kept.push_back( net ); continue; }
            json pinsLeft = json::array();
            for( const auto& pin : net["pins"] )
            {
                const std::string text = pin.is_string() ? pin.get<std::string>() : "";
                const auto dot = text.rfind( '.' );
                const std::string ref = dot == std::string::npos ? "" : text.substr( 0, dot );
                if( !ref.empty() && !added.count( ref ) && symbols.count( wxString::FromUTF8( ref ) ) && args["connects"].is_array() )
                    args["connects"].push_back( { { "pin", text }, { "net", net["name"] } } );
                else
                    pinsLeft.push_back( pin );
            }
            if( pinsLeft.empty() ) continue;
            net["pins"] = pinsLeft;
            kept.push_back( net );
        }
        args["add"]["nets"] = kept;
        if( args["connects"].empty() ) args.erase( "connects" );
        if( kept.empty() && args["add"].value( "components", json::array() ).empty() ) args.erase( "add" );
    }
    std::set<wxString> targeted;
    auto multiUnit = [&]( const wxString& aRef ) { return unitsOf.count( aRef ) && unitsOf[aRef].size() > 1; };
    auto get = [&]( const json& change ) {
        auto ref = ARGS::RequireString( change, "reference" );
        if( !symbols.count( ref ) ) throw std::runtime_error( "No component " + ref.ToStdString() );
        if( repeated.count( ref ) )
            throw std::runtime_error( "Edit one component at a time; a reference on several sheets requires manual review." );
        bool here = false;
        for( auto* item : screen->Items().OfType( SCH_SYMBOL_T ) ) if( item == symbols[ref] ) here = true;
        if( !here ) throw std::runtime_error( "Open the sheet containing " + ref.ToStdString() + " before editing it." );
        if( !targeted.insert( ref ).second ) throw std::runtime_error( "Component is edited more than once: " + ref.ToStdString() );
        return symbols[ref];
    };
    auto array = [&]( const char* name ) {
        auto value = args.value( name, json::array() );
        if( !value.is_array() || value.size() > 48 ) throw std::runtime_error( "Each edit operation array is limited to 48 entries." );
        return value;
    };
    for( const auto& change : array( "updates" ) )
    {
        auto* original = get( change );
        // The other units of a multi-unit part get the same value and footprint.
        for( SCH_SYMBOL* unit : unitsOf[original->GetRef( &path, false )] )
        {
            if( unit == original ) continue;
            auto copy = std::make_unique<SCH_SYMBOL>( *unit );
            if( change.contains( "value" ) ) copy->SetValueFieldText( ARGS::RequireString( change, "value" ) );
            if( change.contains( "footprint" ) )
            {
                ValidateFootprint( copy.get(), path, ARGS::RequireString( change, "footprint" ) );
                copy->SetFootprintFieldText( ARGS::RequireString( change, "footprint" ) );
            }
            plan->changes.push_back( { unit->m_Uuid, std::move( copy ) } );
        }
        auto edited = std::make_unique<SCH_SYMBOL>( *original );
        auto ref = original->GetRef( &path, false ).ToStdString();
        std::string description = ref;
        bool changed = false;
        if( change.contains( "value" ) )
        {
            const auto value = ARGS::RequireString( change, "value" );
            description += ": value " + original->GetValue( true, &path, false ).ToStdString() + " → " + value.ToStdString();
            plan->summary["diff"].push_back({{"kind","update"},{"reference",ref},{"field","Value"},
                {"before",original->GetValue(true,&path,false).ToStdString()},{"after",value.ToStdString()}});
            edited->SetValueFieldText( value ); changed = true;
        }
        if( change.contains( "footprint" ) )
        {
            auto fp = ARGS::RequireString( change, "footprint" );
            ValidateFootprint( edited.get(), path, fp );
            description += "; footprint " + original->GetFootprintFieldText( true, &path, false ).ToStdString() + " → " + fp.ToStdString();
            plan->summary["diff"].push_back({{"kind","update"},{"reference",ref},{"field","Footprint"},
                {"before",original->GetFootprintFieldText(true,&path,false).ToStdString()},{"after",fp.ToStdString()}});
            edited->SetFootprintFieldText( fp ); changed = true;
        }
        if( !changed ) throw std::runtime_error( "An update needs a value or footprint." );
        plan->summary["changes"].push_back( description );
        plan->changes.push_back( { original->m_Uuid, std::move( edited ) } );
    }
    std::map<std::string, std::string> pinTranslations;
    std::set<wxString> replacedOrRemoved;
    for( const auto& change : array( "replacements" ) )
    {
        auto* original = get( change );
        if( multiUnit( original->GetRef( &path, false ) ) )
            throw std::runtime_error( "Replace a multi-unit part in KiCad. edit_circuit can update its value or footprint, "
                                      "remove it, or connect its free pins." );
        auto edited = std::make_unique<SCH_SYMBOL>( *original );
        replacedOrRemoved.insert( original->GetRef( &path, false ) );
        LIB_ID id;
        auto* definition = LoadDesignSymbol( ARGS::RequireString( change, "lib_id" ), id );
        if( definition->GetUnitCount() != 1 ) throw std::runtime_error( "Replacement must be a single-unit symbol." );
        edited->SetLibSymbol( new LIB_SYMBOL( *definition ) ); edited->SetLibId( id );
        auto mapping = change.value( "pin_map", json::object() );
        if( mapping.is_array() )
        {
            json pairs = json::object();
            for( const auto& pair : mapping )
            {
                auto from = ARGS::RequireString( pair, "from" ).ToStdString();
                if( pairs.contains( from ) ) throw std::runtime_error( "An old pin appears twice in pin_map." );
                pairs[from] = ARGS::RequireString( pair, "to" ).ToStdString();
            }
            mapping = std::move( pairs );
        }
        if( !mapping.is_object() ) throw std::runtime_error( "pin_map must map old pin numbers to new pin numbers." );
        std::map<wxString, SCH_PIN*> newPins;
        for( auto* pin : edited->GetPins( &path ) ) newPins[pin->GetNumber()] = pin;
        std::set<wxString> assigned;
        for( auto* old : original->GetPins( &path ) )
        {
            const std::string number = old->GetNumber().ToStdString();
            auto newNumber = wxString::FromUTF8( mapping.value( number, number ) );
            if( !newPins.count( newNumber ) || !assigned.insert( newNumber ).second
                || old->GetPosition() != newPins[newNumber]->GetPosition() )
                throw std::runtime_error( "Replacement would move or merge pin " + number
                    + ". Choose a symbol with compatible pin anchors; arbitrary package rewiring is not automatic yet." );
        }
        for( auto* old : original->GetPins( &path ) )
        {
            const auto number = old->GetNumber().ToStdString();
            const auto prefix = path.Path().AsString().ToStdString() + ":" + original->GetRef( &path, false ).ToStdString() + ".";
            pinTranslations[prefix + number] = prefix + mapping.value( number, number );
        }
        if( assigned.size() != newPins.size() ) throw std::runtime_error( "Replacement has additional pins that need explicit circuit design." );
        if( change.contains( "value" ) ) edited->SetValueFieldText( ARGS::RequireString( change, "value" ) );
        const auto fp = ARGS::OptionalString( change, "footprint", original->GetFootprintFieldText( true, &path, false ) );
        ValidateFootprint( edited.get(), path, fp ); edited->SetFootprintFieldText( fp );
        plan->summary["changes"].push_back( original->GetRef( &path, false ).ToStdString() + ": replace "
            + std::string( original->GetLibId().Format() ) + " → " + std::string( id.Format() )
            + "; preserve reference, UUID, position and all pin anchors" );
        plan->summary["diff"].push_back({{"kind","replace"},{"reference",original->GetRef(&path,false).ToStdString()},
            {"field","Symbol"},{"before",std::string(original->GetLibId().Format())},{"after",std::string(id.Format())}});
        const auto reference=original->GetRef(&path,false).ToStdString();
        if(edited->GetValue(true,&path,false)!=original->GetValue(true,&path,false))
            plan->summary["diff"].push_back({{"kind","update"},{"reference",reference},{"field","Value"},
                {"before",original->GetValue(true,&path,false).ToStdString()},{"after",edited->GetValue(true,&path,false).ToStdString()}});
        if(fp!=original->GetFootprintFieldText(true,&path,false))
            plan->summary["diff"].push_back({{"kind","update"},{"reference",reference},{"field","Footprint"},
                {"before",original->GetFootprintFieldText(true,&path,false).ToStdString()},{"after",fp.ToStdString()}});
        for(auto it=mapping.begin();it!=mapping.end();++it) if(it.key()!=it.value().get<std::string>())
            plan->summary["diff"].push_back({{"kind","replace"},{"reference",reference},{"field","Pin mapping"},{"before",it.key()},{"after",it.value()}});

        plan->changes.push_back( { original->m_Uuid, std::move( edited ) } );
    }
    std::set<std::string> exempt;
    for( const auto& change : array( "removals" ) )
    {
        auto* original = get( change );
        replacedOrRemoved.insert( original->GetRef( &path, false ) );
        const auto ref = original->GetRef( &path, false ).ToStdString();
        for( SCH_SYMBOL* unit : unitsOf[original->GetRef( &path, false )] )   // every unit of the part
        {
            plan->removals.push_back( unit->m_Uuid );
            for( auto* pin : unit->GetPins( &path ) )
                exempt.insert( path.Path().AsString().ToStdString() + ":" + ref + "." + pin->GetNumber().ToStdString() );
        }
        plan->summary["diff"].push_back({{"kind","remove"},{"reference",ref},{"field","Component"},
            {"before",original->GetValue(true,&path,false).ToStdString()},{"after","Removed; inspect retained wires and obsolete PCB footprint"}});
        plan->summary["changes"].push_back( "Remove " + ref + "; existing wires are retained and may have dangling ends" );
    }
    std::map<std::string, std::string> requestedRewires;
    for( const auto& change : array( "rewires" ) )
    {
        const wxString ref = ARGS::RequireString( change, "reference" );
        // Rewiring is independent of value updates, but not of replacement/deletion.
        if( !symbols.count( ref ) || repeated.count( ref ) ) throw std::runtime_error( "Ambiguous or missing rewire reference." );
        if( multiUnit( ref ) )
            throw std::runtime_error( "Rewire a multi-unit part in KiCad; edit_circuit can connect its free pins (connects)." );
        auto* symbol = symbols[ref];
        bool here = false;
        for( auto* item : screen->Items().OfType( SCH_SYMBOL_T ) ) if( item == symbol ) here = true;
        if( !here || symbol->GetUnitCount() != 1 || replacedOrRemoved.count( ref ) )
            throw std::runtime_error( "Rewire a single-unit symbol on this sheet, separately from replacement or removal." );
        SCH_PIN* pin = nullptr;
        for( auto* p : symbol->GetPins( &path ) ) if( p->GetNumber() == ARGS::RequireString( change, "pin" ) ) pin = p;
        if( !pin ) throw std::runtime_error( "Unknown rewire pin." );
        const auto name = ARGS::RequireString( change, "net" );
        wxRegEx safeName( "^[A-Za-z_][A-Za-z0-9_+.-]*$" );
        if( name.size() > 40 || !safeName.Matches( name ) ) throw std::runtime_error( "Use a short net label." );
        SCH_LINE* branch = nullptr;
        SCH_LABEL* label = nullptr;
        const auto start = pin->GetPosition();
        VECTOR2I end;
        for( auto* item : screen->Items().OfType( SCH_LINE_T ) )
        {
            auto* wire = static_cast<SCH_LINE*>( item );
            if( !wire->IsWire() ) continue;
            if( wire->GetStartPoint() == start || wire->GetEndPoint() == start )
            {
                if( branch ) throw std::runtime_error( "This pin has branching wires. Rewire it manually." );
                branch = wire; end = wire->GetStartPoint() == start ? wire->GetEndPoint() : wire->GetStartPoint();
            }
        }
        if( !branch ) throw std::runtime_error( "Automatic rewiring requires an isolated local labelled wire stub." );
        for( auto* item : screen->Items() )
        {
            if( item == branch || item == symbol ) continue;
            if( item->Type() == SCH_LABEL_T && item->GetPosition() == end )
            { if( label ) throw std::runtime_error( "Multiple labels at this pin." ); label = static_cast<SCH_LABEL*>( item ); continue; }
            if( item->Type() == SCH_LINE_T )
            {
                auto* wire = static_cast<SCH_LINE*>( item );
                if( wire->IsWire() && ( IsPointOnSegment( wire->GetStartPoint(), wire->GetEndPoint(), start )
                    || IsPointOnSegment( wire->GetStartPoint(), wire->GetEndPoint(), end ) ) )
                    throw std::runtime_error( "The labelled branch is shared. Rewire it manually." );
            }
            if( item->Type() == SCH_JUNCTION_T && IsPointOnSegment( start, end, item->GetPosition() ) )
                throw std::runtime_error( "The wire has a junction. Rewire it manually." );
            if( item->Type() == SCH_SYMBOL_T )
                for( auto* p : static_cast<SCH_SYMBOL*>( item )->GetPins( &path ) )
                    if( IsPointOnSegment( start, end, p->GetPosition() ) )
                        throw std::runtime_error( "Another pin shares this branch. Rewire it manually." );
        }
        if( !label ) throw std::runtime_error( "The pin has no isolated local label." );
        for( const auto& old : plan->changes ) if( old.originalId == label->m_Uuid ) throw std::runtime_error( "Pin rewired more than once." );
        auto edited = std::make_unique<SCH_LABEL>( *label ); edited->SetText( name );
        plan->summary["changes"].push_back( ref.ToStdString() + "." + pin->GetNumber().ToStdString() + ": "
                                           + label->GetText().ToStdString() + " → " + name.ToStdString() );
        plan->changes.push_back( { label->m_Uuid, std::move( edited ) } );
        const auto key = path.Path().AsString().ToStdString() + ":" + ref.ToStdString() + "." + pin->GetNumber().ToStdString();
        exempt.insert( key ); requestedRewires[key] = name.ToStdString();
    }
    // Connect an existing pin that is free or only carries a no-connect flag:
    // the flag goes, and a labelled stub joins the pin to the named net.
    std::map<std::string, std::string> requestedConnects;
    std::map<std::string, std::string> connectedBefore;
    for( const auto& change : array( "connects" ) )
    {
        const wxString member = ARGS::RequireString( change, "pin" );
        const wxString name = ARGS::RequireString( change, "net" );
        wxRegEx safeName( "^[A-Za-z_][A-Za-z0-9_+.-]*$" );
        if( name.size() > 40 || !safeName.Matches( name ) ) throw std::runtime_error( "Use a short net label." );
        const wxString ref = member.BeforeLast( '.' );
        if( ref.IsEmpty() || !symbols.count( ref ) || repeated.count( ref ) || replacedOrRemoved.count( ref ) )
            throw std::runtime_error( "No single existing component for " + member.ToStdString()
                                      + ", or it is also being replaced or removed." );
        DESIGN_PINS pins;
        PIN_NAMES names;
        for( auto* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            if( symbol->GetRef( &path, false ) != ref ) continue;
            for( auto* pin : symbol->GetPins( &path ) )
            {
                const wxString key = ref + "." + pin->GetNumber();
                if( pins[key].empty() ) IndexPinName( names, ref, pin->GetShownName(), key );
                pins[key].push_back( pin );
            }
        }
        if( pins.empty() ) throw std::runtime_error( "Open the sheet containing " + ref.ToStdString() + " before editing it." );
        const auto keys = ResolvePin( member, pins, names );
        if( keys.size() != 1 ) throw std::runtime_error( "Connect one pin at a time, by number: " + member.ToStdString() );
        const auto key = path.Path().AsString().ToStdString() + ":" + keys.front().ToStdString();
        if( requestedConnects.count( key ) ) throw std::runtime_error( "Pin connected more than once: " + member.ToStdString() );
        std::string was = "Unconnected";
        for( SCH_PIN* pin : pins.at( keys.front() ) )
        {
            const VECTOR2I at = pin->GetPosition();
            for( auto* item : screen->Items() )
            {
                if( item == pin->GetParentSymbol() ) continue;
                bool touches = false;
                if( item->Type() == SCH_NO_CONNECT_T && item->GetPosition() == at )
                {
                    plan->removals.push_back( item->m_Uuid );
                    was = "No connect";
                    continue;
                }
                if( item->Type() == SCH_LINE_T )
                {
                    auto* line = static_cast<SCH_LINE*>( item );
                    touches = line->IsWire() && IsPointOnSegment( line->GetStartPoint(), line->GetEndPoint(), at );
                }
                else if( item->Type() == SCH_SYMBOL_T )
                {
                    for( auto* other : static_cast<SCH_SYMBOL*>( item )->GetPins( &path ) )
                        touches |= other->GetPosition() == at;
                }
                else if( dynamic_cast<SCH_LABEL_BASE*>( item ) || item->Type() == SCH_JUNCTION_T )
                {
                    touches = item->GetPosition() == at;
                }
                if( touches )
                    throw std::runtime_error( member.ToStdString() + " is already connected. Use rewires to move "
                                              "a labelled stub to another net." );
            }
        }
        std::set<wxString> assigned;
        AddNetStubs( json::array( { { { "name", name.ToStdString() }, { "pins", { keys.front().ToStdString() } } } } ),
                     pins, names, assigned, plan->additions, &power );
        exempt.insert( key );
        requestedConnects[key] = name.ToStdString();
        connectedBefore[key] = was;
        plan->summary["changes"].push_back( keys.front().ToStdString() + ": " + was + " → " + name.ToStdString() );
        plan->summary["diff"].push_back( { { "kind", "connect" }, { "reference", ref.ToStdString() },
            { "field", "Pin " + keys.front().AfterLast( '.' ).ToStdString() }, { "before", was }, { "after", name.ToStdString() } } );
    }
    if( args.contains( "add" ) )
    {
        wxString result;
        for( auto& item : ConstructCircuit( &schematic, args.at( "add" ), result, &power ) )
            plan->additions.push_back( std::move( item ) );
        for( const auto& part : args.at( "add" ).at( "components" ) )
            plan->summary["changes"].push_back( "Add " + part.at( "reference" ).get<std::string>() + " " + part.at( "lib_id" ).get<std::string>() );
        for( const auto& net : args.at( "add" ).at( "nets" ) )
            plan->summary["changes"].push_back( "Connect added pins to " + net.at( "name" ).get<std::string>() );
    }
    // A PWR_FLAG on a supply net already drawn, by the builder's rule: a connector or a
    // battery feeds it (on the net, or as SpreadSupply finds) and no power output drives
    // it. With no way to add one, a model added test points until ERC passed.
    std::map<wxString, std::pair<wxString, wxString>> flagFor;   // net -> (flag reference, a part pin on it)
    if( !array( "power_flags" ).empty() )
    {
        auto probe = CloneCircuit( schematic );
        CircuitPinNets( *probe );   // connectivity
        SCH_SHEET_PATH& sheet = probe->CurrentSheet();
        DESIGN_PINS pins;
        std::map<wxString, wxString> netOfPin;
        std::map<SCH_SYMBOL*, std::vector<wxString>> keysOf;
        std::set<wxString> driven, fed, flagged;
        for( auto* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                SCH_CONNECTION* connection = pin->Connection( &sheet );
                if( !connection ) continue;
                const wxString net = connection->Name( true );
                if( symbol->IsPower() )
                {
                    if( symbol->GetLibId().GetUniStringLibItemName() == "PWR_FLAG" ) flagged.insert( net );
                    continue;
                }
                const wxString key = symbol->GetRef( &sheet, false ) + "." + pin->GetNumber();
                if( pins[key].empty() ) keysOf[symbol].push_back( key );
                pins[key].push_back( pin );
                netOfPin[key] = net;
                if( pin->GetType() == ELECTRICAL_PINTYPE::PT_POWER_OUT ) driven.insert( net );
                if( IsSupplySource( symbol ) ) fed.insert( net );
            }
        }
        for( const wxString& net : driven ) fed.erase( net );
        fed = SpreadSupply( fed, keysOf, netOfPin, driven, pins, sheet );
        std::set<wxString> references;
        for( const auto& [ref, symbol] : symbols ) references.insert( ref );
        for( const auto& item : plan->additions )
            if( item->Type() == SCH_SYMBOL_T ) references.insert( static_cast<SCH_SYMBOL*>( item.get() )->GetRef( &path, false ) );
        BOX2I all;
        bool first = true;
        for( auto* item : screen->Items() )
        {
            if( first ) all = item->GetBoundingBox();
            else all.Merge( item->GetBoundingBox() );
            first = false;
        }
        for( const auto& entry : array( "power_flags" ) )
        {
            if( !entry.is_string() ) throw std::runtime_error( "power_flags lists net names, e.g. [\"VCC\"]." );
            const wxString name = wxString::FromUTF8( entry.get<std::string>() );
            wxString member;
            for( const auto& [key, net] : netOfPin )
                if( net == name ) { member = key; break; }
            if( member.IsEmpty() ) throw std::runtime_error( "No part pin is on a net " + name.ToStdString() + " on this sheet." );
            if( flagged.count( name ) || flagFor.count( name ) ) throw std::runtime_error( name.ToStdString() + " already has a PWR_FLAG." );
            if( driven.count( name ) )
                throw std::runtime_error( "A power output on the board drives " + name.ToStdString()
                                          + ": it needs no flag, and one would hide a conflict." );
            if( !fed.count( name ) )
                throw std::runtime_error( "No connector or battery feeds " + name.ToStdString() + " (on the net, or through a "
                                          "switch, diode, fuse, ferrite, inductor, resistor or MOSFET), so a flag would hide "
                                          "a missing supply. Connect its source, or add the regulator it needs." );
            // The flag joins by name: a local label or power symbol of that name must already be there.
            bool named = false;
            for( auto* item : screen->Items() )
            {
                if( item->Type() == SCH_LABEL_T ) named |= static_cast<SCH_LABEL*>( item )->GetText() == name;
                else if( item->Type() == SCH_SYMBOL_T && static_cast<SCH_SYMBOL*>( item )->IsPower() )
                    named |= static_cast<SCH_SYMBOL*>( item )->GetValue( true, &path, false ) == name;
            }
            if( !named )
                throw std::runtime_error( "Net " + name.ToStdString() + " has no local label or power symbol on this sheet "
                                          "for a flag to join; put one on a pin of it with add_label first." );
            LIB_ID id;
            auto* lib = LoadDesignSymbol( "power:PWR_FLAG", id );
            const VECTOR2I at( all.GetLeft() + ToIU( 20 * (double) flagFor.size() ), all.GetTop() - ToIU( 12.7 ) );
            auto flag = std::make_unique<SCH_SYMBOL>( *lib, id, &path, 1, 0, SnapToGrid( at ) );
            wxString ref;
            int n = 0;
            do { ref = wxString::Format( "#FLG%03d", ++n ); } while( references.count( ref ) );
            references.insert( ref );
            flag->SetRef( &path, ref );
            const VECTOR2I flagPin = flag->GetPins( &path ).front()->GetPosition();
            plan->additions.push_back( std::move( flag ) );
            if( std::unique_ptr<SCH_SYMBOL> symbol = power.wants( name, true ) ? power.make( name, flagPin, VECTOR2I( 0, 1 ) ) : nullptr )
            {
                plan->additions.push_back( std::move( symbol ) );
            }
            else
            {
                auto label = std::make_unique<SCH_LABEL>( flagPin, name );
                label->SetTextSize( VECTOR2I( SCH_GRID_IU, SCH_GRID_IU ) );
                plan->additions.push_back( std::move( label ) );
            }
            flagFor[name] = { ref, member };
            plan->summary["changes"].push_back( "Flag " + name.ToStdString() + " as supplied from its connector or battery" );
        }
    }
    if( plan->changes.empty() && plan->removals.empty() && plan->additions.empty() ) throw std::runtime_error( "The edit has no changes." );
    auto staged = CloneCircuit( schematic );
    const auto before = CircuitPinNets( *staged );
    ApplyEditToStage( *staged, *plan );
    const auto after = CircuitPinNets( *staged );
    // Each flag is on its net, not on a new one of the same name.
    for( const auto& [net, flag] : flagFor )
    {
        wxString flagNet, partNet;
        SCH_SHEET_PATH& sheet = staged->CurrentSheet();
        for( auto* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            const wxString ref = symbol->GetRef( &sheet, false );
            for( SCH_PIN* pin : symbol->GetPins( &sheet ) )
            {
                SCH_CONNECTION* connection = pin->Connection( &sheet );
                if( ref == flag.first && connection ) flagNet = connection->Name();
                if( ref + "." + pin->GetNumber() == flag.second && connection ) partNet = connection->Name();
            }
        }
        if( flagNet.IsEmpty() || flagNet != partNet )
            throw std::runtime_error( "The flag for " + net.ToStdString() + " did not join it. Name the net with add_label "
                                      "on one of its pins, then flag it." );
    }
    int preserved = 0;
    for( const auto& [key, net] : before )
    {
        if( exempt.count( key ) ) continue;
        const auto newKey = pinTranslations.count( key ) ? pinTranslations.at( key ) : key;
        if( !after.count( newKey ) || after.at( newKey ) != net )
            throw std::runtime_error( "This change would alter an unrelated pin's net: " + key + ". Adjust the plan or placement." );
        ++preserved;
    }
    auto localName = [&]( const std::string& key ) -> std::string {
        for( auto* item : staged->CurrentSheet().LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            auto* symbol = static_cast<SCH_SYMBOL*>( item );
            for( auto* pin : symbol->GetPins( &staged->CurrentSheet() ) )
                if( staged->CurrentSheet().Path().AsString().ToStdString() + ":" + symbol->GetRef( &staged->CurrentSheet(), false ).ToStdString()
                    + "." + pin->GetNumber().ToStdString() == key )
                {
                    auto* connection = pin->Connection( &staged->CurrentSheet() );
                    return connection ? connection->Name( true ).ToStdString() : "";
                }
        }
        return "";
    };
    for( const auto& [key, requested] : requestedRewires )
        if( localName( key ) != requested ) throw std::runtime_error( "Rewired pin does not reach its requested net: " + key );
    for( const auto& [key, requested] : requestedConnects )
        if( localName( key ) != requested ) throw std::runtime_error( "Connected pin does not reach its requested net: " + key );
    // Added pins as the plan wrote them (number or name) -> numbered keys on the staged sheet.
    DESIGN_PINS stagedPins;
    PIN_NAMES stagedNames;
    for( auto* item : staged->CurrentSheet().LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        auto* symbol = static_cast<SCH_SYMBOL*>( item );
        const wxString ref = symbol->GetRef( &staged->CurrentSheet(), false );
        for( auto* pin : symbol->GetPins( &staged->CurrentSheet() ) )
        {
            const wxString key = ref + "." + pin->GetNumber();
            if( stagedPins[key].empty() ) IndexPinName( stagedNames, ref, pin->GetShownName(), key );
            stagedPins[key].push_back( pin );
        }
    }
    // New pin nets must match what was requested, so geometric collisions cannot hide behind labels.
    if( args.contains( "add" ) )
        for( const auto& net : args["add"]["nets"] )
            for( const auto& p : net["pins"] )
                for( const wxString& resolved : ResolvePin( wxString::FromUTF8( p.get<std::string>() ), stagedPins, stagedNames ) )
                {
                    const std::string key = path.Path().AsString().ToStdString() + ":" + resolved.ToStdString();
                    if( localName( key ) != net["name"].get<std::string>() )
                        throw std::runtime_error( "New pin " + p.get<std::string>() + " does not land on its requested net." );
                }
    if( args.contains( "add" ) && args["add"].contains( "no_connect" ) )
        for( const auto& key : args["add"]["no_connect"] )
        {
            const auto text = key.get<std::string>();
            const auto resolved = ResolvePin( wxString::FromUTF8( text ), stagedPins, stagedNames );
            SCH_PIN* target = resolved.size() == 1 ? stagedPins.at( resolved.front() ).front() : nullptr;
            auto* subgraph = target ? staged->ConnectionGraph()->GetSubgraphForItem( target ) : nullptr;
            if( !subgraph || !subgraph->GetNoConnect() ) throw std::runtime_error( "New pin " + text + " is missing its no-connect marker." );
            for( auto* item : subgraph->GetItems() )
                if( item != target && item->Type() != SCH_NO_CONNECT_T )
                    throw std::runtime_error( "New no-connect pin " + text + " touches existing connectivity. Move the added component." );
        }
    for(const auto& change:array("rewires")) {
        const auto key=path.Path().AsString().ToStdString()+":"+change.at("reference").get<std::string>()+"."+change.at("pin").get<std::string>();
        plan->summary["diff"].push_back({{"kind","rewire"},{"reference",change.at("reference")},
            {"field","Pin "+change.at("pin").get<std::string>()},{"before",before.at(key)},{"after",after.at(key)}});
    }
    if(args.contains("add")) {
        for(const auto& c:args.at("add").at("components")) {
            const auto ref=c.at("reference").get<std::string>();
            for(auto* item:staged->GetCurrentScreen()->Items().OfType(SCH_SYMBOL_T)) {
                auto* symbol=static_cast<SCH_SYMBOL*>(item);auto& sheet=staged->CurrentSheet();
                if(symbol->GetRef(&sheet,false).ToStdString()!=ref)continue;
                plan->summary["diff"].push_back({{"kind","add"},{"reference",ref},{"field","Component"},
                    {"after",symbol->GetValue(true,&sheet,false).ToStdString()+" · "+symbol->GetFootprintFieldText(true,&sheet,false).ToStdString()}});
            }
        }
        for(const auto& net:args.at("add").at("nets")) for(const auto& pin:net.at("pins")) {
            const auto text=pin.get<std::string>();const auto split=text.rfind('.');
            plan->summary["diff"].push_back({{"kind","connect"},{"reference",text.substr(0,split)},
                {"field","Pin "+text.substr(split+1)},{"after",net.at("name")}});
        }
        for(const auto& pin:args.at("add").value("no_connect",json::array())) {
            const auto text=pin.get<std::string>();const auto split=text.rfind('.');
            plan->summary["diff"].push_back({{"kind","connect"},{"reference",text.substr(0,split)},
                {"field","Pin "+text.substr(split+1)},{"after","Intentional no-connect"}});
        }
    }
    plan->summary["preserved_pins"] = preserved;
    plan->summary["verification"] = "KiCad connectivity checked on a staged copy. " + std::to_string( preserved ) + " existing pin nets preserved. ERC runs after the edit is committed.";
    if( aVisual )
    {
        try { plan->summary["visual"]=CircuitVisual(schematic,*staged,plan->summary["diff"]); }
        catch(const std::exception& e) { plan->summary["visual_error"]=e.what(); }
    }
    return plan;
}

json InductiveCircuitEditSummary( const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& plan ) { return plan->summary; }

void InductiveValidateCircuitEdit( SCHEMATIC& schematic, const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& plan )
{
    if( !plan || CircuitSnapshot( schematic ) != plan->before )
        throw std::runtime_error( "The schematic or active sheet changed since this preview. Ask for a fresh preview." );
}

std::unique_ptr<SCHEMATIC> InductiveStageCircuitEdit( SCHEMATIC& schematic, const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& plan )
{
    InductiveValidateCircuitEdit( schematic, plan );
    auto staged = CloneCircuit( schematic );
    ApplyEditToStage( *staged, *plan );
    CircuitPinNets( *staged );
    return staged;
}

static bool CommitCircuitEdit( SCH_EDIT_FRAME* frame, const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& plan, wxString& result )
{
    RequireEditable( frame );
    auto& schematic = frame->Schematic();
    InductiveValidateCircuitEdit( schematic, plan );
    auto* screen = schematic.CurrentSheet().LastScreen();
    if( auto* selection = frame->GetToolManager()->GetTool<SCH_SELECTION_TOOL>() ) selection->ClearSelection();
    SCH_COMMIT commit( frame ); INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
    std::map<KIID, SCH_ITEM*> live;
    for( auto* item : screen->Items() ) live[item->m_Uuid] = item;
    for( const auto& id : plan->removals )
        if( !live.count( id ) ) throw std::runtime_error( "A previewed item is no longer present. Request a new preview." );
    for( const auto& change : plan->changes )
        if( !live.count( change.originalId ) ) throw std::runtime_error( "A previewed item is no longer present. Request a new preview." );
    for( const auto& id : plan->removals ) commit.Remove( live.at( id ), screen );
    for( const auto& change : plan->changes )
    {
        commit.Remove( live.at( change.originalId ), screen );
        commit.Add( static_cast<SCH_ITEM*>( change.replacement->Clone() ), screen );
    }
    for( const auto& item : plan->additions ) commit.Add( static_cast<SCH_ITEM*>( item->Clone() ), screen );
    commit.Push( _( "Apply Circuit Changes" ) ); guard.Pushed();
    frame->GetCanvas()->Refresh();
    wxString erc;
    try { RunERC( frame, erc ); }
    catch( ... ) { erc = "ERC could not finish; run /erc to check the applied schematic."; }
    // Re-resolve live objects after the commit; old pointers may now belong to Undo.
    // Quiet selection leaves the viewport and keyboard focus where the user put them.
    try
    {
        if( auto* selection = frame->GetToolManager()->GetTool<SCH_SELECTION_TOOL>() )
        {
            std::set<std::string> references;
            for( const auto& change : plan->summary.at( "diff" ) )
                references.insert( change.at( "reference" ).get<std::string>() );
            for( auto* item : screen->Items().OfType( SCH_SYMBOL_T ) )
            {
                auto* symbol = static_cast<SCH_SYMBOL*>( item );
                if( references.count( symbol->GetRef( &schematic.CurrentSheet(), false ).ToStdString() ) )
                    selection->AddItemToSel( symbol, true );
            }
            frame->GetCanvas()->Refresh();
        }
    }
    catch( ... ) { /* Feedback must never misreport a committed edit as failed. */ }
    result = "Applied the changes in one undo step. " + erc;
    return true;
}
json InductiveSchematicWorkflowSnapshot(SCHEMATIC& schematic, bool checkpoint)
{
        // Publication follows the reviewed whole design, independent of which
        // sheet the engineer is viewing. Pending edit previews stay sheet-bound.
        json snapshot={{"fingerprint",INDUCTIVE_REVIEW::Hash(CircuitSnapshot(schematic, false))},
                       {"document",schematic.RootScreen()->GetFileName().ToStdString()}};
        if(checkpoint) {
            snapshot["files"]=json::array(); SCH_IO_KICAD_SEXPR io;std::set<SCH_SCREEN*> screens;
            for(const auto& path:schematic.Hierarchy()) if(screens.insert(path.LastScreen()).second) {
                STRING_FORMATTER f;io.FormatSchematicToFormatter(&f,path.Last(),&schematic);
                snapshot["files"].push_back({{"path",path.LastScreen()->GetFileName().ToStdString()},{"content",f.GetString()}});
            }
        }
        return snapshot;
}

void InductiveInstallSchematicTools( SCH_EDIT_FRAME* aFrame, INDUCTIVE_PANEL* aPanel )
{
    if( !aFrame || !aPanel )
        return;

    aPanel->SetWorkflowSnapshot([aFrame](bool checkpoint) -> json { RequireEditable(aFrame);return InductiveSchematicWorkflowSnapshot(aFrame->Schematic(),checkpoint); });
    // One request, one undo step (see INDUCTIVE_UNDO_GROUP).
    auto undoGroup = std::make_shared<INDUCTIVE_UNDO_GROUP>();
    aPanel->SetRequestBoundaryHandler( [aFrame, undoGroup]( bool aBegin ) {
        if( aBegin )
            undoGroup->Begin( aFrame );
        else
            undoGroup->End( aFrame, _( "Inductive Request" ) );
    } );
    aPanel->SetSourcingHandler( [aFrame]( const std::string& aPhase, const json& aIn ) -> json {
        if( aPhase == "collect" )
        {
            aFrame->RecalculateConnections( nullptr, NO_CLEANUP );
            return BomForSourcing( aFrame->Schematic() );
        }
        return ApplySourcedParts( aFrame, aIn.value( "response", json::object() ), aIn.value( "overwrite", false ) );
    } );
    aPanel->SetAttachmentHandler( [aFrame]( const json& file, bool import, wxString& result ) {
        if( INDUCTIVE_ATTACHMENTS::Extension( file.at( "name" ).get<std::string>() ) == ".kicad_mod" )
            return true; // Structural validation is shared; PCB loading performs native validation.
        try
        {
            if( INDUCTIVE_ATTACHMENTS::Extension( file.at( "name" ).get<std::string>() ) == ".kicad_sym" )
            {
                LIB_SYMBOL_MAP symbols;
                try
                {
                    STRING_LINE_READER reader( file.at( "text" ).get<std::string>(), "My Parts" );
                    SCH_IO_KICAD_SEXPR_PARSER parser( &reader ); parser.ParseLib( symbols );
                    if( symbols.empty() ) throw std::runtime_error( "The attached symbol library is empty." );
                }
                catch( ... ) { for( auto& [name, symbol] : symbols ) delete symbol; throw; }
                for( auto& [name, symbol] : symbols ) delete symbol;
                return true;
            }
            if( import ) return ImportAttachedSchematic( aFrame, file, result );
            InductiveReadAttachedSchematic( &aFrame->Schematic(), file );
            return true;
        }
        catch( const IO_ERROR& e ) { result = e.What(); return false; }
    } );

    struct EDIT_STATE { std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT> plan; std::string token; };
    auto edits = std::make_shared<EDIT_STATE>();
    auto editHandler = [aFrame, edits]( const std::string& action, const json& args, wxString& result ) {
        if( action == "preview" || action == "stage" )
        {
            RequireEditable( aFrame );
            aFrame->RecalculateConnections( nullptr, NO_CLEANUP );
            auto plan = InductivePreviewCircuitEdit( aFrame->Schematic(), args, action == "preview" );
            auto token = KIID().AsString().ToStdString();
            auto summary = InductiveCircuitEditSummary( plan ); summary["preview_token"] = token;
            edits->plan = std::move( plan ); edits->token = token;
            result = wxString::FromUTF8( summary.dump() ); return true;
        }
        if( !edits->plan || args.value( "token", "" ) != edits->token )
            throw std::runtime_error( "This preview is no longer available. Ask for a new one." );
        if( action == "summary" ) { InductiveValidateCircuitEdit(aFrame->Schematic(),edits->plan);result=wxString::FromUTF8(edits->plan->summary.dump());return true; }
        if( action == "discard" ) { edits->plan.reset(); result = "Discarded pending changes."; return true; }
        if( action == "focus" ) {
            RequireEditable(aFrame);InductiveValidateCircuitEdit(aFrame->Schematic(),edits->plan);
            const auto ref=args.at("reference").get<std::string>();bool changed=false;
            for(const auto& d:edits->plan->summary["diff"])if(d["reference"]==ref)changed=true;
            if(!changed)throw std::runtime_error("Choose a component in this change.");
            auto& sheet=aFrame->Schematic().CurrentSheet();
            for(auto* item:sheet.LastScreen()->Items().OfType(SCH_SYMBOL_T)) {
                auto* symbol=static_cast<SCH_SYMBOL*>(item);
                if(symbol->GetRef(&sheet,false).ToStdString()==ref){aFrame->FocusOnItem(item);result="Highlighted "+wxString::FromUTF8(ref);return true;}
            }
            result="This component is new. Inspect it in the After preview before applying.";return true;
        }
        if(action!="apply")throw std::runtime_error("Unsupported circuit edit action.");
        auto plan = edits->plan;
        const bool ok = CommitCircuitEdit( aFrame, plan, result );
        edits->plan.reset();
        if( ok ) result << RemoveTinyDanglingWires( aFrame );
        return ok;
    };
    aPanel->SetCircuitEditHandler( editHandler );
    aPanel->SetReviewHandler([aFrame](const std::string& action,const json& args)->json {
        RequireEditable(aFrame);
        auto snapshot=InductiveReviewSnapshot(aFrame->Schematic());
        if(action=="focus")
        {
            if(args.value("fingerprint","")!=snapshot.at("fingerprint").get<std::string>()) throw std::runtime_error("The schematic changed. Refresh the review before highlighting.");
            for(const auto& c:snapshot.at("components")) if(c["id"]==args.at("component_id"))
            {
                const auto& instance=c["instances"].front();
                for(const auto& path:aFrame->Schematic().Hierarchy()) if(path.Path().AsString().ToStdString()==instance.at("sheet").get<std::string>())
                {
                    for(auto* item:path.LastScreen()->Items().OfType(SCH_SYMBOL_T)) if(item->m_Uuid.AsString().ToStdString()==instance.at("uuid").get<std::string>())
                    {aFrame->SetCurrentSheet(path);aFrame->DisplayCurrentSheet();aFrame->FocusOnItem(item);return {{"highlighted",c["reference"]}};}
                }
            }
            throw std::runtime_error("The component is no longer present.");
        }
        return snapshot;
    });

    // Sending a request puts the editor back on its selection arrow, as Esc or a click
    // on the arrow would: a tool left selected (the wire tool, a symbol waiting to be
    // placed) or a half-drawn wire refused every turn with "Finish what you are doing
    // in the schematic first". A drawing tool takes two cancels, the first dropping the
    // item in progress, so this repeats while a tool is on the stack. A modal dialog is
    // left alone: it is the user's to close, and the ready check below still says so.
    aPanel->SetBeforeRequest(
            [aFrame]()
            {
                if( !aFrame->GetToolManager() || !aFrame->IsEnabled() )
                    return;

                for( int i = 0; i < 4 && !aFrame->ToolStackIsEmpty(); ++i )
                    aFrame->GetToolManager()->RunAction( ACTIONS::selectionTool );  // re-entry safe: before the request, outside any commit
            } );

    // Asked before a turn starts, so an engineer who was mid-edit is told in the
    // first second rather than after ninety seconds of work they cannot use.
    aPanel->SetReadyCheck(
            [aFrame]( wxString& aWhy ) -> bool
            {
                try
                {
                    RequireEditable( aFrame );
                    return true;
                }
                catch( const ARGS::BAD_ARGUMENT& )
                {
                    aWhy = _( "Finish what you are doing in the schematic first — press Esc to "
                              "cancel the current tool or close the open dialog, then ask again." );
                    return false;
                }
            } );

    // Installing a draw handler is what puts this editor on the pipeline: turns
    // stop going to a tool loop and start going to the designer, which spends one
    // model request on the whole circuit. The board editor installs none of this
    // and keeps its tools.
    aPanel->SetDrawHandler(
            [aFrame]( const wxString& aDirectory, wxString& aResult ) -> bool
            {
                try
                {
                    RequireEditable( aFrame );
                    return DrawGeneratedSchematic( aFrame, aDirectory, aResult );
                }
                catch( const ARGS::BAD_ARGUMENT& )
                {
                    // Something was started in the editor while the turn ran.
                    aResult = _( "You started editing while I was working, so I left the "
                                 "schematic alone. Press Esc and ask again." );
                    return false;
                }
                catch( const IO_ERROR& e )
                {
                    aResult = e.What();
                    return false;
                }
                catch( ... )
                {
                    aResult = _( "The schematic could not be drawn." );
                    return false;
                }
            } );

    aPanel->SetOpsHandler(
            [aFrame]( const wxString& aOps, wxString& aResult ) -> bool
            {
                try
                {
                    RequireEditable( aFrame );
                    return ApplySchematicOps( aFrame, aOps, aResult );
                }
                catch( const ARGS::BAD_ARGUMENT& e )
                {
                    aResult = wxString::FromUTF8( e.what() );
                    return false;
                }
                catch( const IO_ERROR& e )
                {
                    aResult = e.What();
                    return false;
                }
                catch( ... )
                {
                    aResult = _( "That change could not be applied." );
                    return false;
                }
            } );

    aPanel->SetContextProvider(
            [aFrame]() -> wxString
            {
                if( !aFrame->GetScreen() )
                    return wxEmptyString;

                const std::vector<SYMBOL_REF> symbols = AllSymbols( aFrame );

                wxString out;
                out << wxS( "Editor: schematic (eeschema)\n" );
                out << wxString::Format( wxS( "Symbols: %d across %d sheet(s)\n" ),
                                         (int) symbols.size(),
                                         (int) aFrame->Schematic().Hierarchy().size() );

                const int limit = 60;
                int       i     = 0;

                for( const SYMBOL_REF& entry : symbols )
                {
                    if( i++ >= limit )
                    {
                        out << wxString::Format( wxS( "  ...and %d more\n" ),
                                                 (int) symbols.size() - limit );
                        break;
                    }

                    out << wxS( "  " ) << entry.symbol->GetRef( &entry.path, true ) << wxS( " " )
                        << entry.symbol->GetValue( true, &entry.path, false ) << wxS( "\n" );
                }

                return out;
            } );

    // The sidebar builds its "/" palette from this list, so a command added here
    // appears with no change to the page.  These spend no model quota, which
    // makes them the half of the product that still works when the day's
    // requests are gone.
    // Two kinds of command share this palette: the ones about the open document,
    // answered here, and the ones about the circuit -- parts, nets, cost -- which
    // the panel forwards to the designer because that is where the compiled
    // circuit lives. Both are instant and neither spends a model request.
    aPanel->SetCommands(
            { { wxS( "ERC" ), _( "Run the electrical rule check" ) },
              { wxS( "Check" ), _( "Run ERC on the open schematic" ) },
              { wxS( "BOM" ), _( "List the current symbols, values and footprints" ) },
              { wxS( "Netlist" ), _( "Read the live schematic connectivity" ) },
              { wxS( "PCB" ), _( "Publish the schematic and open its board" ) },
              { wxS( "Clear" ), _( "Clear this sheet as one undoable change" ) } },
            [aFrame]( const wxString& aCommand, wxString& aResult ) -> bool
            {
                if( aCommand.CmpNoCase( "erc" ) == 0 || aCommand.CmpNoCase( "check" ) == 0 )
                {
                    RequireEditable( aFrame );
                    return RunERC( aFrame, aResult );
                }
                if( aCommand.CmpNoCase( "pcb" ) == 0 )
                {
                    RequireEditable( aFrame );
                    return OpenPCB( aFrame, aResult );
                }
                if( aCommand.CmpNoCase( "netlist" ) == 0 )
                {
                    aResult = SchematicNetlist( aFrame );
                    return true;
                }
                if( aCommand.CmpNoCase( "bom" ) == 0 )
                {
                    // Grouped the way a buyer reads it: one line per distinct part.
                    struct LINE { wxArrayString refs; wxString value, footprint, lcsc, mpn; };
                    std::map<wxString, LINE> lines;
                    auto field = []( const SYMBOL_REF& e, const wxString& name )
                    {
                        const SCH_FIELD* f = e.symbol->FindFieldCaseInsensitive( name );
                        return f ? f->GetShownText( &e.path, false ) : wxString();
                    };

                    for( const auto& entry : AllSymbols( aFrame ) )
                    {
                        if( entry.symbol->GetExcludedFromBOM() || entry.symbol->IsPower() )
                            continue;

                        LINE l;
                        l.value = entry.symbol->GetValue( true, &entry.path, false );
                        l.footprint = entry.symbol->GetFootprintFieldText( true, &entry.path, false );
                        l.lcsc = field( entry, wxS( "LCSC" ) );
                        l.mpn = field( entry, wxS( "MPN" ) );
                        const wxString key = l.value + wxS( "\x1f" ) + l.footprint + wxS( "\x1f" ) + l.lcsc
                                             + wxS( "\x1f" ) + l.mpn;
                        LINE& line = lines.emplace( key, l ).first->second;
                        line.refs.Add( entry.symbol->GetRef( &entry.path, true ) );
                    }

                    if( lines.empty() )
                    {
                        aResult = "The schematic has no physical components.";
                        return true;
                    }

                    // A Markdown table the chat renders as one (without the rule under its
                    // header it ran together as a paragraph), in reference order -- C1 before
                    // C10, capacitors before resistors -- with footprints by name alone.
                    auto natural = []( const wxString& a, const wxString& b ) { return StrNumCmp( a, b, true ) < 0; };
                    auto cell = []( wxString aText )
                    {
                        aText.Replace( wxS( "|" ), wxS( "/" ) );
                        return aText.IsEmpty() ? wxString( "-" ) : aText;
                    };

                    std::vector<LINE*> ordered;

                    for( auto& [key, line] : lines )
                    {
                        std::vector<wxString> refs( line.refs.begin(), line.refs.end() );
                        std::sort( refs.begin(), refs.end(), natural );
                        line.refs.Clear();
                        for( const wxString& r : refs )
                            line.refs.Add( r );
                        ordered.push_back( &line );
                    }

                    std::sort( ordered.begin(), ordered.end(),
                               [&]( const LINE* a, const LINE* b ) { return natural( a->refs[0], b->refs[0] ); } );

                    int unsourced = 0;
                    aResult << "| Qty | References | Value | Footprint | LCSC | MPN |\n"
                            << "|---:|---|---|---|---|---|\n";

                    for( const LINE* line : ordered )
                    {
                        if( line->lcsc.IsEmpty() && line->mpn.IsEmpty() )
                            unsourced += (int) line->refs.size();

                        aResult << "| " << line->refs.size() << " | " << cell( wxJoin( line->refs, ',' ) ) << " | "
                                << cell( line->value ) << " | " << cell( line->footprint.AfterFirst( ':' ).IsEmpty()
                                                                         ? line->footprint
                                                                         : line->footprint.AfterFirst( ':' ) )
                                << " | " << cell( line->lcsc ) << " | " << cell( line->mpn ) << " |\n";
                    }

                    if( unsourced )
                    {
                        aResult << wxString::Format(
                                "\n%d part(s) have no real part number yet. Ask: \"Find real parts "
                                "for this design\" to choose in-stock parts.",
                                unsourced );
                    }

                    return true;
                }
                if( aCommand.CmpNoCase( "clear" ) == 0 )
                {
                    RequireEditable( aFrame );
                    return ApplySchematicOps( aFrame, "[{\"op\":\"delete_all\"}]", aResult );
                }
                return false;
            } );

    static const std::vector<ENTRY> tools = BuildTools();

    std::vector<INDUCTIVE_TOOL> declarations;
    declarations.reserve( tools.size() );

    for( const ENTRY& entry : tools )
        declarations.push_back( entry.decl );

    aPanel->SetTools( declarations,
                      [aFrame, editHandler]( const wxString& aName, const wxString& aArgs,
                                wxString& aResult ) -> bool
                      {
                          for( const ENTRY& entry : tools )
                          {
                              if( entry.decl.name != aName )
                                  continue;

                              if( entry.decl.mutates )
                                  RequireEditable( aFrame );

                              const json args = ARGS::ParseObject( aArgs );

                              // A model that placed a part on its own to look at it, then builds the
                              // whole circuit with that part in it: the loose preview is replaced, not
                              // turned into an edit that fails on the duplicate reference (K1 placed
                              // alone, then build_circuit refused twice and the sheet had to be cleared).
                              if( aName == "build_circuit" && args.contains( "components" ) && args["components"].is_array() )
                              {
                                  SCH_SCREEN* screen = aFrame->Schematic().CurrentSheet().LastScreen();
                                  std::set<wxString> planned;
                                  for( const auto& c : args["components"] )
                                      if( c.is_object() && c.contains( "reference" ) && c["reference"].is_string() )
                                          planned.insert( wxString::FromUTF8( c["reference"].get<std::string>() ) );

                                  std::vector<SCH_ITEM*> previews;
                                  bool onlyPreviews = !planned.empty();
                                  for( SCH_ITEM* item : screen->Items() )
                                  {
                                      if( item->Type() != SCH_SYMBOL_T )
                                      {
                                          onlyPreviews = false;   // wires, labels: a real circuit
                                          break;
                                      }
                                      auto* symbol = static_cast<SCH_SYMBOL*>( item );
                                      const wxString ref = symbol->GetRef( &aFrame->GetCurrentSheet() );
                                      if( symbol->GetLibId().GetUniStringLibNickname() == wxS( "power" ) || !planned.count( ref ) )
                                      {
                                          onlyPreviews = false;
                                          break;
                                      }
                                      previews.push_back( item );
                                  }

                                  if( onlyPreviews && !previews.empty() )
                                  {
                                      SCH_COMMIT commit( aFrame );
                                      INDUCTIVE_COMMIT_GUARD<SCH_COMMIT> guard( commit );
                                      for( SCH_ITEM* item : previews )
                                          commit.Remove( item, screen );
                                      commit.Push( _( "Replace Placed Parts" ) );
                                      guard.Pushed();
                                  }
                              }

                              bool existing = false;
                              for( auto* item : aFrame->Schematic().CurrentSheet().LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                              { (void) item; existing = true; break; }
                              if( existing && ( aName == "set_symbol_value" || aName == "set_symbol_footprint"
                                  || aName == "delete_symbols" || aName == "build_circuit" ) )
                              {
                                  json plan;
                                  if( aName == "build_circuit" ) plan["add"] = args;
                                  else if( aName == "delete_symbols" )
                                  {
                                      plan["removals"] = json::array();
                                      for( const auto& ref : args.at( "references" ) ) plan["removals"].push_back( { { "reference", ref } } );
                                  }
                                  else plan["updates"] = json::array( { args } );
                                  return editHandler( "stage", plan, aResult );
                              }
                              if( existing && ( aName == "clear_schematic" || aName == "move_symbol"
                                  || aName == "place_symbol" || aName == "add_wire" || aName == "connect_pins"
                                  || aName == "annotate_schematic" ) )
                                  throw std::runtime_error( "Existing circuits require edit_circuit. Use updates, compatible replacements, isolated labelled-stub rewires or fully specified additions. For unsupported wire geometry, edit manually in KiCad. To intentionally clear a sheet use /clear." );
                              const bool ok = entry.run( aFrame, args, aResult );
                              if( ok && entry.decl.mutates )
                                  aResult << RemoveTinyDanglingWires( aFrame );
                              return ok;
                          }

                          aResult = wxString::Format( _( "Unknown tool '%s'." ), aName );
                          return false;
                      } );
}


json InductiveBomForSourcing( SCHEMATIC& aSchematic )
{
    return BomForSourcing( aSchematic );
}


json InductiveApplySourcedParts( SCHEMATIC& aSchematic, const json& aResponse, bool aOverwrite,
                                 const std::function<void( SCH_SYMBOL*, SCH_SCREEN* )>& aModify )
{
    return ApplySourcedPartsTo( aSchematic, aResponse, aOverwrite, aModify );
}
