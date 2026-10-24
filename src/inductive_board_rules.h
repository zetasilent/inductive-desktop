/*
 * Small board rules the editor tools use: generic KiCad helpers, kept apart from the
 * layout engine (which runs on Inductive's servers, not in the app).
 */

#ifndef INDUCTIVE_BOARD_RULES_H
#define INDUCTIVE_BOARD_RULES_H

#include <board.h>
#include <board_design_settings.h>
#include <drc/drc_engine.h>
#include <footprint.h>
#include <netclass.h>
#include <project.h>
#include <project/net_settings.h>
#include <wildcards_and_files_ext.h>
#include <zone.h>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace INDUCTIVE_BOARD_RULES
{

inline bool IsSilk( PCB_LAYER_ID aLayer ) { return aLayer == F_SilkS || aLayer == B_SilkS; }


/**
 * The antenna keep-out of a radio module (ESP32 WROOM, and the like): a rule area that
 * forbids tracks and copper pour. Espressif's first layout rule is the antenna at the
 * board edge, pointing out, with nothing under it; mid-board the keep-out also takes a
 * third of the board.
 */
inline std::vector<BOX2I> AntennaKeepouts( FOOTPRINT* aFp )
{
    std::vector<BOX2I> boxes;

    for( ZONE* zone : aFp->Zones() )
    {
        if( zone->GetIsRuleArea() && zone->GetDoNotAllowTracks() && zone->GetDoNotAllowZoneFills() )
            boxes.push_back( zone->Outline()->BBox() );
    }

    return boxes;
}


/**
 * Recompile the board's rule engine after a net class or board rule changed, as Board
 * Setup does on OK. Class clearances are compiled into it when the board loads, and a
 * class added since has none. KiCad fits teardrops and fills zones to what it reports:
 * teardrops between the Power class's VBUS and GND came 0.187 mm from the next pad of a
 * USB-C receptacle (0.2 mm apart), and /layout's DRC, which recompiles, found them.
 */
inline void RecompileRules( BOARD* aBoard )
{
    std::shared_ptr<DRC_ENGINE> engine = aBoard->GetDesignSettings().m_DRCEngine;

    if( !engine )
        return;     // the filler builds one from the rules in force

    wxString rulesPath;

    if( !aBoard->GetFileName().IsEmpty() && aBoard->GetProject() )
    {
        wxFileName fn( aBoard->GetFileName() );
        fn.SetExt( FILEEXT::DesignRulesFileExtension );
        rulesPath = aBoard->GetProject()->AbsolutePath( fn.GetFullName() );
    }

    try
    {
        engine->InitEngine( wxFileName( rulesPath ) );
    }
    catch( ... )
    {
        // A rules file that does not compile is run_drc's to report.
    }

    // The clearance cache on each item is keyed on it.
    aBoard->IncrementTimeStamp();
}


/// IPC-2221 track width for a continuous current on an outer layer, rounded up to 0.05 mm.
inline double Ipc2221WidthMm( double aAmps, double aRiseC = 10.0, double aOz = 1.0 )
{
    // I = 0.048 dT^0.44 A^0.725, A in mil^2; copper is 1.378 mil thick per ounce.
    const double area = std::pow( aAmps / ( 0.048 * std::pow( aRiseC, 0.44 ) ), 1.0 / 0.725 );
    const double width = area / ( 1.378 * aOz ) * 0.0254;
    // Up to the next 0.05 mm, never below IPC by more than a micron (0.3005 reads 0.30).
    return std::ceil( ( width - 0.001 ) / 0.05 ) * 0.05;
}


/**
 * Put nets in a net class of the given track width, creating it from the
 * Default class if needed, and dropping exact-name assignments the nets had.
 */
inline void AssignNetClass( BOARD* aBoard, const std::vector<wxString>& aNets, const wxString& aClass,
                            int aWidth, const wxString& aDescription, int aClearance = 0 )
{
    BOARD_DESIGN_SETTINGS&        bds = aBoard->GetDesignSettings();
    std::shared_ptr<NET_SETTINGS> settings = bds.m_NetSettings;
    std::shared_ptr<NETCLASS>     def = settings->GetDefaultNetclass();
    std::shared_ptr<NETCLASS>     nc;

    if( settings->HasNetclass( aClass ) )
    {
        nc = settings->GetNetclasses().at( aClass );
    }
    else
    {
        nc = std::make_shared<NETCLASS>( aClass, false );
        nc->SetClearance( def->GetClearance() );
        nc->SetViaDiameter( def->GetViaDiameter() );
        nc->SetViaDrill( def->GetViaDrill() );
        settings->SetNetclass( aClass, nc );
    }

    nc->SetTrackWidth( aWidth );
    if( aClearance > 0 ) nc->SetClearance( aClearance );
    nc->SetDescription( aDescription );

    auto& patterns = settings->GetNetclassPatternAssignments();
    patterns.erase( std::remove_if( patterns.begin(), patterns.end(),
                                    [&]( const auto& entry )
                                    { return std::find( aNets.begin(), aNets.end(), entry.first->GetPattern() ) != aNets.end(); } ),
                    patterns.end() );

    for( const wxString& name : aNets )
        settings->SetNetclassPatternAssignment( name, aClass );

    settings->ClearAllCaches();
    aBoard->SynchronizeNetsAndNetClasses( false );
    RecompileRules( aBoard );
}

} // namespace INDUCTIVE_BOARD_RULES

#endif
