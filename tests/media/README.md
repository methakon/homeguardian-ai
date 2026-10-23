# Shared test media — private, never committed

Owner directive: any authorised test captures live here.

    /home/swarna-sekhar-dhar/projects/homeguardian-ai/tests/media/

## Rules

1. **Never commit private media to Git.** This directory is gitignored; do not remove the rule.
2. Only captures from the owner's designated non-private area belong here.
3. No private people, conversations, or identifiable content.
4. No public streaming, and no uploading to Oracle or any cloud destination without separate
   approval.
5. Filenames should describe the test, not the household: `hg-cam-08d-screen-off-30min.mp4`,
   not a room or person name.
6. Delete captures when the test is done unless they are the evidence for a completed row.

## What is NOT stored here

- anything from a private area
- anything with identifiable people
- production credentials or tokens
- phone backups (those live under `devices/backup/`, separately gitignored)
