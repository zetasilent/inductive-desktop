/*
 * Design checks: what a reviewing engineer catches on a schematic and ERC does not.
 *
 * ERC knows pin types, not parts: an LM317 whose divider draws less than its minimum load, a
 * USB-C receptacle with no CC resistors, an I2C bus without pull-ups and an IC with no
 * decoupling all pass it. These rules read the netlist (parts, pin names and types, nets) and
 * return one line per finding, each with the fix, so the turn that made a mistake can correct
 * it. Conservative on purpose: a line the engineer has to argue with costs more than one missed.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <wx/arrstr.h>
#include <wx/intl.h>
#include <wx/regex.h>
#include <wx/string.h>

#include <pin_type.h>
#include <sch_connection.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>

namespace INDUCTIVE_CHECKS
{

struct PIN
{
    wxString           number;
    wxString           name;   ///< without overbar markup, upper case
    ELECTRICAL_PINTYPE type = ELECTRICAL_PINTYPE::PT_UNSPECIFIED;
    wxString           net;    ///< empty when connected to nothing
};

struct PART
{
    wxString         ref;
    wxString         value;
    wxString         lib;      ///< the library symbol's name, e.g. LM317_TO-220
    std::vector<PIN> pins;
};


namespace detail
{

inline wxString Base( const wxString& aNet )
{
    return aNet.AfterLast( '/' ).Upper();
}


inline bool IsGround( const wxString& aNet )
{
    const wxString name = Base( aNet );
    return name.Contains( wxS( "GND" ) ) || name.Contains( wxS( "VSS" ) ) || name == wxS( "0V" );
}


/// The voltage a net name states: 3V3, V3V3, +5V, +3.3V, VCC_12V, 1V8; VBUS is 5 V. -1 if none.
inline double RailVoltage( const wxString& aNet )
{
    const wxString name = Base( aNet );

    if( name.Contains( wxS( "VBUS" ) ) )
        return 5;

    // 3V3 and 3.3V alike: read as 3 V before, the latter hid every 3.3 V rail from the checks.
    wxRegEx rail( wxS( "([0-9]+)(\\.([0-9]+))?V([0-9]*)" ) );

    if( !rail.Matches( name ) )
        return -1;

    double whole = 0, frac = 0;
    rail.GetMatch( name, 1 ).ToDouble( &whole );
    const wxString decimals = rail.GetMatch( name, 3 ).IsEmpty() ? rail.GetMatch( name, 4 ) : rail.GetMatch( name, 3 );

    if( !decimals.IsEmpty() && decimals.ToDouble( &frac ) )
        frac /= std::pow( 10.0, (double) decimals.length() );

    return whole + frac;
}


/// The letters a reference starts with: R12 -> R, IC3 -> IC.
inline wxString Prefix( const wxString& aRef )
{
    size_t letters = 0;

    while( letters < aRef.length() && wxIsalpha( aRef[letters] ) )
        ++letters;

    return aRef.Left( letters ).Upper();
}


/// A number with an SI letter that may stand in for the decimal point: 4k7, 4.7K, 100R, 0.1u.
inline double Scaled( const wxString& aText, const std::map<wxChar, double>& aUnits )
{
    wxRegEx number( wxS( "^([0-9]+)(\\.([0-9]+))?([A-Z]?)([0-9]*)$" ) );

    if( !number.Matches( aText ) )
        return 0;

    double whole = 0, frac = 0, after = 0;
    number.GetMatch( aText, 1 ).ToDouble( &whole );

    if( const wxString f = number.GetMatch( aText, 3 ); !f.IsEmpty() && f.ToDouble( &frac ) )
        frac /= std::pow( 10.0, (double) f.length() );

    if( const wxString a = number.GetMatch( aText, 5 ); !a.IsEmpty() && a.ToDouble( &after ) )
        frac += after / std::pow( 10.0, (double) a.length() );

    const wxString unit = number.GetMatch( aText, 4 );

    if( unit.IsEmpty() )
        return aUnits.count( 0 ) ? ( whole + frac ) * aUnits.at( 0 ) : 0;

    auto it = aUnits.find( unit[0] );
    return it == aUnits.end() ? 0 : ( whole + frac ) * it->second;
}


/// A resistor's value in ohms ("1k", "4k7", "4.7K", "100R", "100", "2.2M"); 0 if unreadable.
inline double Ohms( const wxString& aValue )
{
    wxString value = aValue.Upper().BeforeFirst( ' ' );
    value.Replace( wxS( "OHMS" ), wxEmptyString );
    value.Replace( wxS( "OHM" ), wxEmptyString );
    value.Replace( wxString::FromUTF8( "\xCE\xA9" ), wxEmptyString );   // Ω

    return Scaled( value, { { 0, 1 }, { 'R', 1 }, { 'K', 1e3 }, { 'M', 1e6 } } );
}


/// A capacitor's value in farads ("100n", "100nF", "0.1u", "4u7", "10uF 16V", "22p"); 0 if
/// unreadable or given without a unit.
inline double Farads( const wxString& aValue )
{
    // Micro before upper case: Upper() turns the micro sign into a capital Greek mu.
    wxString value = aValue.BeforeFirst( ' ' ).BeforeFirst( '/' );

    for( const char* micro : { "\xC2\xB5", "\xCE\xBC", "\xCE\x9C" } )   // µ, μ, Μ
        value.Replace( wxString::FromUTF8( micro ), wxS( "U" ) );

    value = value.Upper();

    if( value.EndsWith( wxS( "F" ) ) )
        value.RemoveLast();

    return Scaled( value, { { 'P', 1e-12 }, { 'N', 1e-9 }, { 'U', 1e-6 }, { 'M', 1e-3 } } );
}


/// A pin name as shown, without KiCad's overbar markup, upper case.
inline wxString PinName( wxString aName )
{
    for( const wxChar* markup : { wxS( "~{" ), wxS( "_{" ), wxS( "^{" ), wxS( "}" ), wxS( "~" ) } )
        aName.Replace( markup, wxEmptyString );

    return aName.Upper();
}


/// The patterns are this file's own literals, compiled once each (UI thread only).
inline bool Matches( const wxString& aText, const wxChar* aPattern )
{
    static std::map<wxString, std::unique_ptr<wxRegEx>> compiled;
    std::unique_ptr<wxRegEx>&                            re = compiled[aPattern];

    if( !re )
        re = std::make_unique<wxRegEx>( aPattern, wxRE_ADVANCED );

    return re->IsValid() && re->Matches( aText );
}


inline wxString Volts( double aVolts )
{
    return wxString::Format( wxS( "%g V" ), std::round( aVolts * 100 ) / 100 );
}


inline wxString OhmsText( double aOhms )
{
    return aOhms >= 1000 ? wxString::Format( wxS( "%gk" ), aOhms / 1000 ) : wxString::Format( wxS( "%g ohm" ), aOhms );
}

} // namespace detail


/// Every part on every sheet, its units merged, with the net on each pin.
inline std::vector<PART> Collect( SCHEMATIC* aSchematic )
{
    std::map<wxString, PART> byRef;

    for( const SCH_SHEET_PATH& path : aSchematic->BuildSheetListSortedByPageNumbers() )
    {
        SCH_SCREEN* screen = path.LastScreen();

        if( !screen )
            continue;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL*    symbol = static_cast<SCH_SYMBOL*>( item );
            const wxString ref = symbol->GetRef( &path, false );

            if( ref.IsEmpty() || ref.StartsWith( wxS( "#" ) ) || ref.EndsWith( wxS( "?" ) ) )
                continue;

            PART& part = byRef[ref];
            part.ref = ref;
            part.value = symbol->GetValue( false, &path, false );
            part.lib = symbol->GetLibId().GetUniStringLibItemName();

            for( SCH_PIN* pin : symbol->GetPins( &path ) )
            {
                const wxString number = pin->GetNumber();

                if( std::any_of( part.pins.begin(), part.pins.end(), [&]( const PIN& p ) { return p.number == number; } ) )
                    continue;

                SCH_CONNECTION* connection = pin->Connection( &path );
                wxString        net = connection ? connection->Name() : wxString();

                if( net.StartsWith( wxS( "unconnected-" ) ) )
                    net.Clear();

                part.pins.push_back( { number, detail::PinName( pin->GetShownName() ), pin->GetType(), net } );
            }
        }
    }

    std::vector<PART> parts;

    for( auto& [ref, part] : byRef )
        parts.push_back( std::move( part ) );

    return parts;
}


/// The findings, one line each with its fix; empty when nothing applies.
inline std::vector<wxString> Check( const std::vector<PART>& aParts )
{
    using namespace detail;
    using T = ELECTRICAL_PINTYPE;

    struct AT
    {
        const PART* part;
        const PIN*  pin;
    };

    std::map<wxString, std::vector<AT>> nets;

    for( const PART& part : aParts )
        for( const PIN& pin : part.pins )
            if( !pin.net.IsEmpty() )
                nets[pin.net].push_back( { &part, &pin } );

    static const std::vector<AT> none;

    auto on = [&]( const wxString& aNet ) -> const std::vector<AT>&
    {
        auto it = nets.find( aNet );
        return it == nets.end() ? none : it->second;
    };
    // Connected to something besides the pin itself.
    auto connected = [&]( const wxString& aNet )
    {
        return !aNet.IsEmpty() && on( aNet ).size() > 1;
    };
    auto text = []( const PART& aPart )
    {
        return ( aPart.value + wxS( " " ) + aPart.lib ).Upper();
    };
    auto named = [&]( const PART& aPart ) -> wxString
    {
        return aPart.value.IsEmpty() ? aPart.ref : aPart.ref + wxS( " (" ) + aPart.value + wxS( ")" );
    };
    auto twoPin = []( const PART& aPart, const wxChar* aPrefix )
    {
        return aPart.pins.size() == 2 && Prefix( aPart.ref ) == aPrefix;
    };
    auto isIC = []( const PART& aPart )
    {
        const wxString prefix = Prefix( aPart.ref );
        return prefix == wxS( "U" ) || prefix == wxS( "IC" );
    };
    auto isConnector = []( const PART& aPart )
    {
        const wxString prefix = Prefix( aPart.ref );
        return prefix == wxS( "J" ) || prefix == wxS( "P" ) || prefix == wxS( "CN" ) || prefix == wxS( "USB" );
    };
    auto pinNamed = []( const PART& aPart, const wxChar* aPattern ) -> const PIN*
    {
        for( const PIN& pin : aPart.pins )
            if( Matches( pin.name, aPattern ) )
                return &pin;

        return nullptr;
    };
    auto supply = [&]( const wxString& aNet )
    {
        if( aNet.IsEmpty() || IsGround( aNet ) )
            return false;

        if( RailVoltage( aNet ) > 0 || Matches( Base( aNet ), wxS( "^\\+?(VCC|VDD|VIN|VBAT|VSYS|VBUS|VMAIN|VSUP)" ) ) )
            return true;

        for( const AT& at : on( aNet ) )
            if( at.pin->type == T::PT_POWER_IN || at.pin->type == T::PT_POWER_OUT )
                return true;

        return false;
    };
    // The other end of a two-pin part.
    auto otherEnd = []( const PART& aPart, const wxString& aNear ) -> wxString
    {
        if( aPart.pins.size() != 2 )
            return wxEmptyString;

        return aPart.pins[0].net == aNear ? aPart.pins[1].net : aPart.pins[0].net;
    };
    // Two-pin parts of one kind on a net, each once.
    auto twoPins = [&]( const wxString& aNet, const wxChar* aPrefix )
    {
        std::vector<const PART*> found;

        for( const AT& at : on( aNet ) )
            if( twoPin( *at.part, aPrefix ) && std::find( found.begin(), found.end(), at.part ) == found.end() )
                found.push_back( at.part );

        return found;
    };
    auto capsToGround = [&]( const wxString& aNet )
    {
        std::vector<double> farads;

        for( const PART* c : twoPins( aNet, wxS( "C" ) ) )
            if( IsGround( otherEnd( *c, aNet ) ) )
                farads.push_back( Farads( c->value ) );

        return farads;
    };
    auto pulledTo = [&]( const wxString& aNet, const std::function<bool( const wxString& )>& aEnd ) -> const PART*
    {
        for( const PART* r : twoPins( aNet, wxS( "R" ) ) )
            if( otherEnd( *r, aNet ) != aNet && aEnd( otherEnd( *r, aNet ) ) )
                return r;

        return nullptr;
    };
    auto icPins = [&]( const wxString& aNet, const PART* aExcept )
    {
        std::vector<AT> found;

        for( const AT& at : on( aNet ) )
            if( at.part != aExcept && isIC( *at.part ) && at.pin->type != T::PT_POWER_IN )
                found.push_back( at );

        return found;
    };

    // A linear regulator: a power input named VIN and a power output named VOUT.
    auto regulatorPins = [&]( const PART& aPart ) -> std::pair<const PIN*, const PIN*>
    {
        const PIN* in = nullptr;
        const PIN* out = nullptr;

        if( !isIC( aPart ) )
            return { nullptr, nullptr };

        for( const PIN& pin : aPart.pins )
        {
            if( !in && Matches( pin.name, wxS( "^(VIN|IN|VI|INPUT)$" ) ) && pin.type == T::PT_POWER_IN )
                in = &pin;

            if( !out && Matches( pin.name, wxS( "^(VOUT|OUT|VO|OUTPUT)$" ) ) && pin.type == T::PT_POWER_OUT )
                out = &pin;
        }

        return in && out ? std::make_pair( in, out ) : std::make_pair( nullptr, nullptr );
    };
    auto linear = [&]( const PART& aPart )
    {
        return regulatorPins( aPart ).first != nullptr;
    };

    std::vector<wxString> out;

    for( const PART& part : aParts )
    {
        const wxString what = text( part );
        const wxString ref = part.ref;

        // ── ESP32-C3: GPIO2 and GPIO8 are read at reset and must be high ──
        if( what.Contains( wxS( "ESP32-C3" ) ) )
        {
            for( const PIN& pin : part.pins )
            {
                if( !Matches( pin.name, wxS( "^(GPIO|IO)(2|8)$" ) ) )
                    continue;

                if( !supply( pin.net ) && !pulledTo( pin.net, supply ) )
                {
                    out.push_back( wxString::Format( _( "%s %s (pin %s) is a strapping pin with no pull-up: the ESP32-C3 "
                                                        "reads it at reset and needs it high to boot normally. Add a 10k "
                                                        "resistor to 3V3." ),
                                                     ref, pin.name, pin.number ) );
                }
            }
        }

        // ── CH340: R232 high puts an inverter in RXD (read once, at reset) ──
        if( what.Contains( wxS( "CH340" ) ) )
        {
            if( const PIN* r232 = pinNamed( part, wxS( "^R232$" ) );
                r232 && ( supply( r232->net ) || pulledTo( r232->net, supply ) ) )
            {
                out.push_back( wxString::Format( _( "%s R232 (pin %s) is tied high: that enables the CH340's auxiliary RS232 "
                                                    "mode, which inverts RXD, so a TTL UART reads garbage. Leave R232 open "
                                                    "(it has an internal pull-down) or tie it to GND." ),
                                                 named( part ), r232->number ) );
            }
        }

        // ── ESP32: EN needs a pull-up and a capacitor, the power-up delay ──
        if( what.Contains( wxS( "ESP32" ) ) )
        {
            if( const PIN* en = pinNamed( part, wxS( "^(EN|CHIP_EN|CHIP_PU)$" ) ) )
            {
                if( connected( en->net ) && supply( en->net ) )
                {
                    out.push_back( wxString::Format( _( "%s EN is tied straight to the supply: use 10k to 3V3 and 1 uF to "
                                                        "GND instead, the RC delay Espressif requires so the chip starts "
                                                        "after its supply settles." ),
                                                     named( part ) ) );
                }
                else if( !pulledTo( en->net, supply ) )
                {
                    out.push_back( wxString::Format( _( "%s EN has no pull-up: it must not float. Add 10k to 3V3 and 1 uF "
                                                        "to GND (Espressif's power-up delay)." ),
                                                     named( part ) ) );
                }
                else if( capsToGround( en->net ).empty() )
                {
                    out.push_back( wxString::Format( _( "%s EN has a pull-up but no capacitor to GND: add 1 uF, the "
                                                        "power-up delay Espressif requires." ),
                                                     named( part ) ) );
                }
            }
        }

        // ── STM32: BOOT0 low, or it starts the ROM bootloader ──
        if( what.Contains( wxS( "STM32" ) ) )
        {
            if( const PIN* boot = pinNamed( part, wxS( "^BOOT0$" ) ) )
            {
                if( IsGround( boot->net ) || pulledTo( boot->net, IsGround ) )
                {
                }
                else if( connected( boot->net ) && supply( boot->net ) )
                {
                    out.push_back( wxString::Format( _( "%s BOOT0 is tied high: the chip always starts its built-in "
                                                        "bootloader, never your program. Pull it to GND through 10k." ),
                                                     named( part ) ) );
                }
                else
                {
                    out.push_back( wxString::Format( _( "%s BOOT0 has no pull-down: it floats, and the chip may start its "
                                                        "bootloader instead of your program. Add 10k to GND (a button to "
                                                        "3V3 can still select the bootloader)." ),
                                                     named( part ) ) );
                }
            }
        }

        // ── RP2040: flash, core supply, crystal, USB resistors ──
        if( what.Contains( wxS( "RP2040" ) ) )
        {
            const PIN* ss = pinNamed( part, wxS( "^QSPI_SS" ) );

            if( ss && icPins( ss->net, &part ).empty() )
            {
                out.push_back( wxString::Format( _( "%s has no flash on QSPI_SS: it has no internal flash and cannot run "
                                                    "a program. Add a QSPI flash such as a W25Q16JV on the QSPI pins." ),
                                                 named( part ) ) );
            }

            const PIN* vreg = pinNamed( part, wxS( "^VREG_V?OUT$" ) );

            for( const PIN& pin : part.pins )
            {
                if( vreg && pin.name == wxS( "DVDD" ) && pin.net != vreg->net )
                {
                    out.push_back( wxString::Format( _( "%s DVDD (pin %s) is not on VREG_VOUT: the 1.1 V core supply "
                                                        "comes from its own regulator. Join every DVDD pin to VREG_VOUT, "
                                                        "with 1 uF to GND." ),
                                                     named( part ), pin.number ) );
                    break;
                }
            }

            if( const PIN* xin = pinNamed( part, wxS( "^XIN$" ) ) )
            {
                bool clock = false;

                for( const AT& at : on( xin->net ) )
                {
                    const wxString prefix = Prefix( at.part->ref );
                    clock = clock || ( at.part != &part && ( prefix == wxS( "Y" ) || prefix == wxS( "X" ) || isIC( *at.part ) ) );
                }

                if( !clock )
                {
                    out.push_back( wxString::Format( _( "%s has no 12 MHz crystal on XIN/XOUT: USB, including its USB "
                                                        "bootloader, needs one (with its load capacitors)." ),
                                                     named( part ) ) );
                }
            }

            const PART* straight = nullptr;

            for( const PIN& pin : part.pins )
                if( Matches( pin.name, wxS( "^USB_D[PM]$" ) ) )
                    for( const AT& at : on( pin.net ) )
                        if( isConnector( *at.part ) )
                            straight = at.part;

            if( straight )
            {
                out.push_back( wxString::Format( _( "%s USB_DP/USB_DM go straight to %s: the RP2040 needs a 27 ohm series "
                                                    "resistor on each." ),
                                                 named( part ), straight->ref ) );
            }
        }

        // ── Adjustable regulators: minimum load, and the output the divider sets ──
        if( Matches( what, wxS( "LM317|LM338|LM350|LM1117|AMS1117|AP1117|LD1117|LM1086|LM1085|LT1086|LT1117" ) ) )
        {
            const PIN* adj = pinNamed( part, wxS( "^ADJ" ) );
            const PIN* vo = pinNamed( part, wxS( "^(VO|VOUT|OUT|OUTPUT)$" ) );

            if( adj && vo && connected( adj->net ) )
            {
                const PART* r1 = pulledTo( adj->net, [&]( const wxString& aNet ) { return aNet == vo->net; } );
                const PART* r2 = pulledTo( adj->net, IsGround );
                const double ohms1 = r1 ? Ohms( r1->value ) : 0;
                const double ohms2 = r2 ? Ohms( r2->value ) : 0;

                if( ohms1 > 0 && 1.25 / ohms1 < 0.005 )
                {
                    out.push_back( wxString::Format( _( "%s: %s (%s, VO to ADJ) draws only %.1f mA, below the regulator's "
                                                        "minimum load (3.5 to 10 mA in its datasheet); with nothing else "
                                                        "drawing current the output rises. Use 240 ohm (5 mA) or 120 ohm "
                                                        "(10 mA) and scale R2 to keep the output: R2 = R1 x (Vout / 1.25 - 1)." ),
                                                     named( part ), r1->ref, r1->value, 1250 / ohms1 ) );
                }

                const double stated = RailVoltage( vo->net );

                if( ohms1 > 0 && ohms2 > 0 && stated > 0 )
                {
                    const double vout = 1.25 * ( 1 + ohms2 / ohms1 );

                    if( std::abs( vout - stated ) > 0.06 * stated )
                    {
                        out.push_back( wxString::Format( _( "%s: %s and %s set about %s, but its output net %s says %s. "
                                                            "Vout = 1.25 V x (1 + R2 / R1)." ),
                                                         named( part ), r1->ref, r2->ref, Volts( vout ),
                                                         Base( vo->net ), Volts( stated ) ) );
                    }
                }
            }
        }

        // ── Linear regulators: a capacitor on the input and the output ──
        {
            const auto [in, outPin] = regulatorPins( part );

            if( in && outPin )
            {
                const std::vector<double> inCaps = capsToGround( in->net );
                const std::vector<double> outCaps = capsToGround( outPin->net );

                if( connected( in->net ) && inCaps.empty() )
                {
                    out.push_back( wxString::Format( _( "%s has no capacitor on its input (%s, net %s): add the input "
                                                        "capacitor its datasheet asks for (1 to 10 uF) close to the pin." ),
                                                     named( part ), in->name, Base( in->net ) ) );
                }

                if( connected( outPin->net ) && outCaps.empty() )
                {
                    out.push_back( wxString::Format( _( "%s has no capacitor on its output (%s, net %s): a linear "
                                                        "regulator needs one to be stable; see its datasheet (1 to 22 uF)." ),
                                                     named( part ), outPin->name, Base( outPin->net ) ) );
                }
                else if( !outCaps.empty() && Matches( what, wxS( "AMS1117|LM1117|AP1117|LD1117|LM1086" ) )
                         && *std::max_element( outCaps.begin(), outCaps.end() ) < 9.9e-6 )
                {
                    out.push_back( wxString::Format( _( "%s needs at least 10 uF on its output to stay stable (it can "
                                                        "oscillate with less); the largest there now is %s." ),
                                                     named( part ),
                                                     wxString::Format( wxS( "%g uF" ), *std::max_element( outCaps.begin(), outCaps.end() ) * 1e6 ) ) );
                }
            }
        }

        // ── Crystals: a load capacitor on each side ──
        {
            const wxString prefix = Prefix( ref );
            const bool     crystal = ( prefix == wxS( "Y" ) || prefix == wxS( "X" ) || prefix == wxS( "XTAL" ) )
                                 && Matches( what, wxS( "CRYSTAL|XTAL|HZ" ) )
                                 && !Matches( what, wxS( "RESONATOR|OSC" ) )
                                 && std::none_of( part.pins.begin(), part.pins.end(),
                                                  []( const PIN& p ) { return p.type == T::PT_POWER_IN; } );

            if( crystal )
            {
                wxArrayString bare;

                for( const PIN& pin : part.pins )
                    if( connected( pin.net ) && !IsGround( pin.net ) && capsToGround( pin.net ).empty() )
                        bare.Add( Base( pin.net ) );

                if( !bare.IsEmpty() )
                {
                    out.push_back( wxString::Format( _( "%s has no load capacitor on %s: add one from each crystal pin to "
                                                        "GND, 2 x (CL - about 3 pF) for the crystal's load capacitance "
                                                        "CL (15 pF for CL = 10 pF)." ),
                                                     named( part ), wxJoin( bare, ',', '\0' ) ) );
                }
            }
        }

        // ── USB connectors: CC resistors, both pads of each data line, D+ to D+ ──
        if( isConnector( part ) )
        {
            const PIN* cc1 = pinNamed( part, wxS( "^CC1$" ) );
            const PIN* cc2 = pinNamed( part, wxS( "^CC2$" ) );

            if( cc1 && cc2 )
            {
                if( !connected( cc1->net ) && !connected( cc2->net ) )
                {
                    out.push_back( wxString::Format( _( "%s CC1 and CC2 are not connected: a USB-C charger or C-to-C cable "
                                                        "turns on VBUS only when each has its own 5.1k to GND." ),
                                                     named( part ) ) );
                }
                else if( cc1->net == cc2->net )
                {
                    out.push_back( wxString::Format( _( "%s CC1 and CC2 are joined: give each its own 5.1k to GND. Joined, "
                                                        "an e-marked cable reads the board as an audio adapter and gives it "
                                                        "no power (the Raspberry Pi 4 fault)." ),
                                                     named( part ) ) );
                }
                else
                {
                    for( const PIN* cc : { cc1, cc2 } )
                    {
                        if( !connected( cc->net ) )
                        {
                            out.push_back( wxString::Format( _( "%s %s is not connected: add 5.1k to GND. With one, power "
                                                                "comes in one plug orientation only." ),
                                                             named( part ), cc->name ) );
                            continue;
                        }

                        if( !icPins( cc->net, &part ).empty() || pulledTo( cc->net, supply ) )
                            continue;   // a USB-C controller, or a source's pull-up

                        const PART* rd = pulledTo( cc->net, IsGround );

                        if( !rd )
                        {
                            out.push_back( wxString::Format( _( "%s %s has no 5.1k to GND: a USB-C charger will not turn "
                                                                "on VBUS without it." ),
                                                             named( part ), cc->name ) );
                        }
                        else if( const double ohms = Ohms( rd->value ); ohms > 0 && ( ohms < 4600 || ohms > 5700 ) )
                        {
                            out.push_back( wxString::Format( _( "%s %s's pull-down %s is %s: a USB-C sink needs 5.1k "
                                                                "(Rd); other values read as a different device or none." ),
                                                             named( part ), cc->name, rd->ref, rd->value ) );
                        }
                    }
                }
            }

            for( const wxChar* line : { wxS( "D+" ), wxS( "D-" ) } )
            {
                std::vector<const PIN*> pads;

                for( const PIN& pin : part.pins )
                    if( pin.name == line )
                        pads.push_back( &pin );

                std::set<wxString> used;
                bool               open = false;

                for( const PIN* pad : pads )
                {
                    if( connected( pad->net ) )
                        used.insert( pad->net );
                    else
                        open = true;
                }

                if( pads.size() >= 2 && !used.empty() && ( open || used.size() > 1 ) )
                {
                    out.push_back( wxString::Format( _( "%s's %s pads are not joined: connect all of them (A and B side) "
                                                        "to the one %s net, or USB works in one plug orientation only." ),
                                                     named( part ), line, line ) );
                }

                // What each pad reaches, directly or through a series resistor or ferrite.
                const bool plus = wxString( line ) == wxS( "D+" );

                for( const wxString& net : used )
                {
                    std::vector<AT> reached = icPins( net, &part );

                    for( const wxChar* series : { wxS( "R" ), wxS( "FB" ), wxS( "L" ) } )
                        for( const PART* s : twoPins( net, series ) )
                            for( const AT& at : icPins( otherEnd( *s, net ), &part ) )
                                reached.push_back( at );

                    for( const AT& at : reached )
                    {
                        const bool isPlus = Matches( at.pin->name, wxS( "D\\+|(^|_)DP$|USBDP|D_P$" ) );
                        const bool isMinus = Matches( at.pin->name, wxS( "D-|(^|_)DM$|USBDM|D_N$" ) );

                        if( isPlus != isMinus && isPlus != plus )
                        {
                            out.push_back( wxString::Format( _( "%s %s reaches %s %s: D+ and D- are swapped." ),
                                                             named( part ), line, at.part->ref, at.pin->name ) );
                            break;
                        }
                    }
                }
            }
        }

        // ── MOSFETs driven from a pin: held off while the driver resets ──
        if( Prefix( ref ) == wxS( "Q" ) )
        {
            const PIN* gate = pinNamed( part, wxS( "^(G|GATE)$" ) );
            const PIN* source = pinNamed( part, wxS( "^(S|SOURCE)$" ) );

            if( gate && source && connected( gate->net ) )
            {
                // A microcontroller's GPIO, which floats at reset; a gate driver's output
                // (PT_OUTPUT) holds the gate low itself.
                std::set<wxString> sides = { gate->net };
                const PART*        driver = nullptr;
                auto               gpio = []( const AT& aAt )
                {
                    return aAt.pin->type == T::PT_BIDI || aAt.pin->type == T::PT_TRISTATE;
                };

                for( const AT& at : icPins( gate->net, nullptr ) )
                    if( gpio( at ) )
                        driver = at.part;

                for( const PART* r : twoPins( gate->net, wxS( "R" ) ) )
                {
                    for( const AT& at : icPins( otherEnd( *r, gate->net ), nullptr ) )
                    {
                        if( gpio( at ) )
                        {
                            driver = at.part;
                            sides.insert( otherEnd( *r, gate->net ) );
                        }
                    }
                }

                auto toSource = [&]( const wxString& aEnd )
                {
                    return aEnd == source->net || ( IsGround( source->net ) && IsGround( aEnd ) );
                };

                bool held = false;

                for( const wxString& side : sides )
                    held = held || pulledTo( side, toSource );

                if( driver && !held )
                {
                    out.push_back( wxString::Format( _( "%s gate is driven by %s with nothing holding it off while %s "
                                                        "resets (its pins float until the firmware runs): add 100k from "
                                                        "gate to source." ),
                                                     named( part ), driver->ref, driver->ref ) );
                }
            }
        }

        // ── LEDs: something must set the current, and the right amount ──
        const bool led = Prefix( ref ) == wxS( "D" ) && what.Contains( wxS( "LED" ) ) && part.pins.size() == 2;

        if( !led )
            continue;

        const wxString a = part.pins[0].net;
        const wxString b = part.pins[1].net;

        if( ( supply( a ) && IsGround( b ) ) || ( supply( b ) && IsGround( a ) ) )
        {
            out.push_back( wxString::Format( _( "%s is straight across %s and %s with nothing to limit its current: add a "
                                                "series resistor." ),
                                             named( part ), Base( supply( a ) ? a : b ), Base( supply( a ) ? b : a ) ) );
            continue;
        }

        const wxString colour = part.value.Upper();
        const double   vf = colour.Contains( wxS( "BLUE" ) ) || colour.Contains( wxS( "WHITE" ) ) ? 3.0
                            : colour.Contains( wxS( "RED" ) ) || colour.Contains( wxS( "YELLOW" ) )
                                      || colour.Contains( wxS( "ORANGE" ) ) || colour.Contains( wxS( "AMBER" ) )
                              ? 2.0 : 0;

        if( vf == 0 )
            continue;

        bool has3V3 = false;

        for( const auto& [net, pins] : nets )
            has3V3 = has3V3 || std::abs( RailVoltage( net ) - 3.3 ) < 0.01;

        for( int side = 0; side < 2; ++side )
        {
            const wxString here = part.pins[side].net;
            const wxString end = part.pins[1 - side].net;
            std::vector<AT> others;

            for( const AT& at : on( here ) )
                if( at.part != &part )
                    others.push_back( at );

            if( others.size() != 1 || !twoPin( *others[0].part, wxS( "R" ) ) )
                continue;

            const PART&    r = *others[0].part;
            const double   ohms = Ohms( r.value );
            const wxString rFar = otherEnd( r, here );

            if( ohms <= 0 || rFar.IsEmpty() )
                break;

            // Driven from a rail at one end, or from a pin (taken as 3.3 V logic) to ground.
            double volts = std::max( RailVoltage( rFar ), RailVoltage( end ) );

            if( volts <= 0 && has3V3 && ( IsGround( rFar ) || IsGround( end ) ) )
                volts = 3.3;

            if( volts <= 0 )
                break;

            const double amps = ( volts - vf ) / ohms;
            const double target = vf >= 3.0 ? 0.0025 : 0.004;
            const double better = std::max( 10.0, ( volts - vf ) / target );

            if( amps < 0.001 || amps > 0.025 )
            {
                const wxString kind = colour.Contains( wxS( "LED" ) ) ? part.value : part.value + wxS( " LED" );

                out.push_back( wxString::Format( _( "%s (%s, about %.0f V across it) through %s (%s) from about %.1f V: "
                                                    "about %.1f mA, %s. Use about %.0f ohm." ),
                                                 ref, kind, vf, r.ref, r.value, volts, std::max( 0.0, amps * 1000 ),
                                                 amps < 0.001 ? _( "too little to see" ) : _( "over a small LED's 20 mA" ),
                                                 std::round( better / 10 ) * 10 ) );
            }

            break;
        }
    }

    // ── Rails: every IC supply pin decoupled ──
    {
        std::map<wxString, std::vector<wxString>> bare;      // rail -> ICs with no capacitor on it
        std::map<wxString, std::set<wxString>>    supplied;  // rail -> IC supply pins on it

        for( const PART& part : aParts )
        {
            // A linear regulator's own input is the regulator check's (an MCU with an internal
            // regulator output, like the RP2040's VREG_VOUT, still needs its supply pins decoupled).
            const bool regulator = linear( part );

            if( !isIC( part ) || regulator )
                continue;

            // Symbols mark one of several stacked supply pins power-in and the rest passive.
            std::set<wxString> powerNames;

            for( const PIN& pin : part.pins )
                if( pin.type == T::PT_POWER_IN )
                    powerNames.insert( pin.name );

            std::set<wxString> rails;

            for( const PIN& pin : part.pins )
            {
                if( ( pin.type == T::PT_POWER_IN || ( pin.type == T::PT_PASSIVE && powerNames.count( pin.name ) ) )
                    && supply( pin.net ) && connected( pin.net ) )
                {
                    supplied[pin.net].insert( part.ref + wxS( "." ) + pin.number );
                    rails.insert( pin.net );
                }
            }

            for( const wxString& rail : rails )
                if( capsToGround( rail ).empty() )
                    bare[rail].push_back( part.ref );
        }

        for( const auto& [rail, refs] : bare )
        {
            wxArrayString list;

            for( const wxString& r : refs )
                list.Add( r );

            out.push_back( wxString::Format( _( "%s feeds %s with no capacitor to GND: put 100 nF beside each supply "
                                                "pin, and a bulk 10 uF where the rail enters." ),
                                             Base( rail ), wxJoin( list, ',', '\0' ) ) );
        }

        for( const auto& [rail, pins] : supplied )
        {
            if( bare.count( rail ) )
                continue;

            int tiny = 0;

            for( double f : capsToGround( rail ) )
                tiny += f > 0 && f <= 1.1e-6;

            if( pins.size() >= 4 && tiny * 2 < (int) pins.size() )
            {
                out.push_back( wxString::Format( _( "%s feeds %d IC supply pins with %d small capacitor(s): put 100 nF "
                                                    "beside each supply pin, or at least one per pair." ),
                                                 Base( rail ), (int) pins.size(), tiny ) );
            }
        }
    }

    // ── I2C: open-drain lines need pull-ups ──
    for( const auto& [net, pins] : nets )
    {
        if( pins.size() < 2 )
            continue;

        static const wxChar* I2C = wxS( "(^|[^A-Z])(SDA|SCL)([^A-Z]|$)" );
        bool                 bus = Matches( Base( net ), I2C );
        const PART*          connector = nullptr;

        for( const AT& at : pins )
        {
            if( isConnector( *at.part ) )
                connector = at.part;

            if( !twoPin( *at.part, wxS( "R" ) ) && !twoPin( *at.part, wxS( "C" ) ) )
                bus = bus || Matches( at.pin->name, I2C );
        }

        if( !bus )
            continue;

        std::vector<const PART*> ups;

        for( const PART* r : twoPins( net, wxS( "R" ) ) )
            if( supply( otherEnd( *r, net ) ) )
                ups.push_back( r );

        if( ups.empty() )
        {
            out.push_back( wxString::Format( _( "I2C line %s has no pull-up: I2C is open-drain and needs a resistor to the "
                                                "bus supply, 4.7k at 100 kHz or 2.2k at 400 kHz%s." ),
                                             Base( net ),
                                             connector ? wxString::Format( _( " (unless the module on %s has its own)" ),
                                                                           connector->ref )
                                                       : wxString() ) );
        }

        for( const PART* r : ups )
        {
            const double ohms = Ohms( r->value );

            if( ohms > 0 && ( ohms < 1000 || ohms > 10000 ) )
            {
                out.push_back( wxString::Format( _( "I2C pull-up %s on %s is %s: use 2.2k to 10k (%s)." ), r->ref, Base( net ),
                                                 r->value,
                                                 ohms < 1000 ? _( "lower sinks more than a device may pull down" )
                                                             : _( "higher leaves slow edges that misread" ) ) );
            }
        }
    }

    // ── Output enables: a header alone leaves them floating ──
    for( const PART& part : aParts )
    {
        if( !isIC( part ) )
            continue;

        for( const PIN& pin : part.pins )
        {
            if( !Matches( pin.name, wxS( "^/?N?OE#?$" ) ) || pin.net.IsEmpty() || IsGround( pin.net ) || supply( pin.net ) )
                continue;

            const PART* header = nullptr;
            bool        driven = false;

            for( const AT& at : on( pin.net ) )
            {
                if( at.part == &part )
                    continue;

                if( isConnector( *at.part ) )
                    header = at.part;
                else if( !twoPin( *at.part, wxS( "R" ) ) && !twoPin( *at.part, wxS( "C" ) ) )
                    driven = true;   // an IC, a jumper or a switch holds it
            }

            if( !header || driven || pulledTo( pin.net, IsGround ) || pulledTo( pin.net, supply ) )
                continue;

            out.push_back( wxString::Format( _( "%s OE (pin %s) goes only to %s, so it floats whenever nothing is plugged "
                                                "in and the outputs switch on and off at random: add 10k to GND to keep "
                                                "the outputs on by default, or 10k to the supply to hold them off until "
                                                "the controller drives OE low." ),
                                             named( part ), pin.number, header->ref ) );
        }
    }

    return out;
}


/// The findings as the tool results show them, after ERC: a heading and one list line each.
inline wxString Report( const std::vector<PART>& aParts )
{
    const std::vector<wxString> findings = Check( aParts );

    if( findings.empty() )
        return wxEmptyString;

    wxString out = wxS( "\n" ) + _( "Design checks (fix these, or tell the user why not):\n" );

    for( const wxString& line : findings )
        out << wxS( "- " ) << line << wxS( "\n" );

    return out;
}

} // namespace INDUCTIVE_CHECKS
