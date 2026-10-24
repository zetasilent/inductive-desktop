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

#ifndef INDUCTIVE_BOARD_ITEMS_H
#define INDUCTIVE_BOARD_ITEMS_H

/**
 * Every item on a board, reachable by the agent: found by kind, layer, net, reference,
 * area or text; described in one line with a short id; its properties read and set by
 * name (inductive_properties.h); deleted; and the shapes and zones the dedicated tools
 * do not draw (a cutout, a slot, a keep-out, a plane) created. Frame-free, so the QA
 * drives exactly what the editor runs; the editor tools add the undo step and refill.
 */

#include "inductive_properties.h"

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_dimension.h>
#include <pcb_group.h>
#include <pcb_shape.h>
#include <pcb_text.h>
#include <pcb_textbox.h>
#include <pcb_track.h>
#include <zone.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace INDUCTIVE_ITEMS
{

/// Ids are KiCad's own UUIDs; the first eight characters are shown and accepted.
inline wxString ShortId( const BOARD_ITEM* aItem )
{
    return aItem->m_Uuid.AsString().Left( 8 );
}


inline wxString Kind( const BOARD_ITEM* aItem )
{
    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:      return wxS( "footprint" );
    case PCB_PAD_T:            return wxS( "pad" );
    case PCB_TRACE_T:          return wxS( "track" );
    case PCB_ARC_T:            return wxS( "arc" );
    case PCB_VIA_T:            return wxS( "via" );
    case PCB_ZONE_T:           return static_cast<const ZONE*>( aItem )->GetIsRuleArea() ? wxS( "rule_area" ) : wxS( "zone" );
    case PCB_SHAPE_T:          return wxS( "shape" );
    case PCB_FIELD_T:
    case PCB_TEXT_T:           return wxS( "text" );
    case PCB_TEXTBOX_T:        return wxS( "textbox" );
    case PCB_GROUP_T:          return wxS( "group" );
    case PCB_DIM_ALIGNED_T:
    case PCB_DIM_LEADER_T:
    case PCB_DIM_CENTER_T:
    case PCB_DIM_RADIAL_T:
    case PCB_DIM_ORTHOGONAL_T: return wxS( "dimension" );
    default:                   return aItem->GetClass().Lower();
    }
}


inline wxString NoSlash( const wxString& aNet )
{
    return aNet.StartsWith( wxS( "/" ) ) ? aNet.Mid( 1 ) : aNet;
}


inline INDUCTIVE_PROPS::CONTEXT Context( BOARD* aBoard )
{
    INDUCTIVE_PROPS::CONTEXT ctx;
    ctx.scale = &pcbIUScale;
    ctx.netName = [aBoard]( int aCode ) -> wxString
    {
        const NETINFO_ITEM* net = aBoard->FindNet( aCode );
        return net && aCode > 0 ? net->GetNetname() : wxString( wxS( "(no net)" ) );
    };
    ctx.netCode = [aBoard]( const wxString& aName ) -> std::optional<int>
    {
        if( aName.IsEmpty() || INDUCTIVE_PROPS::Key( aName ) == wxS( "none" )
            || INDUCTIVE_PROPS::Key( aName ) == wxS( "(no net)" ) )
            return 0;

        for( const wxString& candidate : { aName, wxS( "/" ) + aName, NoSlash( aName ) } )
            if( const NETINFO_ITEM* net = aBoard->FindNet( candidate ) )
                return net->GetNetCode();

        return std::nullopt;
    };
    ctx.layerId = [aBoard]( const wxString& aName ) -> std::optional<int>
    {
        const PCB_LAYER_ID layer = aBoard->GetLayerID( aName );

        if( layer == UNDEFINED_LAYER )
            return std::nullopt;

        return (int) layer;
    };
    return ctx;
}


/// Every item of the board the agent can address, footprint pads included.
inline void ForEach( BOARD* aBoard, const std::function<void( BOARD_ITEM* )>& aVisit )
{
    for( FOOTPRINT* fp : aBoard->Footprints() )
    {
        aVisit( fp );

        for( PAD* pad : fp->Pads() )
            aVisit( pad );
    }

    for( PCB_TRACK* track : aBoard->Tracks() )
        aVisit( track );

    for( ZONE* zone : aBoard->Zones() )
        aVisit( zone );

    for( BOARD_ITEM* drawing : aBoard->Drawings() )
        aVisit( drawing );

    for( PCB_GROUP* group : aBoard->Groups() )
        aVisit( group );
}


/// An item by its id: the full UUID or a prefix of at least six characters that only it has.
inline BOARD_ITEM* Resolve( BOARD* aBoard, const wxString& aId, wxString* aWhyNot = nullptr )
{
    const wxString id = aId.Lower().Trim().Trim( false );
    BOARD_ITEM*    found = nullptr;
    int            matches = 0;

    if( id.length() >= 6 )
    {
        ForEach( aBoard, [&]( BOARD_ITEM* aItem )
                 {
                     if( aItem->m_Uuid.AsString().Lower().StartsWith( id ) )
                     {
                         found = aItem;
                         ++matches;
                     }
                 } );
    }

    if( matches == 1 )
        return found;

    if( aWhyNot )
    {
        *aWhyNot = matches > 1 ? wxString::Format( wxS( "'%s' matches %d items; give more of the id" ), aId, matches )
                               : wxString::Format( wxS( "no item has id '%s'; find_items lists them" ), aId );
    }

    return nullptr;
}


inline wxString At( const VECTOR2I& aPoint )
{
    return wxString::Format( wxS( "%s,%s" ), INDUCTIVE_PROPS::Mm( aPoint.x, pcbIUScale ),
                             INDUCTIVE_PROPS::Mm( aPoint.y, pcbIUScale ) );
}


inline wxString Box( const BOX2I& aBox )
{
    return wxString::Format( wxS( "(%s)-(%s)" ), At( aBox.GetOrigin() ), At( aBox.GetEnd() ) );
}


inline wxString LayerList( BOARD* aBoard, const LSET& aLayers )
{
    wxArrayString names;

    for( PCB_LAYER_ID layer : aLayers.Seq() )
        names.Add( aBoard->GetLayerName( layer ) );

    return wxJoin( names, ',', '\0' );
}


/// One line: id, kind and what tells it apart.
inline wxString Describe( BOARD* aBoard, BOARD_ITEM* aItem )
{
    using INDUCTIVE_PROPS::Mm;
    wxString out = ShortId( aItem ) + wxS( "  " ) + Kind( aItem ) + wxS( "  " );
    const wxString net = aItem->IsConnected()
                                 ? NoSlash( static_cast<BOARD_CONNECTED_ITEM*>( aItem )->GetNetname() )
                                 : wxString();
    const wxString onNet = net.IsEmpty() ? wxString() : wxS( " net " ) + net;

    switch( aItem->Type() )
    {
    case PCB_FOOTPRINT_T:
    {
        FOOTPRINT* fp = static_cast<FOOTPRINT*>( aItem );
        out << fp->GetReference() << wxS( " " ) << fp->GetValue() << wxS( " at " ) << At( fp->GetPosition() )
            << wxS( " " ) << wxString::FromCDouble( fp->GetOrientation().AsDegrees(), 1 ) << wxS( "deg " )
            << aBoard->GetLayerName( fp->GetLayer() ) << ( fp->IsLocked() ? wxS( " locked" ) : wxS( "" ) );
        break;
    }
    case PCB_PAD_T:
    {
        PAD* pad = static_cast<PAD*>( aItem );
        const VECTOR2I size = pad->GetSize( PADSTACK::ALL_LAYERS );
        out << ( pad->GetParentFootprint() ? pad->GetParentFootprint()->GetReference() : wxString() ) << wxS( "." )
            << pad->GetNumber() << onNet << wxS( " at " ) << At( pad->GetPosition() ) << wxS( " size " )
            << Mm( size.x, pcbIUScale ) << wxS( "x" ) << Mm( size.y, pcbIUScale );
        break;
    }
    case PCB_TRACE_T:
    case PCB_ARC_T:
    {
        PCB_TRACK* track = static_cast<PCB_TRACK*>( aItem );
        out << aBoard->GetLayerName( track->GetLayer() ) << onNet << wxS( " width " ) << Mm( track->GetWidth(), pcbIUScale )
            << wxS( " " ) << At( track->GetStart() ) << wxS( " -> " ) << At( track->GetEnd() );
        break;
    }
    case PCB_VIA_T:
    {
        PCB_VIA* via = static_cast<PCB_VIA*>( aItem );
        PCB_LAYER_ID top, bottom;
        via->LayerPair( &top, &bottom );
        out << wxS( "at " ) << At( via->GetPosition() ) << onNet << wxS( " diameter " )
            << Mm( via->GetWidth( PADSTACK::ALL_LAYERS ), pcbIUScale ) << wxS( " drill " ) << Mm( via->GetDrillValue(), pcbIUScale )
            << wxS( " " ) << aBoard->GetLayerName( top ) << wxS( "-" ) << aBoard->GetLayerName( bottom );
        break;
    }
    case PCB_ZONE_T:
    {
        ZONE* zone = static_cast<ZONE*>( aItem );
        out << LayerList( aBoard, zone->GetLayerSet() ) << onNet;

        if( !zone->GetZoneName().IsEmpty() )
            out << wxS( " \"" ) << zone->GetZoneName() << wxS( "\"" );

        if( zone->GetIsRuleArea() )
        {
            wxArrayString none;
            if( zone->GetDoNotAllowTracks() )     none.Add( wxS( "tracks" ) );
            if( zone->GetDoNotAllowVias() )       none.Add( wxS( "vias" ) );
            if( zone->GetDoNotAllowPads() )       none.Add( wxS( "pads" ) );
            if( zone->GetDoNotAllowZoneFills() )  none.Add( wxS( "copper pours" ) );
            if( zone->GetDoNotAllowFootprints() ) none.Add( wxS( "footprints" ) );
            out << wxS( " keeps out " ) << ( none.IsEmpty() ? wxString( wxS( "nothing" ) ) : wxJoin( none, ',', '\0' ) );
        }
        else
        {
            out << wxS( " priority " ) << zone->GetAssignedPriority();
        }

        out << wxS( " " ) << Box( zone->GetBoundingBox() );
        break;
    }
    case PCB_SHAPE_T:
    {
        PCB_SHAPE* shape = static_cast<PCB_SHAPE*>( aItem );
        out << aBoard->GetLayerName( shape->GetLayer() ) << onNet << wxS( " " ) << shape->ShowShape().Lower();

        switch( shape->GetShape() )
        {
        case SHAPE_T::SEGMENT:   out << wxS( " " ) << At( shape->GetStart() ) << wxS( " -> " ) << At( shape->GetEnd() ); break;
        case SHAPE_T::CIRCLE:    out << wxS( " centre " ) << At( shape->GetCenter() ) << wxS( " radius " ) << Mm( shape->GetRadius(), pcbIUScale ); break;
        case SHAPE_T::ARC:       out << wxS( " " ) << At( shape->GetStart() ) << wxS( " -> " ) << At( shape->GetEnd() ) << wxS( " centre " ) << At( shape->GetCenter() ); break;
        default:                 out << wxS( " " ) << Box( shape->GetBoundingBox() ); break;
        }

        out << wxS( " width " ) << Mm( shape->GetWidth(), pcbIUScale ) << ( shape->IsSolidFill() ? wxS( " filled" ) : wxS( "" ) );
        break;
    }
    case PCB_FIELD_T:
    case PCB_TEXT_T:
    case PCB_TEXTBOX_T:
    {
        const EDA_TEXT* text = dynamic_cast<const EDA_TEXT*>( aItem );
        wxString shown = text ? text->GetText() : wxString();
        if( shown.length() > 60 )
            shown = shown.Left( 57 ) + wxS( "..." );
        out << aBoard->GetLayerName( aItem->GetLayer() ) << wxS( " \"" ) << shown << wxS( "\" at " )
            << At( aItem->GetPosition() );
        break;
    }
    default:
        out << LayerList( aBoard, aItem->GetLayerSet() ) << wxS( " " ) << Box( aItem->GetBoundingBox() );
        break;
    }

    return out;
}


struct QUERY
{
    std::vector<wxString> kinds;    ///< empty: everything but pads
    wxString              layer;
    wxString              net;
    wxString              reference;
    wxString              text;
    std::optional<BOX2I>  area;
};


/// The items that match, in board order; aTotal counts them all when the list is cut.
inline std::vector<BOARD_ITEM*> Find( BOARD* aBoard, const QUERY& aQuery, size_t aLimit, size_t* aTotal = nullptr )
{
    std::vector<BOARD_ITEM*> out;
    size_t                   total = 0;
    const PCB_LAYER_ID       layer = aQuery.layer.IsEmpty() ? UNDEFINED_LAYER : aBoard->GetLayerID( aQuery.layer );
    const wxString           wantNet = NoSlash( aQuery.net ).Lower();
    const wxString           wantRef = aQuery.reference.Upper();
    const wxString           wantText = aQuery.text.Lower();

    ForEach( aBoard, [&]( BOARD_ITEM* aItem )
             {
                 const wxString kind = Kind( aItem );

                 if( aQuery.kinds.empty() ? kind == wxS( "pad" )
                                          : std::find( aQuery.kinds.begin(), aQuery.kinds.end(), kind ) == aQuery.kinds.end() )
                     return;

                 if( !aQuery.layer.IsEmpty() && ( layer == UNDEFINED_LAYER || !aItem->IsOnLayer( layer ) ) )
                     return;

                 if( !wantNet.IsEmpty() )
                 {
                     if( !aItem->IsConnected()
                         || NoSlash( static_cast<BOARD_CONNECTED_ITEM*>( aItem )->GetNetname() ).Lower() != wantNet )
                         return;
                 }

                 if( !wantRef.IsEmpty() )
                 {
                     const FOOTPRINT* fp = aItem->Type() == PCB_FOOTPRINT_T ? static_cast<const FOOTPRINT*>( aItem )
                                                                             : aItem->GetParentFootprint();
                     if( !fp || fp->GetReference().Upper() != wantRef )
                         return;
                 }

                 if( aQuery.area && !aItem->GetBoundingBox().Intersects( *aQuery.area ) )
                     return;

                 if( !wantText.IsEmpty() && !Describe( aBoard, aItem ).Lower().Contains( wantText ) )
                     return;

                 ++total;

                 if( out.size() < aLimit )
                     out.push_back( aItem );
             } );

    if( aTotal )
        *aTotal = total;

    return out;
}


/// Every property the panel shows, one per line: "Name = value  (kind, read-only)".
inline wxString Properties( BOARD_ITEM* aItem, const INDUCTIVE_PROPS::CONTEXT& aCtx )
{
    wxString out;

    for( PROPERTY_BASE* property : INDUCTIVE_PROPS::Visible( aItem ) )
    {
        const wxString value = INDUCTIVE_PROPS::Read( aItem, property, aCtx );

        if( value.IsEmpty() && property->TypeHash() != TYPE_HASH( wxString ) )
            continue;   // a colour, a font: nothing the tools can say or set

        out << property->Name() << wxS( " = " ) << value << wxS( "  (" ) << INDUCTIVE_PROPS::Kind( property )
            << ( property->Writeable( aItem ) ? wxS( "" ) : wxS( ", read-only" ) ) << wxS( ")\n" );
    }

    return out;
}


struct EDIT_REPORT
{
    wxString changes;   ///< "a1b2c3d4 Width 0.25 -> 0.5" lines
    wxString problems;  ///< what was refused, and why
    int      changed = 0;
    bool     copper = false;   ///< a change pours should be refilled for
};


/**
 * Set properties on items: each value is checked and written through the property system,
 * the item staged in aCommit first. A locked item takes only "Locked"; a pad is moved with
 * its footprint, never alone.
 */
inline EDIT_REPORT SetProperties( BOARD* aBoard, BOARD_COMMIT& aCommit, const std::vector<BOARD_ITEM*>& aItems,
                                  const std::vector<std::pair<wxString, wxString>>& aValues, UNITS_PROVIDER* aUnits )
{
    EDIT_REPORT                    report;
    const INDUCTIVE_PROPS::CONTEXT ctx = Context( aBoard );

    for( BOARD_ITEM* item : aItems )
    {
        bool staged = false;

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

            if( item->IsLocked() && key != wxS( "locked" ) )
            {
                report.problems << ShortId( item ) << wxS( ": it is locked; set Locked to false first\n" );
                break;
            }

            if( item->Type() == PCB_PAD_T && ( key == wxS( "position x" ) || key == wxS( "position y" ) ) )
            {
                report.problems << ShortId( item ) << wxS( ": a pad moves with its footprint; use move_footprint\n" );
                continue;
            }

            const wxString before = INDUCTIVE_PROPS::Read( item, property, ctx );

            if( !staged )
            {
                aCommit.Modify( item );
                staged = true;
            }

            const wxString why = INDUCTIVE_PROPS::Write( item, property, text, ctx, aUnits );

            if( !why.IsEmpty() )
            {
                report.problems << ShortId( item ) << wxS( ": " ) << why << wxS( "\n" );
                continue;
            }

            const wxString after = INDUCTIVE_PROPS::Read( item, property, ctx );
            report.changes << ShortId( item ) << wxS( " " ) << property->Name() << wxS( " " ) << before << wxS( " -> " )
                           << after << wxS( "\n" );
            ++report.changed;
            report.copper = report.copper || item->IsOnCopperLayer() || item->Type() == PCB_ZONE_T;
        }
    }

    return report;
}


/// Remove items; a pad or a footprint's own text goes only with its footprint.
inline EDIT_REPORT Delete( BOARD_COMMIT& aCommit, const std::vector<BOARD_ITEM*>& aItems )
{
    EDIT_REPORT report;

    for( BOARD_ITEM* item : aItems )
    {
        if( item->GetParentFootprint() )
        {
            report.problems << ShortId( item ) << wxS( ": part of " ) << item->GetParentFootprint()->GetReference()
                            << wxS( "; delete the footprint, or change the property instead\n" );
            continue;
        }

        if( item->IsLocked() )
        {
            report.problems << ShortId( item ) << wxS( ": it is locked; set Locked to false first\n" );
            continue;
        }

        report.changes << ShortId( item ) << wxS( " " ) << Kind( item ) << wxS( " deleted\n" );
        report.copper = report.copper || item->IsOnCopperLayer();
        ++report.changed;
        aCommit.Remove( item );
    }

    return report;
}


enum class SHAPE_KIND { LINE, RECT, CIRCLE, ARC, POLYGON };


/**
 * A drawn shape: a line (2 points), rectangle (2 corners), circle (centre and a point on
 * it), arc (start, a point on the way, end) or polygon (3 points or more). On Edge.Cuts a
 * closed shape inside the outline is a cutout or slot.
 */
inline std::unique_ptr<PCB_SHAPE> MakeShape( BOARD* aBoard, PCB_LAYER_ID aLayer, SHAPE_KIND aKind,
                                             const std::vector<VECTOR2I>& aPoints, int aWidth, bool aFilled )
{
    auto shape = std::make_unique<PCB_SHAPE>( aBoard );
    shape->SetLayer( aLayer );

    switch( aKind )
    {
    case SHAPE_KIND::LINE:
        shape->SetShape( SHAPE_T::SEGMENT );
        shape->SetStart( aPoints.at( 0 ) );
        shape->SetEnd( aPoints.at( 1 ) );
        break;
    case SHAPE_KIND::RECT:
        shape->SetShape( SHAPE_T::RECTANGLE );
        shape->SetStart( aPoints.at( 0 ) );
        shape->SetEnd( aPoints.at( 1 ) );
        break;
    case SHAPE_KIND::CIRCLE:
        shape->SetShape( SHAPE_T::CIRCLE );
        shape->SetCenter( aPoints.at( 0 ) );
        shape->SetEnd( aPoints.at( 1 ) );
        break;
    case SHAPE_KIND::ARC:
        shape->SetShape( SHAPE_T::ARC );
        shape->SetArcGeometry( aPoints.at( 0 ), aPoints.at( 1 ), aPoints.at( 2 ) );
        break;
    case SHAPE_KIND::POLYGON:
    {
        shape->SetShape( SHAPE_T::POLY );
        SHAPE_POLY_SET poly;
        poly.NewOutline();
        for( const VECTOR2I& p : aPoints )
            poly.Append( p );
        shape->SetPolyShape( poly );
        break;
    }
    }

    shape->SetStroke( STROKE_PARAMS( aWidth, LINE_STYLE::SOLID ) );

    if( aFilled && aKind != SHAPE_KIND::LINE && aKind != SHAPE_KIND::ARC )
        shape->SetFillMode( FILL_T::FILLED_SHAPE );

    return shape;
}


struct ZONE_SPEC
{
    LSET                  layers;
    int                   net = 0;
    std::vector<VECTOR2I> outline;
    wxString              name;
    bool                  keepout = false;
    bool                  noTracks = true, noVias = true, noPads = false, noCopper = true, noFootprints = false;
    int                   priority = 0;
    std::optional<int>    clearance, minWidth;
    ZONE_CONNECTION       pads = ZONE_CONNECTION::THERMAL;
};


/// A copper pour on a net (a plane, a local fill) or a rule area that keeps things out.
inline std::unique_ptr<ZONE> MakeZone( BOARD* aBoard, const ZONE_SPEC& aSpec )
{
    auto zone = std::make_unique<ZONE>( aBoard );
    zone->SetLayerSet( aSpec.layers );
    zone->SetZoneName( aSpec.name );
    zone->SetIsRuleArea( aSpec.keepout );

    SHAPE_POLY_SET outline;
    outline.NewOutline();
    for( const VECTOR2I& p : aSpec.outline )
        outline.Append( p );
    *zone->Outline() = outline;

    if( aSpec.keepout )
    {
        zone->SetDoNotAllowTracks( aSpec.noTracks );
        zone->SetDoNotAllowVias( aSpec.noVias );
        zone->SetDoNotAllowPads( aSpec.noPads );
        zone->SetDoNotAllowZoneFills( aSpec.noCopper );
        zone->SetDoNotAllowFootprints( aSpec.noFootprints );
    }
    else
    {
        zone->SetNetCode( aSpec.net );
        zone->SetAssignedPriority( aSpec.priority );
        zone->SetPadConnection( aSpec.pads );
        zone->SetIslandRemovalMode( ISLAND_REMOVAL_MODE::ALWAYS );

        if( aSpec.clearance )
            zone->SetLocalClearance( *aSpec.clearance );

        if( aSpec.minWidth )
            zone->SetMinThickness( *aSpec.minWidth );
    }

    return zone;
}

} // namespace INDUCTIVE_ITEMS

#endif // INDUCTIVE_BOARD_ITEMS_H
