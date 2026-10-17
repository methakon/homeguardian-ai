#!/usr/bin/env bash
# Pair this workstation with the Mi A3 for Wireless Debugging.
#
# THE PAIRING CODE IS ONE-TIME AND MUST NEVER BE LOGGED OR COMMITTED.
# Therefore this script never accepts the code as an argv (which would land in
# shell history and in Hermes' transcript) and never prints it. Instead the
# manager writes it to a private file which this script consumes and destroys:
#
#   install -m 600 /dev/null /run/user/$(id -u)/hg_pair
#   printf '%s %s\n' '<host:port from the phone>' '<6-digit code>' \
#       > /run/user/$(id -u)/hg_pair
#   ./edge_dev_pair.sh
#
# File format: one entry per line, "<HOST:PORT> <CODE>". The pairing port is the
# one shown under "Pair device with pairing code" on the phone's Wireless
# debugging screen (it is NOT the persistent connect port).
#
# Exit: 0 ok, 4 pairing refused/failed, 3 no endpoint.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

PAIR_FILE="${HG_PAIR_FILE:-/run/user/$(id -u)/hg_pair}"

[[ -f "$PAIR_FILE" ]] || cat >&2 <<EOF && exit 3
No pairing file at $PAIR_FILE.

On the phone: Settings > System > Developer options > Wireless debugging >
"Pair device with pairing code". It shows an IP:PORT and a 6-digit code. Then:

  umask 077; printf '%s %s\n' '<IP:PORT from the phone>' '<6-digit code>' > $PAIR_FILE
  chmod 600 $PAIR_FILE
  $0

The file is shredded by this script whether pairing succeeds or fails.
EOF

# Refuse a world-readable code file.
perm="$(stat -c '%a' "$PAIR_FILE")"
if [[ "$perm" != "600" && "$perm" != "400" ]]; then
  hg_die "$PAIR_FILE is mode $perm - refusing to read a pairing code from a broadly readable file" 4
fi

# Also accept an mDNS-advertised pairing endpoint (the dialog must be open, so
# this is inherently short-lived).
if ! grep -q . "$PAIR_FILE"; then
  found="$(timeout 20 adb mdns services 2>/dev/null | awk '/_adb-tls-pairing\._tcp/ {print $3}' | sort -u)"
  [[ -n "$found" ]] || hg_die "pairing file is empty and no _adb-tls-pairing service is advertised" 3
  hg_log "pairing endpoints from mDNS: $found (the phone must still be showing the code)"
  { while read -r ep; do printf '%s \n' "$ep"; done <<<"$found"; } > "$PAIR_FILE"
fi

RC=0
while read -r EP CODE; do
  [[ -n "$EP" ]] || continue
  if [[ -z "${CODE:-}" ]]; then
    hg_log "no code recorded for $EP - open the pairing dialog and add it to $PAIR_FILE"
    RC=4; continue
  fi
  hg_log "pairing with $EP (code supplied via file, not echoed)"
  # Redirect through hg_redact so even an adb error mentioning the code cannot
  # land in the saved transcript.
  out="$(timeout 30 adb pair "$EP" "$CODE" 2>&1)"; prc=$?
  printf '%s\n' "$out" | hg_redact | sed 's/^/  /' >&2
  if (( prc == 0 )) && grep -qiE 'successfully paired' <<<"$out"; then
    hg_log "PAIRED $EP"
  else
    hg_log "pairing returned rc=$prc for $EP"
    RC=4
  fi
done < <(cat "$PAIR_FILE")

# Destroy the code material regardless of outcome.
if command -v shred >/dev/null 2>&1; then shred -u "$PAIR_FILE" 2>/dev/null || rm -f "$PAIR_FILE"
else rm -f "$PAIR_FILE"; fi
hg_log "pairing file destroyed"

exit "$RC"
