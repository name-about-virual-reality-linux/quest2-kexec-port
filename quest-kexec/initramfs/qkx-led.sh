#!/bin/busybox sh
# qkx-led.sh -- blink the headset status LED purple while we are in kexec.
#
# Purpose: make "this is the kexec'd kernel, not Android" visible at a glance
# without a display attached. The RGB status LED on the Quest 2 is the QTI
# tri-led on PM8150L (DT node /soc/.../qcom,leds@d000, child labels
# red/green/blue on the pwm-lpg channels). The target kernel already has
# CONFIG_LEDS_QTI_TRI_LED=y + CONFIG_LEDS_TRIGGER_TIMER=y and the boot DTB
# carries the node enabled, so this is a pure userspace job -- no kernel or
# DT change needed.
#
# Purple = red + blue at full, green dark.
#
# The blink itself is done in the PMIC hardware via the led "timer" trigger,
# NOT by a userspace loop, for two reasons: it costs no CPU on a headset with
# a dead fan, and it survives switch_root (a shell loop would lose the
# initramfs's /bin/sleep when switch_root deletes the old root).
#
# Env overrides (also settable from the qkx LED_* placeholders below):
#   QKX_LED_DIR      sysfs leds dir          (default /sys/class/leds)
#   QKX_LED_BRIGHT   on-level, 0..255        (default 255)
#   QKX_LED_ON_MS    on time in ms           (default 400)
#   QKX_LED_OFF_MS   off time in ms          (default 400)
#   QKX_LED_WAIT     seconds to wait for the class dir to show up (default 15)

LEDS="${QKX_LED_DIR:-/sys/class/leds}"
BRIGHT="${QKX_LED_BRIGHT:-255}"
ON_MS="${QKX_LED_ON_MS:-400}"
OFF_MS="${QKX_LED_OFF_MS:-400}"
WAIT_S="${QKX_LED_WAIT:-15}"

log() {
	if [ -w /dev/kmsg ]; then
		echo "qkx-led: $*" > /dev/kmsg
	else
		echo "qkx-led: $*"
	fi
}

# led_for <label>: echo the sysfs dir for a channel labelled <label>.
# Handles both the bare DT label ("red") and the usual devicename:colour
# form ("pm8150l:red").
led_for() {
	for d in "$LEDS"/*; do
		[ -d "$d" ] || continue
		case "${d##*/}" in
		"$1" | *:"$1")
			echo "$d"
			return 0
			;;
		esac
	done
	return 1
}

wr() { # wr <path> <value>
	[ -w "$1" ] || return 1
	echo "$2" >"$1" 2>/dev/null
}

# The PMIC probes during do_initcalls, normally well before init runs, but a
# deferred SPMI probe can land later -- give it a bounded window. The
# `sleep || break` keeps this from spinning if the initramfs is already gone
# (post-switch_root, where /bin/sleep no longer exists).
i=0
while [ "$i" -lt "$WAIT_S" ]; do
	[ -d "$LEDS" ] && led_for red >/dev/null 2>&1 && break
	i=$((i + 1))
	sleep 1 2>/dev/null || break
done

RED="$(led_for red)" || {
	log "no tri-led found under $LEDS -- PM8150L tri-led did not probe; no indicator"
	exit 0
}
BLUE="$(led_for blue)" || BLUE=
GREEN="$(led_for green)" || GREEN=

# Green stays dark so red + blue reads as purple rather than washed-out white.
# The DT gives all three channels a default "timer" trigger at brightness 10,
# so clear green explicitly instead of assuming it starts off.
if [ -n "$GREEN" ]; then
	wr "$GREEN/trigger" none
	wr "$GREEN/brightness" 0
fi

# arm <dir>: turn one channel into a hardware blink at BRIGHT.
#
# Order matters. The DT marks these channels qcom,continuous-mode, and
# qpnp_tri_led_set_blink() SWALLOWS the first blink request per channel --
# it only clears the flag and returns. Writing delay_on/delay_off is what
# issues the blink request, and led_delay_on_store() passes 0 for delay_off
# until that has been written too, so the pair must be written after
# brightness (brightness 0 => the "on" phase would be dark) and in this
# order. The first (swallowed) request is issued by the trigger write.
arm() {
	wr "$1/trigger" timer
	wr "$1/brightness" "$BRIGHT"
	wr "$1/delay_on" "$ON_MS"
	wr "$1/delay_off" "$OFF_MS"
}

arm "$RED"
[ -n "$BLUE" ] && arm "$BLUE"

log "kexec indicator armed on $RED${BLUE:+ + $BLUE}: purple blink ${ON_MS}ms/${OFF_MS}ms"
