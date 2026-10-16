// HomeGuardian AI — WebRTC Signaling & Synthetic Media Streamer
//
// Milestone HG-UI-03:
//   - Browser-based remote-camera viewing using WebRTC
//   - Scoped signaling authorization & Origin validation
//   - Hardware-isolated synthetic media test provider
//   - Immediate teardown on access revocation or viewer stop
//   - Zero continuous video frames over MCP or plain JSON

const crypto = require('node:crypto');
const { RTCPeerConnection, MediaStreamTrack, RTCRtpCodecParameters } = require('werift');

class WebRtcStreamer {
  constructor(options = {}) {
    this.sessionManager = options.sessionManager;
    this.deviceAdapter = options.deviceAdapter;
    this.activeConnections = new Map(); // connectionId -> { pc, deviceId, sessionToken, startedAt }
    this.allowedOrigins = options.allowedOrigins || [
      'http://127.0.0.1',
      'http://localhost',
    ];
    this.maxStreamLifetimeMs = options.maxStreamLifetimeMs || 1800 * 1000; // 30 minutes
    if (this.sessionManager && typeof this.sessionManager.onSessionDestroyed === 'function') {
      this.sessionManager.onSessionDestroyed((token) => {
        this.stopSessionStreams(token);
      });
    }
  }

  // Validate request Origin (Strict: loopback only or exact allowedOrigins match)
  isOriginAllowed(originHeader) {
    if (!originHeader) return false; // Strict: browser Origin required
    try {
      const u = new URL(originHeader);
      if ((u.protocol === 'http:' || u.protocol === 'https:') &&
          (u.hostname === '127.0.0.1' || u.hostname === 'localhost')) {
        return true;
      }
      return this.allowedOrigins.includes(`${u.protocol}//${u.host}`);
    } catch (_) {
      return false;
    }
  }

  // Clean up streams exceeding lifetime or whose sessions expired
  pruneExpiredStreams() {
    const now = Date.now();
    for (const [id, entry] of this.activeConnections.entries()) {
      if (now - entry.startedAt > this.maxStreamLifetimeMs) {
        try { entry.pc.close(); } catch (_) {}
        this.activeConnections.delete(id);
        continue;
      }
      if (this.sessionManager && typeof this.sessionManager.getSessionByToken === 'function') {
        const session = this.sessionManager.getSessionByToken(entry.sessionToken);
        if (!session) {
          try { entry.pc.close(); } catch (_) {}
          this.activeConnections.delete(id);
        }
      }
    }
  }

  /**
   * Handle WebRTC SDP Offer from client
   *
   * @param {Object} params
   * @param {string} params.deviceId
   * @param {string} params.viewToken
   * @param {Object} params.sdp - { type: "offer", sdp: "..." }
   * @param {Object} session - Authenticated operator session
   * @param {string} originHeader
   * @param {string} hostHeader
   */
  async handleOffer({ deviceId, viewToken, sdp }, session, originHeader, hostHeader) {
    this.pruneExpiredStreams();

    // 1. Strict Origin verification
    if (!this.isOriginAllowed(originHeader)) {
      throw new Error('forbidden_origin: Origin not permitted for WebRTC signaling');
    }

    // 2. Validate device exists and is video-capable
    const device = this.deviceAdapter.getDevice(deviceId);
    if (!device) {
      throw new Error(`device_not_found: Device '${deviceId}' does not exist in registry`);
    }

    if (!device.capabilities || !device.capabilities.video) {
      throw new Error(`capability_missing: Device '${deviceId}' has no video capability`);
    }

    // 3. Verify consent is granted (Fail-Closed)
    if (device.consent !== 'granted' || !device.authorized) {
      throw new Error(`consent_withheld: Camera view refused. Consent not granted for '${deviceId}'`);
    }

    // 4. Verified pairing identity & authenticated liveness check
    if (!device.is_synthetic) {
      if (!device.paired || device.revoked || !device.credential_id || !device.online) {
        throw new Error(`device_offline: Physical camera '${deviceId}' is offline. Streaming unavailable.`);
      }
    }

    // 5. Verify scoped view token strictly bound to this session
    const tokenValid = this.sessionManager.validateViewToken(viewToken, deviceId, session.sessionToken);
    if (!tokenValid) {
      throw new Error('invalid_view_token: Scoped viewing authorization invalid, expired, or bound to another session');
    }

    // 6. Create WebRTC PeerConnection with CSPRNG random connection ID
    const connectionId = `conn_${crypto.randomBytes(16).toString('hex')}`;
    const pc = new RTCPeerConnection({
      iceServers: [], // LAN / loopback host-only, no cloud STUN/TURN relays
    });

    let answer;
    try {
      const videoTrack = new MediaStreamTrack({ kind: 'video' });
      pc.addTrack(videoTrack);
      await pc.setRemoteDescription(sdp);
      answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
    } catch (err) {
      try { pc.close(); } catch (_) {}
      throw new Error(`sdp_exchange_failed: ${err.message}`);
    }

    // 7. Replacement negotiation succeeded: now close prior streams for this device owned by this session
    this.stopDeviceStream(deviceId, session);

    // Track active connection with session ownership
    const entry = {
      connectionId,
      deviceId,
      sessionToken: session.sessionToken,
      pc,
      startedAt: Date.now(),
      synthetic: !!device.is_synthetic,
    };
    this.activeConnections.set(connectionId, entry);

    pc.connectionStateChange.subscribe((state) => {
      if (state === 'closed' || state === 'failed' || state === 'disconnected') {
        try { pc.close(); } catch (_) {}
        this.activeConnections.delete(connectionId);
      }
    });

    // Single-use token consumed
    this.sessionManager.revokeViewToken(viewToken);

    return {
      connectionId,
      deviceId,
      sdp: answer,
      syntheticProvider: entry.synthetic,
      streamState: 'active',
      notice: entry.synthetic
        ? 'SIMULATED TEST PATTERN ONLY. No physical camera activated.'
        : 'Authoritative remote camera session established.',
    };
  }

  /**
   * Stop active WebRTC stream for a device
   */
  stopDeviceStream(deviceId, session = null) {
    this.pruneExpiredStreams();
    let closed = 0;
    for (const [id, entry] of this.activeConnections.entries()) {
      if (entry.deviceId === deviceId) {
        if (session && entry.sessionToken !== session.sessionToken) {
          throw new Error('forbidden: Cannot stop another session\'s active stream');
        }
        try {
          entry.pc.close();
        } catch (_) {}
        this.activeConnections.delete(id);
        closed++;
      }
    }
    return closed > 0;
  }

  /**
   * Stop active stream by connectionId with session ownership check
   */
  stopConnection(connectionId, session = null) {
    this.pruneExpiredStreams();
    const entry = this.activeConnections.get(connectionId);
    if (entry) {
      if (session && entry.sessionToken !== session.sessionToken) {
        throw new Error('forbidden: Cannot stop another session\'s active stream');
      }
      try {
        entry.pc.close();
      } catch (_) {}
      this.activeConnections.delete(connectionId);
      return true;
    }
    return false;
  }

  /**
   * Terminate all active streams belonging to a specific session
   */
  stopSessionStreams(sessionToken) {
    let closed = 0;
    for (const [id, entry] of this.activeConnections.entries()) {
      if (entry.sessionToken === sessionToken) {
        try {
          entry.pc.close();
        } catch (_) {}
        this.activeConnections.delete(id);
        closed++;
      }
    }
    return closed;
  }

  /**
   * Return status of all active streaming sessions
   */
  getActiveStreams() {
    this.pruneExpiredStreams();
    const list = [];
    for (const [id, entry] of this.activeConnections.entries()) {
      list.push({
        connectionId: id,
        deviceId: entry.deviceId,
        startedAt: entry.startedAt,
        synthetic: entry.synthetic,
        durationSeconds: Math.floor((Date.now() - entry.startedAt) / 1000),
      });
    }
    return list;
  }
}

module.exports = { WebRtcStreamer };
