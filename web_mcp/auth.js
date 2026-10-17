// HomeGuardian AI — Lightweight & Secure Browser Session & Auth Manager
//
// Milestone HG-UI-03:
//   - Authenticated server-side session store with strict TTL
//   - Secure, HttpOnly, SameSite=Strict cookies
//   - Cryptographic CSRF token generation & validation
//   - Zero secret exposure in URLs (query strings strictly rejected)
//   - Short-lived, device-scoped viewing tokens for WebRTC

const crypto = require('node:crypto');

class SessionManager {
  constructor(options = {}) {
    this.sessionTtlMs = options.sessionTtlMs || 3600 * 1000; // 1 hour
    this.viewTokenTtlMs = options.viewTokenTtlMs || 60 * 1000; // 60 seconds
    this.sessions = new Map(); // token -> Session
    this.viewTokens = new Map(); // viewToken -> { deviceId, sessionId, expiresAt }
    
    // Operator credentials (fail-closed, overridden by HG_OPERATOR_PASSWORD)
    this.operatorUsername = process.env.HG_OPERATOR_USER || 'operator';
    if (process.env.HG_OPERATOR_PASSWORD) {
      this.operatorPassword = process.env.HG_OPERATOR_PASSWORD;
    } else {
      this.operatorPassword = crypto.randomBytes(16).toString('hex');
    }
    try {
      const os = require('node:os');
      const fs = require('node:fs');
      const path = require('node:path');
      const repoRoot = options.repoRoot || path.resolve(__dirname, '..');
      const configDir = path.join(repoRoot, 'config');
      const defaultSecretFile = fs.existsSync(configDir)
        ? path.join(configDir, 'operator.secret')
        : path.join(os.tmpdir(), 'hg_operator_password.secret');
      const secretFile = process.env.HG_OPERATOR_SECRET_FILE || defaultSecretFile;
      fs.writeFileSync(secretFile, this.operatorPassword, { mode: 0o600 });
      try { fs.chmodSync(secretFile, 0o600); } catch (_) {}
      console.log(`[Auth] Operator credential for '${this.operatorUsername}' stored securely in 0600 file: ${secretFile}`);
    } catch (_) {
      console.log(`[Auth] Operator credential initialized in memory.`);
    }
    this.sessionDestroyListeners = [];
  }

  // Parse HTTP cookies from request
  parseCookies(req) {
    const list = {};
    const cookieHeader = req.headers.cookie;
    if (!cookieHeader) return list;

    cookieHeader.split(';').forEach((cookie) => {
      let [name, ...rest] = cookie.split('=');
      name = name.trim();
      if (!name) return;
      const value = rest.join('=').trim();
      list[name] = decodeURIComponent(value);
    });
    return list;
  }

  // Authenticate operator credentials (constant-time, safe for any input length)
  verifyCredentials(username, password) {
    if (typeof username !== 'string' || typeof password !== 'string') return false;
    if (!username || !password) return false;
    const uHash = crypto.createHash('sha256').update(username).digest();
    const expUHash = crypto.createHash('sha256').update(this.operatorUsername).digest();
    const pHash = crypto.createHash('sha256').update(password).digest();
    const expPHash = crypto.createHash('sha256').update(this.operatorPassword).digest();
    const uMatch = crypto.timingSafeEqual(uHash, expUHash);
    const pMatch = crypto.timingSafeEqual(pHash, expPHash);
    return uMatch && pMatch;
  }

  // Create an authenticated session
  createSession(username = 'operator') {
    const sessionToken = crypto.randomBytes(32).toString('hex');
    const csrfToken = crypto.randomBytes(32).toString('hex');
    const now = Date.now();
    const expiresAt = now + this.sessionTtlMs;

    const session = {
      sessionToken,
      csrfToken,
      username,
      createdAt: now,
      expiresAt,
    };

    this.sessions.set(sessionToken, session);
    return session;
  }

  // Validate session from request
  getSessionFromRequest(req) {
    // 1. Strict invariant: reject any token passed in URL query string
    const url = new URL(req.url, 'http://127.0.0.1');
    if (url.searchParams.has('token') || url.searchParams.has('k') || url.searchParams.has('session')) {
      return null; // Prohibited: credentials in URL
    }

    // 2. Check cookies first
    const cookies = this.parseCookies(req);
    let token = cookies['hg_session'];

    // 3. Check Authorization header as fallback
    if (!token && req.headers.authorization) {
      const parts = req.headers.authorization.split(' ');
      if (parts[0] === 'Bearer' && parts[1]) {
        token = parts[1];
      }
    }

    if (!token) return null;

    const session = this.sessions.get(token);
    if (!session) return null;

    if (Date.now() > session.expiresAt) {
      this.sessions.delete(token);
      this.notifySessionDestroyed(token);
      return null;
    }

    return session;
  }

  // Retrieve session by raw token (for stream / background validation)
  getSessionByToken(token) {
    if (!token) return null;
    const session = this.sessions.get(token);
    if (!session) return null;
    if (Date.now() > session.expiresAt) {
      this.sessions.delete(token);
      this.notifySessionDestroyed(token);
      return null;
    }
    return session;
  }

  onSessionDestroyed(cb) {
    if (typeof cb === 'function') {
      this.sessionDestroyListeners.push(cb);
    }
  }

  notifySessionDestroyed(sessionToken) {
    for (const cb of this.sessionDestroyListeners) {
      try {
        cb(sessionToken);
      } catch (_) {}
    }
  }

  // Verify CSRF for state-mutating requests
  verifyCsrf(req, session) {
    if (req.method === 'GET' || req.method === 'HEAD' || req.method === 'OPTIONS') {
      return true;
    }
    const csrfHeader = req.headers['x-csrf-token'];
    return this.validateCsrfToken(session, csrfHeader);
  }

  // Validate CSRF token string directly against session
  validateCsrfToken(session, csrfToken) {
    if (!session || !session.csrfToken || !csrfToken) {
      return false;
    }
    try {
      return crypto.timingSafeEqual(
        Buffer.from(String(csrfToken)),
        Buffer.from(String(session.csrfToken))
      );
    } catch (_) {
      return false;
    }
  }

  // Destroy session
  destroySession(sessionToken) {
    if (sessionToken) {
      this.sessions.delete(sessionToken);
      this.notifySessionDestroyed(sessionToken);
    }
  }

  // Issue a short-lived, device-scoped viewing token for WebRTC
  issueScopedViewToken(session, deviceId) {
    if (!session || !deviceId) return null;
    const viewToken = 'vt_' + crypto.randomBytes(24).toString('hex');
    const expiresAt = Date.now() + this.viewTokenTtlMs;

    this.viewTokens.set(viewToken, {
      viewToken,
      deviceId,
      sessionToken: session.sessionToken,
      expiresAt,
    });

    return {
      viewToken,
      deviceId,
      expiresInSeconds: Math.floor(this.viewTokenTtlMs / 1000),
    };
  }

  // Validate a scoped view token (strictly bound to device and parent session)
  validateViewToken(viewToken, deviceId, expectedSessionToken = null) {
    if (!viewToken || !deviceId) return false;
    const entry = this.viewTokens.get(viewToken);
    if (!entry) return false;

    if (Date.now() > entry.expiresAt) {
      this.viewTokens.delete(viewToken);
      return false;
    }

    if (entry.deviceId !== deviceId) {
      return false;
    }

    if (expectedSessionToken && entry.sessionToken !== expectedSessionToken) {
      return false;
    }

    // Verify parent session is still active
    const session = this.sessions.get(entry.sessionToken);
    if (!session || Date.now() > session.expiresAt) {
      this.viewTokens.delete(viewToken);
      return false;
    }

    return true;
  }

  // Revoke view token
  revokeViewToken(viewToken) {
    if (viewToken) {
      this.viewTokens.delete(viewToken);
    }
  }
}

module.exports = { SessionManager };
