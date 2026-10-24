/*
 * This program source code file is part of Inductive, built on KiCad.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef INDUCTIVE_REPORTS_H
#define INDUCTIVE_REPORTS_H

/**
 * Crash and error reports, sent to the backend (POST /v1/reports), so that a failure
 * somebody hit is known without their having to write in about it.
 *
 * A crash is what macOS's own crash report says about it: the exception, the signal,
 * and the crashing thread's frames by function and library name. An error is an
 * unexpected exception the sidebar caught: where, and its message. Neither carries the
 * design or a file path -- home directories become "~" before anything leaves, and the
 * crash report's paths, user id and process details are never read out of it.
 *
 * Sent only for a signed-in user, and off when settings.json has "send_reports": false
 * or INDUCTIVE_NO_REPORTS is set. Nothing here runs on the UI thread except building
 * an error report, and nothing here throws.
 */

#include <widgets/inductive_cloud_auth.h>
#include <widgets/inductive_version.h>

#include <nlohmann/json.hpp>

#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined( _WIN32 )
#include <fcntl.h>
#include <unistd.h>
#endif

namespace INDUCTIVE_REPORTS
{
namespace detail
{
/// Where the sidebar keeps its chats and settings (the chats folder's parent).
inline wxString DataDir()
{
#if defined( __WXMAC__ )
    return wxFileName::GetHomeDir() + wxS( "/Library/Application Support/Inductive" );
#elif defined( __WXMSW__ )
    return wxStandardPaths::Get().GetUserConfigDir() + wxS( "\\Inductive" );
#else
    wxString xdg;
    return wxGetEnv( wxS( "XDG_DATA_HOME" ), &xdg ) && !xdg.IsEmpty()
                   ? xdg + wxS( "/inductive" )
                   : wxFileName::GetHomeDir() + wxS( "/.local/share/inductive" );
#endif
}


inline nlohmann::json ReadJson( const wxString& aPath )
{
    std::ifstream in( aPath.fn_str() );

    if( !in )
        return nlohmann::json();

    return nlohmann::json::parse( std::istreambuf_iterator<char>( in ),
                                  std::istreambuf_iterator<char>(), nullptr, false );
}


inline std::string Str( const nlohmann::json& aObject, const char* aKey )
{
    if( !aObject.is_object() )
        return {};

    const auto it = aObject.find( aKey );
    return it != aObject.end() && it->is_string() ? it->get<std::string>() : std::string();
}


/// Home directories name the user; control characters have no place in a log line.
inline std::string Scrub( const std::string& aText )
{
    static const std::regex home( R"((/Users/|/home/)[^/\s'"]+)" );
    static const std::regex windows( R"([A-Za-z]:\\Users\\[^\\\s'"]+)", std::regex::icase );

    std::string out = std::regex_replace( aText, home, "~" );
    out = std::regex_replace( out, windows, "~" );

    for( char& c : out )
    {
        if( static_cast<unsigned char>( c ) < 0x20 && c != '\n' && c != '\t' )
            c = ' ';
    }

    return out;
}


/// Reporting is on, and there is someone signed in to send it as.
inline bool Enabled()
{
    if( wxGetEnv( wxS( "INDUCTIVE_NO_REPORTS" ), nullptr ) )
        return false;

    if( !INDUCTIVE_CLOUD_AUTH::HasSession() || !INDUCTIVE_CLOUD_AUTH::LoadConfig().configured() )
        return false;

    const nlohmann::json settings = ReadJson( DataDir() + wxS( "/settings.json" ) );

    if( settings.is_object() )
    {
        const auto it = settings.find( "send_reports" );

        if( it != settings.end() && it->is_boolean() && !it->get<bool>() )
            return false;
    }

    return true;
}


inline std::string Dump( const nlohmann::json& aReport )
{
    return aReport.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace );
}


/**
 * The report macOS wrote, as sent: header fields (app, version, OS, incident) and,
 * from the body, the exception and the crashing thread's frames -- each by symbol and
 * library name, or library and offset when there is no symbol. The body holds paths,
 * the user's id and the process's details too; none of it is read.
 */
inline nlohmann::json CrashReport( const nlohmann::json& aHeader, const nlohmann::json& aBody )
{
    nlohmann::json out = { { "kind", "crash" },
                           { "app", Str( aHeader, "app_name" ) },
                           { "app_version", Str( aHeader, "app_version" ) },
                           { "os", Str( aHeader, "os_version" ) },
                           { "incident", Str( aHeader, "incident_id" ) } };

    if( !aBody.is_object() )
        return out;

    // A report in a shape this does not know still says that the app crashed.
    try
    {
        std::string exception;

        if( const auto it = aBody.find( "exception" ); it != aBody.end() && it->is_object() )
        {
            for( const char* key : { "type", "signal", "subtype" } )
            {
                if( const std::string part = Str( *it, key ); !part.empty() )
                    exception += ( exception.empty() ? "" : " " ) + part;
            }
        }

        if( exception.empty() )
        {
            if( const auto it = aBody.find( "termination" ); it != aBody.end() )
                exception = Str( *it, "indicator" );
        }

        out["exception"] = Scrub( exception );

        std::vector<std::string> images;

        if( const auto it = aBody.find( "usedImages" ); it != aBody.end() && it->is_array() )
        {
            for( const nlohmann::json& image : *it )
            {
                const std::string name = Str( image, "name" );
                images.push_back( name.empty() ? "?" : name );
            }
        }

        const auto threads = aBody.find( "threads" );

        if( threads == aBody.end() || !threads->is_array() || threads->empty() )
            return out;

        size_t crashed = 0;

        if( const auto it = aBody.find( "faultingThread" ); it != aBody.end()
            && it->is_number_unsigned() && it->get<size_t>() < threads->size() )
        {
            crashed = it->get<size_t>();
        }
        else
        {
            for( size_t i = 0; i < threads->size(); ++i )
            {
                const auto& t = ( *threads )[i];

                if( t.is_object() && t.value( "triggered", false ) )
                    crashed = i;
            }
        }

        nlohmann::json frames = nlohmann::json::array();
        const auto&    thread = ( *threads )[crashed];

        if( const auto it = thread.find( "frames" ); thread.is_object() && it != thread.end()
            && it->is_array() )
        {
            for( const nlohmann::json& f : *it )
            {
                if( frames.size() >= 40 || !f.is_object() )
                    break;

                const size_t      index = f.value( "imageIndex", size_t( -1 ) );
                const std::string image = index < images.size() ? images[index] : "?";
                const std::string symbol = Str( f, "symbol" );
                std::string       line;

                if( !symbol.empty() )
                    line = symbol + " +" + std::to_string( f.value( "symbolLocation", 0LL ) );
                else
                {
                    char offset[32];
                    std::snprintf( offset, sizeof( offset ), "0x%llx",
                                   static_cast<unsigned long long>( f.value( "imageOffset", 0ULL ) ) );
                    line = std::string( "+" ) + offset;
                }

                frames.push_back( Scrub( line + " (" + image + ")" ) );
            }
        }

        out["frames"] = frames;
    }
    catch( ... )
    {
    }

    return out;
}
} // namespace detail


/**
 * Send what macOS recorded about Inductive's crashes in the last two weeks, each once,
 * at most five a launch. Call it once the stored session is read; later calls in the
 * same process do nothing. A report that could not be sent is tried again next launch.
 */
inline void SendCrashes()
{
#if defined( __WXMAC__ )
    static std::atomic<bool> started{ false };

    if( started.exchange( true ) )
        return;

    std::thread(
            []()
            {
                try
                {
                    if( !detail::Enabled() )
                        return;

                    const wxString dir = wxFileName::GetHomeDir()
                                         + wxS( "/Library/Logs/DiagnosticReports" );
                    const wxString marks = detail::DataDir() + wxS( "/reports" );

                    if( !wxDir::Exists( dir )
                        || ( !wxDir::Exists( marks )
                             && !wxFileName::Mkdir( marks, 0700, wxPATH_MKDIR_FULL ) ) )
                    {
                        return;
                    }

                    wxArrayString files;
                    wxDir::GetAllFiles( dir, &files, wxS( "Inductive*.ips" ), wxDIR_FILES );

                    const wxDateTime        oldest = wxDateTime::Now() - wxTimeSpan::Days( 14 );
                    static const std::regex incidentId( "^[A-Za-z0-9-]{8,64}$" );
                    int                     sent = 0;

                    for( const wxString& file : files )
                    {
                        if( sent >= 5 )
                            break;

                        if( wxFileName( file ).GetModificationTime() < oldest )
                            continue;

                        std::ifstream in( file.fn_str() );
                        std::string   first;

                        if( !in || !std::getline( in, first ) )
                            continue;

                        const nlohmann::json header = nlohmann::json::parse( first, nullptr, false );
                        const std::string    incident = detail::Str( header, "incident_id" );

                        if( detail::Str( header, "bundleID" ).rfind( "com.inductive", 0 ) != 0
                            || !std::regex_match( incident, incidentId ) )
                        {
                            continue;
                        }

                        // Claimed before it is sent, so that the other editor, starting at the
                        // same moment, does not send it too.
                        const wxString marker = marks + wxS( "/" ) + wxString::FromUTF8( incident );
                        const int      fd = open( marker.fn_str(), O_CREAT | O_EXCL | O_WRONLY, 0600 );

                        if( fd < 0 )
                            continue;

                        close( fd );

                        const nlohmann::json body = nlohmann::json::parse(
                                std::istreambuf_iterator<char>( in ),
                                std::istreambuf_iterator<char>(), nullptr, false );

                        const INDUCTIVE_CLOUD_AUTH::ApiResponse response =
                                INDUCTIVE_CLOUD_AUTH::ApiRequest(
                                        wxS( "POST" ), wxS( "/v1/reports" ),
                                        detail::Dump( detail::CrashReport( header, body ) ) );

                        // Refused as malformed is final; anything else is tried again.
                        if( response.status != 202 && response.status != 400 )
                        {
                            wxRemoveFile( marker );
                            break;
                        }

                        ++sent;
                    }
                }
                catch( ... )
                {
                }
            } )
            .detach();
#endif
}


/**
 * Report an unexpected exception that was caught (and shown to the user) at aWhere.
 * The same failure is reported once a process, and no more than ten failures in all.
 */
inline void Error( const std::string& aApp, const std::string& aWhere, const std::string& aMessage )
{
    try
    {
        std::string message = detail::Scrub( aMessage );

        // A JSON parse error quotes what it was reading, which may be the design.
        if( const size_t at = message.find( "last read:" ); at != std::string::npos )
            message.erase( at );

        message = message.substr( 0, 300 );

        static std::mutex            lock;
        static std::set<std::string> seen;

        {
            std::lock_guard<std::mutex> held( lock );

            if( seen.size() >= 10 || !seen.insert( aWhere + "\n" + message ).second )
                return;
        }

        const nlohmann::json report = { { "kind", "error" },
                                        { "app", aApp },
                                        { "app_version", INDUCTIVE_VERSION },
                                        { "os", std::string( wxGetOsDescription().utf8_str() ) },
                                        { "where", aWhere },
                                        { "message", message } };

        std::thread(
                [body = detail::Dump( report )]()
                {
                    try
                    {
                        if( detail::Enabled() )
                            INDUCTIVE_CLOUD_AUTH::ApiRequest( wxS( "POST" ), wxS( "/v1/reports" ), body );
                    }
                    catch( ... )
                    {
                    }
                } )
                .detach();
    }
    catch( ... )
    {
    }
}


/// The failures worth a report: not a tool's own complaint about its input or the
/// document (KI_PARAM_ERROR, std::runtime_error), but a bug -- a logic or JSON error,
/// an out-of-range index, an exhausted allocator, something not even an exception.
inline bool Unexpected( const std::exception* aError )
{
    return !aError || !dynamic_cast<const std::runtime_error*>( aError );
}
} // namespace INDUCTIVE_REPORTS

#endif
