#!/bin/sh
# USB-MIDI device mode for op1-og (owner: t5).
#
# The OG OP-1 is a USB device: plugged into a computer it shows a MIDI port "OP-1 Midi"
# (firmware-behaviour.md §9, PID 0x0004 MidiStreaming). On a Linux board with a USB device controller
# (Raspberry Pi 4/5 USB-C or Pi Zero OTG with dtoverlay=dwc2, any board listed in /sys/class/udc) this
# script creates the same role with the kernel's f_midi gadget function. The gadget's ALSA port is a
# hardware sequencer port, so op1-og connects it to "OP-1 Midi" both ways by itself (also when the gadget
# appears after op1-og started).
#
# The USB IDs are the Linux Foundation's generic composite-gadget IDs, not Teenage Engineering's: this is
# a port, not the device, and it must not pretend to be one.
#
#   sudo src/midi/usb-midi-gadget.sh up        create and bind the gadget
#   sudo src/midi/usb-midi-gadget.sh down      unbind and remove it
#   src/midi/usb-midi-gadget.sh status         is there a UDC, is the gadget bound
#   src/midi/usb-midi-gadget.sh --dry-run up   print the configfs writes without doing them
set -eu

NAME=op1og
G=/sys/kernel/config/usb_gadget/$NAME
DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; shift; fi
cmd="${1:-status}"

run() {  # a configfs change; printed instead in dry-run mode
  if [ "$DRY" = 1 ]; then echo "+ $*"; else sh -c "$*"; fi
}

udc() { ls /sys/class/udc 2>/dev/null | head -n 1; }

case "$cmd" in
  status)
    u="$(udc)"
    echo "udc: ${u:-none (this machine cannot be a USB device)}"
    if [ -d "$G" ]; then echo "gadget: $G bound to '$(cat "$G/UDC" 2>/dev/null)'"; else echo "gadget: not created"; fi
    ;;
  up)
    u="$(udc)"
    if [ -z "$u" ] && [ "$DRY" = 0 ]; then
      echo "usb-midi-gadget: no USB device controller (/sys/class/udc is empty); nothing to do" >&2
      exit 2
    fi
    run "modprobe libcomposite"
    run "mkdir -p $G/strings/0x409 $G/configs/c.1/strings/0x409 $G/functions/midi.usb0"
    run "echo 0x1d6b > $G/idVendor"     # Linux Foundation
    run "echo 0x0104 > $G/idProduct"    # Multifunction Composite Gadget
    run "echo 0x0100 > $G/bcdDevice"
    run "echo 0x0200 > $G/bcdUSB"
    run "echo op1-og-native > $G/strings/0x409/manufacturer"
    run "echo 'OP-1 OG (native port)' > $G/strings/0x409/product"
    run "echo 'MIDI' > $G/configs/c.1/strings/0x409/configuration"
    run "echo 100 > $G/configs/c.1/MaxPower"
    run "echo 1 > $G/functions/midi.usb0/in_ports"
    run "echo 1 > $G/functions/midi.usb0/out_ports"
    run "echo OP1Midi > $G/functions/midi.usb0/id"
    run "ln -sf $G/functions/midi.usb0 $G/configs/c.1/"
    run "echo ${u:-UDC} > $G/UDC"
    ;;
  down)
    [ -d "$G" ] || { echo "usb-midi-gadget: not created"; exit 0; }
    run "echo '' > $G/UDC || true"
    run "rm -f $G/configs/c.1/midi.usb0"
    run "rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/midi.usb0 $G/strings/0x409 $G"
    ;;
  *) sed -n '2,18p' "$0"; exit 64 ;;
esac
