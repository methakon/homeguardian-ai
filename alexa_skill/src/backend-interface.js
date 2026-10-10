'use strict';

/**
 * HomeGuardian AI — backend interface (Alexa side).
 *
 * This module defines the CONTRACT between the Alexa skill and the HomeGuardian
 * C++ core. The Alexa skill NEVER talks to the C++ process or any sensor
 * directly. It calls this interface, which is implemented here with mock data
 * for the simulator-first prototype.
 *
 * When a real backend is available (an authenticated local service — see
 * docs/ARCHITECTURE.md "Alexa+ integration findings"), a new implementation of
 * `HomeGuardianBackend` can fetch data over that authenticated interface
 * WITHOUT any change to the intent handlers below. The handlers depend only on
 * this interface, not on any backend or sensor.
 *
 * Mock safety rules:
 *  - No camera/microphone status is exposed (those sensors are not activated
 *    and must never be controllable from Alexa).
 *  - No personally identifiable family/sensor detail is returned. All values
 *    are aggregate, non-sensitive placeholders.
 *  - No network access, no disk access, no credentials.
 */

class HomeGuardianBackend {
  /**
   * @returns {Promise<{status: string, capture_enabled: boolean}>}
   */
  async getStatus() {
    // MOCK: the real service would return its lifecycle state. capture_enabled
    // is intentionally hard-coded false: the simulator must never imply that
    // any physical sensor is active.
    return {
      status: 'HomeGuardian AI is running',
      capture_enabled: false,
    };
  }

  /**
   * @returns {Promise<{count: number, highest_severity: string|null, alerts: Array<{id: string, severity: string, message: string}>}>}
   */
  async getAlertSummary() {
    // MOCK: no real alerts. The real backend would query its alert store and
    // return only aggregate, non-sensitive summaries.
    return { count: 0, highest_severity: null, alerts: [] };
  }

  /**
   * @returns {Promise<{count: number, routines: Array<{name: string, enabled: boolean}>}>}
   */
  async getRoutineStatus() {
    // MOCK: no configured routines in the prototype.
    return { count: 0, routines: [] };
  }
}

module.exports = { HomeGuardianBackend };
