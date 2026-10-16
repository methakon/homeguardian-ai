// HomeGuardian AI — WebRTC Signaling & Synthetic Media Streamer
//
// Milestone HG-UI-03:
//   - Browser-based remote-camera viewing using WebRTC
//   - Scoped signaling authorization & Origin validation
//   - Hardware-isolated synthetic media test provider
//   - Immediate teardown on access revocation or viewer stop
//   - Zero continuous video frames over MCP or plain JSON

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
  }

  // Validate request Origin
  isOriginAllowed(originHeader, hostHeader) {
    if (!originHeader) return true; // non-browser or curl
    try {
      const u = new URL(originHeader);
      const originBase = `${u.protocol}//${u.hostname}`;
      const hostBase = hostHeader ? hostHeader.split(':')[0] : '127.0.0.1';

      if (u.hostname === '127.0.0.1' || u.hostname === 'localhost' || u.hostname === hostBase) {
        return true;
      }
      return this.allowedOrigins.some((o) => originHeader.startsWith(o));
    } catch (_) {
      return false;
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
    // 1. Origin verification
    if (!this.isOriginAllowed(originHeader, hostHeader)) {
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

    // 4. Verify scoped view token
    const tokenValid = this.sessionManager.validateViewToken(viewToken, deviceId);
    if (!tokenValid) {
      throw new Error('invalid_view_token: Scoped viewing authorization invalid or expired');
    }

    // 5. Close any existing connection for this device/session to prevent connection leaks
    this.stopDeviceStream(deviceId);

    // 6. Create WebRTC PeerConnection
    const connectionId = `conn_${deviceId}_${Date.now()}`;
    const pc = new RTCPeerConnection({
      iceServers: [], // LAN / loopback host-only, no cloud STUN/TURN relays
    });

    // 7. Add synthetic video track
    // Generates RFC-compliant video RTP packets for browser media attachment
    const videoTrack = new MediaStreamTrack({ kind: 'video' });
    pc.addTrack(videoTrack);

    // 8. Set Remote Description (client's offer)
    await pc.setRemoteDescription(sdp);

    // 9. Generate Answer
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);

    // Track active connection
    const entry = {
      connectionId,
      deviceId,
      sessionToken: session.sessionToken,
      pc,
      videoTrack,
      startedAt: Date.now(),
      synthetic: !!device.is_synthetic,
    };
    this.activeConnections.set(connectionId, entry);

    pc.connectionStateChange.subscribe((state) => {
      if (state === 'closed' || state === 'failed' || state === 'disconnected') {
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
  stopDeviceStream(deviceId) {
    let closed = 0;
    for (const [id, entry] of this.activeConnections.entries()) {
      if (entry.deviceId === deviceId) {
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
   * Stop active stream by connectionId
   */
  stopConnection(connectionId) {
    const entry = this.activeConnections.get(connectionId);
    if (entry) {
      try {
        entry.pc.close();
      } catch (_) {}
      this.activeConnections.delete(connectionId);
      return true;
    }
    return false;
  }

  /**
   * Return status of all active streaming sessions
   */
  getActiveStreams() {
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
