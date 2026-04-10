#!/bin/sh
set -e

# Seed /etc/asterisk with defaults if the directory is empty.
# This handles both a fresh named volume and an empty bind-mount directory.
if [ -z "$(ls -A /etc/asterisk 2>/dev/null)" ]; then
    echo "[entrypoint] /etc/asterisk is empty — copying default configs"
    cp -r /usr/share/asterisk/etc-defaults/. /etc/asterisk/
fi

exec /usr/sbin/asterisk -fpg
