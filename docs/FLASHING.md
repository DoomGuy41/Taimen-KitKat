# Flashing Taimen KitKat on a stock Pixel 2 XL

This guide starts from a Pixel 2 XL running its stock Android and ends with KitKat booted.
Expect about 30 minutes.

> **Everything on the phone is erased** (apps, photos, internal storage), several times along
> the way. Back up first.

## What you need

- A **Pixel 2 XL** (`taimen`) whose bootloader can be unlocked. Units sold by Verizon usually
  have *OEM unlocking* greyed out and cannot run this.
- A USB cable and a computer (Linux, macOS or Windows).
- The latest **Android platform-tools** (adb and fastboot):
  <https://developer.android.com/tools/releases/platform-tools>. On Windows, also install the
  [Google USB driver](https://developer.android.com/studio/run/win-usb).
- The battery above 50 %.
- Downloads:
  - Google's factory image **RP1A.201005.004.A1** for taimen, 1.8 GB:
    <https://dl.google.com/dl/android/aosp/taimen-rp1a.201005.004.a1-factory-2f5c4987.zip>
    (also listed on <https://developers.google.com/android/images#taimen>). SHA-256:
    `2f5c4987a8e1c29bb2843d7f7d36721d52f2eb25a0fe4c74b3590e34a06eb44f`
  - From the [Taimen KitKat release](https://github.com/DoomGuy41/Taimen-KitKat/releases):
    `Taimen-KitKat-<version>-fastboot.zip`.

Open a terminal (Windows: Command Prompt) in the folder where platform-tools are, or add them
to your `PATH`. Check with `fastboot --version`.

## 1. Unlock the bootloader

1. On the phone, open **Settings > About phone** and tap **Build number** seven times.
2. In **Settings > System > Advanced > Developer options**, turn on **OEM unlocking** and
   **USB debugging**.
3. Connect the phone, accept the USB debugging prompt, then reboot to the bootloader:
   ```sh
   adb reboot bootloader
   ```
   (Or power off, then hold **Power + Volume down**.)
4. Unlock the bootloader and the critical partitions. Each command asks for confirmation on the
   phone: select it with the volume keys and press Power. The phone is wiped.
   ```sh
   fastboot flashing unlock
   fastboot flashing unlock_critical
   ```
   `unlock_critical` is required on the Pixel 2 to flash the bootloader in the next step.

From now on, every boot shows a warning that the software cannot be checked. That is normal
with an unlocked bootloader; it goes away after a few seconds.

## 2. Flash the Android 11 factory image on slot A

KitKat reuses the kernel, bootloader, radio (modem firmware) and DTBO of this exact build. The
system image is hardwired to the partitions of **slot A**, so the factory image must go there.

1. Unzip `taimen-rp1a.201005.004.a1-factory-2f5c4987.zip` and open the
   `taimen-rp1a.201005.004.a1` folder in the terminal.
2. With the phone in the bootloader:
   ```sh
   fastboot --set-active=a
   ```
3. Run the factory script, `./flash-all.sh` on Linux/macOS or `flash-all.bat` on Windows. It
   flashes the bootloader (TMZ30m), the radio and Android 11, then reboots. Let Android 11 boot
   once: you can skip its setup.
4. Back to the bootloader: power off, then hold **Power + Volume down** (or `adb reboot
   bootloader` if USB debugging is on). Check:
   ```sh
   fastboot getvar current-slot          # a
   fastboot getvar version-bootloader    # TMZ30m
   fastboot getvar version-baseband      # g8998-00034-2006052136
   ```

## 3. Flash KitKat

1. Unzip `Taimen-KitKat-<version>-fastboot.zip` and open the folder in the terminal.
2. With the phone in the bootloader, run `./flash-all.sh` on Linux/macOS or `flash-all.bat` on
   Windows.

The script checks the phone (model, unlocked, bootloader and radio versions) and the image
checksums, then:
1. makes slot A active;
2. flashes `boot_a` and `system_a`;
3. formats userdata;
4. reboots.

Doing it by hand is the same:

```sh
fastboot --set-active=a
fastboot flash boot_a boot.img
fastboot flash system_a system.img
fastboot format:ext4 userdata
fastboot reboot
```

## 4. First boot

The Pixel boot animation plays for about two minutes on the first boot, while KitKat optimizes
the apps. Then the setup wizard starts: choose a language, connect to wifi and sign in to your
Google account (or skip).

Tips:
- **adb.** USB is in MTP mode. For adb, enable **Developer options** (tap *Build number* seven
  times) and **USB debugging**.
- **GPS.** The first fix is much faster once wifi or mobile data is connected (XTRA orbit data
  is downloaded automatically) and outdoors.
- **Baseband version.** *About phone* shows the radio version (`g8998-00034-2006052136`) and
  the stock 4.4 kernel.

## Going back to stock Android

Flash the factory image again (step 2: `flash-all.sh` of RP1A.201005.004.A1). This wipes the
phone. Only once stock Android boots, you may relock the bootloader with `fastboot flashing
lock` (this wipes again). **Never relock while KitKat is installed**: the phone would refuse
to boot.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `flash-all` complains about the bootloader or radio | Redo step 2, making sure slot A is active before running the factory `flash-all`. |
| The phone boots Android 11 instead of KitKat | `fastboot --set-active=a`, then `fastboot reboot`. |
| Boot animation for more than 5 minutes | Format userdata again (`fastboot format:ext4 userdata`) and reboot. |
| `fastboot` does not see the phone (Windows) | Install the Google USB driver, then try another USB port or cable. |
| `FAILED (remote: 'Flashing is not allowed for Critical Partitions')` | Run `fastboot flashing unlock_critical` (step 1). |
