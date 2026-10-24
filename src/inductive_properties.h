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

#ifndef INDUCTIVE_PROPERTIES_H
#define INDUCTIVE_PROPERTIES_H

/**
 * Any property an item shows in KiCad's Properties panel, read and written by name in the
 * units the tools speak: millimetres, degrees, layer and net names. The dedicated tools
 * cover the common edits; this covers the rest -- a track's width, a zone's clearance, a
 * text's size, a pad's shape, a symbol's mirror -- through the same property system, with
 * the same validators, the panel uses. Shared by both editors; the editor supplies its
 * scale and how to name its layers and nets.
 */

#include <eda_item.h>
#include <geometry/eda_angle.h>
#include <inspectable.h>
#include <properties/property.h>
#include <properties/property_mgr.h>
#include <properties/property_validator.h>
#include <units_provider.h>

#include <wx/regex.h>
#include <wx/string.h>

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace INDUCTIVE_PROPS
{

struct CONTEXT
{
    const EDA_IU_SCALE*                                 scale = nullptr;
    std::function<wxString( int )>                      netName;   ///< net code -> name
    std::function<std::optional<int>( const wxString& )> netCode;  ///< name -> net code
    std::function<std::optional<int>( const wxString& )> layerId;  ///< a board's own layer names
};


/// The properties the Properties panel would show for this item, in its order.
inline std::vector<PROPERTY_BASE*> Visible( EDA_ITEM* aItem )
{
    std::vector<PROPERTY_BASE*> out;

    for( PROPERTY_BASE* property : PROPERTY_MANAGER::Instance().GetProperties( TYPE_HASH( *aItem ) ) )
    {
        if( !property->IsHiddenFromPropertiesManager() && property->Available( aItem ) )
            out.push_back( property );
    }

    return out;
}


/// "track width", "Track_Width" and "Width" all find "Width" when it is the only match.
inline wxString Key( wxString aName )
{
    aName.MakeLower();
    aName.Replace( wxS( "_" ), wxS( " " ) );
    aName.Replace( wxS( "-" ), wxS( " " ) );
    aName.Trim().Trim( false );
    return aName;
}


inline PROPERTY_BASE* Find( EDA_ITEM* aItem, const wxString& aName )
{
    const wxString want = Key( aName );

    for( PROPERTY_BASE* property : Visible( aItem ) )
    {
        if( Key( property->Name() ) == want )
            return property;
    }

    return nullptr;
}


/// A length in internal units: "1.5" and "1.5mm" are millimetres; mil, in, um also read.
inline std::optional<double> ParseLength( wxString aText, const EDA_IU_SCALE& aScale )
{
    aText = aText.Lower();
    aText.Replace( wxS( " " ), wxEmptyString );

    double factor = aScale.IU_PER_MM;

    if( aText.EndsWith( wxS( "mm" ) ) )       aText.RemoveLast( 2 );
    else if( aText.EndsWith( wxS( "mil" ) ) ) { aText.RemoveLast( 3 ); factor = aScale.IU_PER_MILS; }
    else if( aText.EndsWith( wxS( "um" ) ) )  { aText.RemoveLast( 2 ); factor = aScale.IU_PER_MM / 1000.0; }
    else if( aText.EndsWith( wxS( "in" ) ) )  { aText.RemoveLast( 2 ); factor = aScale.IU_PER_MILS * 1000.0; }

    double value = 0;

    if( !aText.ToCDouble( &value ) || !std::isfinite( value ) )
        return std::nullopt;

    return value * factor;
}


inline std::optional<bool> ParseBool( wxString aText )
{
    aText = aText.Lower().Trim().Trim( false );

    if( aText == wxS( "true" ) || aText == wxS( "yes" ) || aText == wxS( "1" ) || aText == wxS( "on" ) )
        return true;

    if( aText == wxS( "false" ) || aText == wxS( "no" ) || aText == wxS( "0" ) || aText == wxS( "off" ) )
        return false;

    return std::nullopt;
}


inline bool IsLength( const PROPERTY_BASE* aProperty )
{
    return aProperty->Display() == PT_SIZE || aProperty->Display() == PT_COORD;
}


inline wxString Mm( double aIU, const EDA_IU_SCALE& aScale )
{
    wxString text = wxString::FromCDouble( aIU / aScale.IU_PER_MM, 4 );

    while( text.Contains( wxS( "." ) ) && ( text.EndsWith( wxS( "0" ) ) || text.EndsWith( wxS( "." ) ) ) )
        text.RemoveLast();

    return text;
}


/// The value as the tools write it; empty when the type has no text form (a colour).
inline wxString Read( EDA_ITEM* aItem, PROPERTY_BASE* aProperty, const CONTEXT& aCtx )
{
    const wxAny   any = aItem->Get( aProperty );
    const size_t  type = aProperty->TypeHash();
    const EDA_IU_SCALE& scale = *aCtx.scale;

    // A net before choices: once the Properties panel has run, Net carries every net as a
    // choice, labelled without the leading "/" the tools accept either way.
    if( aProperty->Display() == PT_NET && aCtx.netName )
    {
        int code = 0;

        if( any.GetAs<int>( &code ) )
            return aCtx.netName( code );
    }

    if( aProperty->HasChoices() )
    {
        int value = 0;

        if( any.GetAs<int>( &value ) )
        {
            const wxPGChoices& choices = aProperty->Choices();

            for( unsigned i = 0; i < choices.GetCount(); ++i )
                if( choices.GetValue( i ) == value )
                    return choices.GetLabel( i );

            return wxString::Format( wxS( "%d" ), value );
        }
    }

    if( type == TYPE_HASH( int ) )
    {
        const int value = any.As<int>();

        if( aProperty->Display() == PT_NET && aCtx.netName )
            return aCtx.netName( value );

        return IsLength( aProperty ) ? Mm( value, scale ) : wxString::Format( wxS( "%d" ), value );
    }

    if( type == TYPE_HASH( long long ) )
    {
        const long long value = any.As<long long>();
        return IsLength( aProperty ) ? Mm( (double) value, scale ) : wxString::Format( wxS( "%lld" ), value );
    }

    if( type == TYPE_HASH( double ) )
    {
        const double value = any.As<double>();
        return IsLength( aProperty ) ? Mm( value, scale ) : wxString::FromCDouble( value, 4 );
    }

    if( type == TYPE_HASH( bool ) )
        return any.As<bool>() ? wxS( "true" ) : wxS( "false" );

    if( type == TYPE_HASH( wxString ) )
        return any.As<wxString>();

    if( type == TYPE_HASH( EDA_ANGLE ) )
        return wxString::FromCDouble( any.As<EDA_ANGLE>().AsDegrees(), 2 );

    if( type == TYPE_HASH( std::optional<int> ) )
    {
        const std::optional<int> value = any.As<std::optional<int>>();

        if( !value )
            return wxS( "none" );

        return IsLength( aProperty ) ? Mm( *value, scale ) : wxString::Format( wxS( "%d" ), *value );
    }

    if( type == TYPE_HASH( std::optional<double> ) )
    {
        const std::optional<double> value = any.As<std::optional<double>>();
        return value ? wxString::FromCDouble( *value, 4 ) : wxString( wxS( "none" ) );
    }

    wxString text;

    if( any.GetAs<wxString>( &text ) )
        return text;

    return wxEmptyString;
}


/// What the value can be, for the model: a unit, "true/false" or the choices.
inline wxString Kind( PROPERTY_BASE* aProperty )
{
    if( aProperty->Display() == PT_NET )
        return wxS( "net name" );

    if( aProperty->HasChoices() )
    {
        wxArrayString labels;
        const wxPGChoices& choices = aProperty->Choices();

        for( unsigned i = 0; i < choices.GetCount() && i < 12; ++i )
            labels.Add( choices.GetLabel( i ) );

        return wxS( "one of: " ) + wxJoin( labels, '|', '\0' ) + ( choices.GetCount() > 12 ? wxS( "|..." ) : wxS( "" ) );
    }

    const size_t type = aProperty->TypeHash();

    if( IsLength( aProperty ) )                          return wxS( "mm" );
    if( aProperty->Display() == PT_NET )                 return wxS( "net name" );
    if( type == TYPE_HASH( bool ) )                      return wxS( "true/false" );
    if( type == TYPE_HASH( EDA_ANGLE ) || aProperty->Display() == PT_DEGREE
        || aProperty->Display() == PT_DECIDEGREE )       return wxS( "degrees" );
    if( type == TYPE_HASH( wxString ) )                  return wxS( "text" );
    if( type == TYPE_HASH( std::optional<int> ) || type == TYPE_HASH( std::optional<double> ) )
        return wxS( "number or none" );

    return wxS( "number" );
}


/**
 * Set one property from text. Empty on success, else why not -- in words the model can
 * act on. aUnits formats a validator's complaint; the item is changed only when every
 * check passed. The caller owns the commit (Modify the item first) and the undo step.
 */
inline wxString Write( EDA_ITEM* aItem, PROPERTY_BASE* aProperty, const wxString& aText,
                       const CONTEXT& aCtx, UNITS_PROVIDER* aUnits )
{
    if( !aProperty->Writeable( aItem ) )
        return wxString::Format( wxS( "'%s' is read-only here" ), aProperty->Name() );

    const size_t        type = aProperty->TypeHash();
    const EDA_IU_SCALE& scale = *aCtx.scale;
    wxAny               value;

    if( aProperty->Display() == PT_NET )
    {
        std::optional<int> code = aCtx.netCode ? aCtx.netCode( aText ) : std::nullopt;

        if( !code )
            return wxString::Format( wxS( "there is no net '%s'" ), aText );

        value = *code;
    }
    else if( aProperty->HasChoices() )
    {
        const wxPGChoices& choices = aProperty->Choices();
        const wxString     want = Key( aText );
        std::optional<int> chosen;

        for( unsigned i = 0; i < choices.GetCount() && !chosen; ++i )
            if( Key( choices.GetLabel( i ) ) == want )
                chosen = choices.GetValue( i );

        // A board's own layer names ("GND plane" for In1.Cu) as well as the standard ones.
        if( !chosen && aCtx.layerId )
            chosen = aCtx.layerId( aText );

        if( !chosen )
            return wxString::Format( wxS( "'%s' is not a choice for '%s' (%s)" ), aText, aProperty->Name(),
                                     Kind( aProperty ) );

        value = *chosen;
    }
    else if( type == TYPE_HASH( int ) )
    {
        if( IsLength( aProperty ) )
        {
            std::optional<double> iu = ParseLength( aText, scale );

            if( !iu || std::abs( *iu ) > std::numeric_limits<int>::max() / 2.0 )
                return wxString::Format( wxS( "'%s' is not a length for '%s' (mm)" ), aText, aProperty->Name() );

            value = (int) std::lround( *iu );
        }
        else
        {
            long number = 0;

            if( !aText.ToLong( &number ) )
                return wxString::Format( wxS( "'%s' is not a whole number for '%s'" ), aText, aProperty->Name() );

            value = (int) number;
        }
    }
    else if( type == TYPE_HASH( long long ) )
    {
        std::optional<double> iu = IsLength( aProperty ) ? ParseLength( aText, scale ) : std::nullopt;
        double                number = 0;

        if( iu )
            value = (long long) std::llround( *iu );
        else if( aText.ToCDouble( &number ) )
            value = (long long) std::llround( number );
        else
            return wxString::Format( wxS( "'%s' is not a number for '%s'" ), aText, aProperty->Name() );
    }
    else if( type == TYPE_HASH( double ) )
    {
        std::optional<double> number = IsLength( aProperty ) ? ParseLength( aText, scale ) : std::nullopt;
        double                plain = 0;

        if( !number && aText.ToCDouble( &plain ) && std::isfinite( plain ) )
            number = plain;

        if( !number )
            return wxString::Format( wxS( "'%s' is not a number for '%s'" ), aText, aProperty->Name() );

        value = *number;
    }
    else if( type == TYPE_HASH( bool ) )
    {
        std::optional<bool> flag = ParseBool( aText );

        if( !flag )
            return wxString::Format( wxS( "'%s' is not true or false for '%s'" ), aText, aProperty->Name() );

        value = *flag;
    }
    else if( type == TYPE_HASH( wxString ) )
    {
        value = aText;
    }
    else if( type == TYPE_HASH( EDA_ANGLE ) )
    {
        wxString degrees = aText.Lower();
        degrees.Replace( wxS( "deg" ), wxEmptyString );
        degrees.Replace( wxS( "°" ), wxEmptyString );
        double number = 0;

        if( !degrees.Trim().Trim( false ).ToCDouble( &number ) || !std::isfinite( number ) )
            return wxString::Format( wxS( "'%s' is not an angle in degrees for '%s'" ), aText, aProperty->Name() );

        value = EDA_ANGLE( number, DEGREES_T );
    }
    else if( type == TYPE_HASH( std::optional<int> ) || type == TYPE_HASH( std::optional<double> ) )
    {
        const bool none = Key( aText ) == wxS( "none" ) || aText.IsEmpty();
        std::optional<double> number;
        double                plain = 0;

        if( !none )
        {
            number = IsLength( aProperty ) ? ParseLength( aText, scale ) : std::nullopt;

            if( !number && aText.ToCDouble( &plain ) && std::isfinite( plain ) )
                number = plain;

            if( !number )
                return wxString::Format( wxS( "'%s' is not a number or none for '%s'" ), aText, aProperty->Name() );
        }

        if( type == TYPE_HASH( std::optional<int> ) )
            value = none ? std::optional<int>() : std::optional<int>( (int) std::lround( *number ) );
        else
            value = none ? std::optional<double>() : std::optional<double>( *number );
    }
    else
    {
        return wxString::Format( wxS( "'%s' cannot be set from text" ), aProperty->Name() );
    }

    wxAny            check = value;
    VALIDATOR_RESULT problem = aProperty->Validate( std::move( check ), aItem );

    if( problem && *problem )
        return wxString::Format( wxS( "'%s': %s" ), aProperty->Name(), ( *problem )->Format( aUnits ) );

    try
    {
        if( !aItem->Set( aProperty, value ) )
            return wxString::Format( wxS( "'%s' does not apply to this item" ), aProperty->Name() );
    }
    catch( const std::exception& e )
    {
        return wxString::Format( wxS( "'%s' was not set: %s" ), aProperty->Name(), e.what() );
    }

    return wxEmptyString;
}

} // namespace INDUCTIVE_PROPS

#endif // INDUCTIVE_PROPERTIES_H
