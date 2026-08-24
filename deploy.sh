#!/usr/bin/env bash
# Build and deploy mod-host to the device.
# Syncs source, compiles on-device (aarch64), installs over the
# mod-host-pistomp deb's files, then restarts mod-host.
#
# This is a *development* shortcut: it overwrites files owned by the
# mod-host-pistomp package, so apt will consider them modified and the next
# `apt install --reinstall mod-host-pistomp` (or a version bump) reverts them.
# For a real release, land on master and bump the version in pi-gen-pistomp.
#
# Usage:
#   ./deploy.sh
set -euo pipefail

HOST="${PISTOMP_HOST:-pistomp.local}"
USER="${PISTOMP_USER:-pistomp}"
TARGET="${USER}@${HOST}"
PREFIX="/usr"
REMOTE_SRC="/tmp/mod-host"

echo "==> Syncing source to ${TARGET}:${REMOTE_SRC}"
ssh "${TARGET}" "mkdir -p ${REMOTE_SRC}"
rsync -az --delete \
    --exclude='.git' --exclude='*.o' --exclude='*.so' --exclude='*.dylib' \
    --exclude='*.dSYM' --exclude='mod-host' --exclude='src/info.h' \
    ./ "${TARGET}:${REMOTE_SRC}/"

# Build deps. The device is a runtime image, so the -dev packages the deb's
# chroot has are not necessarily here. Missing libreadline-dev is a hard
# compile error; missing libfftw3-dev silently drops -DHAVE_FFTW335 and gives
# you a binary that differs from the shipped deb, which is worse.
echo "==> Checking build deps"
ssh "${TARGET}" "set -e; missing=; \
    [ -e /usr/include/readline/readline.h ] || missing=\"\$missing libreadline-dev\"; \
    pkg-config --atleast-version=3.3.5 fftw3 fftw3f 2>/dev/null || missing=\"\$missing libfftw3-dev\"; \
    pkg-config --exists jack || missing=\"\$missing libjack-jackd2-dev\"; \
    if [ -n \"\$missing\" ]; then \
        echo \"    installing:\$missing\"; \
        sudo apt-get update -qq && sudo apt-get install -y -qq \$missing; \
    else echo '    ok'; fi"

echo "==> Building on device"
ssh "${TARGET}" "set -e; \
    pkg-config --atleast-version=1.9.0 jack || { \
        echo 'ERROR: pkg-config cannot resolve jack >= 1.9.0 on device' >&2; \
        echo \"       Found: \$(pkg-config --modversion jack 2>&1)\" >&2; \
        exit 1; }; \
    make -C ${REMOTE_SRC} -j\$(nproc)"

ssh "${TARGET}" "nm -D --undefined-only ${REMOTE_SRC}/mod-host | grep -q jack_internal_client_load" \
    || { echo "ERROR: built binary lacks jack_internal_client_load (HAVE_JACK2 off)" >&2; exit 1; }

# Stop first: overwriting the running executable in place gives ETXTBSY.
# mod-ui has Requires=mod-host, so stopping mod-host stops mod-ui too (and
# mod-ala-pi-stomp, which mod-ui Wants) — and systemd does NOT bring dependents
# back when mod-host starts again. Starting mod-ui is what restores the whole
# stack: it pulls mod-host via Requires and pi-stomp via Wants.
echo "==> Stopping mod-host"
ssh "${TARGET}" "sudo systemctl stop mod-host"

echo "==> Installing to ${PREFIX}"
ssh "${TARGET}" "sudo make -C ${REMOTE_SRC} install PREFIX=${PREFIX}"

echo "==> Starting mod-host + mod-ui"
ssh "${TARGET}" "sudo systemctl start mod-ui"
ssh "${TARGET}" "systemctl is-active mod-host mod-ui mod-ala-pi-stomp || true"

echo "==> Done"
