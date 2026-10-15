#!/bin/bash
# de_service.sh -- bring the Quest 2 panel up and put a desktop on it.
#
# This is the only thing that takes the DRM master (through
# /usr/local/bin/kms_dual_image) and the only thing that drives X :99. It is
# launched from /etc/rc.local by the qkx-init PID 1 shim.
#
# Two stages share one presenter:
#   stage 2  KDE Plasma 6 (kwin_x11 + plasmashell)    <- the goal
#   stage 1  openbox + one terminal per eye           <- verified fallback
#
# Stage 2 is tried first and then *health-checked*: if kwin_x11 or
# plasmashell is not alive, or plasmashell dies later (this SoC has OOM-wedged
# on kwin+plasmashell twice), we fall back to stage 1 rather than leaving the
# panel on the presenter's test pattern. The previous bring-up did exactly
# that -- it stalled before kwin even started (no kwin.log on the target at
# all, only "nohup: ignoring input" in de_service.log) and nobody noticed,
# because nothing was checked and nothing was logged.
#
# So: every step below is either existence-checked or timeout-bounded, and
# every step says out loud whether it worked.
#
# Tunables (all optional):
#   QKX_STAGE=auto|1|2   default auto: try Plasma, fall back to stage 1
#   QKX_MIRROR=0|1       mirror left-eye half onto right eye (default 1)
#   QKX_PUMP_MS=n        frame pump period, default 200 (5 Hz)
#   QKX_COMPOSE=Q|N|O    kwin compositor hint, default Q (software QPainter;
#                        Xvfb has no GLX, and KWin 6's default is OpenGL)

export PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
export HOME=/root
export DISPLAY=:99
export XDG_RUNTIME_DIR=/run/xdg
export XDG_CURRENT_DESKTOP=KDE
export KDE_SESSION_VERSION=6
export KDE_NO_IPV6=1

# Software rendering everywhere: Xvfb has no DRI, and llvmpipe is plenty for
# a desktop that is snapshotted a few times a second.
export QT_QPA_PLATFORM=xcb
export QT_QUICK_BACKEND=software
export QT_XCB_GL_INTEGRATION=none
export LIBGL_ALWAYS_SOFTWARE=1

# Presenter geometry/behaviour. QKX_ROT=1 is the eyes-measured CCW rotation.
export QKX_ROT=1
export QKX_DT_MS="${QKX_PUMP_MS:-200}"
export QKX_HOLD=0
# Do NOT set QKX_NODISABLE: the disable pass is what gives the modeset a
# clean regulator sequence.
unset QKX_NODISABLE

STAGE="${QKX_STAGE:-auto}"
MIRROR="${QKX_MIRROR:-1}"
export KWIN_COMPOSE="${QKX_COMPOSE:-Q}"

RUN=/run
FRAME="$RUN/frame.xwd"
TMO=""
command -v timeout >/dev/null 2>&1 && TMO="timeout 20"

log() { echo "[de_service $(date '+%H:%M:%S')] $*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- process hygiene
stop_everything() {
	# Matches the presenters/DE bits we own. None of these patterns match
	# this script's own command line, so pkill cannot kill us.
	local p
	for p in kms_dual_image mirror_xwd.py "Xvfb :99" openbox kwin_x11 \
		plasmashell xterm "xwd -display"; do
		pkill -9 -f "$p" 2>/dev/null
	done
	sleep 1
	mkdir -p /tmp/.X11-unix "$XDG_RUNTIME_DIR" "$RUN"
	chmod 1777 /tmp/.X11-unix
	chmod 0700 "$XDG_RUNTIME_DIR"
	rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 "$FRAME" "$FRAME.tmp"
}

# ---------------------------------------------------------------------- swap
enable_swap() {
	# kwin+plasmashell have OOM-wedged this board twice. The target kernel
	# has CONFIG_ZRAM=y built in, so a compressed swap device is one sysfs
	# write away and costs nothing when it is not used.
	if grep -qv '^Filename' /proc/swaps 2>/dev/null; then
		log "swap already active"
		return 0
	fi
	if [ -e /sys/block/zram0/disksize ] && have mkswap && have swapon; then
		echo 1G >/sys/block/zram0/disksize 2>/dev/null
		if mkswap /dev/zram0 >/dev/null 2>&1 && swapon /dev/zram0 2>/dev/null; then
			log "zram swap (1G) enabled"
			return 0
		fi
	fi
	log "note: no zram swap; running unswapped"
}

# ----------------------------------------------------------------- presenter
start_presenter() {
	if [ ! -x /usr/local/bin/kms_dual_image ]; then
		log "ERR presenter /usr/local/bin/kms_dual_image missing"
		return 1
	fi
	nohup /usr/local/bin/kms_dual_image "$FRAME" >>"$RUN/kms_dual_image.log" 2>&1 &
	PRESENTER_PID=$!
	sleep 1
	if ! kill -0 "$PRESENTER_PID" 2>/dev/null; then
		log "ERR presenter exited during startup (see $RUN/kms_dual_image.log)"
		return 1
	fi
	# The backlight node appears with the display driver, not before.
	echo 255 >/sys/class/backlight/panel0-backlight/brightness 2>/dev/null
	log "presenter up (pid $PRESENTER_PID, frame=$FRAME)"
}

# ---------------------------------------------------------------------- X :99
start_x() {
	if ! have Xvfb; then
		log "ERR Xvfb missing"
		return 1
	fi
	Xvfb :99 -screen 0 3664x1920x24 \
		-extension DRI2 -extension DRI3 -extension GLX \
		-ac -noreset >"$RUN/xvfb.log" 2>&1 &
	XVFB_PID=$!
	local i=0
	while [ "$i" -lt 60 ]; do
		# xsetroot doubles as the readiness probe and the backdrop paint.
		if xsetroot -display :99 -solid '#1e1e2e' 2>/dev/null; then
			log "X :99 up (pid $XVFB_PID, 3664x1920x24)"
			return 0
		fi
		if ! kill -0 "$XVFB_PID" 2>/dev/null; then
			log "ERR Xvfb exited during startup (see $RUN/xvfb.log)"
			return 1
		fi
		sleep 0.5
		i=$((i + 1))
	done
	log "ERR Xvfb never became ready after 30s"
	return 1
}

# ------------------------------------------------------------------ frame pump
prime_frame() {
	if $TMO xwd -display :99 -root -silent -out "$FRAME.tmp" 2>/dev/null; then
		mv -f "$FRAME.tmp" "$FRAME"
		log "initial frame captured"
	else
		log "warn: initial xwd failed; presenter stays on its test pattern"
	fi
}

start_pump() {
	if have python3 && [ -f /usr/local/bin/mirror_xwd.py ]; then
		python3 /usr/local/bin/mirror_xwd.py \
			--frame "$FRAME" --period-ms "$QKX_DT_MS" --mirror "$MIRROR" \
			>>"$RUN/pump.log" 2>&1 &
		log "frame pump: python (mirror=$MIRROR, ${QKX_DT_MS}ms)"
		return 0
	fi
	# Fallback: plain periodic xwd, no eye mirroring.
	local period="0.$((QKX_DT_MS / 100))"
	(
		while :; do
			sleep "$period"
			xwd -display :99 -root -silent -out "$FRAME.tmp" 2>/dev/null &&
				mv -f "$FRAME.tmp" "$FRAME"
		done
	) >"$RUN/pump.log" 2>&1 &
	log "frame pump: shell, ${period}s (python3 or mirror_xwd.py missing)"
}

# -------------------------------------------------------------------- stage 1
stage1() {
	if ! have openbox; then
		log "ERR openbox missing; no stage 1 available"
		return 1
	fi
	openbox --sm-disable >"$RUN/openbox.log" 2>&1 &
	OPENBOX_PID=$!
	sleep 1
	if ! kill -0 "$OPENBOX_PID" 2>/dev/null; then
		log "ERR openbox exited during startup"
		return 1
	fi
	[ -x /usr/local/bin/show_eyes.sh ] &&
		/usr/local/bin/show_eyes.sh >"$RUN/eyes.log" 2>&1
	log "STAGE 1 up: openbox + one terminal per eye"
}

# -------------------------------------------------------------------- stage 2
seed_kde_config() {
	mkdir -p /root/.config
	# No splash screen: it only delays the first visible frame.
	cat <<'EOF' >/root/.config/ksplashrc
[KSplash]
Engine=None
Theme=None
EOF
	# Baloo's file indexer is pure overhead for a snapshot desktop and a
	# real memory sink. Turn it off at the source rather than with env vars
	# (BALOO_DISABLED is not a thing KDE reads).
	cat <<'EOF' >/root/.config/baloofilerc
[Basic Settings]
Indexing-Enabled=false
EOF
}

stage2() {
	local b
	for b in kwin_x11 plasmashell; do
		have "$b" || {
			log "stage 2 unavailable: $b is not installed"
			return 1
		}
	done

	seed_kde_config

	if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && have dbus-launch; then
		eval "$($TMO dbus-launch --sh-syntax 2>/dev/null)" || true
		if [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
			log "session dbus started"
		else
			log "warn: no session dbus; Plasma will run degraded"
		fi
	fi

	# KWIN_COMPOSE=Q is the software compositor: Xvfb advertises no GLX, so
	# KWin's OpenGL default would fail here.
	KWIN_COMPOSE="$KWIN_COMPOSE" kwin_x11 >"$RUN/kwin.log" 2>&1 &
	KWIN_PID=$!
	sleep 3
	if ! kill -0 "$KWIN_PID" 2>/dev/null; then
		log "ERR kwin_x11 exited during startup (see $RUN/kwin.log)"
		return 1
	fi

	plasmashell --no-respawn >"$RUN/plasmashell.log" 2>&1 &
	SHELL_PID=$!
	sleep 8
	if ! kill -0 "$SHELL_PID" 2>/dev/null; then
		log "ERR plasmashell exited during startup (see $RUN/plasmashell.log)"
		return 1
	fi

	log "STAGE 2 up: kwin_x11 (pid $KWIN_PID) + plasmashell (pid $SHELL_PID)"
	return 0
}

# plasmashell is the piece the OOM killer reaches for first. Watch it for a
# few minutes and drop to the stage 1 layout if it goes away, so the panel is
# never left showing a frozen frame.
stage2_watch() {
	local n=0
	while [ "$n" -lt 20 ]; do
		sleep 15
		n=$((n + 1))
		if ! kill -0 "$SHELL_PID" 2>/dev/null; then
			log "plasmashell exited after startup (OOM?); falling back to stage 1"
			stage1
			return
		fi
	done
}

# ------------------------------------------------------------------------ main
log "start: stage=$STAGE mirror=$MIRROR compose=$KWIN_COMPOSE pump=${QKX_DT_MS}ms"
stop_everything
enable_swap

start_presenter || exit 1
start_x || exit 1
prime_frame
start_pump

case "$STAGE" in
1)
	stage1 || log "stage 1 failed; presenter keeps showing its last frame"
	;;
2)
	stage2 || log "stage 2 failed (requested explicitly, no fallback)"
	;;
*)
	if stage2; then
		stage2_watch &
	else
		log "falling back to stage 1"
		stage1 || log "stage 1 also failed; based on the presenter only"
	fi
	;;
esac

log "ready"
