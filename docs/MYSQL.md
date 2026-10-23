# MySQL for the published portal

The portal host uses the **same MySQL server and the same credential pattern** as the
other `berhampore.in` VM, so there is one place to look for application data and one
set of operational habits.

## How the working VM does it

The applications on the other host read four values from their own `.env`:

```
MYSQL_HOST=127.0.0.1      # there: an SSH local forward to the server
MYSQL_PORT=3307
MYSQL_USER=<app account>
MYSQL_PASSWORD=<set out of band>
```

The forward exists because that host has no route to the database subnet. Our portal
host **does** have one, so it connects the same way but without the tunnel:

```
MYSQL_HOST=10.0.0.99
MYSQL_PORT=3306
MYSQL_USER=<app account>
MYSQL_PASSWORD=<set out of band>
MYSQL_DATABASE=homeguardian
```

Those five lines live in `/opt/homeguardian/portal/db.env`, mode `0600`,
`root:www-data` — the same treatment as the portal's other secrets. They are never in
the repository, never in a systemd unit body, and never echoed by a script.

## Verified reachability

```
from the portal host: 10.0.0.99:3306 reachable
authentication:       SELECT CURRENT_USER() -> mylife@%
```

## Why the portal uses the shared database with an `hf_` prefix

The application account has `USAGE` only:

```
GRANT USAGE ON *.* TO `mylife`@`%`
GRANT ALL PRIVILEGES ON `myjob_agent`.* TO `mylife`@`%`
```

so it cannot create the portal's database:

```
CREATE DATABASE homeguardian
  -> ERROR 1044 (42000): Access denied for user 'mylife'@'%' to database 'homeguardian'
```

A dedicated database is therefore not available, so the portal uses the existing
database the account already owns and namespaces its tables with `hf_`:

```
MYSQL_DATABASE=myjob_agent
MYSQL_TABLE_PREFIX=hf_
```

`scripts/mysql/homeguardian_schema.sql` creates only `hf_portal_audit`,
`hf_alert_mirror` and `hf_device_visibility`. It creates no other object, alters no
existing table, and can be re-run safely. It runs as the application account:

```
set -a; . /opt/homeguardian/portal/db.env; set +a
MYSQL_PWD="$MYSQL_PASSWORD" mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
  -u "$MYSQL_USER" "$MYSQL_DATABASE" < scripts/mysql/homeguardian_schema.sql
```

If an administrator later grants a dedicated database, change the two values above and
re-run the script with the `hf_` prefix removed; nothing else depends on the name.

Verify from the portal host without printing the secret:

```
set -a; . /opt/homeguardian/portal/db.env; set +a
MYSQL_PWD="$MYSQL_PASSWORD" mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
  -u "$MYSQL_USER" -N -B -e "SHOW TABLES FROM $MYSQL_DATABASE;"
```

## What the tables are for

| table | purpose |
|---|---|
| `hf_portal_audit` | sign-in and portal-action audit; the browser holds no durable identity, so the server is the record |
| `hf_alert_mirror` | read-only copy of alerts the native store already committed; the C++ application stays the authority and sole writer |
| `hf_device_visibility` | owner-approved per-household visibility; empty by default, so a public account sees no private device |

The native core keeps SQLite as its store. MySQL is the portal-side record, not a
second source of truth for alerts: a mirror row may lag or be missing, and the portal
must report the native answer, not the mirror's.
