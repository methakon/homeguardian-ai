-- HomeGuardian portal — MySQL provisioning for the deployed portal host.
--
-- Run this ONCE, as a MySQL administrator, on the same server the other
-- berhampore.in VM already uses (10.0.0.99:3306). The application credential
-- (MYSQL_USER / MYSQL_PASSWORD in the portal's 0600 db.env) has USAGE only and
-- cannot create databases, so this grant step is an owner/admin action.
--
-- Do not commit a real password. Pass the application password in from the
-- environment when you run it, for example:
--
--   mysql -h 10.0.0.99 -u admin -p < homeguardian_schema.sql
--
-- and let the application keep using the existing credential pattern:
-- MYSQL_HOST=10.0.0.99, MYSQL_PORT=3306, MYSQL_DATABASE=homeguardian.

CREATE DATABASE IF NOT EXISTS homeguardian
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- The portal connects from the private subnet (10.0.0.0/24), not from localhost.
-- Reuse the existing application account where possible; otherwise create a
-- dedicated least-privilege one and set its password out of band.
--
-- GRANT SELECT, INSERT, UPDATE ON homeguardian.* TO 'mylife'@'%';
--
-- A dedicated account is preferred (the existing account also owns myjob_agent):
--
-- CREATE USER 'homeguardian'@'10.0.0.%' IDENTIFIED BY '<set-out-of-band>';
-- GRANT SELECT, INSERT, UPDATE, DELETE ON homeguardian.* TO 'homeguardian'@'10.0.0.%';
-- FLUSH PRIVILEGES;

USE homeguardian;

-- Server-side sign-in and portal action audit. Required by the portal's
-- cookieless session model: the browser holds no durable identity, so the
-- server records what happened, when, and to which pseudonymous subject.
CREATE TABLE IF NOT EXISTS portal_audit (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  occurred_at   DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  request_id    CHAR(32)        NOT NULL,
  -- Google `sub` hashed with a server-held key: stable for correlation,
  -- not reversible without the key, never the raw address.
  subject_hash  CHAR(64)        NOT NULL,
  action        VARCHAR(48)     NOT NULL,
  result        VARCHAR(24)     NOT NULL,
  detail        VARCHAR(255)    NOT NULL DEFAULT '',
  remote_hash   CHAR(64)        NOT NULL DEFAULT '',
  PRIMARY KEY (id),
  KEY idx_audit_time (occurred_at),
  KEY idx_audit_subject (subject_hash, occurred_at),
  KEY idx_audit_result (result, occurred_at)
) ENGINE=InnoDB;

-- Mirror of alerts already committed by the native store. Read-only copy for
-- reporting; the native application remains the authority and the sole writer
-- of alert state. Rows are keyed by the native alert id so a re-sync is idempotent.
CREATE TABLE IF NOT EXISTS alert_mirror (
  alert_id      VARCHAR(96)  NOT NULL,
  observed_at   DATETIME(3)  NOT NULL,
  detector_id   VARCHAR(64)  NOT NULL,
  severity      VARCHAR(16)  NOT NULL,
  provenance    VARCHAR(24)  NOT NULL,
  summary       VARCHAR(255) NOT NULL DEFAULT '',
  mirrored_at   DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (alert_id),
  KEY idx_alert_time (observed_at)
) ENGINE=InnoDB;

-- Device visibility decisions. Empty by default: a public account sees no
-- private device until an owner-approved per-household entry exists here.
CREATE TABLE IF NOT EXISTS device_visibility (
  device_id     VARCHAR(96)  NOT NULL,
  household_id  VARCHAR(96)  NOT NULL,
  approved      TINYINT(1)   NOT NULL DEFAULT 0,
  approved_by   VARCHAR(96)  NOT NULL DEFAULT '',
  approved_at   DATETIME(3)  NULL,
  PRIMARY KEY (device_id),
  KEY idx_visibility_household (household_id, approved)
) ENGINE=InnoDB;
