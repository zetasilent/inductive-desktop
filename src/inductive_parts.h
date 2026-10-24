// Local, immutable source assets with atomic metadata updates. No cloud dependency.
#ifndef INDUCTIVE_PARTS_H
#define INDUCTIVE_PARTS_H
#include "inductive_attachments.h"
#include "inductive_datasheets.h"
#include "inductive_review.h"
#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/file.h>
#include <kiid.h>
#include <filesystem>
#include <algorithm>
#include <set>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace INDUCTIVE_PARTS
{
using json = nlohmann::json;
namespace fs = std::filesystem;
inline bool ValidId( const std::string& id )
{
    return id.size() == 36 && id.find_first_not_of( "0123456789abcdef-" ) == std::string::npos;
}
inline void Write( const wxString& path, const std::string& text )
{
    wxTempFile out( path );
    if( !out.IsOpened() || !out.Write( wxString::FromUTF8( text ), wxConvUTF8 ) || !out.Commit() )
        throw std::runtime_error( "Could not save My Parts. Check the library folder is writable." );
}
inline std::string Read( const wxString& path, size_t limit = INDUCTIVE_ATTACHMENTS::MAX_FILE )
{
    if( fs::is_symlink( path.ToStdString() ) ) throw std::runtime_error( "My Parts cannot read a linked asset." );
    wxFFile in( path, "rb" );
    wxString text;
    if( !in.IsOpened() || in.Length() < 0 || (size_t) in.Length() > limit || !in.ReadAll( &text, wxConvUTF8 ) )
        throw std::runtime_error( "My Parts entry is missing, damaged or too large." );
    return text.ToStdString();
}
// Serialise writers from the schematic and PCB windows/processes. Reads see atomic renames.
class LOCK
{
#ifdef __WXMSW__
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED region{};
#else
    int fd;
#endif
public:
    explicit LOCK( const wxString& root )
    {
#ifdef __WXMSW__
        handle = CreateFileW( (root + "/.write.lock").wc_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr );
        if( handle == INVALID_HANDLE_VALUE ) throw std::runtime_error( "Could not lock My Parts for writing." );
        if( !LockFileEx( handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &region ) )
        { CloseHandle( handle ); throw std::runtime_error( "My Parts is busy in another window." ); }
#else
        fd = ::open( (root + "/.write.lock").ToStdString().c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600 );
        if( fd < 0 ) throw std::runtime_error( "Could not lock My Parts for writing." );
        if( ::flock( fd, LOCK_EX ) != 0 ) { ::close( fd ); throw std::runtime_error( "My Parts is busy in another window." ); }
#endif
    }
    ~LOCK()
    {
#ifdef __WXMSW__
        UnlockFileEx( handle, 0, 1, 0, &region ); CloseHandle( handle );
#else
        ::flock( fd, LOCK_UN ); ::close( fd );
#endif
    }
    LOCK( const LOCK& ) = delete;
};
class STORE
{
    wxString root;
    wxString Dir( const std::string& id ) const
    {
        if( !ValidId( id ) ) throw std::runtime_error( "Invalid My Parts id." );
        wxString path = root + "/" + wxString::FromUTF8( id );
        if( fs::is_symlink( path.ToStdString() ) ) throw std::runtime_error( "My Parts cannot use a linked entry." );
        return path;
    }
public:
    explicit STORE( const wxString& r ) : root( r ) {}
    json Get( const std::string& id, bool source = false ) const
    {
        const auto dir = Dir( id );
        auto entry = json::parse( Read( dir + "/metadata.json", 4 * 1024 * 1024 ) );
        if( entry.value( "id", "" ) != id || entry.value( "version", 0 ) != 1 )
            throw std::runtime_error( "Unsupported My Parts entry." );
        if( source )
        {
            if( entry["kind"] == "datasheet" ) entry.update( INDUCTIVE_DATASHEETS::ReadPDF(dir + "/source.pdf",entry["name"]) );
            else entry["text"] = Read( dir + "/source.txt" );
        }
        return entry;
    }
    json List( const std::string& query = "" ) const
    {
        json list = json::array();
        if( !fs::exists( root.ToStdString() ) ) return list;
        std::string needle = query;
        std::transform( needle.begin(), needle.end(), needle.begin(), []( unsigned char c ){ return std::tolower( c ); } );
        for( const auto& dir : fs::directory_iterator( root.ToStdString() ) )
        {
            if( !dir.is_directory() || dir.is_symlink() ) continue;
            const std::string id = dir.path().filename().string();
            if( !ValidId( id ) ) continue;
            try
            {
                auto entry = Get( id );
                if( entry.value( "archived", false ) ) continue;
                std::string haystack = entry.dump();
                std::transform( haystack.begin(), haystack.end(), haystack.begin(), []( unsigned char c ){ return std::tolower( c ); } );
                entry.erase( "summary" );
                entry.erase( "specifications" );
                if( needle.empty() || haystack.find( needle ) != std::string::npos ) list.push_back( entry );
            }
            catch( const std::exception& ) { /* A damaged entry cannot hide the rest of the library. */ }
        }
        std::sort( list.begin(), list.end(), []( const json& a, const json& b ) {
            return a.value( "name", "" ) < b.value( "name", "" );
        } );
        return list;
    }
    json Add( const json& file )
    {
        const auto summary = INDUCTIVE_DATASHEETS::Summary( file );
        if( !wxFileName::Mkdir( root, 0700, wxPATH_MKDIR_FULL ) && !wxDirExists( root ) )
            throw std::runtime_error( "Could not create My Parts." );
        LOCK lock( root );
        const auto entries = List();
        if( entries.size() >= 1000 ) throw std::runtime_error( "My Parts supports up to 1000 active entries." );
        // Match content as well as kind; never rely on a hash collision to deduplicate source files.
        for( const auto& entry : entries )
        {
            try {
                const char* key = summary["kind"] == "datasheet" ? "url" : "text";
                if( entry["kind"] == summary["kind"] && Get( entry["id"],true )[key] == file[key] ) return Get( entry["id"] );
            }
            catch( const std::exception& ) { /* Retain a new valid source even if an older asset is damaged. */ }
        }
        std::string id = KIID().AsString().ToStdString();
        const auto stage = root + "/pending-" + wxString::FromUTF8( id );
        if( !wxMkdir( stage, 0700 ) ) throw std::runtime_error( "Could not stage My Parts entry." );
        json entry = { { "version", 1 }, { "revision", 1 }, { "id", id }, { "name", file["name"] },
                       { "kind", summary["kind"] }, { "summary", summary },
                       { "manufacturer", "" }, { "mpn", "" }, { "datasheet", "" },
                       { "description", "" }, { "datasheet_id", "" }, { "footprint_id", "" }, { "symbol_name", "" }, { "archived", false } };
        if(summary["kind"]=="datasheet") {entry["pdf_sha256"]=INDUCTIVE_REVIEW::Hash(INDUCTIVE_DATASHEETS::Data(file)); entry["specifications"]=json::array();}
        try
        {
            if( summary["kind"] == "datasheet" ) INDUCTIVE_DATASHEETS::WritePDF(stage + "/source.pdf",file);
            else Write( stage + "/source.txt", file["text"].get<std::string>() );
            Write( stage + "/metadata.json", entry.dump( 2 ) );
            fs::rename( stage.ToStdString(), Dir( id ).ToStdString() );
        }
        catch( ... ) { fs::remove_all( stage.ToStdString() ); throw; }
        return entry;
    }
    json Update( const std::string& id, const json& changes, int revision )
    {
        LOCK lock( root );
        auto entry = Get( id );
        if( entry.value( "archived", false ) ) throw std::runtime_error( "This part was removed. Refresh My Parts." );
        if( entry.value( "revision", 0 ) != revision )
            throw std::runtime_error( "This part changed in another window. Refresh My Parts and try again." );
        for( const char* key : { "manufacturer", "mpn", "datasheet", "description", "footprint_id", "symbol_name", "datasheet_id" } )
        {
            if( !changes.contains( key ) ) continue;
            if( !changes[key].is_string() || changes[key].get<std::string>().size() > 1000 )
                throw std::runtime_error( "Part metadata must be text, at most 1000 characters per field." );
            entry[key] = changes[key];
        }
        if( !entry["datasheet"].get<std::string>().empty()
            && entry["datasheet"].get<std::string>().rfind( "https://", 0 ) != 0 )
            throw std::runtime_error( "Use an https datasheet URL." );
        if( !entry.value("datasheet_id", "").empty() )
        {
            auto sheet = Get(entry["datasheet_id"]);
            if( entry["kind"] == "datasheet" || sheet["kind"] != "datasheet" || sheet.value("archived",false) )
                throw std::runtime_error("Link a part to an active PDF datasheet from My Parts.");
        }
        const json* selected = nullptr;
        if( !entry["symbol_name"].get<std::string>().empty() )
        {
            if( entry["kind"] != "symbol_library" ) throw std::runtime_error( "Only symbol libraries have a selected symbol." );
            for( const auto& symbol : entry["summary"]["symbols"] )
                if( symbol.value( "name", "" ) == entry["symbol_name"].get<std::string>() ) selected = &symbol;
            if( !selected ) throw std::runtime_error( "Choose a symbol from this library." );
        }
        if( !entry["footprint_id"].get<std::string>().empty() )
        {
            auto footprint = Get( entry["footprint_id"] );
            if( !selected || footprint["kind"] != "footprint" || footprint.value( "archived", false ) )
                throw std::runtime_error( "Choose a symbol and an active footprint from My Parts." );
            if( selected->at( "pins" ).empty() ) throw std::runtime_error( "No pin numbers are available for this symbol. Pair a symbol with explicit pins." );
            std::set<std::string> pads;
            for( const auto& pad : footprint["summary"]["pads"] ) pads.insert( pad.get<std::string>() );
            for( const auto& pin : selected->at( "pins" ) )
                if( !pads.count( pin.value( "number", "" ) ) )
                    throw std::runtime_error( "Footprint is missing symbol pin " + pin.value( "number", "" ) + ". Choose a matching footprint." );
        }
        entry["revision"] = revision + 1;
        Write( Dir( id ) + "/metadata.json", entry.dump( 2 ) );
        return entry;
    }
    wxString PDFPath( const std::string& id ) const
    {
        auto entry = Get(id,true);
        if( entry["kind"] != "datasheet" || entry.value("archived",false) ) throw std::runtime_error("Choose an active PDF datasheet.");
        INDUCTIVE_DATASHEETS::Inspect(entry);
        return Dir(id) + "/source.pdf";
    }
    json SaveSpecifications(const std::string& id,const json& facts,int revision)
    {
        LOCK lock(root); auto entry=Get(id,true);
        if(entry.value("archived",false) || entry["kind"]!="datasheet") throw std::runtime_error("Choose an active PDF datasheet.");
        if(entry.value("revision",0)!=revision) throw std::runtime_error("These specifications changed in another window. Refresh before saving.");
        const auto summary=INDUCTIVE_DATASHEETS::Inspect(entry);
        entry["specifications"]=INDUCTIVE_REVIEW::Facts(facts,summary.at("pages"),true);
        entry["pdf_sha256"]=INDUCTIVE_REVIEW::Hash(INDUCTIVE_DATASHEETS::Data(entry));
        entry.erase("url");entry["revision"]=revision+1;
        Write(Dir(id)+"/metadata.json",entry.dump(2));return entry;
    }
    void Archive( const std::string& id )
    {
        LOCK lock( root );
        auto entry = Get( id );
        entry["archived"] = true;
        entry["revision"] = entry.value( "revision", 0 ) + 1;
        Write( Dir( id ) + "/metadata.json", entry.dump( 2 ) );
    }
};
}
#endif
