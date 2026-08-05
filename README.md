# Arduino USB Driver

Linux USB bulk endpoint character driver for Arduino-compatible boards or custom
firmware that exposes one bulk-in and one bulk-out endpoint.

## Build

```sh
make
```

The build requires kernel headers matching the running kernel.

## Configure Device IDs

Check your board:

```sh
lsusb
```

Then update `USB_ARD_VENDOR_ID` and `USB_ARD_PRODUCT_ID` in `arduino_usb.c`.
The default is `2341:0043`, a common Arduino Uno R3 ID.

Many Arduino boards use existing serial drivers such as `cdc_acm`, `ch341`,
`cp210x`, or `ftdi_sio`. This driver is only useful when the selected USB
interface has both bulk-in and bulk-out endpoints.

## Load

```sh
sudo insmod arduino_usb.ko
dmesg | tail
ls -l /dev/ard*
```

Unload:

```sh
sudo rmmod arduino_usb
```

## Test

```sh
echo hello | sudo tee /dev/ard192
sudo dd if=/dev/ard192 bs=64 count=1 status=none
```

If another kernel driver already owns the interface, unbind that driver first
or add the relevant blacklist rule for your test environment.
