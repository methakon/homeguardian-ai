# Google sign-in deployment and acceptance

Task: HG-WEB-PORTAL-GOOGLE-LOGIN-51 (tracked in issue #25 comments).

## Published deployment

https://homeguardian.berhampore.in/ now serves the cookieless OIDC gateway on the
existing approved HTTPS tunnel. The previous dashboard is retained in
`src/web/index.html`, served through the gateway, and hidden behind authentication
and household authorization. No second dashboard or public MCP port was added.

The dedicated owner-created Web client belongs to project
`homeguardian-ai-web-portal`. Its registered origin is
`https://homeguardian.berhampore.in`; its registered callback is
`https://homeguardian.berhampore.in/auth/callback`. The existing Tantra client and
its redirects were not modified. Do not create a duplicate client.

Client ID and secret are installed only in
`/opt/homeguardian/gateway/gateway.env` (0600). The secret is used for server-side
code exchange, not browser code. No credential JSON or environment file is tracked.

Gateway environment contract:

- HG_GOOGLE_CLIENT_ID and HG_GOOGLE_CLIENT_SECRET: dedicated client credentials.
- HG_PUBLIC_ORIGIN=https://homeguardian.berhampore.in
- HG_GOOGLE_REDIRECT_URI=https://homeguardian.berhampore.in/auth/callback
- HG_GATEWAY_BIND=127.0.0.1; HG_GATEWAY_PORT=8810
- HG_MCP_URL: existing loopback native read-only MCP endpoint.
- HG_MCP_TOKEN: server-only backend credential.
- HG_PORTAL_PATH: deployed copy of the existing src/web/index.html.

The tunnel's HomeGuardian route now targets http://127.0.0.1:8810 instead of the
old bridge on 8091. Other tunnel routes, mail, WireGuard and Alexa are unchanged.

## Login and refresh behavior

1. Open the published page and click Sign in with Google. Allow its popup.
2. The original tab generates a memory-only browser verifier. A same-origin POST
   starts login and binds its challenge to server-held state, nonce and PKCE.
3. Google redirects the popup to the callback. The server exchanges the code and
   validates the ID-token signature, issuer, audience, azp, time claims and nonce.
4. A single-use handoff code returns via exact-origin popup messaging. If Google
   severs the popup opener, the original tab retrieves completion using a bounded,
   expiring server rendezvous and its memory-only verifier. Both paths share
   single-use consumption; only the initiating tab can obtain the session.
5. The browser holds its token only in memory; no authentication cookies,
   localStorage or sessionStorage. Reloading requires a new sign-in.
6. Logout revokes the server session and clears memory. Switch account revokes the
   current session before starting another Google account-selection flow.

After authentication the existing dashboard shell is visible, with a VIEWER banner
and logout/account-switch controls. Restricted controls are disabled and private
readings say "Household access not granted", rather than displaying fabricated zeros.
Every identity receives VIEWER. Google identity is not a household grant.
Production currently has no household authorizer: status and alert summaries
return 403 household_authorization_required without querying MCP, even after
Google login. Devices, camera/media, consent, administrator actions and mutations
are also denied. This intentionally prevents accidental household disclosure.
A separately approved household authorization model is still required for real
private-data access; there is no email-based or administrator bypass.

## Verified evidence

Focused offline suites (synthetic RSA/JWKS and token endpoint fixtures):

- node web_mcp/test_oauth.js: 199 assertions passed, 0 failed.
- node web_mcp/test_client.js: 26 passed, 0 failed.
- node web_mcp/test_devices_dashboard.js: 161 passed, 0 failed.

Coverage includes token validation and expiry, state/nonce/replay, browser-bound
handoff, default-deny household isolation, explicit authorized-subject test seam,
MCP unavailable responses, logout/expiry, and cookieless security headers.
The explicit household-authorizer seam is supplied only by tests, not the
production executable. Backend failures are unavailable, never fabricated zeros.

Public HTTPS probes after tunnel readback:

- /: 200, parser-blocking /assets/portal-session.js bootstrap present.
- /health: 200, oidc_configured=true, cookies=false.
- /api/status, /api/alerts, /api/devices, /api/camera/frame: anonymous 401.
- Browser: visible Sign in with Google page; an actual click opens Google's
  account sign-in prompt without redirect_uri_mismatch.

The deployed service subsequently recorded successful real Google token validation
and VIEWER session creation, but the owner reported a stuck completion page: the
original implementation depended solely on popup opener messaging. The replacement
has a regression harness executing the served scripts with opener=null against the
HTTP gateway; the original tab becomes authenticated and the dashboard is visible.
Real-account UI/logout acceptance of that replacement still requires a fresh owner
login. The task is not DONE; private-data acceptance is also blocked on the separate
household grant model. Do not close the tracking issue on fixture results.
