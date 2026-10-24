/*
 * Inductive: rotations for JLCPCB's assembly placement (CPL) file.
 *
 * KiCad draws a package at zero rotation the IPC-7351 way (pin 1 top left); JLCPCB's
 * parts library draws many of them a quarter or half turn from that, and its machines
 * place by its own library. A placement file of KiCad's raw angles therefore turns SOICs,
 * QFNs and SOT-23s on the assembled board. The table below is the correction per package,
 * first match wins: JLCKicadTools' cpl_rotations_db.csv (github.com/matthewlai/
 * JLCKicadTools, GPL-3.0 like KiCad), which KiBot's JLCPCB output also uses.
 *
 * Bottom-side parts: KiCad mirrors them, JLCPCB does not, so the angle is mirrored
 * (180 - angle) before the correction -- KiBot's JLCPCB settings (mirror_bottom).
 *
 * A footprint field "JLCPCB Rotation Offset" (kicad-jlcpcb-tools' name) overrides the
 * table for one part.
 */

#ifndef INDUCTIVE_JLCPCB_H
#define INDUCTIVE_JLCPCB_H

#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace INDUCTIVE_JLCPCB
{

/// The correction in degrees for a footprint (its library item name, e.g. "SOIC-8_3.9x4.9mm_P1.27mm"),
/// or nothing when JLCPCB's library draws it as KiCad does.
inline std::optional<double> RotationCorrection( const std::string& aFootprintName )
{
    static const std::vector<std::pair<std::regex, double>> table = []
    {
        const std::vector<std::pair<const char*, double>> rows = {
            { "^R_Array_Convex_", 90 },
            { "^R_Array_Concave_", 90 },
            { "^SOT-223", 180 },
            { "^SOT-23", -90 },
            { "^SOT-89", 180 },
            { "^TSOT-23", 180 },
            { "^SOT-353", 180 },
            { "^SOT-363", 180 },
            { "^LQFP-", 270 },
            { "^TQFP-", 270 },
            { "^SOP-4_", 0 },
            { "^SOP-(?!18_)", 270 },
            { "^TSSOP-", 270 },
            { "^SSOP-", 270 },
            { "^DFN-", 270 },
            { "^SOIC-", 270 },
            { "^SOP-18_", 0 },
            { "^VSSOP-8_3.0x3.0mm_P0.65mm", 270 },
            { "^VSSOP-8_", 180 },
            { "^VSSOP-10_", 270 },
            { "^VSON-8_", 270 },
            { "^TSOP-6", 270 },
            { "^UDFN-10", 270 },
            { "^USON-10", 270 },
            { "^TDSON-8-1", 270 },
            { "^CP_EIA-", 180 },
            { "^CP_Elec_8x5.4", 180 },
            { "^CP_Elec_8x10.5", 180 },
            { "^CP_Elec_6.3x7.7", 180 },
            { "^CP_Elec_8x6.7", 180 },
            { "^CP_Elec_8x10", 180 },
            { "^CP_Elec_10x10", 180 },
            { "^(.*?_|V)?QFN-(16|20|24|28|40)(-|_|$)", 270 },
            { "^Bosch_LGA-8_2x2.5mm_P0.65mm_ClockwisePinNumbering", 90 },
            { "^PowerPAK_SO-8_Single", 270 },
            { "^HTSSOP-28-1EP_4.4x9.7mm*", 270 },
            { "^PUIAudio_SMT_0825_S_4_R*", 270 },
            { "^USB_C_Receptacle_HRO_TYPE-C-31-M-12*", 180 },
            { "^ESP32-W", 270 },
            { "^SOIC127P798X216-8N", -90 },
            { "^SW_DIP_SPSTx01_Slide_Copal_CHS-01B_W7.62mm_P1.27mm", -180 },
            { "^BatteryHolder_Keystone_1060_1x2032", -180 },
            { "^SO-14", -90 },
            { "^HTSSOP-", 270 },
            { "^USB_C_Receptacle_XKB_U262-16XN-4BVC11", 0 },
            { "^Relay_DPDT_Omron_G6K-2F-Y", 270 },
            { "^RP2040-QFN-56", 270 },
            { "^TO-277", 90 },
            { "^SW_SPST_B3", 90 },
            { "^Transformer_Ethernet_Pulse_HX0068ANL", 270 },
            { "^JST_GH_SM", 180 },
            { "^JST_PH_S", 180 },
            { "^Diodes_PowerDI3333-8", 270 },
            { "^Quectel_L80-R", 270 },
            { "^SC-74-6", 180 },
            { "^SOT-143", 180 },
            { "^PinHeader_2x05_P1\\.27mm_Vertical", 90 },
            { "^LED_WS2812B-2020_PLCC4_2.0x2.0mm", 90 },
            { "^^WSON-8-1EP_6x5mm_P1.27mm", -90 },
        };

        std::vector<std::pair<std::regex, double>> compiled;

        for( const auto& [pattern, degrees] : rows )
            compiled.emplace_back( std::regex( pattern, std::regex::ECMAScript ), degrees );

        return compiled;
    }();

    for( const auto& [pattern, degrees] : table )
    {
        if( std::regex_search( aFootprintName, pattern ) )
            return degrees;
    }

    return std::nullopt;
}


/// 0 <= degrees < 360.
inline double Normalize( double aDegrees )
{
    double d = std::fmod( aDegrees, 360.0 );

    if( d < 0 )
        d += 360.0;

    // -0.0 and 359.99999 from floating point read as 0.
    if( d > 360.0 - 1e-6 || std::abs( d ) < 1e-6 )
        d = 0.0;

    return d;
}


/// The Rotation column: KiCad's angle (counter-clockwise, as KiCad reports it for either
/// side) with the package's correction, mirrored for the bottom side.
inline double CplRotation( double aKicadDegrees, bool aBottom, double aCorrection )
{
    const double base = aBottom ? 180.0 - aKicadDegrees : aKicadDegrees;
    return Normalize( base + aCorrection );
}

} // namespace INDUCTIVE_JLCPCB

#endif
