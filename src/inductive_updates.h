/*
 * Inductive's own updates.
 *
 * At launch the app asks the server (GET /v1/app/update) whether a newer release
 * exists. If one does, a window that cannot be dismissed covers the app: Update now
 * or Quit. Update downloads the release, checks its SHA-256 against the server's
 * answer, its code signature against the running app's team, and its version
 * against the one offered; then the app quits, a small script swaps the bundles and
 * opens the new one. On Windows the release is the installer: checked against the
 * server's SHA-256, then run silently into the same folder once Inductive has quit.
 * A check that cannot reach the server lets the app start: the server still refuses
 * model requests from editors below MIN_CLIENT_VERSION.
 *
 * Only release builds update themselves: INDUCTIVE_UPDATE_CHANNEL is written into
 * the bundle by `INDUCTIVE_UPDATE_CHANNEL=stable tools/install-app.sh` (and into
 * share\kicad\inductive\cloud.env by the Windows package). A development build has
 * none, so it never replaces itself.
 */

#ifndef INDUCTIVE_UPDATES_H
#define INDUCTIVE_UPDATES_H

#include <widgets/inductive_cloud_auth.h>
#include <widgets/inductive_version.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/settings.h>
#include <wx/stattext.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <thread>

#ifdef __WXMAC__
#include <CommonCrypto/CommonDigest.h>
#else
#include <picosha2.h>
#include <wx/utils.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

namespace INDUCTIVE_UPDATES
{

struct RELEASE
{
    std::string version;
    std::string url;
    std::string sha256;
    std::string notes;
    long long   size = 0;
};

/// The channel this build follows, or empty for a build that never updates itself.
inline wxString Channel()
{
    wxString channel = INDUCTIVE_CLOUD_AUTH::LoadConfig().updateChannel.Lower();
    return channel == wxS( "off" ) ? wxString() : channel;
}

/// The outermost .app the running program is in: what an update replaces.
inline wxString OuterBundle()
{
    const wxString exe = wxStandardPaths::Get().GetExecutablePath();
    const int at = exe.Find( wxS( ".app/" ) );
    return at == wxNOT_FOUND ? wxString() : exe.Left( at + 4 );
}

/// The innermost .app: what the user opened, and what opens again after the update.
inline wxString LaunchedBundle()
{
    const wxString exe = wxStandardPaths::Get().GetExecutablePath();
    const int at = exe.rfind( wxS( ".app/" ) );
    return at == wxNOT_FOUND ? wxString() : exe.Left( at + 4 );
}

/// A path as one single-quoted shell word.
inline std::string Quote( const wxString& aText )
{
    std::string out = "'";
    for( char c : std::string( aText.utf8_str() ) )
        out += c == '\'' ? std::string( "'\\''" ) : std::string( 1, c );
    return out + "'";
}

/// Run a command (worker threads only); its exit status, with stdout and stderr in aOutput.
inline int Run( const std::string& aCommand, std::string* aOutput = nullptr )
{
#ifdef _WIN32
    FILE* pipe = _popen( ( aCommand + " 2>&1" ).c_str(), "r" );
#else
    FILE* pipe = popen( ( aCommand + " 2>&1" ).c_str(), "r" );
#endif
    if( !pipe )
        return -1;
    char        buffer[4096];
    std::string output;
    while( size_t n = fread( buffer, 1, sizeof( buffer ), pipe ) )
        output.append( buffer, n );
#ifdef _WIN32
    const int status = _pclose( pipe );
#else
    const int status = pclose( pipe );
#endif
    if( aOutput )
        *aOutput = output;
    return status;
}

/// The names of the folders directly inside aPath.
inline wxArrayString Folders( const wxString& aPath )
{
    wxArrayString names;
    if( !wxDirExists( aPath ) )
        return names;
    wxDir dir( aPath );
    wxString name;
    for( bool more = dir.IsOpened() && dir.GetFirst( &name, wxEmptyString, wxDIR_DIRS | wxDIR_HIDDEN ); more;
         more = dir.GetNext( &name ) )
        names.Add( name );
    return names;
}

/// Where downloads wait to be installed: nothing in it is the user's.
inline wxString UpdatesRoot()
{
#ifdef _WIN32
    wxString local;
    if( !wxGetEnv( wxS( "LOCALAPPDATA" ), &local ) || local.IsEmpty() )
        return wxString();
    return local + wxS( "\\Inductive\\Updates" );
#else
    return wxFileName::GetHomeDir() + "/Library/Caches/Inductive/Updates";
#endif
}

/// Delete a folder of ours and everything in it.
inline void RemoveFolder( const wxString& aPath )
{
#ifdef _WIN32
    if( wxDirExists( aPath ) )
        wxFileName::Rmdir( aPath, wxPATH_RMDIR_RECURSIVE );
#else
    Run( "/bin/rm -rf " + Quote( aPath ) );
#endif
}

/// SHA-256 of a file in lowercase hex, or empty if it cannot be read.
inline std::string Sha256( const wxString& aPath )
{
#ifdef __WXMAC__
    std::ifstream in( std::string( aPath.utf8_str() ), std::ios::binary );
    if( !in )
        return "";
    CC_SHA256_CTX ctx;
    CC_SHA256_Init( &ctx );
    std::vector<char> buffer( 1 << 20 );
    while( in.read( buffer.data(), buffer.size() ) || in.gcount() > 0 )
        CC_SHA256_Update( &ctx, buffer.data(), (CC_LONG) in.gcount() );
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final( digest, &ctx );
    static const char* hex = "0123456789abcdef";
    std::string out;
    for( unsigned char b : digest )
    {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
#else
    std::ifstream in( aPath.fn_str(), std::ios::binary );
    if( !in )
        return "";
    picosha2::hash256_one_by_one hasher;
    std::vector<char> buffer( 1 << 20 );
    while( in.read( buffer.data(), buffer.size() ) || in.gcount() > 0 )
        hasher.process( buffer.begin(), buffer.begin() + in.gcount() );
    hasher.finish();
    return picosha2::get_hash_hex_string( hasher );
#endif
}

/// The team that signed a bundle ("not set" for an ad-hoc signature), or empty if unsigned.
inline std::string TeamId( const wxString& aBundle )
{
    std::string output;
    if( Run( "/usr/bin/codesign -dv --verbose=2 " + Quote( aBundle ), &output ) != 0 )
        return "";
    const size_t at = output.find( "TeamIdentifier=" );
    if( at == std::string::npos )
        return "";
    const size_t start = at + strlen( "TeamIdentifier=" );
    return output.substr( start, output.find( '\n', start ) - start );
}

/// x.y.z as three numbers, or nothing for anything else.
inline std::optional<std::array<int, 3>> ParseVersion( const std::string& aText )
{
    std::array<int, 3> parts{};
    size_t at = 0;
    for( int i = 0; i < 3; ++i )
    {
        size_t end = at;
        while( end < aText.size() && isdigit( (unsigned char) aText[end] ) && end - at < 4 )
            ++end;
        if( end == at || ( i < 2 ? end >= aText.size() || aText[end] != '.' : end != aText.size() ) )
            return std::nullopt;
        parts[i] = std::stoi( aText.substr( at, end - at ) );
        at = end + 1;
    }
    return parts;
}

/// Ask the server. Nothing when this build is up to date, or the server cannot be reached.
inline std::optional<RELEASE> Check( const wxString& aChannel )
{
    const INDUCTIVE_CLOUD_AUTH::CONFIG config = INDUCTIVE_CLOUD_AUTH::LoadConfig();
    if( config.apiBase.IsEmpty() || aChannel.IsEmpty() )
        return std::nullopt;
#if defined( __aarch64__ ) || defined( __arm64__ ) || defined( _M_ARM64 )
    const char* arch = "arm64";
#else
    const char* arch = "x86_64";
#endif
#ifdef _WIN32
    const char* platform = "windows";
#else
    const char* platform = "macos";
#endif
    try
    {
        KICAD_CURL_EASY curl;
        curl.SetURL( std::string( INDUCTIVE_CLOUD_AUTH::ApiBaseNormalized( config.apiBase ).utf8_str() )
                     + "/v1/app/update?platform=" + platform + "&arch=" + arch + "&version=" + INDUCTIVE_VERSION
                     + "&channel=" + curl.Escape( std::string( aChannel.utf8_str() ) ) );
        curl.SetHeader( "X-Inductive-Version", INDUCTIVE_VERSION );
        curl.SetConnectTimeout( 4 );
        curl.SetTimeout( 8 );
        if( curl.Perform() != CURLE_OK || curl.GetResponseStatusCode() != 200 )
            return std::nullopt;
        const nlohmann::json answer = nlohmann::json::parse( curl.GetBuffer() );
        if( !answer.value( "update", false ) )
            return std::nullopt;
        RELEASE release;
        release.version = answer.at( "version" ).get<std::string>();
        release.url = answer.at( "url" ).get<std::string>();
        release.sha256 = answer.at( "sha256" ).get<std::string>();
        release.size = answer.at( "size" ).get<long long>();
        release.notes = answer.value( "notes", "" );
        // Never a downgrade, and the version names a cache folder: x.y.z only.
        const auto offered = ParseVersion( release.version ), running = ParseVersion( INDUCTIVE_VERSION );
        if( !offered || !running || *offered <= *running )
            return std::nullopt;
        // https, or a loopback server when the app itself talks to one (development).
        const bool loopback = config.apiBase.StartsWith( wxS( "http://127.0.0.1:" ) )
                              && release.url.rfind( "http://127.0.0.1:", 0 ) == 0;
        if( ( release.url.rfind( "https://", 0 ) != 0 && !loopback ) || release.size <= 0
            || release.sha256.find_first_not_of( "0123456789abcdef" ) != std::string::npos || release.sha256.size() != 64 )
            return std::nullopt;
        return release;
    }
    catch( const std::exception& )
    {
        return std::nullopt;
    }
}

/**
 * The window shown while an update is required. It cannot be closed except by
 * quitting, and ends with wxID_OK once the swap script is waiting for the app to quit.
 */
class DIALOG : public wxDialog
{
public:
    DIALOG( wxWindow* aParent, RELEASE aRelease ) :
            wxDialog( aParent, wxID_ANY, _( "Update Inductive" ), wxDefaultPosition, wxDefaultSize, wxCAPTION ),
            m_release( std::move( aRelease ) )
    {
        // Native controls, not a web page: this window is the only way forward, so it
        // must work even if the web view does not.
        auto* sizer = new wxBoxSizer( wxVERTICAL );
        m_title = new wxStaticText( this, wxID_ANY, wxEmptyString );
        m_title->SetFont( m_title->GetFont().Bold().Scaled( 1.25f ) );
        m_lead = new wxStaticText( this, wxID_ANY, wxEmptyString );
        m_notes = new wxStaticText( this, wxID_ANY, wxString::FromUTF8( m_release.notes ) );
        m_notes->SetForegroundColour( wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT ) );
        m_gauge = new wxGauge( this, wxID_ANY, 1000, wxDefaultPosition, wxSize( -1, 8 ) );
        m_amount = new wxStaticText( this, wxID_ANY, wxEmptyString );
        m_amount->SetForegroundColour( wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT ) );
        m_quit = new wxButton( this, wxID_CANCEL, _( "Quit" ) );
        m_update = new wxButton( this, wxID_OK, _( "Update Now" ) );
        m_update->SetDefault();
        auto* buttons = new wxBoxSizer( wxHORIZONTAL );
        buttons->AddStretchSpacer();
        buttons->Add( m_quit, 0, wxRIGHT, 8 );
        buttons->Add( m_update );
        sizer->Add( m_title, 0, wxLEFT | wxRIGHT | wxTOP, 20 );
        sizer->Add( m_lead, 0, wxLEFT | wxRIGHT | wxTOP, 20 );
        sizer->Add( m_notes, 0, wxLEFT | wxRIGHT | wxTOP, 20 );
        sizer->Add( m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 20 );
        sizer->Add( m_amount, 0, wxLEFT | wxRIGHT | wxTOP, 20 );
        sizer->AddSpacer( 16 );
        sizer->Add( buttons, 0, wxEXPAND | wxALL, 20 );
        SetSizer( sizer );
        SetMinClientSize( wxSize( 440, -1 ) );
        m_update->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { if( !m_busy ) Start(); } );
        m_quit->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { Quit(); } );
        // The red button and Escape quit: there is no way past this window but the update.
        Bind( wxEVT_CLOSE_WINDOW, [this]( wxCloseEvent& ) { Quit(); } );
        Post( "available" );
        CentreOnScreen();
    }

    ~DIALOG() override
    {
        m_cancel = true;
        if( m_worker.joinable() )
            m_worker.join();
    }

private:
    static wxString Megabytes( long long aBytes )
    {
        return wxString::Format( aBytes < 10485760 ? "%.1f MB" : "%.0f MB", aBytes / 1048576.0 );
    }

    void Post( const std::string& aState, const std::string& aMessage = "", long long aReceived = 0 )
    {
        const wxString version = wxString::FromUTF8( m_release.version );
        const bool working = aState == "downloading" || aState == "verifying" || aState == "installing";
        m_lead->SetForegroundColour( aState == "error" ? wxColour( 220, 70, 50 )
                                                       : wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT ) );
        if( aState == "available" )
        {
            m_title->SetLabel( _( "Update required" ) );
            m_lead->SetLabel( wxString::Format( _( "Inductive %s is ready (you have %s). Update to keep using "
                                                   "Inductive; it takes a minute and reopens by itself." ),
                                                version, INDUCTIVE_VERSION ) );
        }
        else if( aState == "downloading" )
        {
            m_title->SetLabel( wxString::Format( _( "Downloading Inductive %s" ), version ) );
            m_lead->SetLabel( _( "Keep this window open. Quit stops the download." ) );
        }
        else if( aState == "verifying" )
        {
            m_title->SetLabel( _( "Checking the update" ) );
#ifdef _WIN32
            m_lead->SetLabel( _( "Making sure the download is complete and is the one Inductive published." ) );
#else
            m_lead->SetLabel( _( "Making sure the download is complete and signed by Inductive." ) );
#endif
        }
        else if( aState == "installing" )
        {
            m_title->SetLabel( _( "Installing" ) );
            m_lead->SetLabel( _( "Inductive will close and reopen in a moment." ) );
        }
        else
        {
            m_title->SetLabel( _( "Update didn't finish" ) );
            m_lead->SetLabel( wxString::FromUTF8( aMessage ) );
        }
        m_notes->Show( aState == "available" && !m_release.notes.empty() );
        m_gauge->Show( working );
        m_amount->Show( aState == "downloading" );
        if( aState == "downloading" && m_release.size > 0 )
        {
            m_gauge->SetValue( (int) std::min<long long>( 1000, aReceived * 1000 / m_release.size ) );
            m_amount->SetLabel( Megabytes( aReceived ) + _( " of " ) + Megabytes( m_release.size ) );
        }
        else if( working )
        {
            m_gauge->Pulse();
        }
        m_update->Show( aState == "available" || aState == "error" );
        m_update->SetLabel( aState == "error" ? _( "Try Again" ) : _( "Update Now" ) );
        m_quit->Enable( aState != "installing" );
        m_lead->Wrap( 420 );
        m_notes->Wrap( 420 );
        Layout();
        Fit();
    }

    void Quit()
    {
        m_cancel = true;
        EndModal( wxID_CANCEL );
    }

    /// Download, check and stage the update on a worker; the UI hears back through CallAfter.
    void Start()
    {
#ifdef _WIN32
        // <install>\bin\<editor>.exe: the installer replaces <install>, and the editor reopens.
        const wxFileName exe( wxStandardPaths::Get().GetExecutablePath() );
        wxFileName       root( exe.GetPath(), wxEmptyString );
        root.RemoveLastDir();
        const wxString outer = root.GetPath();
        const wxString launched = exe.GetFullPath();
        if( UpdatesRoot().IsEmpty() || !wxFileName::FileExists( outer + wxS( "\\uninstall.exe" ) ) )
        {
            Post( "error", "Install Inductive with its installer, open it from the Start menu, and update again." );
            return;
        }
#else
        const wxString outer = OuterBundle();
        if( outer.IsEmpty() || outer.Contains( wxS( "/AppTranslocation/" ) ) || outer.StartsWith( wxS( "/Volumes/" ) ) )
        {
            Post( "error", "Move Inductive to your Applications folder, open it from there, and update again." );
            return;
        }
        const wxString launched = LaunchedBundle();
#endif
        if( m_worker.joinable() )
            m_worker.join();
        m_busy = true;
        m_cancel = false;
        Post( "downloading" );
        m_worker = std::thread( [this, outer, launched] { Work( outer, launched ); } );
    }

    void Fail( const std::string& aMessage )
    {
        CallAfter( [this, aMessage] { m_busy = false; Post( "error", aMessage ); } );
    }

    /// Download the release to aFile, reporting progress about five times a second.
    /// False when it did not finish (Fail has been called) or Quit was pressed.
    bool Download( const wxString& aFile )
    {
        {
            std::ofstream out( aFile.fn_str(), std::ios::binary );
            KICAD_CURL_EASY curl;
            curl.SetURL( m_release.url );
            curl.SetFollowRedirects( true );
            curl.SetConnectTimeout( 15 );
            curl.SetStallTimeout( 1024, 60 );
            curl.SetOutputStream( &out );
            curl.SetTransferCallback(
                    [this]( size_t, size_t aNow, size_t, size_t )
                    {
                        if( m_cancel )
                            return 1;
                        const long long now = (long long) aNow;
                        CallAfter( [this, now] { if( m_busy ) Post( "downloading", "", now ); } );
                        return 0;
                    },
                    200000 );
            const int code = curl.Perform();
            out.close();
            if( m_cancel )
                return false;
            if( code != CURLE_OK || curl.GetResponseStatusCode() != 200 )
            {
                Fail( "The download didn't finish. Check your connection and try again." );
                return false;
            }
        }
        return true;
    }

    void Work( const wxString& aOuter, const wxString& aLaunched )
    {
        const wxString dir = UpdatesRoot() + wxFileName::GetPathSeparator() + wxString::FromUTF8( m_release.version );
        // A previous attempt's files go first; nothing in this folder is the user's.
        RemoveFolder( dir );
        if( !wxFileName::Mkdir( dir, 0700, wxPATH_MKDIR_FULL ) )
            return Fail( "Couldn't make a folder for the download." );
#ifdef _WIN32
        WorkWindows( wxFileName( dir, wxEmptyString ), aOuter, aLaunched );
#else
        WorkMac( wxFileName( dir, wxEmptyString ), aOuter, aLaunched );
#endif
    }

#ifdef _WIN32
    void WorkWindows( const wxFileName& dir, const wxString& aRoot, const wxString& aLaunched )
    {
        // 1. Download the installer.
        const wxString setup = dir.GetPath() + "\\Inductive-Setup.exe";
        if( !Download( setup ) )
            return;

        // 2. It is the file the server named. Quit stays available until the script starts.
        CallAfter( [this] { if( m_busy ) Post( "verifying" ); } );
        if( (long long) wxFileName::GetSize( setup ).GetValue() != m_release.size || Sha256( setup ) != m_release.sha256 )
            return Fail( "The download was damaged. Try again." );
        if( m_cancel )
            return;

        // 3. A script waits for this app (and any other Inductive window from this install)
        //    to quit, runs the installer silently into the same folder -- asking Windows for
        //    an administrator only for an install for all users -- and opens the app again.
        const wxString script = dir.GetPath() + "\\install.ps1";
        std::ofstream( script.fn_str() ) << R"PS(# install.ps1: wait for Inductive to quit, install the update into the same folder, reopen.
param([int]$AppPid, [string]$Setup, [string]$Root, [string]$Reopen)
$until = (Get-Date).AddSeconds(60)
while ((Get-Process -Id $AppPid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 100 }
if (Get-Process -Id $AppPid -ErrorAction SilentlyContinue) { exit 1 }   # still running: offered again next launch
$bin = Join-Path $Root 'bin'
function Others { Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($bin, [StringComparison]::OrdinalIgnoreCase) } }
$until = (Get-Date).AddMinutes(10)
while ((Others) -and (Get-Date) -lt $until) { Start-Sleep -Seconds 1 }
if (Others) { exit 1 }
$probe = Join-Path $Root '.update-probe'
try { [IO.File]::WriteAllText($probe, ''); Remove-Item $probe; $all = $false } catch { $all = $true }
try {
  if ($all) { $p = Start-Process -FilePath $Setup -ArgumentList '/S', '/allusers', "/D=$Root" -Verb RunAs -Wait -PassThru }
  else { $p = Start-Process -FilePath $Setup -ArgumentList '/S', '/currentuser', "/D=$Root" -Wait -PassThru }
  if ($p.ExitCode -eq 0) { Remove-Item $Setup -ErrorAction SilentlyContinue }
} catch { }
Start-Process -FilePath $Reopen
)PS";
        const wxString pid = wxString::Format( wxS( "%lu" ), wxGetProcessId() );
        CallAfter(
                [this, script, pid, setup, aRoot, aLaunched]
                {
                    // wxExecute belongs to the main thread.
                    const wxString shell = wxGetOSDirectory() + wxS( "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" );
                    const wxChar*  argv[] = { shell.wc_str(), wxS( "-NoProfile" ), wxS( "-NonInteractive" ),
                                              wxS( "-ExecutionPolicy" ), wxS( "Bypass" ), wxS( "-WindowStyle" ),
                                              wxS( "Hidden" ), wxS( "-File" ), script.wc_str(), wxS( "-AppPid" ),
                                              pid.wc_str(), wxS( "-Setup" ), setup.wc_str(), wxS( "-Root" ),
                                              aRoot.wc_str(), wxS( "-Reopen" ), aLaunched.wc_str(), nullptr };
                    if( m_cancel )
                        return;
                    if( wxExecute( const_cast<wxChar**>( argv ), wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE ) <= 0 )
                    {
                        m_busy = false;
                        Post( "error", "The update couldn't be started. Try again." );
                        return;
                    }
                    Post( "installing" );
                    EndModal( wxID_OK );
                } );
    }
#else
    void WorkMac( const wxFileName& dir, const wxString& aOuter, const wxString& aLaunched )
    {
        const wxString zip = dir.GetPath() + "/update.zip";
        if( !Download( zip ) )
            return;

        // 2. It is the file the server named, the build it named, signed by the same team.
        //    Quit stays available, and once pressed nothing below installs anything.
        CallAfter( [this] { if( m_busy ) Post( "verifying" ); } );
        if( (long long) wxFileName::GetSize( zip ).GetValue() != m_release.size || Sha256( zip ) != m_release.sha256 )
            return Fail( "The download was damaged. Try again." );
        const wxString unpacked = dir.GetPath() + "/unpacked";
        wxFileName::Mkdir( unpacked, 0700, wxPATH_MKDIR_FULL );
        if( Run( "/usr/bin/ditto -x -k " + Quote( zip ) + " " + Quote( unpacked ) ) != 0 )
            return Fail( "The update couldn't be unpacked. Try again." );
        const wxString fresh = unpacked + "/Inductive.app";
        if( !wxFileName::DirExists( fresh ) )
            return Fail( "The update doesn't contain Inductive. Try again later." );
        if( Run( "/usr/bin/codesign --verify --deep --strict " + Quote( fresh ) ) != 0 )
            return Fail( "The update's signature doesn't check out, so it wasn't installed." );
        const std::string team = TeamId( aOuter );
        if( !team.empty() && team != "not set" && TeamId( fresh ) != team )
            return Fail( "The update isn't signed by Inductive, so it wasn't installed." );
        std::string shown;
        Run( "/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' " + Quote( fresh + "/Contents/Info.plist" ),
             &shown );
        while( !shown.empty() && isspace( (unsigned char) shown.back() ) )
            shown.pop_back();
        if( shown != m_release.version )
            return Fail( "The update is a different version than offered, so it wasn't installed." );
        if( m_cancel )
            return;

        // 3. A script waits for this app to quit, swaps the bundles -- asking for an
        //    administrator's password only if the app's folder needs one -- and opens
        //    the new app. The old one stays beside the download until the next update.
        const wxString swap = dir.GetPath() + "/swap.sh";
        const wxString install = dir.GetPath() + "/install.sh";
        const wxString previous = dir.GetPath() + "/previous.app";
        std::ofstream( std::string( swap.utf8_str() ) )
                << "#!/bin/sh\n"
                   "# swap.sh APP NEW PREVIOUS: the new bundle takes the app's place.\n"
                   "mv \"$1\" \"$3\" || exit 1\n"
                   "mv \"$2\" \"$1\" || { mv \"$3\" \"$1\"; exit 1; }\n";
        std::ofstream( std::string( install.utf8_str() ) )
                << "#!/bin/sh\n"
                   "# install.sh PID SWAP APP NEW PREVIOUS ZIP REOPEN\n"
                   "i=0; while kill -0 \"$1\" 2>/dev/null && [ $i -lt 600 ]; do sleep 0.1; i=$((i+1)); done\n"
                   "kill -0 \"$1\" 2>/dev/null && exit 1   # still running: never two copies; offered again next launch\n"
                   "if [ -w \"$(dirname \"$3\")\" ] && [ -w \"$3\" ]; then\n"
                   "  /bin/sh \"$2\" \"$3\" \"$4\" \"$5\" && rm -f \"$6\"\n"
                   "else\n"
                   "  /usr/bin/osascript - \"$2\" \"$3\" \"$4\" \"$5\" <<'APPLESCRIPT' && rm -f \"$6\"\n"
                   "on run argv\n"
                   "  do shell script \"/bin/sh \" & quoted form of item 1 of argv & \" \" & quoted form of item 2 of argv"
                   " & \" \" & quoted form of item 3 of argv & \" \" & quoted form of item 4 of argv"
                   " with prompt \"Inductive needs your password to install its update.\" with administrator privileges\n"
                   "end run\n"
                   "APPLESCRIPT\n"
                   "fi\n"
                   "/usr/bin/open \"$7\"\n";
        const std::string command = "/bin/sh " + Quote( install ) + " " + std::to_string( getpid() ) + " "
                                    + Quote( swap ) + " " + Quote( aOuter ) + " " + Quote( fresh ) + " "
                                    + Quote( previous ) + " " + Quote( zip ) + " " + Quote( aLaunched )
                                    + " >/dev/null 2>&1 &";
        if( system( command.c_str() ) != 0 )
            return Fail( "The update couldn't be started. Try again." );
        CallAfter( [this] { Post( "installing" ); EndModal( wxID_OK ); } );
    }
#endif

    RELEASE           m_release;
    wxStaticText*     m_title = nullptr;
    wxStaticText*     m_lead = nullptr;
    wxStaticText*     m_notes = nullptr;
    wxGauge*          m_gauge = nullptr;
    wxStaticText*     m_amount = nullptr;
    wxButton*         m_quit = nullptr;
    wxButton*         m_update = nullptr;
    std::thread       m_worker;
    std::atomic<bool> m_cancel{ false };
    bool              m_busy = false;
};

/**
 * Check once per process, at launch. If an update is required, cover the app until
 * it is installed or the user quits; either way the app then exits.
 */
inline void CheckAtLaunch()
{
#if defined( __WXMAC__ ) || defined( _WIN32 )
    static std::atomic<bool> started{ false };
    if( started.exchange( true ) )
        return;
    const wxString channel = Channel();
    if( channel.IsEmpty() )
        return;
    std::thread(
            [channel]
            {
                // Downloads and replaced apps of earlier updates; the one that installed
                // this version (and the app it replaced) stays until the next update.
                // A newer version's folder may be a download in progress in another window.
                const wxString cache = UpdatesRoot();
                const auto running = ParseVersion( INDUCTIVE_VERSION );
                for( const wxString& name : Folders( cache ) )
                {
                    const auto version = ParseVersion( std::string( name.utf8_str() ) );
                    if( version && running && *version < *running )
                        RemoveFolder( cache + wxFileName::GetPathSeparator() + name );
                }
                std::optional<RELEASE> release = Check( channel );
                if( !release || !wxTheApp )
                    return;
                wxTheApp->CallAfter(
                        [release]
                        {
                            DIALOG dialog( wxTheApp->GetTopWindow(), *release );
                            dialog.ShowModal();
                            // Updated (the script opens the new app) or quit: this copy ends.
                            // Another window's modal loop (an error KiCad showed at startup)
                            // can hold the main loop open, so a moment later it ends anyway:
                            // the swap waits for this process, and nothing here is unsaved yet.
                            for( wxWindow* window : wxTopLevelWindows )
                                window->Hide();
                            std::thread( [] { std::this_thread::sleep_for( std::chrono::seconds( 5 ) ); _exit( 0 ); } ).detach();
                            wxTheApp->ExitMainLoop();
                        } );
            } )
            .detach();
#endif
}

} // namespace INDUCTIVE_UPDATES

#endif
