-- HomeGuardian portal tables.
--
-- Decision (owner-directed): a dedicated `homeguardian` database could not be
-- created — the application account holds only
--
--   GRANT USAGE ON *.* TO `mylife`@`%`
--   GRANT ALL PRIVILEGES ON `myjob_agent`.* TO `mylife`@`%`
--
-- so `CREATE DATABASE homeguardian` fails with ERROR 1044. The portal therefore
-- keeps using the SAME database and server as the other berhampore.in VM and
-- namespaces every table with the `hf_` prefix. No existing table is touched.
--
-- Run as the application account (its grants already allow this):
--
--   set -a; . /opt/homeguardian/portal/db.env; set +a
--   MYSQL_PWD="$MYSQL_PASSWORD" mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
--     -u "$MYSQL_USER" "$MYSQL_DATABASE" < scripts/mysql/homeguardian_schema.sql
--
-- Connection values: MYSQL_HOST=10.0.0.99, MYSQL_PORT=3306,
-- MYSQL_DATABASE=myjob_agent, MYSQL_TABLE_PREFIX=hf_, in a 0600 db.env.

CREATE TABLE IF NOT EXISTS hf_portal_audit (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  occurred_at   DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  request_id    CHAR(32)        NOT NULL,
  -- Google `sub` hashed with a server-held key: stable for correlation,
  -- not reversible without the key, never a raw address.
  subject_hash  CHAR(64)        NOT NULL,
  action        VARCHAR(48)     NOT NULL,
  result        VARCHAR(24)     NOT NULL,
  detail        VARCHAR(255)    NOT NULL DEFAULT '',
  remote_hash   CHAR(64)        NOT NULL DEFAULT '',
  PRIMARY KEY (id),
  KEY idx_hf_audit_time (occurred_at),
  KEY idx_hf_audit_subject (subject_hash, occurred_at),
  KEY idx_hf_audit_result (result, occurred_at)
) ENGINE=InnoDB;

-- Mirror of alerts already committed by the native store. Read-only copy for
-- reporting; the C++ application remains the authority and the sole writer of
-- alert state. Keyed by native alert id so a re-sync is idempotent.
CREATE TABLE IF NOT EXISTS hf_alert_mirror (
  alert_id      VARCHAR(96)  NOT NULL,
  observed_at   DATETIME(3)  NOT NULL,
  detector_id   VARCHAR(64)  NOT NULL,
  severity      VARCHAR(16)  NOT NULL,
  provenance    VARCHAR(24)  NOT NULL,
  summary       VARCHAR(255) NOT NULL DEFAULT '',
  mirrored_at   DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (alert_id),
  KEY idx_hf_alert_time (observed_at)
) ENGINE=InnoDB;

-- Device visibility decisions. Empty by default: a public account sees no
-- private device until an owner-approved per-household row exists here.
CREATE TABLE IF NOT EXISTS hf_device_visibility (
  device_id     VARCHAR(96)  NOT NULL,
  household_id  VARCHAR(96)  NOT NULL,
  approved      TINYINT(1)   NOT NULL DEFAULT 0,
  approved_by   VARCHAR(96)  NOT NULL DEFAULT '',
  approved_at   DATETIME(3)  NULL,
  PRIMARY KEY (device_id),
  KEY idx_hf_vis_household (household_id, approved)
) ENGINE=InnoDB;
