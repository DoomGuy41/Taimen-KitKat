@ECHO OFF
:: Taimen KitKat: flashes boot.img and system.img to slot A of a Pixel 2 XL in fastboot mode and
:: formats userdata. Prerequisites: unlocked bootloader and the factory image RP1A.201005.004.A1
:: flashed on slot A (see FLASHING.md). Erases all user data.
PATH=%PATH%;"%SYSTEMROOT%\System32"
cd /d "%~dp0"
fastboot getvar product 2>&1 | findstr /r /c:"^product: *taimen" >NUL || (echo ERROR: no Pixel 2 XL ^(taimen^) in fastboot mode & exit /b 1)
fastboot getvar version-bootloader 2>&1 | findstr /c:"TMZ30m" >NUL || (echo ERROR: bootloader TMZ30m required: flash RP1A.201005.004.A1 first & exit /b 1)
fastboot getvar version-baseband 2>&1 | findstr /c:"g8998-00034-2006052136" >NUL || (echo ERROR: radio g8998-00034-2006052136 required: flash RP1A.201005.004.A1 first & exit /b 1)
echo This erases all data on the phone. Press a key to continue, close the window to cancel.
pause >NUL
fastboot --set-active=a || exit /b 1
fastboot flash boot_a boot.img || exit /b 1
fastboot flash system_a system.img || exit /b 1
fastboot format:ext4 userdata || exit /b 1
fastboot reboot
echo Done. The first boot takes about 2 minutes.
pause
