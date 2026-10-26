/*
 * A board the layout service sent back, merged into the board the editor has open:
 * items matched by KIID, nets by name, net classes as the service left them. Kept
 * apart from the editor so the headless QA can check a merged board against the
 * service's own.
 */

#ifndef INDUCTIVE_BOARD_APPLY_H
#define INDUCTIVE_BOARD_APPLY_H

#include "inductive_board_rules.h"

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <board_stackup_manager/board_stackup.h>
#include <component_classes/component_class.h>
#include <footprint.h>
#include <netclass.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_group.h>
#include <pcb_track.h>
#include <project/net_settings.h>
#include <zone.h>

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <unordered_set>

namespace INDUCTIVE_BOARD_APPLY
{

using json = nlohmann::json;

/// The net classes and net-to-class patterns the server left (board units; null inherits).
inline void ApplyNetClasses( BOARD* aBoard, const json& aClasses )
{
    std::shared_ptr<NET_SETTINGS> settings = aBoard->GetDesignSettings().m_NetSettings;

    if( !settings || !aClasses.is_object() )
        return;

    auto opt = []( const json& aValue ) -> std::optional<int>
    {
        return aValue.is_number_integer() ? std::optional<int>( aValue.get<int>() ) : std::nullopt;
    };

    for( const json& entry : aClasses.value( "classes", json::array() ) )
    {
        const wxString name = wxString::FromUTF8( entry.value( "name", std::string() ) );

        if( name.IsEmpty() || name == NETCLASS::Default )
            continue;

        std::shared_ptr<NETCLASS> nc = settings->HasNetclass( name ) ? settings->GetNetclasses().at( name ) : nullptr;

        if( !nc )
        {
            nc = std::make_shared<NETCLASS>( name, false );
            settings->SetNetclass( name, nc );
        }

        nc->SetTrackWidth( opt( entry.value( "track_width", json() ) ) );
        nc->SetClearance( opt( entry.value( "clearance", json() ) ) );
        nc->SetViaDiameter( opt( entry.value( "via_diameter", json() ) ) );
        nc->SetViaDrill( opt( entry.value( "via_drill", json() ) ) );
        nc->SetDescription( wxString::FromUTF8( entry.value( "description", std::string() ) ) );
    }

    if( aClasses.contains( "patterns" ) && aClasses["patterns"].is_array() )
    {
        settings->ClearNetclassPatternAssignments();

        for( const json& pair : aClasses["patterns"] )
        {
            if( pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string() )
                settings->SetNetclassPatternAssignment( wxString::FromUTF8( pair[0].get<std::string>() ),
                                                        wxString::FromUTF8( pair[1].get<std::string>() ) );
        }
    }

    settings->ClearAllCaches();
    aBoard->SynchronizeNetsAndNetClasses( false );
    INDUCTIVE_BOARD_RULES::RecompileRules( aBoard );
}


/**
 * Take the service board's copper layers (count, types and stackup). Returns whether
 * they changed, so the editor can rebuild its layer controls. Before the items, so
 * pours on new inner layers have them.
 */
inline bool ApplyLayers( BOARD* aBoard, BOARD* aFresh )
{
    if( aFresh->GetCopperLayerCount() == aBoard->GetCopperLayerCount() )
        return false;

    aBoard->SetCopperLayerCount( aFresh->GetCopperLayerCount() );
    aBoard->SetEnabledLayers( aFresh->GetEnabledLayers() );

    for( PCB_LAYER_ID layer : LSET::AllCuMask( aFresh->GetCopperLayerCount() ).Seq() )
        aBoard->SetLayerType( layer, aFresh->GetLayerType( layer ) );

    aBoard->GetDesignSettings().GetStackupDescriptor() = aFresh->GetDesignSettings().GetStackupDescriptor();
    return true;
}


/**
 * The same footprint, whatever the order of its pads and drawings: KiCad's own
 * comparison goes by position in those lists, and a footprint's move assignment (which
 * SwapItemData and undo use) re-adds its children in reverse. Fields are matched by
 * name, since a copied footprint's fields get new ids.
 */
inline bool SameFootprint( FOOTPRINT* aA, FOOTPRINT* aB )
{
    if( aA->GetPosition() != aB->GetPosition() || aA->GetOrientation() != aB->GetOrientation()
        || aA->GetLayer() != aB->GetLayer() || aA->GetAttributes() != aB->GetAttributes()
        || aA->GetFPID() != aB->GetFPID() )
    {
        return false;
    }

    // Fields by name (a copy gives them new ids), everything else by id.
    std::map<wxString, BOARD_ITEM*> mine, theirs;

    auto key = []( BOARD_ITEM* aItem )
    {
        return aItem->Type() == PCB_FIELD_T ? wxS( "field:" ) + static_cast<PCB_FIELD*>( aItem )->GetName()
                                            : aItem->m_Uuid.AsString();
    };

    aA->RunOnChildren( [&]( BOARD_ITEM* aItem ) { mine[key( aItem )] = aItem; }, RECURSE_MODE::NO_RECURSE );
    aB->RunOnChildren( [&]( BOARD_ITEM* aItem ) { theirs[key( aItem )] = aItem; }, RECURSE_MODE::NO_RECURSE );

    if( mine.size() != theirs.size() )
        return false;

    for( const auto& [id, item] : theirs )
    {
        auto it = mine.find( id );

        if( it == mine.end() || it->second->Type() != item->Type() || !( *it->second == *item ) )
            return false;
    }

    return true;
}


/**
 * Stage, in aCommit, everything that makes aBoard's items aFresh's: items are matched by
 * their KIID; new ones are added, ones gone are removed and changed ones take the
 * service's state (a pour also when only its fill changed). Nets are matched by name.
 */
inline void StageItems( BOARD* aBoard, BOARD_COMMIT& aCommit, BOARD* aFresh )
{
    BOARD* board = aBoard;
    BOARD_COMMIT& commit = aCommit;

    auto collect = []( BOARD* aBoard )
    {
        std::map<KIID, BOARD_ITEM*> items;

        for( FOOTPRINT* fp : aBoard->Footprints() )
            items[fp->m_Uuid] = fp;

        for( PCB_TRACK* track : aBoard->Tracks() )
            items[track->m_Uuid] = track;

        for( ZONE* zone : aBoard->Zones() )
            items[zone->m_Uuid] = zone;

        for( BOARD_ITEM* item : aBoard->Drawings() )
            items[item->m_Uuid] = item;

        return items;
    };

    const std::map<KIID, BOARD_ITEM*> live = collect( board ), fresh = collect( aFresh );

    auto netFor = [&]( const wxString& aName )
    {
        NETINFO_ITEM* net = aName.IsEmpty() ? nullptr : board->FindNet( aName );
        return net ? net : NETINFO_LIST::OrphanedItem();
    };

    // A copy of the server's item with this board's nets (read while the server's board lives).
    // Groups and component classes belong to a board, so the copy has no group and its
    // footprint classes are found again by name here, as loading a file does: the server's
    // board, which the clone points into, is freed when this merge ends (crashes in DRC,
    // autosave and the viewers after /layout, 2026-10-25).
    auto copyOf = [&]( BOARD_ITEM* aItem )
    {
        auto* copy = static_cast<BOARD_ITEM*>( aItem->Clone() );
        copy->SetParentGroup( nullptr );

        if( auto* connected = dynamic_cast<BOARD_CONNECTED_ITEM*>( copy ) )
            connected->SetNet( netFor( connected->GetNetname() ) );

        if( copy->Type() == PCB_FOOTPRINT_T )
        {
            auto* footprint = static_cast<FOOTPRINT*>( copy );

            for( PAD* pad : footprint->Pads() )
                pad->SetNet( netFor( pad->GetNetname() ) );

            std::unordered_set<wxString> classes;

            if( const COMPONENT_CLASS* served = static_cast<FOOTPRINT*>( aItem )->GetStaticComponentClass() )
            {
                for( const COMPONENT_CLASS* part : served->GetConstituentClasses() )
                    classes.insert( part->GetName() );
            }

            footprint->ResolveComponentClassNames( board, classes );
        }

        return copy;
    };

    // aCopy takes aExisting's place, in its group too, as KiCad's footprint exchange does.
    auto replace = [&]( BOARD_ITEM* aExisting, BOARD_ITEM* aCopy )
    {
        if( EDA_GROUP* group = aExisting->GetParentGroup() )
        {
            commit.Modify( group->AsEdaItem(), nullptr, RECURSE_MODE::NO_RECURSE );
            group->RemoveItem( aExisting );
            group->AddItem( aCopy );
        }

        commit.Remove( aExisting );
        commit.Add( aCopy );
    };

    auto sameFill = []( ZONE* aA, ZONE* aB )
    {
        for( PCB_LAYER_ID layer : aA->GetLayerSet().Seq() )
        {
            aA->BuildHashValue( layer );
            aB->BuildHashValue( layer );

            if( aA->GetHashValue( layer ) != aB->GetHashValue( layer ) )
                return false;
        }

        return true;
    };

    for( const auto& [id, item] : live )
    {
        if( !fresh.count( id ) )
            commit.Remove( item );
    }

    for( const auto& [id, item] : fresh )
    {
        auto it = live.find( id );

        if( it == live.end() )
        {
            commit.Add( copyOf( item ) );
            continue;
        }

        BOARD_ITEM* now = it->second;

        if( now->Type() != item->Type() )
        {
            replace( now, copyOf( item ) );
            continue;
        }

        bool same = now->Type() == PCB_FOOTPRINT_T
                            ? SameFootprint( static_cast<FOOTPRINT*>( now ), static_cast<FOOTPRINT*>( item ) )
                            : *now == *item;

        if( same && now->Type() == PCB_ZONE_T )
            same = sameFill( static_cast<ZONE*>( now ), static_cast<ZONE*>( item ) );

        if( same )
            continue;

        // An item with children (a footprint's pads, fields and graphics; a table's cells) is
        // replaced, as KiCad's own footprint exchange does: swapping its data and deleting
        // the copy would free the original children while connectivity and the view still
        // point at them.
        if( dynamic_cast<BOARD_ITEM_CONTAINER*>( now ) )
        {
            replace( now, copyOf( item ) );
            continue;
        }

        BOARD_ITEM* copy = copyOf( item );
        commit.Modify( now );
        now->SwapItemData( copy );
        delete copy;
    }

}

} // namespace INDUCTIVE_BOARD_APPLY

#endif
