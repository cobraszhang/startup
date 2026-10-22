#!/bin/sh
# User-side autostart snippet for session scripts (~/.xinitrc, window
# manager autostart hooks, etc.). The user supervisor waits (bounded,
# see below) until the system-side supervisor's control socket exists,
# then starts and supervises application-class entries only.
#
#   sh contrib/startup-user.sh &      # or paste the exec line below
#
# BOOTLAUNCHER_WAITMAX=<seconds> bounds the wait (0 starts
# immediately). Endpoints: system side /tmp/bootlauncher.ctl, user
# side /tmp/bootlauncher-user.ctl (env BOOTLAUNCHER_CTL overrides).
exec /usr/local/bin/startup -d user
