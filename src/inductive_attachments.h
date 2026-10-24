// Bounded KiCad attachment parsing. No file paths or model instructions are trusted.
#ifndef INDUCTIVE_ATTACHMENTS_H
#define INDUCTIVE_ATTACHMENTS_H
#include <nlohmann/json.hpp>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace INDUCTIVE_ATTACHMENTS
{
constexpr size_t MAX_FILE = 1024 * 1024;
struct NODE
{
    std::string atom;
    std::vector<NODE> children;
    bool list = false;
    std::string Head() const { return children.empty() ? "" : children[0].atom; }
};

class PARSER
{
    const std::string& text;
    size_t pos = 0, count = 0;
    void Space() { while( pos < text.size() && std::isspace( (unsigned char) text[pos] ) ) ++pos; }
    NODE Read( int depth )
    {
        if( depth > 64 || ++count > 100000 ) throw std::runtime_error( "KiCad file is too complex." );
        Space();
        if( pos == text.size() ) throw std::runtime_error( "Incomplete KiCad file." );
        NODE node;
        if( text[pos] == '(' )
        {
            node.list = true;
            ++pos; Space();
            while( pos < text.size() && text[pos] != ')' )
            { node.children.push_back( Read( depth + 1 ) ); Space(); }
            if( pos == text.size() ) throw std::runtime_error( "Unclosed KiCad expression." );
            ++pos;
            if( node.children.empty() || node.children[0].list )
                throw std::runtime_error( "Invalid KiCad expression." );
        }
        else if( text[pos] == '"' )
        {
            ++pos;
            bool closed = false;
            while( pos < text.size() )
            {
                char c = text[pos++];
                if( c == '"' ) { closed = true; break; }
                if( c == '\\' )
                {
                    if( pos == text.size() ) break;
                    c = text[pos++];
                }
                node.atom += c;
            }
            if( !closed ) throw std::runtime_error( "Unclosed KiCad string." );
        }
        else
        {
            while( pos < text.size() && !std::isspace( (unsigned char) text[pos] )
                   && text[pos] != '(' && text[pos] != ')' && text[pos] != '"' )
                node.atom += text[pos++];
            if( node.atom.empty() ) throw std::runtime_error( "Invalid KiCad token." );
        }
        return node;
    }
public:
    explicit PARSER( const std::string& t ) : text( t ) {}
    NODE Parse()
    {
        if( text.empty() || text.size() > MAX_FILE || text.find( '\0' ) != std::string::npos )
            throw std::runtime_error( "KiCad attachments must be UTF-8 text, at most 1 MiB each." );
        if( text.compare( 0, 3, "\xef\xbb\xbf" ) == 0 ) pos = 3;
        NODE n = Read( 0 ); Space();
        if( pos != text.size() || !n.list ) throw std::runtime_error( "Invalid KiCad document." );
        return n;
    }
};

inline std::string Extension( const std::string& name )
{
    const auto dot = name.rfind( '.' );
    std::string ext = dot == std::string::npos ? "" : name.substr( dot );
    for( char& c : ext ) c = std::tolower( (unsigned char) c );
    return ext;
}

inline nlohmann::json Inspect( const nlohmann::json& file )
{
    if( !file.is_object() || !file.contains( "name" ) || !file["name"].is_string()
        || !file.contains( "text" ) || !file["text"].is_string() )
        throw std::runtime_error( "Missing attachment name or text." );
    std::string name = file["name"], text = file["text"];
    if( name.empty() || name.size() > 200 || name.find_first_of( "/\\\r\n" ) != std::string::npos
        || name.find( '\0' ) != std::string::npos )
        throw std::runtime_error( "Invalid attachment filename." );
    auto ext = Extension( name );
    NODE root = PARSER( text ).Parse();
    if( ( ext != ".kicad_mod" && ext != ".kicad_sch" && ext != ".kicad_sym" )
        || ( ext == ".kicad_mod" && root.Head() != "footprint" && root.Head() != "module" )
        || ( ext == ".kicad_sch" && root.Head() != "kicad_sch" )
        || ( ext == ".kicad_sym" && root.Head() != "kicad_symbol_lib" ) )
        throw std::runtime_error( "File contents do not match a KiCad schematic, symbol library or footprint." );
    nlohmann::json result = { { "name", name }, { "kind", ext == ".kicad_mod" ? "footprint" : ext == ".kicad_sym" ? "symbol_library" : "schematic" },
                             { "bytes", text.size() }, { "pads", nlohmann::json::array() },
                             { "symbols", nlohmann::json::array() },
                             { "ports", nlohmann::json::array() },
                             { "child_sheets", 0 } };
    for( const auto& n : root.children )
    {
        if( !n.list ) continue;
        if( n.Head() == "sheet" ) result["child_sheets"] = result["child_sheets"].get<int>() + 1;
        if( n.Head() == "pad" && n.children.size() > 3 ) result["pads"].push_back( n.children[1].atom );
        if( n.Head() == "hierarchical_label" && n.children.size() > 1 ) result["ports"].push_back( n.children[1].atom );
        if( n.Head() == "symbol" )
        {
            nlohmann::json symbol = nlohmann::json::object();
            if( ext == ".kicad_sym" && n.children.size() > 1 )
            {
                symbol["name"] = n.children[1].atom;
                symbol["pins"] = nlohmann::json::array();
                for( const auto& unit : n.children )
                    if( unit.Head() == "symbol" )
                        for( const auto& pin : unit.children )
                            if( pin.Head() == "pin" )
                            {
                                nlohmann::json info = nlohmann::json::object();
                                if( pin.children.size() > 1 ) info["type"] = pin.children[1].atom;
                                for( const auto& field : pin.children )
                                    if( ( field.Head() == "number" || field.Head() == "name" ) && field.children.size() > 1 )
                                        info[field.Head()] = field.children[1].atom;
                                symbol["pins"].push_back( info );
                            }
            }
            for( const auto& field : n.children )
            {
                if( field.Head() == "lib_id" && field.children.size() > 1 ) symbol["lib_id"] = field.children[1].atom;
                if( field.Head() == "property" && field.children.size() > 2 )
                {
                    const auto& key = field.children[1].atom;
                    if( key == "Reference" || key == "Value" || key == "Footprint" ) symbol[key] = field.children[2].atom;
                }
            }
            result["symbols"].push_back( symbol );
        }
    }
    return result;
}

/// A part download as the page sends it, before the server has opened it.
inline bool IsPartDownload( const nlohmann::json& file )
{
    if( !file.is_object() || !file.contains( "name" ) || !file["name"].is_string() || !file.contains( "url" )
        || !file["url"].is_string() )
        return false;
    const auto ext = Extension( file["name"].get<std::string>() );
    return ( ext == ".zip" || ext == ".lib" ) && file["url"].get_ref<const std::string&>().rfind( "data:", 0 ) == 0;
}

/// A bundle's symbol library and footprints as ordinary attachments, for native checks.
inline std::vector<nlohmann::json> BundlePieces( const nlohmann::json& file )
{
    std::vector<nlohmann::json> pieces;
    const auto& b = file.at( "bundle" );
    if( b.contains( "symbol_library" ) && b["symbol_library"].is_string() )
        pieces.push_back( { { "name", "part.kicad_sym" }, { "text", b["symbol_library"] } } );
    for( const auto& fp : b.value( "footprints", nlohmann::json::array() ) )
        if( fp.is_object() && fp.contains( "file" ) && fp["file"].is_string() && fp.contains( "text" ) )
            pieces.push_back( { { "name", fp["file"].get<std::string>() + ".kicad_mod" }, { "text", fp["text"] } } );
    return pieces;
}

/// A vendor part download the server opened and reduced.
inline bool IsBundle( const nlohmann::json& file )
{
    return file.is_object() && file.contains( "bundle" ) && file["bundle"].is_object();
}

/// A file name the server chose for us: letters, digits, '.', '_' and '-' only.
inline bool SafeBundleFile( const nlohmann::json& v )
{
    if( !v.is_string() ) return false;
    const std::string s = v;
    return !s.empty() && s.size() <= 80 && s[0] != '.'
           && s.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" ) == std::string::npos;
}

constexpr size_t MAX_MODEL = 8 * 1024 * 1024;

/**
 * Re-check a server-made bundle with the same bounded parser as any attachment:
 * the server is trusted to have chosen the files, not to have produced valid
 * KiCad text. Returns a summary for the model.
 */
inline nlohmann::json InspectBundle( const nlohmann::json& file )
{
    if( !IsBundle( file ) || !file.contains( "name" ) || !file["name"].is_string() )
        throw std::runtime_error( "Invalid part download." );
    const std::string name = file["name"];
    if( name.empty() || name.size() > 200 || name.find_first_of( std::string( "/\\\r\n\0", 5 ) ) != std::string::npos )
        throw std::runtime_error( "Invalid attachment filename." );
    const auto& b = file["bundle"];
    nlohmann::json result = { { "name", name }, { "kind", "part_bundle" }, { "symbols", nlohmann::json::array() },
                              { "footprints", nlohmann::json::array() }, { "models", nlohmann::json::array() },
                              { "checks", nlohmann::json::array() }, { "skipped_files", 0 } };
    if( b.contains( "symbol_library" ) && b["symbol_library"].is_string() )
    {
        auto lib = Inspect( { { "name", "part.kicad_sym" }, { "text", b["symbol_library"] } } );
        if( lib["kind"] != "symbol_library" ) throw std::runtime_error( "Invalid symbol library in the part download." );
        const auto listed = b.value( "symbols", nlohmann::json::array() );
        for( auto symbol : lib["symbols"] )
        {
            for( const auto& entry : listed )
                if( entry.is_object() && entry.value( "name", "" ) == symbol.value( "name", "" ) && entry.contains( "footprint" ) )
                    symbol["footprint"] = entry["footprint"];
            result["symbols"].push_back( symbol );
        }
        if( result["symbols"].size() > 32 ) throw std::runtime_error( "The part download has too many symbols." );
    }
    std::set<std::string> files, models;
    for( const auto& m : b.value( "models", nlohmann::json::array() ) )
    {
        if( !m.is_object() || !SafeBundleFile( m.value( "file", nlohmann::json() ) ) || !m.contains( "data" ) || !m["data"].is_string()
            || m["data"].get_ref<const std::string&>().size() > MAX_MODEL / 3 * 4 + 8 )
            throw std::runtime_error( "Invalid 3D model in the part download." );
        const std::string f = m["file"];
        const auto ext = Extension( f );
        if( ( ext != ".step" && ext != ".stp" ) || !models.insert( f ).second )
            throw std::runtime_error( "Invalid 3D model in the part download." );
        result["models"].push_back( f );
    }
    for( const auto& fp : b.value( "footprints", nlohmann::json::array() ) )
    {
        if( !fp.is_object() || !SafeBundleFile( fp.value( "file", nlohmann::json() ) ) || !fp.contains( "text" ) )
            throw std::runtime_error( "Invalid footprint in the part download." );
        const std::string f = fp["file"];
        if( !files.insert( f ).second || files.size() > 32 ) throw std::runtime_error( "Invalid footprint in the part download." );
        auto info = Inspect( { { "name", f + ".kicad_mod" }, { "text", fp["text"] } } );
        const auto model = fp.value( "model", nlohmann::json() );
        if( !model.is_null() && ( !model.is_string() || !models.count( model.get<std::string>() ) ) )
            throw std::runtime_error( "A footprint in the part download names a missing 3D model." );
        result["footprints"].push_back( { { "file", f }, { "pads", info["pads"] }, { "model", model } } );
    }
    if( result["symbols"].empty() && result["footprints"].empty() )
        throw std::runtime_error( "The part download has no KiCad symbol or footprint." );
    for( const auto& c : b.value( "checks", nlohmann::json::array() ) )
        if( c.is_object() && c.contains( "message" ) && c["message"].is_string() && result["checks"].size() < 40 )
            result["checks"].push_back( { { "level", c.value( "level", "info" ) },
                                          { "message", c["message"].get<std::string>().substr( 0, 400 ) } } );
    result["skipped_files"] = b.value( "skipped", nlohmann::json::array() ).size();
    return result;
}
}
#endif
