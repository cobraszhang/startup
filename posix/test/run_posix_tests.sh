#!/bin/sh
# run_posix_tests.sh - POSIX launcher test suite.
# Run from anywhere:  sh posix/test/run_posix_tests.sh
# Exits 0 only when every test passes.
cd "$(dirname "$0")/.." || exit 1

ORDER_OUT=/tmp/bootlauncher-order.txt
EXPECTED=$(printf 'first\nsecond\nthird')
PASS=0
FAIL=0

report() {
    if [ "$2" = "0" ]; then
        echo "PASS  $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $1"
        FAIL=$((FAIL + 1))
    fi
}

check_order_file() {
    if [ -f "$ORDER_OUT" ] && [ "$(cat "$ORDER_OUT")" = "$EXPECTED" ]; then
        return 0
    fi
    return 1
}

echo "== 1. build (cc -std=c89 -pedantic -Wall -Wextra) =="
make clean >/dev/null 2>&1
make >/tmp/posix-build.log 2>&1
report "build" $?

echo "== 2. dependency order (foreground) =="
rm -f "$ORDER_OUT"
# plain boot mode: explicitly headless (with WSLg a plain call opens the GUI)
env -u DISPLAY -u WAYLAND_DISPLAY ./build/bootlauncher -c test/order.conf >/tmp/posix-t1.log 2>&1
rc=$?
sleep 1   # children are detached; give the last one time to append
if [ "$rc" = 0 ] && check_order_file; then ok=0; else ok=1; fi
report "start order first->second->third" $ok

echo "== 3. dependency cycle rejection =="
env -u DISPLAY -u WAYLAND_DISPLAY ./build/bootlauncher -c test/cycle.conf >/tmp/posix-t2.log 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q 'dependency cycle' /tmp/posix-t2.log; then ok=0; else ok=1; fi
report "cycle detected and startup aborted" $ok

echo "== 4. unknown dependency rejection =="
env -u DISPLAY -u WAYLAND_DISPLAY ./build/bootlauncher -c test/baddep.conf >/tmp/posix-t3.log 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q "unknown app 'ghost'" /tmp/posix-t3.log; then ok=0; else ok=1; fi
report "unknown dependency detected" $ok

echo "== 5. daemon mode (-d) =="
rm -f "$ORDER_OUT"
./build/bootlauncher -c test/order.conf -d
sleep 2
if check_order_file; then ok=0; else ok=1; fi
report "detached launcher still orders correctly" $ok

echo "== 6. make install + systemd unit (needs root or sudo) =="
if [ "$(id -u)" = "0" ]; then SUDO=""
elif sudo -n true 2>/dev/null; then SUDO="sudo"
else SUDO=""; fi

if [ "$(id -u)" = "0" ] || sudo -n true 2>/dev/null; then
    $SUDO make install >/tmp/posix-t4.log 2>&1
    report "make install -> /usr/local/bin + /usr/local/conf" $?

    $SUDO cp test/order.conf /usr/local/conf/startup.conf
    $SUDO cp contrib/bootlauncher.service /etc/systemd/system/
    $SUDO systemctl daemon-reload
    rm -f "$ORDER_OUT"
    $SUDO systemctl start bootlauncher.service
    sleep 2
    if check_order_file; then ok=0; else ok=1; fi
    report "systemd unit starts apps in dependency order" $ok

    # cleanup (leave /usr/local/bin/bootlauncher installed)
    $SUDO systemctl disable bootlauncher.service >/dev/null 2>&1
    $SUDO rm -f /etc/systemd/system/bootlauncher.service
    $SUDO systemctl daemon-reload
else
    echo "SKIP  not root and no passwordless sudo (run: wsl -u root)"
fi

echo "== 7. default config = <exe dir>/../conf/startup.conf =="
LT=/tmp/lt-rel
rm -rf "$LT" && mkdir -p "$LT/bin" "$LT/conf"
cp build/bootlauncher "$LT/bin/bootlauncher"
cp test/order.conf "$LT/conf/startup.conf"
rm -f "$ORDER_OUT"
(cd / && env -u DISPLAY -u WAYLAND_DISPLAY "$LT/bin/bootlauncher") >/tmp/posix-t5.log 2>&1
rc=$?
sleep 1
if [ "$rc" = 0 ] && check_order_file; then ok=0; else ok=1; fi
report "exe-relative ../conf resolution (cwd-independent)" $ok
rm -rf "$LT"

echo "== 8. CLI management interface =="
CLI_T=/tmp/posix-cli.conf
cp test/order.conf "$CLI_T"

# 8a list: shows all three entries with their fields
./build/bootlauncher -c "$CLI_T" list > /tmp/posix-t6a.log 2>&1
if [ $? = 0 ] && grep -q '^first$' /tmp/posix-t6a.log \
   && grep -q 'deps = first' /tmp/posix-t6a.log; then ok=0; else ok=1; fi
report "cli list" $ok

# 8b order: deterministic dependency-resolved plan
./build/bootlauncher -c "$CLI_T" order > /tmp/posix-t6b.log 2>&1
if [ "$(cat /tmp/posix-t6b.log)" = "$(printf '1. first\n2. second\n3. third')" ]; then ok=0; else ok=1; fi
report "cli order" $ok

# 8c mutate: add + set + dep + rename (references follow the rename)
./build/bootlauncher -c "$CLI_T" add extra "echo extra >> $ORDER_OUT" >/dev/null 2>&1 \
  && ./build/bootlauncher -c "$CLI_T" set extra wait 200 >/dev/null 2>&1 \
  && ./build/bootlauncher -c "$CLI_T" dep add extra third >/dev/null 2>&1 \
  && ./build/bootlauncher -c "$CLI_T" rename third three >/dev/null 2>&1
./build/bootlauncher -c "$CLI_T" order > /tmp/posix-t6c.log 2>&1
if [ "$(cat /tmp/posix-t6c.log)" = "$(printf '1. first\n2. second\n3. three\n4. extra')" ]; then ok=0; else ok=1; fi
report "cli add/set/dep/rename round-trip" $ok

# 8d check: cycle detection after making first depend on extra
./build/bootlauncher -c "$CLI_T" dep add first extra >/dev/null 2>&1
./build/bootlauncher -c "$CLI_T" check >/tmp/posix-t6d.log 2>&1
rc1=$?
./build/bootlauncher -c "$CLI_T" dep del first extra >/dev/null 2>&1
./build/bootlauncher -c "$CLI_T" check >/tmp/posix-t6d2.log 2>&1
rc2=$?
if [ "$rc1" != 0 ] && grep -q 'dependency cycle' /tmp/posix-t6d.log \
   && [ "$rc2" = 0 ] && grep -q 'configuration OK' /tmp/posix-t6d2.log; then ok=0; else ok=1; fi
report "cli check (cycle detection + fix)" $ok

# 8e run: mutated configuration really starts the new entry in order
rm -f "$ORDER_OUT"
./build/bootlauncher -c "$CLI_T" run >/tmp/posix-t6e.log 2>&1
rc=$?
sleep 1
if [ "$rc" = 0 ] && [ "$(cat "$ORDER_OUT" 2>/dev/null)" = "$(printf 'first\nsecond\nthird\nextra')" ]; then ok=0; else ok=1; fi
report "cli run applies edits (order first,second,third,extra)" $ok

# 8f rm: entry and dangling references disappear
./build/bootlauncher -c "$CLI_T" rm three >/dev/null 2>&1
./build/bootlauncher -c "$CLI_T" order > /tmp/posix-t6f.log 2>&1
if grep -q 'extra' /tmp/posix-t6f.log \
   && ! grep -q 'three' /tmp/posix-t6f.log \
   && ! grep -q 'deps = three' /tmp/posix-t6f.log; then ok=0; else ok=1; fi
report "cli rm (drops entry and references)" $ok

# 8g error paths: duplicate name / unknown dependency exit non-zero
./build/bootlauncher -c "$CLI_T" add first "echo x" >/dev/null 2>&1; rca=$?
./build/bootlauncher -c "$CLI_T" dep add extra ghost >/dev/null 2>&1; rcb=$?
if [ "$rca" != 0 ] && [ "$rcb" != 0 ]; then ok=0; else ok=1; fi
report "cli error exit codes" $ok
rm -f "$CLI_T"

echo "== 9. GTK GUI self-test (needs gtk+-3.0 dev + a display) =="
if grep -q HAVE_GUI /tmp/posix-build.log 2>/dev/null \
   && [ -n "$DISPLAY$WAYLAND_DISPLAY" ]; then
    # scripted: load model, verify deps pre-selection, run topology check;
    # every status line is mirrored to stderr as "[gui-status] ..."
    cp test/order.conf /tmp/bootlauncher-gui.conf
    APPBOOT_GUI_SELFTEST=1 APPBOOT_GUI_EXPECT_N=3 \
        ./build/bootlauncher -c /tmp/bootlauncher-gui.conf gui \
        >/tmp/posix-t9.log 2>&1
    rc=$?
    if [ "$rc" = 0 ] && grep -q '1. first' /tmp/posix-t9.log \
       && grep -q '2. second' /tmp/posix-t9.log \
       && grep -q '3. third' /tmp/posix-t9.log; then ok=0; else ok=1; fi
    report "gtk gui load + deps pre-select + topology (scripted)" $ok
    rm -f /tmp/bootlauncher-gui.conf
else
    echo "SKIP  built without GTK or no graphical display"
fi

echo
echo "========  $PASS passed, $FAIL failed  ========"
[ "$FAIL" = 0 ] || exit 1
exit 0
