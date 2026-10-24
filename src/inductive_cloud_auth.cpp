/*
 * Browser sign-in (OAuth for native apps, with PKCE) and the session it leaves.
 */

#include <widgets/inductive_cloud_auth.h>
#include <widgets/inductive_version.h>

#include <wx/filename.h>
#include <wx/file.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <kicad_curl/kicad_curl_easy.h>

#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __WXMAC__
#include <Security/Security.h>
#endif

namespace INDUCTIVE_CLOUD_AUTH
{

/// Apply each KEY=value line of an env-style file. Missing files are normal.
static void readEnvFile( const wxString& aPath,
                         const std::function<void( const wxString&, const wxString& )>& aApply )
{
    // wxFile reports a failed open through wxLog, which surfaces as an error dialog
    // for a condition that is not an error. Suppressed for the probe.
    wxLogNull suppressMissingFile;
    wxFile    f;

    if( !wxFileName::FileExists( aPath ) || !f.Open( aPath, wxFile::read ) )
        return;

    wxString contents;
    f.ReadAll( &contents );
    f.Close();

    wxString rest = contents;

    while( !rest.IsEmpty() )
    {
        wxString line = rest.BeforeFirst( '\n' );
        rest = rest.AfterFirst( '\n' );
        line.Trim( true ).Trim( false );

        if( line.StartsWith( wxS( "export " ) ) )
            line = line.Mid( 7 );

        if( line.IsEmpty() || line.StartsWith( wxS( "#" ) ) )
            continue;

        const wxString key = line.BeforeFirst( '=' );
        wxString       val = line.AfterFirst( '=' );
        val.Trim( true ).Trim( false );

        if( ( val.StartsWith( wxS( "\"" ) ) && val.EndsWith( wxS( "\"" ) ) )
            || ( val.StartsWith( wxS( "'" ) ) && val.EndsWith( wxS( "'" ) ) ) )
        {
            val = val.Mid( 1, val.length() - 2 );
        }

        aApply( key, val );
    }
}


/// The cloud.env the installer writes into the bundle: the public defaults a
/// downloaded app needs to reach Inductive's server. Editors are nested apps, so
/// walk up from the executable as WebDir() does.
static wxString bundledEnvFile()
{
    auto dir = std::filesystem::path(
            wxStandardPaths::Get().GetExecutablePath().ToStdString() ).parent_path();

    for( int level = 0; level < 7 && !dir.empty(); ++level, dir = dir.parent_path() )
    {
        const auto candidate = dir / "SharedSupport" / "inductive" / "cloud.env";

        if( std::filesystem::exists( candidate ) )
            return wxString::FromUTF8( candidate.string() );
    }

    return wxEmptyString;
}


CONFIG LoadConfig()
{
    CONFIG c;

    // Each source fills only the keys still empty, in order of precedence:
    //   1. ~/.config/inductive/env -- the user's or developer's own settings
    //   2. the process environment (a stale launchctl value cannot beat the file)
    //   3. the defaults shipped inside the app bundle
    auto fill = [&c]( const wxString& key, const wxString& val )
    {
        wxString* slot = nullptr;

        if( key == wxS( "INDUCTIVE_API_BASE" ) )
            slot = &c.apiBase;
        else if( key == wxS( "INDUCTIVE_GCP_PROJECT_ID" ) )
            slot = &c.projectId;
        else if( key == wxS( "INDUCTIVE_FIREBASE_API_KEY" ) )
            slot = &c.firebaseApiKey;
        else if( key == wxS( "INDUCTIVE_FIREBASE_AUTH_DOMAIN" ) )
            slot = &c.firebaseAuthDomain;
        else if( key == wxS( "INDUCTIVE_UPDATE_CHANNEL" ) )
            slot = &c.updateChannel;

        if( slot && slot->IsEmpty() && !val.IsEmpty() )
            *slot = val;
    };

    const wxString home = wxFileName::GetHomeDir();
    readEnvFile( home + wxFileName::GetPathSeparator() + wxS( ".config" )
                         + wxFileName::GetPathSeparator() + wxS( "inductive" )
                         + wxFileName::GetPathSeparator() + wxS( "env" ),
                 fill );

    for( const wxString key : { wxS( "INDUCTIVE_API_BASE" ), wxS( "INDUCTIVE_GCP_PROJECT_ID" ),
                                wxS( "INDUCTIVE_FIREBASE_API_KEY" ),
                                wxS( "INDUCTIVE_FIREBASE_AUTH_DOMAIN" ),
                                wxS( "INDUCTIVE_UPDATE_CHANNEL" ) } )
    {
        wxString v;

        if( wxGetEnv( key, &v ) )
            fill( key, v.Trim().Trim( false ) );
    }

    const wxString bundled = bundledEnvFile();

    if( !bundled.IsEmpty() )
        readEnvFile( bundled, fill );

    // Every request to this base carries a bearer token, and sign-in sends the
    // refresh token to it. Plain http would put both on the wire, so anything but
    // https is treated as not configured -- except a loopback server in development.
    const wxString base = c.apiBase.Lower();

    if( !base.StartsWith( wxS( "https://" ) ) && !base.StartsWith( wxS( "http://127.0.0.1:" ) )
        && !base.StartsWith( wxS( "http://localhost:" ) ) )
    {
        c.apiBase.clear();
    }

    return c;
}

wxString ApiBaseNormalized( const wxString& aBase )
{
    wxString b = aBase;
    while( b.EndsWith( wxS( "/" ) ) )
        b.RemoveLast();
    return b;
}

// ── where the refresh token is kept ─────────────────────────────────────────────

/// Keychain service, and the account names under it.
static const char* const KEYCHAIN_SERVICE = "com.inductive.desktop";
/// "-v2": created with an access list naming all three Inductive apps. The first item
/// ("session-refresh-token") trusted only the build that created it, so the other
/// editors -- and every rebuilt copy -- could overwrite it but never read it back, and
/// each launch began signed out. It is left alone: deleting an item this app does not
/// own can make macOS prompt, and it is never read again.
static const char* const REFRESH_TOKEN_ITEM = "session-refresh-token-v2";
/// What sign-in stored before sessions: an hour-long ID token, removed on sight.
static const char* const LEGACY_ID_TOKEN_ITEM = "identity-platform-id-token";


#ifdef __WXMAC__

static SecKeychainItemRef findItem( const char* aItem, std::string* aSecret = nullptr )
{
    void*              data = nullptr;
    UInt32             len = 0;
    SecKeychainItemRef item = nullptr;

    const OSStatus st = SecKeychainFindGenericPassword(
            nullptr, (UInt32) strlen( KEYCHAIN_SERVICE ), KEYCHAIN_SERVICE,
            (UInt32) strlen( aItem ), aItem, aSecret ? &len : nullptr,
            aSecret ? &data : nullptr, &item );

    if( st != errSecSuccess )
        return nullptr;

    if( aSecret && data )
    {
        aSecret->assign( static_cast<const char*>( data ), len );
        SecKeychainItemFreeContent( nullptr, data );
    }

    return item;
}


static std::string storeRead( const char* aItem )
{
    std::string        secret;
    SecKeychainItemRef item = findItem( aItem, &secret );

    if( item )
        CFRelease( item );

    return secret;
}


static bool storeAdd( const char* aItem, const std::string& aSecret );
static void storeDelete( const char* aItem );


static bool storeWrite( const char* aItem, const std::string& aSecret )
{
    // Replaced in place where it exists: there is never a moment, between a delete
    // and an add, at which a crash would leave no session stored. But an app the item
    // does not trust can still overwrite it, and then cannot read it back: only a
    // read-back proves the write. Otherwise it is recreated with the editors' access.
    if( SecKeychainItemRef item = findItem( aItem ) )
    {
        const OSStatus st = SecKeychainItemModifyAttributesAndData(
                item, nullptr, (UInt32) aSecret.size(), aSecret.data() );
        CFRelease( item );

        if( st == errSecSuccess && storeRead( aItem ) == aSecret )
            return true;

        storeDelete( aItem );
    }

    return storeAdd( aItem, aSecret );
}


static void storeDelete( const char* aItem )
{
    if( SecKeychainItemRef item = findItem( aItem ) )
    {
        SecKeychainItemDelete( item );
        CFRelease( item );
    }
}


/**
 * Who may read the session: the project manager and both editors. Trusted by their
 * code signature, so a rebuilt or updated app signed by the same team still reads it.
 */
static SecAccessRef editorsAccess()
{
    // .../Inductive.app[/Contents/Applications/<editor>.app]/Contents/MacOS/<exe>
    std::filesystem::path root, at( wxStandardPaths::Get().GetExecutablePath().ToStdString() );

    for( ; !at.empty() && at != at.parent_path(); at = at.parent_path() )
    {
        if( at.extension() == ".app" )
            root = at;   // the outermost .app wins
    }

    CFMutableArrayRef        trusted = CFArrayCreateMutable( nullptr, 0, &kCFTypeArrayCallBacks );
    SecTrustedApplicationRef self = nullptr;

    if( SecTrustedApplicationCreateFromPath( nullptr, &self ) == errSecSuccess )
    {
        CFArrayAppendValue( trusted, self );
        CFRelease( self );
    }

    if( !root.empty() )
    {
        for( const std::filesystem::path& app : { root, root / "Contents/Applications/Inductive Schematic.app",
                                                  root / "Contents/Applications/Inductive PCB.app" } )
        {
            SecTrustedApplicationRef ref = nullptr;

            if( std::filesystem::exists( app )
                && SecTrustedApplicationCreateFromPath( app.c_str(), &ref ) == errSecSuccess )
            {
                CFArrayAppendValue( trusted, ref );
                CFRelease( ref );
            }
        }
    }

    SecAccessRef access = nullptr;
    SecAccessCreate( CFSTR( "Inductive sign-in" ), trusted, &access );
    CFRelease( trusted );
    return access;
}


/// A new item, created readable by every Inductive app. The access list is given at
/// creation: changing it afterwards can make macOS ask for the Keychain password.
static bool storeAdd( const char* aItem, const std::string& aSecret )
{
    SecKeychainAttribute attrs[] = {
        { kSecServiceItemAttr, (UInt32) strlen( KEYCHAIN_SERVICE ), (void*) KEYCHAIN_SERVICE },
        { kSecAccountItemAttr, (UInt32) strlen( aItem ), (void*) aItem },
        { kSecLabelItemAttr, (UInt32) strlen( KEYCHAIN_SERVICE ), (void*) KEYCHAIN_SERVICE },
    };
    SecKeychainAttributeList list = { 3, attrs };
    SecAccessRef             access = editorsAccess();
    SecKeychainItemRef       item = nullptr;

    const OSStatus st = SecKeychainItemCreateFromContent( kSecGenericPasswordItemClass, &list,
                                                          (UInt32) aSecret.size(), aSecret.data(),
                                                          nullptr, access, &item );
    if( access )
        CFRelease( access );

    if( item )
        CFRelease( item );

    return st == errSecSuccess;
}

#else

/// ~/.config/inductive/<item>, readable by this user only.
static std::string storePath( const char* aItem )
{
    const wxString dir = wxFileName::GetHomeDir() + wxFileName::GetPathSeparator()
                         + wxS( ".config" ) + wxFileName::GetPathSeparator()
                         + wxS( "inductive" );
    wxFileName::Mkdir( dir, 0700, wxPATH_MKDIR_FULL );
    return std::string( ( dir + wxFileName::GetPathSeparator() ).utf8_str() ) + aItem;
}


static std::string storeRead( const char* aItem )
{
    const int fd = ::open( storePath( aItem ).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC );

    if( fd < 0 )
        return {};

    std::string out;
    char        buf[512];
    ssize_t     n;

    while( out.size() < 4096 && ( n = ::read( fd, buf, sizeof( buf ) ) ) > 0 )
        out.append( buf, static_cast<size_t>( n ) );

    ::close( fd );

    while( !out.empty() && ( out.back() == '\n' || out.back() == '\r' || out.back() == ' ' ) )
        out.pop_back();

    return out;
}


static bool storeWrite( const char* aItem, const std::string& aSecret )
{
    // Written beside the target and renamed over it: created 0600 from the first
    // byte (no umask window), and never half-written.
    const std::string path = storePath( aItem );
    const std::string temp = path + ".tmp";

    ::unlink( temp.c_str() );
    const int fd = ::open( temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                           0600 );

    if( fd < 0 )
        return false;

    const bool written = ::write( fd, aSecret.data(), aSecret.size() )
                                 == static_cast<ssize_t>( aSecret.size() )
                         && ::fsync( fd ) == 0;
    ::close( fd );

    if( !written || ::rename( temp.c_str(), path.c_str() ) != 0 )
    {
        ::unlink( temp.c_str() );
        return false;
    }

    return true;
}


static void storeDelete( const char* aItem )
{
    ::unlink( storePath( aItem ).c_str() );
}

#endif


// ── secrets for one sign-in ──────────────────────────────────────────────────────

static std::string base64Url( const unsigned char* aData, size_t aLen )
{
    static const char* const alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve( ( aLen * 4 + 2 ) / 3 );

    for( size_t i = 0; i < aLen; i += 3 )
    {
        const unsigned v = ( unsigned( aData[i] ) << 16 )
                           | ( i + 1 < aLen ? unsigned( aData[i + 1] ) << 8 : 0u )
                           | ( i + 2 < aLen ? unsigned( aData[i + 2] ) : 0u );
        out.push_back( alphabet[( v >> 18 ) & 63] );
        out.push_back( alphabet[( v >> 12 ) & 63] );

        if( i + 1 < aLen )
            out.push_back( alphabet[( v >> 6 ) & 63] );

        if( i + 2 < aLen )
            out.push_back( alphabet[v & 63] );
    }

    return out;
}


/// 256 bits from the OS's CSPRNG, base64url; empty if it could not be had.
static std::string randomSecret()
{
    unsigned char bytes[32];

#ifdef __WXMAC__
    if( SecRandomCopyBytes( kSecRandomDefault, sizeof( bytes ), bytes ) != errSecSuccess )
        return {};
#else
    FILE* urandom = fopen( "/dev/urandom", "rb" );
    const bool ok = urandom && fread( bytes, 1, sizeof( bytes ), urandom ) == sizeof( bytes );

    if( urandom )
        fclose( urandom );

    if( !ok )
        return {};
#endif

    return base64Url( bytes, sizeof( bytes ) );
}


/// RFC 7636 S256: BASE64URL(SHA256(verifier)).
static std::string pkceChallenge( const std::string& aVerifier )
{
    std::vector<unsigned char> digest( picosha2::k_digest_size );
    picosha2::hash256( aVerifier.begin(), aVerifier.end(), digest.begin(), digest.end() );
    return base64Url( digest.data(), digest.size() );
}


// ── the session ──────────────────────────────────────────────────────────────────

namespace
{
struct SESSION
{
    /// Guards the fields below. Held only for a few instructions -- never across
    /// the network or the Keychain -- because the UI thread takes it.
    std::mutex                            lock;
    std::string                           refreshToken;
    std::string                           accessToken;
    std::chrono::steady_clock::time_point accessExpiry;
    /// Moves with every sign-in and sign-out, so that work which began under one
    /// session cannot write its result into the next.
    unsigned                              generation = 0;

    /// One refresh at a time: two would spend the same refresh token. Workers only.
    std::mutex refreshing;
    /// Every Keychain read and write, with the generation check that decides
    /// whether to make it. Workers only: the Keychain can stop to ask the user.
    std::mutex storing;
};


SESSION& session()
{
    static SESSION s;
    return s;
}


/**
 * The three apps share one session through the Keychain, and every refresh
 * replaces its refresh token: the server ends a session whose spent token comes
 * back, which is how a stolen one is caught. So an app that refreshed with the
 * token it read at launch, after another app had already replaced it, ended the
 * session for all three -- signed out after the first hour with two editors open.
 *
 * Refreshes and sign-ins in every app take this lock (an flock on a file in the
 * user's own temporary folder) and read the Keychain under it, so a token is
 * spent at most once. Workers only: it is held across the network.
 */
class EVERY_APP_LOCK
{
public:
    EVERY_APP_LOCK()
    {
#ifdef __WXMAC__
        char dir[PATH_MAX] = {};
        const size_t got = confstr( _CS_DARWIN_USER_TEMP_DIR, dir, sizeof( dir ) );

        if( got == 0 || got > sizeof( dir ) )
            return;   // no private folder: refresh unlocked, as before
#else
        const char dir[] = "";
        return;       // the Keychain is shared only on macOS
#endif

        const std::string path = std::string( dir ) + "com.inductive.desktop.session.lock";
        m_fd = open( path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600 );

        while( m_fd >= 0 && flock( m_fd, LOCK_EX ) != 0 && errno == EINTR )
        {
        }
    }

    ~EVERY_APP_LOCK()
    {
        if( m_fd >= 0 )
            close( m_fd );   // releases the lock
    }

    EVERY_APP_LOCK( const EVERY_APP_LOCK& ) = delete;
    EVERY_APP_LOCK& operator=( const EVERY_APP_LOCK& ) = delete;

private:
    int m_fd = -1;
};


/// The access token in hand, if it has more than a minute left. Call with `lock` held.
std::string usableAccessToken( const SESSION& s )
{
    if( s.accessToken.empty()
        || std::chrono::steady_clock::now() + std::chrono::seconds( 60 ) >= s.accessExpiry )
    {
        return {};
    }

    return s.accessToken;
}


struct TOKEN_RESULT
{
    enum { ISSUED, REFUSED, UNREACHABLE } outcome = UNREACHABLE;
    std::string accessToken;
    std::string refreshToken;
    long        expiresIn = 0;
};


/// POST a JSON body to the server, without credentials of our own.
bool postJson( const std::string& aPath, const nlohmann::json& aBody, int& aStatus,
               std::string& aResponse )
{
    const CONFIG cfg = LoadConfig();

    if( !cfg.configured() )
        return false;

    try
    {
        KICAD_CURL_EASY    curl;
        std::ostringstream stream;

        curl.SetURL( std::string( ApiBaseNormalized( cfg.apiBase ).utf8_str() ) + aPath );
        curl.SetHeader( "Content-Type", "application/json" );
        curl.SetHeader( "Accept", "application/json" );
        curl.SetHeader( "X-Inductive-Version", INDUCTIVE_VERSION );
        curl.SetTimeout( 30 );
        curl.SetFollowRedirects( false );
        curl.SetOutputStream( &stream );
        curl.SetPostFields( aBody.dump() );

        if( curl.Perform() != 0 )
            return false;

        aStatus = curl.GetResponseStatusCode();
        aResponse = stream.str();
        return true;
    }
    catch( ... )
    {
        return false;
    }
}


TOKEN_RESULT requestTokens( const nlohmann::json& aGrant )
{
    TOKEN_RESULT result;
    int          status = 0;
    std::string  response;

    if( !postJson( "/v1/auth/token", aGrant, status, response ) )
        return result;

    const nlohmann::json body = nlohmann::json::parse( response, nullptr, false );

    if( !body.is_object() )
        return result;

    if( status == 200 && body.contains( "access_token" ) && body["access_token"].is_string()
        && body.contains( "refresh_token" ) && body["refresh_token"].is_string() )
    {
        result.outcome = TOKEN_RESULT::ISSUED;
        result.accessToken = body["access_token"].get<std::string>();
        result.refreshToken = body["refresh_token"].get<std::string>();
        result.expiresIn = body.contains( "expires_in" ) && body["expires_in"].is_number()
                                   ? body["expires_in"].get<long>()
                                   : 0;
    }
    else if( status == 400 && body.value( "error", std::string() ) == "invalid_grant" )
    {
        result.outcome = TOKEN_RESULT::REFUSED;
    }

    return result;
}


/// End a session on the server. Best effort: it answers the same either way.
void revokeOnServer( const std::string& aRefreshToken )
{
    int         status = 0;
    std::string response;

    if( !aRefreshToken.empty() )
        postJson( "/v1/auth/revoke", nlohmann::json{ { "token", aRefreshToken } }, status, response );
}


/// Hold and store the session a sign-in just produced, ending any it replaces.
bool adoptSession( const TOKEN_RESULT& aTokens )
{
    SESSION&    s = session();
    std::string replaced;

    {
        EVERY_APP_LOCK              everyApp;
        std::lock_guard<std::mutex> storing( s.storing );

        if( !storeWrite( REFRESH_TOKEN_ITEM, aTokens.refreshToken ) )
            return false;

        {
            std::lock_guard<std::mutex> held( s.lock );
            replaced.swap( s.refreshToken );
            s.refreshToken = aTokens.refreshToken;
            s.accessToken = aTokens.accessToken;
            s.accessExpiry = std::chrono::steady_clock::now()
                             + std::chrono::seconds( aTokens.expiresIn );
            ++s.generation;
        }

        storeDelete( LEGACY_ID_TOKEN_ITEM );
    }

    // Signing in again while signed in: the old session belongs to nobody now.
    revokeOnServer( replaced );
    return true;
}
} // namespace


void LoadSession()
{
    SESSION&                    s = session();
    std::lock_guard<std::mutex> storing( s.storing );

    // Once anything has signed in or out in this process, memory is the truth and
    // the Keychain may be behind it: a sign-out's delete can still be queued.
    {
        std::lock_guard<std::mutex> held( s.lock );

        if( s.generation != 0 || !s.refreshToken.empty() )
            return;
    }

    const std::string stored = storeRead( REFRESH_TOKEN_ITEM );

    std::lock_guard<std::mutex> held( s.lock );

    // Unless that happened while the Keychain was answering.
    if( s.generation == 0 && s.refreshToken.empty() )
        s.refreshToken = stored;
}


bool HasSession()
{
    SESSION&                    s = session();
    std::lock_guard<std::mutex> held( s.lock );
    return !s.refreshToken.empty();
}


std::string AccessToken( bool* aSignedOut )
{
    SESSION& s = session();

    if( aSignedOut )
        *aSignedOut = false;

    {
        std::lock_guard<std::mutex> held( s.lock );

        if( std::string token = usableAccessToken( s ); !token.empty() )
            return token;
    }

    std::lock_guard<std::mutex> refreshing( s.refreshing );
    EVERY_APP_LOCK              everyApp;   // through the refresh and its Keychain write
    std::string                 refreshToken;
    unsigned                    generation;

    {
        std::lock_guard<std::mutex> held( s.lock );

        // Another thread may have refreshed while this one waited its turn.
        if( std::string token = usableAccessToken( s ); !token.empty() )
            return token;
    }

    // Another app may have refreshed since this one read the Keychain: the token
    // in memory is spent, and the one stored is the session's.
    std::string stored;

    {
        std::lock_guard<std::mutex> storing( s.storing );
        stored = storeRead( REFRESH_TOKEN_ITEM );
    }

    {
        std::lock_guard<std::mutex> held( s.lock );

        // Not after a sign-out here, whose Keychain delete may still be queued:
        // what is stored then is the session this app just ended.
        if( !stored.empty() && stored != s.refreshToken && ( !s.refreshToken.empty() || s.generation == 0 ) )
            s.refreshToken = stored;

        refreshToken = s.refreshToken;
        generation = s.generation;
    }

    if( refreshToken.empty() )
    {
        if( aSignedOut )
            *aSignedOut = true;

        return {};
    }

    const TOKEN_RESULT result = requestTokens( nlohmann::json{
            { "grant_type", "refresh_token" }, { "refresh_token", refreshToken } } );

    // Offline, or the server is having a bad minute: keep the session for next time.
    if( result.outcome == TOKEN_RESULT::UNREACHABLE )
        return {};

    bool current;

    {
        std::lock_guard<std::mutex> storing( s.storing );

        {
            std::lock_guard<std::mutex> held( s.lock );
            current = s.generation == generation;

            if( current && result.outcome == TOKEN_RESULT::ISSUED )
            {
                s.refreshToken = result.refreshToken;
                s.accessToken = result.accessToken;
                s.accessExpiry = std::chrono::steady_clock::now()
                                 + std::chrono::seconds( result.expiresIn );
            }
            else if( current )
            {
                // The server ended the session: revoked, expired, or replayed elsewhere.
                s.refreshToken.clear();
                s.accessToken.clear();
                ++s.generation;
            }
        }

        // The old refresh token is spent: the Keychain must not offer it after a
        // restart. Or there is no session any more, and it must offer nothing.
        if( current && result.outcome == TOKEN_RESULT::ISSUED )
            storeWrite( REFRESH_TOKEN_ITEM, result.refreshToken );
        else if( current )
            storeDelete( REFRESH_TOKEN_ITEM );
    }

    if( current && result.outcome == TOKEN_RESULT::ISSUED )
        return result.accessToken;

    // Signed out or in again while the refresh was in flight: the session it
    // renewed belongs to nobody now, so end it rather than leave it live.
    if( !current && result.outcome == TOKEN_RESULT::ISSUED )
        revokeOnServer( result.refreshToken );

    if( aSignedOut )
        *aSignedOut = !HasSession();

    return {};
}


void InvalidateAccessToken( const std::string& aToken )
{
    SESSION&                    s = session();
    std::lock_guard<std::mutex> held( s.lock );

    if( s.accessToken == aToken )
        s.accessToken.clear();
}


void SignOut()
{
    SESSION&    s = session();
    std::string refreshToken;
    unsigned    generation;

    {
        std::lock_guard<std::mutex> held( s.lock );
        refreshToken.swap( s.refreshToken );
        s.accessToken.clear();
        generation = ++s.generation;
    }

    std::thread(
            [refreshToken, generation]()
            {
                SESSION& s = session();

                {
                    // The Keychain first: it is quick, and a quit right after
                    // signing out must not leave the session there to be loaded.
                    std::lock_guard<std::mutex> storing( s.storing );
                    bool                        current;

                    {
                        std::lock_guard<std::mutex> held( s.lock );
                        current = s.generation == generation;
                    }

                    // Unless a new sign-in has already stored its own.
                    if( current )
                        storeDelete( REFRESH_TOKEN_ITEM );

                    storeDelete( LEGACY_ID_TOKEN_ITEM );
                }

                revokeOnServer( refreshToken );
            } )
            .detach();
}


static std::string urlEncode( const std::string& s )
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve( s.size() * 3 );

    for( unsigned char c : s )
    {
        if( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' )
            || c == '-' || c == '_' || c == '.' || c == '~' )
        {
            out.push_back( (char) c );
        }
        else
        {
            out.push_back( '%' );
            out.push_back( hex[c >> 4] );
            out.push_back( hex[c & 15] );
        }
    }

    return out;
}

static std::string extractQueryParam( const std::string& req, const char* key )
{
    const std::string needle = std::string( key ) + "=";
    // Only the request line — scanning the whole message matches `?` inside
    // Referer and treats "state=… HTTP/1.1\r\nHost:…" as the state value.
    const auto lineEnd = req.find( '\r' );
    const std::string line =
            req.substr( 0, lineEnd == std::string::npos ? req.size() : lineEnd );
    const auto q = line.find( '?' );

    if( q == std::string::npos )
        return {};

    const std::string query = line.substr( q + 1 );
    size_t            pos = 0;

    while( pos < query.size() )
    {
        const size_t amp = query.find( '&', pos );
        const std::string part =
                query.substr( pos, amp == std::string::npos ? std::string::npos : amp - pos );
        // Request line ends with " HTTP/1.x" — strip that from the last param.
        std::string field = part;
        const auto  sp = field.find( ' ' );

        if( sp != std::string::npos )
            field = field.substr( 0, sp );

        if( field.rfind( needle, 0 ) == 0 )
        {
            std::string val = field.substr( needle.size() );
            // Minimal percent-decode for JWT (base64url — usually unescaped)
            std::string decoded;
            for( size_t i = 0; i < val.size(); ++i )
            {
                if( val[i] == '%' && i + 2 < val.size() )
                {
                    auto hexVal = []( char c ) -> int
                    {
                        if( c >= '0' && c <= '9' )
                            return c - '0';
                        if( c >= 'a' && c <= 'f' )
                            return c - 'a' + 10;
                        if( c >= 'A' && c <= 'F' )
                            return c - 'A' + 10;
                        return -1;
                    };
                    const int hi = hexVal( val[i + 1] );
                    const int lo = hexVal( val[i + 2] );
                    if( hi >= 0 && lo >= 0 )
                    {
                        decoded.push_back( (char) ( ( hi << 4 ) | lo ) );
                        i += 2;
                        continue;
                    }
                }
                decoded.push_back( val[i] == '+' ? ' ' : val[i] );
            }
            return decoded;
        }
        if( amp == std::string::npos )
            break;
        pos = amp + 1;
    }

    return {};
}

/**
 * The page the browser shows after it hands the sign-in back to the app.
 *
 * Self-contained -- it is served from 127.0.0.1 by this process and may be the only
 * thing on screen when the browser comes back -- and in the same look as the
 * sign-in page on Cloud Run: the Inductive coil, lit when sign-in worked.
 */
static std::string CallbackPage( bool aSignedIn )
{
    static const char* const coil =
            "M 0 55 H 26.7 C 28.9 42.3 34.4 29.8 42.1 20.8 C 49.8 11.9 59.8 6.7 69.8 6.7 C 79.8 6.7 89.8 11.9 97.5 20.8 C 105.2 29.8 110.7 42.3 112.9 55 C 115.2 67.7 114.3 80.2 111 89.2 C 107.8 98.1 102.3 103.3 96.8 103.3 C 91.3 103.3 85.9 98.1 82.6 89.2 C 79.4 80.2 78.4 67.7 80.7 55 C 82.9 42.3 88.4 29.8 96.1 20.8 C 103.9 11.9 113.8 6.7 123.8 6.7 C 133.8 6.7 143.8 11.9 151.5 20.8 C 159.3 29.8 164.7 42.3 167 55 C 169.2 67.7 168.3 80.2 165.1 89.2 C 161.8 98.1 156.4 103.3 150.9 103.3 C 145.3 103.3 139.9 98.1 136.7 89.2 C 133.4 80.2 132.5 67.7 134.7 55 C 137 42.3 142.4 29.8 150.2 20.8 C 157.9 11.9 167.9 6.7 177.9 6.7 C 187.9 6.7 197.8 11.9 205.6 20.8 C 213.3 29.8 218.8 42.3 221 55 C 223.3 67.7 222.3 80.2 219.1 89.2 C 215.9 98.1 210.4 103.3 204.9 103.3 C 199.4 103.3 193.9 98.1 190.7 89.2 C 187.5 80.2 186.5 67.7 188.8 55 C 191 42.3 196.5 29.8 204.2 20.8 C 211.9 11.9 221.9 6.7 231.9 6.7 C 241.9 6.7 251.9 11.9 259.6 20.8 C 267.4 29.8 272.8 42.3 275 55 H 300";

    const std::string title = aSignedIn ? "You're signed in" : "Sign-in didn't finish";
    const std::string lede =
            aSignedIn ? "Return to Inductive. You can close this tab."
                      : "This sign-in link is out of date. Start again from the Inductive app: "
                        "choose Sign in in the account menu.";

    std::string html;
    html.reserve( 4096 );
    html += "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta name='color-scheme' content='light dark'><title>";
    html += title;
    html += " \xC2\xB7 Inductive</title><style>"
            ":root{--bg:#f7f7f4;--fg:#26251e;--muted:rgba(38,37,30,.6);--soft:rgba(38,37,30,.4);"
            "--cur:#ff4a1c}"
            "@media(prefers-color-scheme:dark){:root{--bg:#000;--fg:#f7f7f4;"
            "--muted:rgba(247,247,244,.62);--soft:rgba(247,247,244,.4)}}"
            "html,body{height:100%}"
            "body{margin:0;display:grid;place-items:center;background:var(--bg);color:var(--fg);"
            "font-family:system-ui,-apple-system,'Helvetica Neue',Helvetica,Arial,sans-serif;"
            "-webkit-font-smoothing:antialiased;text-align:center;padding:24px;box-sizing:border-box}"
            "main{max-width:360px;animation:rise .5s cubic-bezier(.2,.8,.2,1) both}"
            "svg{width:132px;height:auto;margin-bottom:40px;color:var(--fg);overflow:visible}"
            ".cur{stroke:var(--cur);stroke-dasharray:16 100;animation:trace 2.6s linear infinite}"
            ".lit .cur{stroke-dasharray:100 0;animation:fill .7s cubic-bezier(.4,0,.2,1) both}"
            "h1{margin:0;font-size:30px;font-weight:400;line-height:1.15;letter-spacing:-.03em}"
            "p{margin:12px auto 0;color:var(--muted);font-size:15px;line-height:1.5;max-width:32ch;"
            "text-wrap:balance}"
            "@keyframes trace{from{stroke-dashoffset:116}to{stroke-dashoffset:0}}"
            "@keyframes fill{from{stroke-dasharray:16 100}to{stroke-dasharray:100 0}}"
            "@keyframes rise{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}"
            "@media(prefers-reduced-motion:reduce){main,.cur{animation:none}}"
            "</style></head><body><main";
    html += aSignedIn ? " class='lit'>" : ">";
    html += "<svg viewBox='0 0 300 110' fill='none' role='img' aria-label='Inductive'>"
            "<path stroke='currentColor' stroke-width='7.33' stroke-linecap='round' d='";
    html += coil;
    html += "'/><path class='cur' pathLength='100' stroke-width='7.33' stroke-linecap='round' d='";
    html += coil;
    html += "'/></svg><h1>";
    html += title;
    html += "</h1><p>";
    html += lede;
    html += "</p></main></body></html>";
    return html;
}

void BeginBrowserSignIn( const CONFIG& aConfig,
                         std::function<void( const wxString& aError, bool aAbandoned )> aOnDone )
{
    std::thread( [aConfig, aOnDone]()
    {
        // The state ties the callback to this attempt; the verifier is what makes
        // the code worth anything, and it never leaves this process.
        const std::string state = randomSecret();
        const std::string verifier = randomSecret();

        if( state.empty() || verifier.empty() )
        {
            aOnDone( wxS( "could not generate a sign-in secret" ), false );
            return;
        }

        const int fd = ::socket( AF_INET, SOCK_STREAM, 0 );

        if( fd < 0 )
        {
            aOnDone( wxS( "could not open loopback socket" ), false );
            return;
        }

        int yes = 1;
        setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof( yes ) );

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        addr.sin_port = htons( 0 );

        if( bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) != 0
            || listen( fd, 16 ) != 0 )
        {
            ::close( fd );
            aOnDone( wxS( "could not bind loopback callback port" ), false );
            return;
        }

        sockaddr_in bound{};
        socklen_t   blen = sizeof( bound );
        getsockname( fd, reinterpret_cast<sockaddr*>( &bound ), &blen );
        const int port = ntohs( bound.sin_port );

        const std::string redirect =
                "http://127.0.0.1:" + std::to_string( port ) + "/callback";
        const std::string base = ApiBaseNormalized( aConfig.apiBase ).ToStdString();

        const std::string url =
                base + "/auth/login"
                + "?projectId=" + urlEncode( aConfig.projectId.ToStdString() )
                + "&apiKey=" + urlEncode( aConfig.firebaseApiKey.ToStdString() )
                + "&authDomain=" + urlEncode( aConfig.firebaseAuthDomain.ToStdString() )
                + "&state=" + urlEncode( state )
                + "&redirect_uri=" + urlEncode( redirect )
                + "&code_challenge=" + pkceChallenge( verifier )
                + "&code_challenge_method=S256";

        wxLaunchDefaultBrowser( wxString::FromUTF8( url ) );

        // Keep listening until we see our callback (matching state + code).
        // Browsers often open a first TCP connection for a probe/favicon that is
        // not the OAuth redirect; accepting that once and closing the socket
        // produced "OAuth state mismatch" after a real Google sign-in had already
        // succeeded in the browser.
        const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds( 180 );
        wxString    error = wxS( "sign-in timed out, was cancelled, or callback state mismatched" );
        bool        answered = false;   // the browser came back with our code

        while( std::chrono::steady_clock::now() < deadline )
        {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                                      deadline - std::chrono::steady_clock::now() )
                                      .count();
            struct timeval tv
            {
                left > 0 ? (long) left : 1L, 0
            };
            setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );

            const int client = accept( fd, nullptr, nullptr );

            if( client < 0 )
                break;

            std::string req;
            {
                char    chunk[4096];
                ssize_t n = 0;

                while( req.size() < 256 * 1024 )
                {
                    n = ::recv( client, chunk, sizeof( chunk ), 0 );

                    if( n <= 0 )
                        break;

                    req.append( chunk, static_cast<size_t>( n ) );

                    if( req.find( "\r\n\r\n" ) != std::string::npos )
                        break;
                }
            }

            // The redirect gets the page; anything else the browser asks this port for
            // (a favicon, a probe) gets a plain 404 rather than a second copy of it.
            const bool        isCallback = req.compare( 0, 14, "GET /callback?" ) == 0;
            const std::string gotCode = extractQueryParam( req, "code" );
            const bool        ours = isCallback && !gotCode.empty()
                              && extractQueryParam( req, "state" ) == state;
            bool              signedIn = false;

            // Redeemed before the page is sent, so the page tells the truth.
            if( ours )
            {
                answered = true;

                const TOKEN_RESULT tokens = requestTokens( nlohmann::json{
                        { "grant_type", "authorization_code" },
                        { "code", gotCode },
                        { "code_verifier", verifier },
                        { "redirect_uri", redirect } } );

                if( tokens.outcome == TOKEN_RESULT::ISSUED && adoptSession( tokens ) )
                {
                    signedIn = true;
                    error.clear();
                }
                else if( tokens.outcome == TOKEN_RESULT::ISSUED )
                {
                    revokeOnServer( tokens.refreshToken );
                    error = wxS( "could not store the sign-in in the Keychain" );
                }
                else if( tokens.outcome == TOKEN_RESULT::REFUSED )
                {
                    error = wxS( "the server refused this sign-in — try sign-in again" );
                }
                else
                {
                    error = wxS( "couldn't reach Inductive's server to finish sign-in" );
                }
            }

            const std::string body = isCallback ? CallbackPage( signedIn ) : std::string();
            const std::string response =
                    std::string( isCallback ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found" )
                    + "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store"
                      "\r\nContent-Length: "
                    + std::to_string( body.size() ) + "\r\nConnection: close\r\n\r\n" + body;
            ::send( client, response.data(), response.size(), 0 );
            ::close( client );

            // A code is single-use: whatever became of ours, this attempt is over.
            if( ours )
                break;
        }

        ::close( fd );
        aOnDone( error, !answered );
    } ).detach();
}


ApiResponse ApiRequest( const wxString& aMethod, const wxString& aRelativePath,
                        const std::string& aJsonBody, long aTimeoutSecs,
                        const std::function<bool()>& aKeepGoing )
{
    ApiResponse out;

    const CONFIG cfg = LoadConfig();

    if( !cfg.configured() )
    {
        out.error = wxS( "Cloud not configured" );
        return out;
    }

    wxString path = aRelativePath;

    if( path.IsEmpty() || path[0] != wxS( '/' ) )
        path = wxS( "/" ) + path;

    const wxString url = ApiBaseNormalized( cfg.apiBase ) + path;
    const std::string method = aMethod.Upper().ToStdString();

    for( int attempt = 0;; ++attempt )
    {
        out = ApiResponse();

        bool              signedOut = false;
        const std::string token = AccessToken( &signedOut );

        if( token.empty() )
        {
            out.signedOut = signedOut;
            out.error = signedOut ? wxS( "Not signed in" )
                                  : wxS( "Could not reach Inductive's server" );
            return out;
        }

        try
        {
            KICAD_CURL_EASY curl;
            curl.SetURL( std::string( url.utf8_str() ) );
            curl.SetHeader( "Authorization", "Bearer " + token );
            curl.SetHeader( "Accept", "application/json" );
            curl.SetHeader( "X-Inductive-Version", INDUCTIVE_VERSION );
            curl.SetTimeout( aTimeoutSecs );
            curl.SetFollowRedirects( false );

            if( aKeepGoing )
                curl.SetTransferCallback( [&]( size_t, size_t, size_t, size_t ) { return aKeepGoing() ? 0 : 1; },
                                          100000 );

            std::ostringstream stream;
            curl.SetOutputStream( &stream );

            if( method == "POST" )
            {
                curl.SetHeader( "Content-Type", "application/json" );
                curl.SetPostFields( aJsonBody.empty() ? std::string( "{}" ) : aJsonBody );
            }

            const int performed = curl.Perform();

            if( performed != 0 )
            {
                out.error = aKeepGoing && !aKeepGoing() ? wxString( wxS( "Cancelled" ) )
                                                        : wxString::Format( wxS( "Network error (%d)" ), performed );
                return out;
            }

            out.status = curl.GetResponseStatusCode();
            out.body = stream.str();
        }
        catch( const std::exception& e )
        {
            out.error = wxString::FromUTF8( e.what() );
        }
        catch( ... )
        {
            out.error = wxS( "Request failed" );
        }

        // Refused before the route ran (the server checks the token first), so a
        // second try with a fresh token cannot do anything twice.
        if( out.status == 401 && attempt == 0 )
        {
            InvalidateAccessToken( token );
            continue;
        }

        return out;
    }
}

} // namespace INDUCTIVE_CLOUD_AUTH
