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

#ifndef INDUCTIVE_SCH_ITEMS_H
#define INDUCTIVE_SCH_ITEMS_H

/**
 * Every item of a schematic, on every sheet, reachable by the agent: symbols, power
 * symbols, wires, buses, labels, junctions, no-connect flags, texts, sheets. Found by
 * kind, sheet, net, reference, text or area; described in one line with a short id; its
 * properties read and set by name (inductive_properties.h); deleted. A pin is addressed
 * as "U1.5" or "U1.EN", so a label or a no-connect flag lands exactly on it. Frame-free:
 * the editor stages each change in its commit, which makes the undo step.
 */

#include "inductive_properties.h"

#include <sch_commit.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <sch_text.h>
#include <schematic.h>
#include <connection_graph.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace INDUCTIVE_SCH_ITEMS
{

/// An item where it lives: the sheet it is drawn on (the first, when a sheet is reused).
struct PLACED
{
    SCH_ITEM*      item = nullptr;
    SCH_SHEET_PATH path;
    SCH_SCREEN*    screen = nullptr;
};


inline wxString ShortId( const SCH_ITEM* aItem )
{
    return aItem->m_Uuid.AsString().Left( 8 );
}


inline wxString Kind( const SCH_ITEM* aItem )
{
    switch( aItem->Type() )
    {
    case SCH_SYMBOL_T:          return static_cast<const SCH_SYMBOL*>( aItem )->IsPower() ? wxS( "power" ) : wxS( "symbol" );
    case SCH_LINE_T:
    {
        const SCH_LINE* line = static_cast<const SCH_LINE*>( aItem );
        return line->IsWire() ? wxS( "wire" ) : line->IsBus() ? wxS( "bus" ) : wxS( "line" );
    }
    case SCH_LABEL_T:           return wxS( "label" );
    case SCH_GLOBAL_LABEL_T:    return wxS( "global_label" );
    case SCH_HIER_LABEL_T:      return wxS( "hier_label" );
    case SCH_DIRECTIVE_LABEL_T: return wxS( "directive_label" );
    case SCH_JUNCTION_T:        return wxS( "junction" );
    case SCH_NO_CONNECT_T:      return wxS( "no_connect" );
    case SCH_TEXT_T:            return wxS( "text" );
    case SCH_TEXTBOX_T:         return wxS( "textbox" );
    case SCH_SHEET_T:           return wxS( "sheet" );
    case SCH_BUS_WIRE_ENTRY_T:
    case SCH_BUS_BUS_ENTRY_T:   return wxS( "bus_entry" );
    case SCH_SHAPE_T:           return wxS( "shape" );
    default:                    return aItem->GetClass().Lower();
    }
}


inline INDUCTIVE_PROPS::CONTEXT Context()
{
    INDUCTIVE_PROPS::CONTEXT ctx;
    ctx.scale = &schIUScale;
    return ctx;
}


/// Every top-level item on every sheet, each once. The sheet list is built, not read from
/// Hierarchy(): that is a cache, empty until an editor fills it, and reading it empty
/// trips a wxCHECK -- a modal assert dialog in this build.
inline std::vector<PLACED> All( SCHEMATIC* aSchematic )
{
    std::vector<PLACED>  out;
    std::set<SCH_ITEM*>  seen;

    for( const SCH_SHEET_PATH& path : aSchematic->BuildSheetListSortedByPageNumbers() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items() )
        {
            if( seen.insert( item ).second )
                out.push_back( { item, path, screen } );
        }
    }

    return out;
}


inline wxString NoSlash( const wxString& aNet )
{
    return aNet.StartsWith( wxS( "/" ) ) ? aNet.Mid( 1 ) : aNet;
}


/// The nets an item is on: its own for a wire or label, each pin's for a symbol.
inline std::set<wxString> Nets( const PLACED& aPlaced )
{
    std::set<wxString> nets;

    if( aPlaced.item->Type() == SCH_SYMBOL_T )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( aPlaced.item );

        for( SCH_PIN* pin : symbol->GetPins( &aPlaced.path ) )
            if( SCH_CONNECTION* c = pin->Connection( &aPlaced.path ) )
                nets.insert( c->Name() );
    }
    else if( SCH_CONNECTION* c = aPlaced.item->Connection( &aPlaced.path ) )
    {
        nets.insert( c->Name() );
    }

    return nets;
}


inline wxString At( const VECTOR2I& aPoint )
{
    return wxString::Format( wxS( "%s,%s" ), INDUCTIVE_PROPS::Mm( aPoint.x, schIUScale ),
                             INDUCTIVE_PROPS::Mm( aPoint.y, schIUScale ) );
}


/// One line: id, kind and what tells it apart; the sheet when the design has several.
inline wxString Describe( const PLACED& aPlaced, bool aMultiSheet )
{
    SCH_ITEM* item = aPlaced.item;
    wxString  out = ShortId( item ) + wxS( "  " ) + Kind( item ) + wxS( "  " );
    wxString  net;

    if( item->Type() != SCH_SYMBOL_T )
    {
        std::set<wxString> nets = Nets( aPlaced );
        if( !nets.empty() )
            net = wxS( " net " ) + NoSlash( *nets.begin() );
    }

    switch( item->Type() )
    {
    case SCH_SYMBOL_T:
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
        out << symbol->GetRef( &aPlaced.path, false ) << wxS( " " ) << symbol->GetValue( false, &aPlaced.path, false )
            << wxS( " " ) << symbol->GetLibId().Format().wx_str() << wxS( " at " ) << At( symbol->GetPosition() );
        break;
    }
    case SCH_LINE_T:
    {
        SCH_LINE* line = static_cast<SCH_LINE*>( item );
        out << At( line->GetStartPoint() ) << wxS( " -> " ) << At( line->GetEndPoint() ) << net;
        break;
    }
    case SCH_LABEL_T:
    case SCH_GLOBAL_LABEL_T:
    case SCH_HIER_LABEL_T:
    case SCH_DIRECTIVE_LABEL_T:
    case SCH_TEXT_T:
    case SCH_TEXTBOX_T:
    {
        const EDA_TEXT* text = dynamic_cast<const EDA_TEXT*>( item );
        wxString shown = text ? text->GetText() : wxString();
        if( shown.length() > 60 )
            shown = shown.Left( 57 ) + wxS( "..." );
        out << wxS( "\"" ) << shown << wxS( "\" at " ) << At( item->GetPosition() ) << net;
        break;
    }
    case SCH_SHEET_T:
    {
        SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item );
        out << wxS( "\"" ) << sheet->GetName() << wxS( "\" file " ) << sheet->GetFileName() << wxS( " at " )
            << At( sheet->GetPosition() );
        break;
    }
    default:
        out << wxS( "at " ) << At( item->GetPosition() ) << net;
        break;
    }

    if( aMultiSheet )
        out << wxS( "  [sheet " ) << aPlaced.path.PathHumanReadable( false ) << wxS( "]" );

    return out;
}


struct QUERY
{
    std::vector<wxString> kinds;   ///< empty: everything
    wxString              sheet;   ///< a sheet's name or path ("/Power/")
    wxString              net;
    wxString              reference;
    wxString              text;
    std::optional<BOX2I>  area;
};


inline std::vector<PLACED> Find( SCHEMATIC* aSchematic, const QUERY& aQuery, size_t aLimit, size_t* aTotal = nullptr )
{
    std::vector<PLACED> out;
    size_t              total = 0;
    const bool          multi = aSchematic->BuildSheetListSortedByPageNumbers().size() > 1;
    const wxString      wantNet = NoSlash( aQuery.net ).Lower();
    const wxString      wantRef = aQuery.reference.Upper();
    const wxString      wantText = aQuery.text.Lower();
    const wxString      wantSheet = aQuery.sheet.Lower();

    for( const PLACED& placed : All( aSchematic ) )
    {
        if( !aQuery.kinds.empty()
            && std::find( aQuery.kinds.begin(), aQuery.kinds.end(), Kind( placed.item ) ) == aQuery.kinds.end() )
            continue;

        if( !wantSheet.IsEmpty() )
        {
            const wxString path = placed.path.PathHumanReadable( false ).Lower();
            const wxString name = placed.path.Last() ? placed.path.Last()->GetName().Lower() : wxString();

            auto bare = []( wxString aPath )
            {
                while( aPath.length() > 1 && aPath.EndsWith( wxS( "/" ) ) )
                    aPath.RemoveLast();
                return aPath;
            };

            if( path != wantSheet && name != wantSheet && bare( path ) != bare( wantSheet ) )
                continue;
        }

        if( !wantNet.IsEmpty() )
        {
            bool on = false;
            for( const wxString& net : Nets( placed ) )
                on = on || NoSlash( net ).Lower() == wantNet;
            if( !on )
                continue;
        }

        if( !wantRef.IsEmpty() )
        {
            if( placed.item->Type() != SCH_SYMBOL_T
                || static_cast<SCH_SYMBOL*>( placed.item )->GetRef( &placed.path, false ).Upper() != wantRef )
                continue;
        }

        if( aQuery.area && !placed.item->GetBoundingBox().Intersects( *aQuery.area ) )
            continue;

        if( !wantText.IsEmpty() && !Describe( placed, multi ).Lower().Contains( wantText ) )
            continue;

        ++total;

        if( out.size() < aLimit )
            out.push_back( placed );
    }

    if( aTotal )
        *aTotal = total;

    return out;
}


inline std::optional<PLACED> Resolve( SCHEMATIC* aSchematic, const wxString& aId, wxString* aWhyNot = nullptr )
{
    const wxString        id = aId.Lower().Trim().Trim( false );
    std::optional<PLACED> found;
    int                   matches = 0;

    if( id.length() >= 6 )
    {
        for( const PLACED& placed : All( aSchematic ) )
        {
            if( placed.item->m_Uuid.AsString().Lower().StartsWith( id ) )
            {
                found = placed;
                ++matches;
            }
        }
    }

    if( matches == 1 )
        return found;

    if( aWhyNot )
    {
        *aWhyNot = matches > 1 ? wxString::Format( wxS( "'%s' matches %d items; give more of the id" ), aId, matches )
                               : wxString::Format( wxS( "no item has id '%s'; find_items lists them" ), aId );
    }

    return std::nullopt;
}


inline wxString Properties( SCH_ITEM* aItem )
{
    const INDUCTIVE_PROPS::CONTEXT ctx = Context();
    wxString                       out;

    for( PROPERTY_BASE* property : INDUCTIVE_PROPS::Visible( aItem ) )
    {
        const wxString value = INDUCTIVE_PROPS::Read( aItem, property, ctx );

        if( value.IsEmpty() && property->TypeHash() != TYPE_HASH( wxString ) )
            continue;

        out << property->Name() << wxS( " = " ) << value << wxS( "  (" ) << INDUCTIVE_PROPS::Kind( property )
            << ( property->Writeable( aItem ) ? wxS( "" ) : wxS( ", read-only" ) ) << wxS( ")\n" );
    }

    return out;
}


struct EDIT_REPORT
{
    wxString changes;
    wxString problems;
    int      changed = 0;
};


/**
 * Set properties through the property system, each item staged on its own screen. A
 * symbol's per-sheet values (Reference, Unit) are set on the sheet displayed only: the
 * property system reads and writes them through the current sheet.
 */
using STAGE = std::function<void( SCH_ITEM*, SCH_SCREEN* )>;

inline EDIT_REPORT SetProperties( const STAGE& aModify, const std::vector<PLACED>& aItems,
                                  const std::vector<std::pair<wxString, wxString>>& aValues,
                                  UNITS_PROVIDER* aUnits, const SCH_SHEET_PATH& aCurrent )
{
    EDIT_REPORT                    report;
    const INDUCTIVE_PROPS::CONTEXT ctx = Context();

    for( const PLACED& placed : aItems )
    {
        SCH_ITEM* item = placed.item;
        bool      staged = false;

        for( const auto& [name, text] : aValues )
        {
            PROPERTY_BASE* property = INDUCTIVE_PROPS::Find( item, name );

            if( !property )
            {
                wxArrayString names;
                for( PROPERTY_BASE* p : INDUCTIVE_PROPS::Visible( item ) )
                    if( p->Writeable( item ) )
                        names.Add( p->Name() );

                report.problems << ShortId( item ) << wxS( ": no property '" ) << name << wxS( "' on a " ) << Kind( item )
                                << wxS( "; it has " ) << wxJoin( names, ',', '\0' ) << wxS( "\n" );
                continue;
            }

            const wxString key = INDUCTIVE_PROPS::Key( property->Name() );

            if( item->Type() == SCH_SYMBOL_T && ( key == wxS( "reference" ) || key == wxS( "unit" ) )
                && placed.path != aCurrent )
            {
                report.problems << ShortId( item ) << wxS( ": its " ) << property->Name()
                                << wxS( " belongs to its sheet; set it with that sheet open\n" );
                continue;
            }

            const wxString before = INDUCTIVE_PROPS::Read( item, property, ctx );

            if( !staged )
            {
                aModify( item, placed.screen );
                staged = true;
            }

            const wxString why = INDUCTIVE_PROPS::Write( item, property, text, ctx, aUnits );

            if( !why.IsEmpty() )
            {
                report.problems << ShortId( item ) << wxS( ": " ) << why << wxS( "\n" );
                continue;
            }

            report.changes << ShortId( item ) << wxS( " " ) << property->Name() << wxS( " " ) << before << wxS( " -> " )
                           << INDUCTIVE_PROPS::Read( item, property, ctx ) << wxS( "\n" );
            ++report.changed;
        }
    }

    return report;
}


/// aRemove takes the item off its screen (the editor through its commit, so Undo restores it).
inline EDIT_REPORT Delete( const STAGE& aRemove, const std::vector<PLACED>& aItems )
{
    EDIT_REPORT report;

    for( const PLACED& placed : aItems )
    {
        if( placed.item->Type() == SCH_SHEET_T )
        {
            report.problems << ShortId( placed.item ) << wxS( ": a sheet holds a whole page of the design; "
                                                              "the user deletes sheets themselves\n" );
            continue;
        }

        report.changes << ShortId( placed.item ) << wxS( " " ) << Kind( placed.item ) << wxS( " deleted\n" );
        ++report.changed;
        aRemove( placed.item, placed.screen );
    }

    return report;
}


struct PIN_AT
{
    SCH_SYMBOL*    symbol = nullptr;
    SCH_PIN*       pin = nullptr;
    SCH_SHEET_PATH path;
    SCH_SCREEN*    screen = nullptr;
};


/// "U1.5" by number or "U1.EN" by name; a name shared by several pins is refused.
inline std::optional<PIN_AT> FindPin( SCHEMATIC* aSchematic, const wxString& aPin, wxString* aWhyNot )
{
    const wxString ref = aPin.BeforeFirst( '.' );
    const wxString which = aPin.AfterFirst( '.' );

    if( ref.IsEmpty() || which.IsEmpty() )
    {
        *aWhyNot = wxString::Format( wxS( "'%s' is not a pin; write it as U1.5 or U1.EN" ), aPin );
        return std::nullopt;
    }

    std::vector<PIN_AT> byNumber, byName;

    for( const PLACED& placed : All( aSchematic ) )
    {
        if( placed.item->Type() != SCH_SYMBOL_T )
            continue;

        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( placed.item );

        if( symbol->GetRef( &placed.path, false ).CmpNoCase( ref ) != 0 )
            continue;

        for( SCH_PIN* pin : symbol->GetPins( &placed.path ) )
        {
            if( pin->GetNumber().CmpNoCase( which ) == 0 )
                byNumber.push_back( { symbol, pin, placed.path, placed.screen } );
            else if( pin->GetShownName().CmpNoCase( which ) == 0 )
                byName.push_back( { symbol, pin, placed.path, placed.screen } );
        }
    }

    if( byNumber.size() == 1 )
        return byNumber.front();

    if( byNumber.empty() && byName.size() == 1 )
        return byName.front();

    *aWhyNot = byName.size() > 1 ? wxString::Format( wxS( "%s has %d pins named '%s'; give the number" ), ref,
                                                     (int) byName.size(), which )
                                 : wxString::Format( wxS( "%s has no pin '%s'" ), ref, which );
    return std::nullopt;
}


/// A label on a pin reads away from the symbol: left of a pin on its left side, and so on.
inline SPIN_STYLE AwayFrom( const PIN_AT& aPin )
{
    switch( aPin.pin->PinDrawOrient( aPin.symbol->GetTransform() ) )
    {
    case PIN_ORIENTATION::PIN_RIGHT: return SPIN_STYLE::LEFT;     // the pin runs right into the body
    case PIN_ORIENTATION::PIN_LEFT:  return SPIN_STYLE::RIGHT;
    case PIN_ORIENTATION::PIN_UP:    return SPIN_STYLE::BOTTOM;
    case PIN_ORIENTATION::PIN_DOWN:  return SPIN_STYLE::UP;
    default:                         return SPIN_STYLE::RIGHT;
    }
}

} // namespace INDUCTIVE_SCH_ITEMS

#endif // INDUCTIVE_SCH_ITEMS_H
