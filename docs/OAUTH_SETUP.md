# Google sign-in: the one owner step

The portal's OIDC gateway is implemented, deployed on the host and verified. The
remaining action is creating the OAuth client, which Google only exposes through the
Cloud Console — there is no public API and `gcloud` has no command for it.

Evidence that the API path does not exist (run against the project, with an owner role):

```
gcloud services enable clientauthconfig.googleapis.com   -> AUTH_PERMISSION_DENIED
gcloud alpha iap oauth-brands list                        -> SERVICE_DISABLED
clientauthconfig v1/brands                                -> 400 INVALID_ARGUMENT (all forms)
clientauthconfig v1/projects/{id}/clients                 -> 404
oauth2 v1/projects/{id}/clients                           -> 404
```

Do not create the client by any unsupported route, and do not reuse another
application's client: a Web client accepts only its exact registered redirect URIs, so
reuse fails with `redirect_uri_mismatch` and would tie this portal to another app.

## Create the dedicated client

Console: <https://console.cloud.google.com/apis/credentials?project=tantra-acharya-auth>

1. Confirm no client named `HomeGuardian AI Web Portal` exists; if it does, use it.
2. **Create credentials → OAuth client ID → Web application**.
3. Name: `HomeGuardian AI Web Portal`.
4. Authorized JavaScript origins: `https://homeguardian.berhampore.in`
5. Authorized redirect URIs: `https://homeguardian.berhampore.in/auth/callback`
6. Create, then copy the Client ID and Client secret.

The client secret is shown once. Put the values straight into the server's protected
environment; do not paste them into chat, a browser console, an issue or a commit.

## Install the credentials

```
sudo -n tee -a /opt/homeguardian/gateway/gateway.env >/dev/null <<'ENV'
HG_GOOGLE_CLIENT_ID=<value>
HG_GOOGLE_CLIENT_SECRET=<value>
HG_GOOGLE_REDIRECT_URI=https://homeguardian.berhampore.in/auth/callback
ENV
sudo chmod 600 /opt/homeguardian/gateway/gateway.env
sudo systemctl restart hg-gateway.service
```

`gateway.env` is `root:www-data` 0600. The client secret is used only for the
server-side token exchange; it never reaches the browser, is never logged, and is not
in the repository (`.env` is gitignored at `.gitignore:161`).

## Verify without a human

```
# the redirect is now accepted: expect a consent/302, not redirect_uri_mismatch
curl -sS -o /dev/null -D - -H 'Host: homeguardian.berhampore.in' \
  'http://127.0.0.1:8810/login' | grep -i '^location:'

# anonymous access stays closed
curl -sS -o /dev/null -w '%{http_code}\n' -H 'Host: homeguardian.berhampore.in' \
  'http://127.0.0.1:8810/api/status'    # 401
```

A real sign-in cannot be automated: it needs a person in a browser with a Google
account. That step also proves server-side validation, the VIEWER role and logout.

## After a successful sign-in

Repoint the Cloudflare tunnel from the bridge (8091) to the gateway (8810) and verify
through the public name:

```
/                      200   page
/login                 302   -> accounts.google.com (or 200 signed-in state)
/api/status            401   anonymous
/api/alerts            401   anonymous
/api/camera/status     403   camera_state   (authenticated VIEWER)
/api/devices           403   device_identities
```

Until the client exists the gateway reports the dependency honestly:

```
/login -> 503 {"error":"google_client_id_missing","dependency":["HG_GOOGLE_CLIENT_ID"]}
```
