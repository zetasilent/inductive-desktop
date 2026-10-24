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

#ifndef INDUCTIVE_TOOL_ARGS_H
#define INDUCTIVE_TOOL_ARGS_H

/**
 * Argument validation for Inductive's editor tools.
 *
 * Every value in here arrives as JSON that a language model wrote, which makes
 * it the least trustworthy input in the program: not adversarial by default, but
 * routinely wrong in ways that ordinary code does not defend against.  A model
 * that means 2.5 mm will occasionally emit the string "2.5mm", the number 2500,
 * `null`, or nothing at all, and a model that has been talked into misbehaving by
 * text it read off a board will emit whatever that text asked for.
 *
 * Two of those failures are memory-unsafe rather than merely wrong, and both are
 * reachable from a single bad number:
 *
 *   - Board coordinates are **nanometres in an int32**, so the representable
 *     range is +/-2.147 metres.  EDA_IU_SCALE::mmToIU() is an unchecked
 *     `(int) ( mm * 1e6 + 0.5 )`; a double outside int range converts with
 *     **undefined behaviour**, not a wrap.
 *   - `NaN` and `Infinity` reach the same cast.  nlohmann::json parses them
 *     happily from a JSON number, and `(int) NaN` is likewise undefined.
 *
 * So no double from the model may reach KiCad without passing isfinite() and a
 * range check.  Everything here throws BAD_ARGUMENT on refusal rather than
 * returning a sentinel: the panel's dispatcher already catches std::exception
 * around every tool call and turns it into a failed call the model can read and
 * correct, so a validation failure costs one tool round and never the editor.
 *
 * The helpers are deliberately strict.  Silently coercing "2.5mm" to 2.5 would
 * teach the model that the sloppy form works, and it would then use it somewhere
 * the coercion does not apply.  Refusing with a message that names the field and
 * the expected type gets it right on the next round.
 */

#include <nlohmann/json.hpp>
#include <wx/string.h>
#include <wx/translation.h>

#include <cmath>
#include <stdexcept>
#include <string>

namespace INDUCTIVE_ARGS
{

/// The board open_pcb writes for a project that has none: empty, two copper layers by
/// default, in the current file format (an older version makes KiCad show "This file was
/// created by an older version" on a brand-new board). native-board-qa checks the
/// version against SEXPR_BOARD_FILE_VERSION.
inline const char* const EMPTY_BOARD = "(kicad_pcb (version 20260206) (generator \"inductive\"))\n";


/**
 * A refusal to act on the model's arguments.
 *
 * Derives from std::runtime_error so the panel's existing catch of
 * std::exception reports what() to the model verbatim.  The message is written
 * for the model to act on: it names the offending field and what was expected.
 */
class BAD_ARGUMENT : public std::runtime_error
{
public:
    explicit BAD_ARGUMENT( const wxString& aMessage ) :
            std::runtime_error( std::string( aMessage.utf8_str() ) )
    {}
};


/**
 * The largest coordinate, in millimetres, a tool will accept.
 *
 * The int32 nanometre representation reaches +/-2147 mm, but nothing legitimate
 * approaches that: KiCad's own maximum board is a metre, and the biggest panel
 * anyone fabricates is well under half of one.  Refusing at 1000 mm keeps a
 * mistyped exponent an error message rather than a board with a footprint two
 * kilometres away that the user then has to find, and leaves the conversion
 * itself a factor of two clear of the representable edge.
 */
constexpr double MAX_COORD_MM = 1000.0;

/// The largest size or length a tool will accept, in millimetres.
constexpr double MAX_SIZE_MM = 1000.0;

/// The smallest non-zero dimension worth accepting.  Below this KiCad rounds to
/// nothing and the user gets an invisible object they cannot select.
constexpr double MIN_SIZE_MM = 0.001;


/**
 * Parse a tool's argument blob into an object.
 *
 * The panel hands arguments over as the JSON text of whatever the model emitted.
 * A model that calls a zero-argument tool sometimes sends nothing at all, so an
 * empty string is an empty object rather than an error.
 */
inline nlohmann::json ParseObject( const wxString& aArgs )
{
    const std::string text = std::string( aArgs.utf8_str() );

    if( text.empty() )
        return nlohmann::json::object();

    const nlohmann::json parsed = nlohmann::json::parse( text, nullptr, false );

    if( parsed.is_discarded() )
        throw BAD_ARGUMENT( _( "The arguments were not valid JSON." ) );

    if( parsed.is_null() )
        return nlohmann::json::object();

    if( !parsed.is_object() )
        throw BAD_ARGUMENT( _( "The arguments must be a JSON object." ) );

    return parsed;
}


/// True if the key is present and not JSON null.  Absent and null are the same
/// thing here: models emit null for "I have no value for this".
inline bool Has( const nlohmann::json& aArgs, const char* aKey )
{
    const auto it = aArgs.find( aKey );
    return it != aArgs.end() && !it->is_null();
}


/**
 * Reject text that must never reach a document, whatever it names.
 *
 * Two classes, and the second is the reason this function exists separately.
 *
 * **Control characters** travel invisibly into a board file, a filename and the
 * sidebar transcript.  Nothing this surface accepts has a use for one.
 *
 * **KiCad text variables.**  This is a live exfiltration chain, not a
 * hypothetical.  `common/common.cpp:616` resolves any unmatched `${NAME}` in
 * board or schematic text out of the *process environment*:
 *
 *     else if( wxGetEnv( strVarName, &tmp ) )
 *
 * and an API key can sit in that same environment.  So a model that has been
 * talked into writing the literal text `${SOME_API_KEY}` into a footprint value
 * does not write that string -- KiCad
 * renders the key onto the canvas and bakes it into every Gerber, PDF and BOM
 * that board is exported to.  The board then carries the credential to whoever
 * receives the fabrication files.
 *
 * KiCad's own field validator does not stop this: `common/validators.cpp`
 * excludes only \r, \n and \t.  So it is stopped here, at the one point every
 * model-supplied string passes through, before any commit is opened.
 */
inline void RejectDangerousText( const wxString& aValue, const char* aKey )
{
    for( wxUniChar c : aValue )
    {
        if( c < 0x20 || c == 0x7F )
        {
            throw BAD_ARGUMENT( wxString::Format(
                    _( "Argument '%s' contains a control character." ), aKey ) );
        }
    }

    if( aValue.Contains( wxS( "${" ) ) || aValue.Contains( wxS( "@{" ) ) )
    {
        throw BAD_ARGUMENT( wxString::Format(
                _( "Argument '%s' contains a text variable ( ${...} ), which KiCad would "
                   "expand out of the environment when it renders the document. Write the "
                   "literal text instead." ),
                aKey ) );
    }
}


/**
 * A required string.
 *
 * Rejects the empty string as well as the wrong type: every string argument in
 * this tool surface names something -- a reference designator, a layer, a net --
 * and the empty name is never the one that was meant.
 */
inline wxString RequireString( const nlohmann::json& aArgs, const char* aKey )
{
    const auto it = aArgs.find( aKey );

    if( it == aArgs.end() || it->is_null() )
        throw BAD_ARGUMENT( wxString::Format( _( "Missing required argument '%s'." ), aKey ) );

    if( !it->is_string() )
    {
        throw BAD_ARGUMENT(
                wxString::Format( _( "Argument '%s' must be a string." ), aKey ) );
    }

    const std::string raw = it->get<std::string>();
    wxString          value = wxString::FromUTF8( raw );

    // FromUTF8() answers an EMPTY string for invalid UTF-8 rather than failing,
    // so a malformed blob would otherwise arrive here as "no value" instead of
    // as an error, and be reported as a missing argument the model did supply.
    if( value.IsEmpty() && !raw.empty() )
    {
        throw BAD_ARGUMENT(
                wxString::Format( _( "Argument '%s' is not valid UTF-8." ), aKey ) );
    }

    value.Trim( true ).Trim( false );

    if( value.IsEmpty() )
        throw BAD_ARGUMENT( wxString::Format( _( "Argument '%s' must not be empty." ), aKey ) );

    RejectDangerousText( value, aKey );

    return value;
}


/// An optional string, defaulting when absent, null or empty. Models send "" for an
/// optional argument they mean to leave out often enough that refusing it is a failed
/// tool call for nothing.
inline wxString OptionalString( const nlohmann::json& aArgs, const char* aKey,
                                const wxString& aDefault = wxEmptyString )
{
    if( !Has( aArgs, aKey ) )
        return aDefault;

    if( const auto it = aArgs.find( aKey ); it->is_string() )
    {
        const std::string raw = it->get<std::string>();
        wxString          value = wxString::FromUTF8( raw );

        // Blank, not malformed: FromUTF8() answers empty for invalid UTF-8 too.
        if( ( raw.empty() || !value.IsEmpty() ) && value.Trim( true ).Trim( false ).IsEmpty() )
            return aDefault;
    }

    return RequireString( aArgs, aKey );
}


/**
 * A finite double within [aMin, aMax].
 *
 * This is the function that stands between the model and the undefined
 * behaviour described at the top of this file.  Every millimetre value in the
 * tool surface goes through it or through one of the wrappers below.
 */
inline double RequireNumber( const nlohmann::json& aArgs, const char* aKey, double aMin,
                             double aMax )
{
    const auto it = aArgs.find( aKey );

    if( it == aArgs.end() || it->is_null() )
        throw BAD_ARGUMENT( wxString::Format( _( "Missing required argument '%s'." ), aKey ) );

    // Strings are refused rather than parsed.  "2.5mm" is the common model error
    // and accepting it here would leave the unit silently ignored.
    if( !it->is_number() )
    {
        throw BAD_ARGUMENT( wxString::Format(
                _( "Argument '%s' must be a number, with no unit suffix." ), aKey ) );
    }

    const double value = it->get<double>();

    if( !std::isfinite( value ) )
    {
        throw BAD_ARGUMENT(
                wxString::Format( _( "Argument '%s' is not a finite number." ), aKey ) );
    }

    if( value < aMin || value > aMax )
    {
        throw BAD_ARGUMENT( wxString::Format(
                _( "Argument '%s' is %g, which is outside the allowed range %g to %g." ), aKey,
                value, aMin, aMax ) );
    }

    return value;
}


/**
 * An optional finite double within range, defaulting when absent.
 *
 * The DEFAULT IS NOT RANGE-CHECKED -- it is the caller's own value, returned
 * untouched. That is deliberate, but it is a trap worth naming: passing a
 * document's existing coordinate as the default and then converting the result
 * will refuse an item that legitimately sits outside the accepted band. Convert
 * only what the model actually supplied; see move_footprint.
 */
inline double OptionalNumber( const nlohmann::json& aArgs, const char* aKey, double aDefault,
                              double aMin, double aMax )
{
    if( !Has( aArgs, aKey ) )
        return aDefault;

    return RequireNumber( aArgs, aKey, aMin, aMax );
}


/// A required coordinate in millimetres, bounded well inside the int32 nanometre range.
inline double RequireCoordMm( const nlohmann::json& aArgs, const char* aKey )
{
    return RequireNumber( aArgs, aKey, -MAX_COORD_MM, MAX_COORD_MM );
}


/// An optional coordinate in millimetres.
inline double OptionalCoordMm( const nlohmann::json& aArgs, const char* aKey, double aDefault )
{
    return OptionalNumber( aArgs, aKey, aDefault, -MAX_COORD_MM, MAX_COORD_MM );
}


/// A required positive dimension in millimetres -- a width, a diameter, a length.
inline double RequireSizeMm( const nlohmann::json& aArgs, const char* aKey )
{
    return RequireNumber( aArgs, aKey, MIN_SIZE_MM, MAX_SIZE_MM );
}


/// An optional positive dimension in millimetres.
inline double OptionalSizeMm( const nlohmann::json& aArgs, const char* aKey, double aDefault )
{
    if( !Has( aArgs, aKey ) )
        return aDefault;

    return RequireSizeMm( aArgs, aKey );
}


/**
 * A required integer within [aMin, aMax].
 *
 * Goes through the double path first so that 1e40, NaN and 2.5 are all refused
 * with an explanation rather than converted.  json::get<int> would truncate the
 * last of those silently and is undefined for the first two.
 */
inline int RequireInt( const nlohmann::json& aArgs, const char* aKey, int aMin, int aMax )
{
    const double value = RequireNumber( aArgs, aKey, static_cast<double>( aMin ),
                                        static_cast<double>( aMax ) );

    if( value != std::floor( value ) )
    {
        throw BAD_ARGUMENT(
                wxString::Format( _( "Argument '%s' must be a whole number." ), aKey ) );
    }

    return static_cast<int>( value );
}


/// An optional integer within range.
inline int OptionalInt( const nlohmann::json& aArgs, const char* aKey, int aDefault, int aMin,
                        int aMax )
{
    if( !Has( aArgs, aKey ) )
        return aDefault;

    return RequireInt( aArgs, aKey, aMin, aMax );
}


/**
 * An optional boolean.
 *
 * Models emit `"true"` as often as `true`, and the string form is unambiguous,
 * so it is accepted here where a numeric coercion would not be.
 */
inline bool OptionalBool( const nlohmann::json& aArgs, const char* aKey, bool aDefault )
{
    if( !Has( aArgs, aKey ) )
        return aDefault;

    const auto& value = aArgs.at( aKey );

    if( value.is_boolean() )
        return value.get<bool>();

    if( value.is_string() )
    {
        const wxString text = wxString::FromUTF8( value.get<std::string>() ).Lower();

        if( text == wxS( "true" ) || text == wxS( "yes" ) || text == wxS( "1" ) )
            return true;

        if( text == wxS( "false" ) || text == wxS( "no" ) || text == wxS( "0" ) )
            return false;
    }

    throw BAD_ARGUMENT( wxString::Format( _( "Argument '%s' must be true or false." ), aKey ) );
}


/**
 * A required array of strings, bounded in length.
 *
 * The bound is the point: a delete tool that takes a list is the one place a
 * single call can do unbounded damage, and the panel's per-turn mutation budget
 * counts calls rather than items.  A single string is accepted as a list of one,
 * because models emit that form constantly.
 */
inline std::vector<wxString> RequireStringArray( const nlohmann::json& aArgs, const char* aKey,
                                                 size_t aMaxItems = 200 )
{
    const auto it = aArgs.find( aKey );

    if( it == aArgs.end() || it->is_null() )
        throw BAD_ARGUMENT( wxString::Format( _( "Missing required argument '%s'." ), aKey ) );

    std::vector<wxString> out;

    if( it->is_string() )
    {
        out.push_back( RequireString( aArgs, aKey ) );
        return out;
    }

    if( !it->is_array() )
    {
        throw BAD_ARGUMENT(
                wxString::Format( _( "Argument '%s' must be an array of strings." ), aKey ) );
    }

    if( it->empty() )
        throw BAD_ARGUMENT( wxString::Format( _( "Argument '%s' is empty." ), aKey ) );

    if( it->size() > aMaxItems )
    {
        throw BAD_ARGUMENT( wxString::Format(
                _( "Argument '%s' has %llu entries; at most %llu are allowed in one call." ),
                aKey, (unsigned long long) it->size(), (unsigned long long) aMaxItems ) );
    }

    for( const auto& entry : *it )
    {
        if( !entry.is_string() )
        {
            throw BAD_ARGUMENT(
                    wxString::Format( _( "Every entry in '%s' must be a string." ), aKey ) );
        }

        wxString value = wxString::FromUTF8( entry.get<std::string>() );
        value.Trim( true ).Trim( false );

        if( value.IsEmpty() )
        {
            throw BAD_ARGUMENT(
                    wxString::Format( _( "'%s' contains an empty entry." ), aKey ) );
        }

        RejectDangerousText( value, aKey );

        out.push_back( value );
    }

    return out;
}


/**
 * One of a fixed set of words, matched case-insensitively.
 *
 * Returns the index into aChoices so the caller can switch on it.  The message
 * lists the accepted values, which is what lets a model recover on the next
 * round instead of guessing again.
 */
inline int RequireChoice( const nlohmann::json& aArgs, const char* aKey,
                          const std::vector<wxString>& aChoices )
{
    const wxString value = RequireString( aArgs, aKey );

    for( size_t i = 0; i < aChoices.size(); ++i )
    {
        if( value.CmpNoCase( aChoices[i] ) == 0 )
            return static_cast<int>( i );
    }

    wxString list;

    for( size_t i = 0; i < aChoices.size(); ++i )
        list << ( i ? wxS( ", " ) : wxS( "" ) ) << aChoices[i];

    throw BAD_ARGUMENT( wxString::Format( _( "Argument '%s' must be one of: %s." ), aKey, list ) );
}


/// An optional choice, returning aDefault when absent.
inline int OptionalChoice( const nlohmann::json& aArgs, const char* aKey,
                           const std::vector<wxString>& aChoices, int aDefault )
{
    if( !Has( aArgs, aKey ) )
        return aDefault;

    return RequireChoice( aArgs, aKey, aChoices );
}

}  // namespace INDUCTIVE_ARGS

#endif  // INDUCTIVE_TOOL_ARGS_H
