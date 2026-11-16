#!/bin/sh
# UNTESTED draft. Makes the Cardputer Zero appear as a USB MIDI 1.0 device (f_midi) to a computer. Run as root.
# Needs the dwc2 controller in device role (see docs/USB-GADGET.md). Undo: run with "off".
set -e
G=/sys/kernel/config/usb_gadget/midiscope
if [ "$1" = off ]; then echo "" > $G/UDC; rm -f $G/configs/c.1/midi.usb0; rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/midi.usb0 $G/strings/0x409 $G; exit 0; fi
modprobe libcomposite; modprobe usb_f_midi
mkdir -p $G; cd $G
echo 0x1d6b > idVendor; echo 0x0104 > idProduct          # Linux Foundation, multifunction composite gadget
mkdir -p strings/0x409; echo "Cardputer Zero" > strings/0x409/product; echo "kurogedelic" > strings/0x409/manufacturer; echo 0001 > strings/0x409/serialnumber
mkdir -p functions/midi.usb0; echo f_midi > functions/midi.usb0/id; echo 1 > functions/midi.usb0/in_ports; echo 1 > functions/midi.usb0/out_ports
mkdir -p configs/c.1/strings/0x409; echo MIDI > configs/c.1/strings/0x409/configuration
ln -s functions/midi.usb0 configs/c.1/
ls /sys/class/udc | head -1 > UDC
