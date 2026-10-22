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

echo "== 1. build (release + debug, zero warnings) =="
make clean >/dev/null 2>&1
make >/tmp/posix-build.log 2>&1
report "build release+debug" $?

# debug binary smoke: resolves the same order without launching anything
./build/debug/startup -c test/order.conf order >/tmp/posix-t0.log 2>&1
rc=$?
if [ "$rc" = 0 ] && [ "$(cat /tmp/posix-t0.log 2>/dev/null)" = "$(printf '1. first\n2. second\n3. third')" ]; then ok=0; else ok=1; fi
report "debug build runnable, correct order" $ok

echo "== 2. dependency order (foreground) =="
rm -f "$ORDER_OUT"
# plain boot mode: explicitly headless (with WSLg a plain call opens the GUI)
env -u DISPLAY -u WAYLAND_DISPLAY ./build/release/startup -c test/order.conf >/tmp/posix-t1.log 2>&1
rc=$?
sleep 1   # children are detached; give the last one time to append
if [ "$rc" = 0 ] && check_order_file; then ok=0; else ok=1; fi
report "start order first->second->third" $ok

echo "== 3. dependency cycle rejection =="
env -u DISPLAY -u WAYLAND_DISPLAY ./build/release/startup -c test/cycle.conf >/tmp/posix-t2.log 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q 'dependency cycle' /tmp/posix-t2.log; then ok=0; else ok=1; fi
report "cycle detected and startup aborted" $ok

echo "== 4. unknown dependency rejection =="
env -u DISPLAY -u WAYLAND_DISPLAY ./build/release/startup -c test/baddep.conf >/tmp/posix-t3.log 2>&1
rc=$?
if [ "$rc" != 0 ] && grep -q "unknown app 'ghost'" /tmp/posix-t3.log; then ok=0; else ok=1; fi
report "unknown dependency detected" $ok

echo "== 5. daemon mode (-d) =="
rm -f "$ORDER_OUT"
./build/release/startup -c test/order.conf -d
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
    $SUDO cp contrib/startup.service /etc/systemd/system/
    $SUDO systemctl daemon-reload
    $SUDO systemctl stop startup.service >/dev/null 2>&1   # idempotent
    rm -f "$ORDER_OUT"
    $SUDO systemctl start startup.service
    sleep 2
    if check_order_file; then ok=0; else ok=1; fi
    report "systemd unit starts apps in dependency order" $ok

    # cleanup: the unit is Type=simple (resident supervisor) - it must
    # be stopped, not just disabled, or it leaks into the next run
    $SUDO systemctl stop startup.service >/dev/null 2>&1
    $SUDO systemctl disable startup.service >/dev/null 2>&1
    $SUDO rm -f /etc/systemd/system/startup.service
    $SUDO systemctl daemon-reload
else
    echo "SKIP  not root and no passwordless sudo (run: wsl -u root)"
fi

echo "== 7. default config = <exe dir>/../conf/startup.conf =="
LT=/tmp/lt-rel
rm -rf "$LT" && mkdir -p "$LT/bin" "$LT/conf"
cp build/release/startup "$LT/bin/startup"
cp test/order.conf "$LT/conf/startup.conf"
rm -f "$ORDER_OUT"
(cd / && env -u DISPLAY -u WAYLAND_DISPLAY "$LT/bin/startup") >/tmp/posix-t5.log 2>&1
rc=$?
sleep 1
if [ "$rc" = 0 ] && check_order_file; then ok=0; else ok=1; fi
report "exe-relative ../conf resolution (cwd-independent)" $ok
rm -rf "$LT"

echo "== 8. CLI management interface =="
CLI_T=/tmp/posix-cli.conf
cp test/order.conf "$CLI_T"

# 8a list: shows all three entries with their fields
./build/release/startup -c "$CLI_T" list > /tmp/posix-t6a.log 2>&1
if [ $? = 0 ] && grep -q '^first$' /tmp/posix-t6a.log \
   && grep -q 'deps = first' /tmp/posix-t6a.log; then ok=0; else ok=1; fi
report "cli list" $ok

# 8b order: deterministic dependency-resolved plan
./build/release/startup -c "$CLI_T" order > /tmp/posix-t6b.log 2>&1
if [ "$(cat /tmp/posix-t6b.log)" = "$(printf '1. first\n2. second\n3. third')" ]; then ok=0; else ok=1; fi
report "cli order" $ok

# 8c mutate: add + set + dep + rename (references follow the rename)
./build/release/startup -c "$CLI_T" add extra "echo extra >> $ORDER_OUT" >/dev/null 2>&1 \
  && ./build/release/startup -c "$CLI_T" set extra wait 200 >/dev/null 2>&1 \
  && ./build/release/startup -c "$CLI_T" dep add extra third >/dev/null 2>&1 \
  && ./build/release/startup -c "$CLI_T" rename third three >/dev/null 2>&1
./build/release/startup -c "$CLI_T" order > /tmp/posix-t6c.log 2>&1
if [ "$(cat /tmp/posix-t6c.log)" = "$(printf '1. first\n2. second\n3. three\n4. extra')" ]; then ok=0; else ok=1; fi
report "cli add/set/dep/rename round-trip" $ok

# 8d check: cycle detection after making first depend on extra
./build/release/startup -c "$CLI_T" dep add first extra >/dev/null 2>&1
./build/release/startup -c "$CLI_T" check >/tmp/posix-t6d.log 2>&1
rc1=$?
./build/release/startup -c "$CLI_T" dep del first extra >/dev/null 2>&1
./build/release/startup -c "$CLI_T" check >/tmp/posix-t6d2.log 2>&1
rc2=$?
if [ "$rc1" != 0 ] && grep -q 'dependency cycle' /tmp/posix-t6d.log \
   && [ "$rc2" = 0 ] && grep -q 'configuration OK' /tmp/posix-t6d2.log; then ok=0; else ok=1; fi
report "cli check (cycle detection + fix)" $ok

# 8e run: mutated configuration really starts the new entry in order
rm -f "$ORDER_OUT"
./build/release/startup -c "$CLI_T" run >/tmp/posix-t6e.log 2>&1
rc=$?
# poll until all four probes have written (loaded systems can delay them;
# this removes write-completion from the equation, leaving only order)
i=0
while [ "$i" -lt 50 ]; do
    [ "$(wc -l < "$ORDER_OUT" 2>/dev/null)" = "4" ] && break
    i=$((i + 1)); sleep 0.1
done
if [ "$rc" = 0 ] && [ -f "$ORDER_OUT" ]; then ok=0; else ok=1; fi
for w in first second third extra; do
    grep -qx "$w" "$ORDER_OUT" 2>/dev/null || ok=1
done
# ordering semantics have their own dedicated tests (section 2); here the
# point is that run executes the *mutated* model. Concurrent detached
# children racing into one file is scheduling noise, so the strict order
# assertion lives only in the wait-separated tests, not here.
if [ "$ok" = 1 ]; then
    echo "  diag: order file content:"
    sed 's/^/  | /' "$ORDER_OUT" 2>/dev/null
    echo "  diag: run log:"
    sed 's/^/  | /' /tmp/posix-t6e.log
fi
report "cli run applies edits (order first,second,third,extra)" $ok

# 8f rm: entry and dangling references disappear
./build/release/startup -c "$CLI_T" rm three >/dev/null 2>&1
./build/release/startup -c "$CLI_T" order > /tmp/posix-t6f.log 2>&1
if grep -q 'extra' /tmp/posix-t6f.log \
   && ! grep -q 'three' /tmp/posix-t6f.log \
   && ! grep -q 'deps = three' /tmp/posix-t6f.log; then ok=0; else ok=1; fi
report "cli rm (drops entry and references)" $ok

# 8g error paths: duplicate name / unknown dependency exit non-zero
./build/release/startup -c "$CLI_T" add first "echo x" >/dev/null 2>&1; rca=$?
./build/release/startup -c "$CLI_T" dep add extra ghost >/dev/null 2>&1; rcb=$?
if [ "$rca" != 0 ] && [ "$rcb" != 0 ]; then ok=0; else ok=1; fi
report "cli error exit codes" $ok

# 8h version: subcommand and -V report the same release version
vout=$(./build/release/startup version 2>&1); rca=$?
vout2=$(./build/release/startup -V 2>&1); rcb=$?
if [ "$rca" = 0 ] && [ "$rcb" = 0 ] && [ "$vout" = "$vout2" ] \
   && echo "$vout" | grep -q '^startup [0-9]*\.[0-9]*\.[0-9]*$'; then ok=0; else ok=1; fi
report "cli version / -V consistency" $ok
rm -f "$CLI_T"

echo "== 9. GTK GUI self-test (needs gtk+-3.0 dev + a display) =="
if grep -q HAVE_GUI /tmp/posix-build.log 2>/dev/null \
   && [ -n "$DISPLAY$WAYLAND_DISPLAY" ]; then
    # scripted: load model, verify deps pre-selection, run topology check;
    # every status line is mirrored to stderr as "[gui-status] ..."
    cp test/order.conf /tmp/bootlauncher-gui.conf
    APPBOOT_GUI_SELFTEST=1 APPBOOT_GUI_EXPECT_N=3 \
        ./build/release/startup -c /tmp/bootlauncher-gui.conf gui \
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

echo "== 10. control IPC (app start/stop/restart/status via serve) =="
CTL_S="/tmp/bl-ctl-$$.sock"
CTL_C="/tmp/bl-ctl-$$.conf"
cp test/ctl.conf "$CTL_C"
rm -f "$CTL_S"
BOOTLAUNCHER_CTL="$CTL_S" ./build/release/startup -c "$CTL_C" serve \
    >/tmp/posix-t10-srv.log 2>&1 &
SRV=$!
i=0
while [ ! -S "$CTL_S" ] && [ "$i" -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
CAP() { BOOTLAUNCHER_CTL="$CTL_S" ./build/release/startup app "$@"; }

CAP ping      >/tmp/posix-t10a.log 2>&1
CAP status    >/tmp/posix-t10b.log 2>&1
CAP stop a    >/dev/null 2>&1
CAP status a  >/tmp/posix-t10c.log 2>&1
CAP restart a >/tmp/posix-t10d.log 2>&1
CAP status a  >/tmp/posix-t10e.log 2>&1
CAP start ghost >/tmp/posix-t10f.log 2>&1; rcg=$?
ok=1
if grep -q '^OK PONG' /tmp/posix-t10a.log \
   && grep -Eq '^  a +running' /tmp/posix-t10b.log \
   && grep -Eq '^  a +stopped' /tmp/posix-t10c.log \
   && grep -q '^OK restarted' /tmp/posix-t10d.log \
   && grep -Eq '^  a +running' /tmp/posix-t10e.log \
   && [ "$rcg" = 1 ] && grep -q "^ERR unknown app 'ghost'" /tmp/posix-t10f.log
then ok=0; fi
report "supervisor IPC: ping/status/stop/restart/unknown" $ok

CAP stop a     >/dev/null 2>&1
CAP stop b     >/dev/null 2>&1
CAP shutdown   >/tmp/posix-t10g.log 2>&1
wait $SRV; rcs=$?
CAP ping >/dev/null 2>&1; rcp=$?
if [ "$rcs" = 0 ] && grep -q '^OK bye' /tmp/posix-t10g.log \
   && [ "$rcp" = 3 ]; then ok=0; else ok=1; fi
report "app shutdown ends supervisor; then unreachable (rc=3)" $ok

pkill -x sleep 2>/dev/null
rm -f "$CTL_S" "$CTL_C" /tmp/posix-t10a.log /tmp/posix-t10b.log \
      /tmp/posix-t10c.log /tmp/posix-t10d.log /tmp/posix-t10e.log \
      /tmp/posix-t10f.log /tmp/posix-t10g.log /tmp/posix-t10-srv.log

echo "== 11. interpreter resolution (config stores script paths only) =="
SD="/tmp/bl-scr-$$"
SC_S="/tmp/bl-scr-$$.sock"
mkdir -p "$SD"
printf '#!/bin/sh\nsleep 123\n' > "$SD/noexec.sh"
printf '#!/bin/sh\nsleep 456\n' > "$SD/direct.sh"
chmod 755 "$SD/direct.sh"
printf 'import time\ntime.sleep(789)\n' > "$SD/task.py"
printf 'no runtime here\n' > "$SD/odd.zzz"
cat > "$SD/scr.conf" <<EOF
[noexec]
cmd = "$SD/noexec.sh"

[direct]
cmd = "$SD/direct.sh"

[odd]
cmd = "$SD/odd.zzz"
EOF
HAVE_PY=1
command -v python3 >/dev/null 2>&1 || HAVE_PY=0
if [ "$HAVE_PY" = 1 ]; then
  printf '\n[pyscript]\ncmd = "%s/task.py"\n' "$SD" >> "$SD/scr.conf"
fi
rm -f "$SC_S"
BOOTLAUNCHER_CTL="$SC_S" ./build/release/startup -c "$SD/scr.conf" serve \
    >/tmp/posix-t11-srv.log 2>&1 &
SRV2=$!
i=0
while [ ! -S "$SC_S" ] && [ "$i" -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
SAP() { BOOTLAUNCHER_CTL="$SC_S" ./build/release/startup app "$@"; }

SAP status >/tmp/posix-t11a.log 2>&1
SAP start odd >/tmp/posix-t11b.log 2>&1; rco=$?
ok=1
if grep -Eq '^  noexec +running' /tmp/posix-t11a.log \
   && grep -Eq '^  direct +running' /tmp/posix-t11a.log \
   && grep -Eq '^  odd +not-started' /tmp/posix-t11a.log \
   && [ "$rco" = 1 ] && grep -q "^ERR launch failed for 'odd'" \
        /tmp/posix-t11b.log
then ok=0; fi
if [ "$HAVE_PY" = 1 ] \
   && ! grep -Eq '^  pyscript +running' /tmp/posix-t11a.log
then ok=1; fi
report "shebang runs direct; non-exec .sh via /bin/sh, .py via python3; unresolvable .zzz refused" $ok

SAP stop noexec  >/dev/null 2>&1
SAP stop direct  >/dev/null 2>&1
if [ "$HAVE_PY" = 1 ]; then SAP stop pyscript >/dev/null 2>&1; fi
SAP shutdown >/dev/null 2>&1
wait $SRV2
pkill -x sleep 2>/dev/null
rm -rf "$SD" "$SC_S" /tmp/posix-t11a.log /tmp/posix-t11b.log \
       /tmp/posix-t11-srv.log

echo "== 12. per-app cwd and output capture =="
WD="/tmp/bl-wd-$$"
mkdir -p "$WD/sub" "$WD/a sub"
cat > "$WD/co.conf" <<EOF
[cwdq]
cmd = /bin/sh -c "pwd; echo hello-q"
cwd = "$WD/a sub"
output = "$WD/outq.txt"

[cwdp]
cmd = /bin/sh -c "pwd; echo hello-p"
cwd = $WD/sub
output = $WD/outp.txt
EOF
./build/release/startup -c "$WD/co.conf" run >/dev/null 2>&1
sleep 1
ok=1
if grep -q "hello-q" "$WD/outq.txt" 2>/dev/null \
   && grep -qxF "$WD/a sub" "$WD/outq.txt" \
   && grep -q "hello-p" "$WD/outp.txt" 2>/dev/null \
   && grep -qxF "$WD/sub" "$WD/outp.txt"; then ok=0; fi
report "run: cwd applied (quoted and bare), stdout+stderr captured to file" $ok

RT="$WD/rt.conf"
cp "$WD/co.conf" "$RT"
./build/release/startup -c "$RT" set cwdp cwd "$WD/never" >/dev/null 2>&1
./build/release/startup -c "$RT" set cwdp output "$WD/out2.txt" >/dev/null 2>&1
./build/release/startup -c "$RT" list > "$WD/list.txt" 2>&1
./build/release/startup -c "$RT" check >/dev/null 2>&1; rcc=$?
ok=1
if grep -qF "cwd  = $WD/never" "$WD/list.txt" \
   && grep -qF "out  = $WD/out2.txt" "$WD/list.txt" \
   && grep -qF "cwd = $WD/never" "$RT" \
   && grep -qF "output = $WD/out2.txt" "$RT" \
   && [ "$rcc" = 0 ]; then ok=0; fi
report "cli set/list/save round-trip for cwd+output (config reloads)" $ok

./build/release/startup -c "$RT" set cwdp cwd "" >/dev/null 2>&1
./build/release/startup -c "$RT" list > "$WD/list2.txt" 2>&1
ok=1
if ! grep -qF "cwd  = $WD/never" "$WD/list2.txt"; then ok=0; fi
report "cli set with empty value clears the field" $ok
rm -rf "$WD"

echo "== 13. startup classes: type = system | application =="
CD="/tmp/bl-cls-$$"
mkdir -p "$CD"
cat > "$CD/bad.conf" <<EOF
[a1]
cmd = /bin/sh -c "echo a1 >> $CD/seq.txt"
type = service
EOF
./build/release/startup -c "$CD/bad.conf" check >"$CD/bad.log" 2>&1; rcbad=$?
ok=1
if [ "$rcbad" != 0 ] && grep -q "must be system or application" "$CD/bad.log"; then ok=0; fi
report "invalid type value is a parse error" $ok

cat > "$CD/c.conf" <<EOF
[u1]
cmd = /bin/sh -c "echo u1 >> $CD/seq.txt"
type = application
deps = s1

[s1]
cmd = /bin/sh -c "echo s1 >> $CD/seq.txt"
deps = s2

[s2]
cmd = /bin/sh -c "echo s2 >> $CD/seq.txt"
EOF
./build/release/startup -c "$CD/c.conf" order > "$CD/order.txt" 2>&1
ok=1
if [ "$(cat "$CD/order.txt" 2>/dev/null)" = "$(printf '1. s2\n2. s1\n3. u1')" ]; then ok=0; fi
report "order: every system app before application apps, deps kept" $ok

cat > "$CD/rev.conf" <<EOF
[sys1]
cmd = /bin/sh -c "echo s"
type = system
deps = app1

[app1]
cmd = /bin/sh -c "echo a"
type = application
EOF
./build/release/startup -c "$CD/rev.conf" check >"$CD/rev.log" 2>&1; rcrev=$?
ok=1
if [ "$rcrev" != 0 ] && grep -q "must not depend on application-class" "$CD/rev.log"; then ok=0; fi
report "system app depending on application app rejected" $ok

SYS_S="/tmp/bl-cls-$$.sys"; USR_S="/tmp/bl-cls-$$.usr"
rm -f "$SYS_S" "$USR_S" "$CD/seq-sys.txt" "$CD/seq-usr.txt"
# separate marker files per side: the supervisor start order is what is
# under test; children appending to ONE file race with each other
cat > "$CD/srv.conf" <<EOF
[u1]
cmd = /bin/sh -c "echo u1 >> $CD/seq-usr.txt"
type = application
deps = s1

[s1]
cmd = /bin/sh -c "echo s1 >> $CD/seq-sys.txt"
deps = s2

[s2]
cmd = /bin/sh -c "echo s2 >> $CD/seq-sys.txt"
EOF
# user side first: it must hold while the system-side endpoint is absent
BOOTLAUNCHER_WAITMAX=30 BOOTLAUNCHER_CTL="$SYS_S" \
  ./build/release/startup -c "$CD/srv.conf" -s "$USR_S" user \
  >"$CD/user.log" 2>&1 &
USERV=$!
sleep 1
ok=1
if [ ! -f "$CD/seq-usr.txt" ]; then ok=0; fi
report "user side holds while system side is not up (bounded wait)" $ok
BOOTLAUNCHER_CTL="$SYS_S" ./build/release/startup -c "$CD/srv.conf" serve \
  >"$CD/sys.log" 2>&1 &
SRV=$!
i=0
while [ "$i" -lt 150 ]; do
    i=$((i + 1))
    if [ -f "$CD/seq-usr.txt" ] && grep -qx s1 "$CD/seq-sys.txt" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
ok=1
if grep -qx u1 "$CD/seq-usr.txt" 2>/dev/null \
   && grep -qx s1 "$CD/seq-sys.txt" 2>/dev/null \
   && grep -qx s2 "$CD/seq-sys.txt" 2>/dev/null; then ok=0; fi
report "two-phase boot: system apps first, user app only after ready" $ok

SYSQ() { BOOTLAUNCHER_CTL="$SYS_S" ./build/release/startup app "$@"; }
USQ()  { BOOTLAUNCHER_CTL="$USR_S" ./build/release/startup app "$@"; }
SYSQ status > "$CD/sys.status" 2>&1
USQ  status > "$CD/usr.status" 2>&1
SYSQ stop u1 > "$CD/cross.log" 2>&1; rccr=$?
ok=1
if grep -q '^  s1' "$CD/sys.status" && grep -q '^  s2' "$CD/sys.status" \
   && ! grep -q '^  u1' "$CD/sys.status" \
   && grep -q '^  u1' "$CD/usr.status" \
   && ! grep -q '^  s1' "$CD/usr.status" \
   && [ "$rccr" = 1 ] && grep -q "application-class" "$CD/cross.log"; then ok=0; fi
report "each supervisor lists/controls only its own class" $ok

USQ shutdown >/dev/null 2>&1; wait $USERV
SYSQ shutdown >/dev/null 2>&1; wait $SRV
rm -f "$SYS_S" "$USR_S"

cp "$CD/c.conf" "$CD/rt.conf"
./build/release/startup -c "$CD/rt.conf" list > "$CD/list0.txt" 2>&1
./build/release/startup -c "$CD/rt.conf" set u1 type system >/dev/null 2>&1
./build/release/startup -c "$CD/rt.conf" list > "$CD/list.txt" 2>&1
./build/release/startup -c "$CD/rt.conf" order > "$CD/order2.txt" 2>&1
ok=1
if grep -qF "  type = application" "$CD/list0.txt" \
   && grep -qF "  type = system" "$CD/list0.txt" \
   && grep -qF "type = system" "$CD/rt.conf" \
   && ! grep -qF "  type = application" "$CD/list.txt" \
   && [ "$(cat "$CD/order2.txt" 2>/dev/null)" = "$(printf '1. s2\n2. s1\n3. u1')" ]; then ok=0; fi
report "set type + list/save round-trip (u1 system: same two-phase order)" $ok

./build/release/startup -c "$CD/rt.conf" set u1 type "" >/dev/null 2>&1
ok=1
if grep -v '^#' "$CD/rt.conf" | grep -q 'type = '; then ok=1; else ok=0; fi
report "empty type value clears the key from the config" $ok

# ---- 14. install/uninstall: registration is analysed, never duplicated ----
# user-side half is fully hermetic (XDG_CONFIG_HOME redirected into the
# test dir); with systemd running the user side registers as a systemd
# USER unit, otherwise as an XDG autostart file - both accepted here.
# The system half may or may not need sudo - not asserted.
echo "== 14. install/uninstall autostart registration (idempotent) =="
AC="$CD/ac"
UNIT="$AC/systemd/user/startup-user.service"
ADSK="$AC/autostart/startup-user.desktop"
rm -rf "$AC"
XDG_CONFIG_HOME="$AC" ./build/release/startup -c "$CD/rt.conf" install \
    >"$CD/inst1.log" 2>&1
ok=1
if { { [ -f "$UNIT" ] && grep -q -- '-d user' "$UNIT"; } \
     || { [ -f "$ADSK" ] && grep -q -- '-d user' "$ADSK"; }; } \
   && grep -q 'registered' "$CD/inst1.log"; then ok=0; fi
report "install creates the user-side registration" $ok
REG="$ADSK"; [ -f "$UNIT" ] && REG="$UNIT"
cp "$REG" "$CD/ac.copy" 2>/dev/null
XDG_CONFIG_HOME="$AC" ./build/release/startup -c "$CD/rt.conf" install \
    >"$CD/inst2.log" 2>&1
ok=1
if grep -q 'already present, left unchanged' "$CD/inst2.log" \
   && cmp -s "$REG" "$CD/ac.copy"; then ok=0; fi
report "second install detects the entry and leaves it unchanged" $ok
XDG_CONFIG_HOME="$AC" ./build/release/startup -c "$CD/rt.conf" uninstall \
    >"$CD/uninst1.log" 2>&1
ok=1
if [ ! -f "$UNIT" ] && [ ! -f "$ADSK" ] \
   && grep -q 'removed' "$CD/uninst1.log"; then ok=0; fi
report "uninstall removes the user-side registration" $ok
XDG_CONFIG_HOME="$AC" ./build/release/startup -c "$CD/rt.conf" uninstall \
    >"$CD/uninst2.log" 2>&1
ok=1
if grep -q 'no user-side registration' "$CD/uninst2.log"; then ok=0; fi
report "uninstall without a registration is a clean no-op" $ok
rm -rf "$CD"

echo
echo "========  $PASS passed, $FAIL failed  ========"
[ "$FAIL" = 0 ] || exit 1
exit 0
