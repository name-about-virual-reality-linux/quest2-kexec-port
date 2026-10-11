#!/bin/sh
# Print state of the regulators the JDI panel depends on (works on Android and the kexec kernel).
for r in /sys/class/regulator/regulator.*; do
	n=$(cat $r/name 2>/dev/null)
	case "$n" in
	*lcdb*|*l12*|*l13*|*display_panel_avdd*|*lab*|*ibb*)
		echo "$n: state=$(cat $r/state 2>/dev/null) users=$(cat $r/num_users 2>/dev/null) uV=$(cat $r/microvolts 2>/dev/null)"
		for c in $r/*-supply $r/*.*; do [ -L "$c" ] && echo "    consumer: $(basename $c)"; done 2>/dev/null
		;;
	esac
done
