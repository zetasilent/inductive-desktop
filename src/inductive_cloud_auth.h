/*
 * Cloud auth for the Inductive panel: browser sign-in, and the session it leaves.
 *
 * Sign-in follows OAuth 2.0 for native apps (RFC 8252) with PKCE (RFC 7636): the
 * system browser signs in, hands this process a one-time code on 127.0.0.1, and the
 * code -- redeemable only with a verifier that never leaves this process -- buys a
 * session from Inductive's server. No credential ever travels in a URL or reaches
 * the webview.
 *
 * The session is two tokens. The access token goes with every request, lasts
 * minutes, and lives only in memory. The refresh token buys the next access token,
 * is replaced every time it is used, and is kept in the Keychain so a session
 * outlives a restart. The server ends the whole session if a replaced refresh token
 * is ever presented again, so a copied one is good for at most one use.
 */

#ifndef INDUCTIVE_CLOUD_AUTH_H
#define INDUCTIVE_CLOUD_AUTH_H

#include <kicommon.h>
#include <wx/string.h>

#include <functional>
#include <string>

namespace INDUCTIVE_CLOUD_AUTH
{
/**
 * Read cloud config from the environment.
 *
 *   INDUCTIVE_API_BASE                e.g. https://api.example.com
 *   INDUCTIVE_GCP_PROJECT_ID          Identity Platform / GCP project
 *   INDUCTIVE_FIREBASE_API_KEY        Web API key for the hosted login page
 *   INDUCTIVE_FIREBASE_AUTH_DOMAIN    Exact Firebase authDomain (never derived)
 *   INDUCTIVE_UPDATE_CHANNEL          Release channel this build updates from (stable);
 *                                     empty or "off" in development builds
 *
 * Prefer ~/.config/inductive/env (stable for Dock/Finder launches). Process env
 * fills keys the file omits.
 */
struct CONFIG
{
    wxString apiBase;
    wxString projectId;
    wxString firebaseApiKey;
    wxString firebaseAuthDomain;
    wxString updateChannel;
    bool     configured() const
    {
        return !apiBase.IsEmpty() && !projectId.IsEmpty() && !firebaseApiKey.IsEmpty()
               && !firebaseAuthDomain.IsEmpty();
    }
};

CONFIG LoadConfig();

wxString ApiBaseNormalized( const wxString& aBase );

/**
 * Read the stored session into memory. The Keychain may ask the user first, and
 * this blocks until they answer: call it once, at startup, from a worker thread.
 */
void LoadSession();

/// Whether a session is held. Never blocks; safe on the UI thread.
bool HasSession();

/**
 * An access token for the next request, refreshed first when there is none or it
 * is within a minute of expiring.
 *
 * Blocks on the network and the Keychain: worker threads only. Empty when there is
 * no session -- including one the server has just ended -- in which case
 * aSignedOut is set; or when the server could not be reached, in which case it is
 * not, and the session is kept for the next try.
 */
std::string AccessToken( bool* aSignedOut = nullptr );

/// The server refused aToken: drop it, so the next AccessToken() refreshes.
void InvalidateAccessToken( const std::string& aToken );

/**
 * Forget the session at once (HasSession() is false on return), then remove it
 * from the Keychain and end it on the server, in the background.
 */
void SignOut();

/**
 * Open the system browser to the Cloud Run /auth/login page, receive the code on
 * loopback, and redeem it. On success the session is stored and held before
 * aOnDone runs with an empty error.
 *
 * aAbandoned is set when the browser never came back -- the tab was closed, or
 * the user simply walked away -- which is not an error worth showing anyone.
 *
 * aOnDone runs on a worker thread; the panel marshals it to the UI.
 */
void BeginBrowserSignIn( const CONFIG& aConfig,
                         std::function<void( const wxString& aError, bool aAbandoned )> aOnDone );

/**
 * Authenticated Cloud Run request. Blocks: worker threads only.
 *
 * aMethod is "GET" or "POST". aRelativePath starts with '/', e.g. "/v1/me".
 * aJsonBody is sent for POST (may be empty "{}"). Never logs a token. aKeepGoing, when
 * given, is asked about ten times a second; false abandons the request ("Cancelled").
 */
struct ApiResponse
{
    int         status = 0;
    std::string body;
    wxString    error;
    bool        signedOut = false;  ///< No session, or the server ended it.
};

KICOMMON_API ApiResponse ApiRequest( const wxString& aMethod, const wxString& aRelativePath,
                        const std::string& aJsonBody = {}, long aTimeoutSecs = 45,
                        const std::function<bool()>& aKeepGoing = {} );

} // namespace INDUCTIVE_CLOUD_AUTH

#endif
