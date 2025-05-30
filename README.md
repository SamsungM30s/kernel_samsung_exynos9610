![Fresh Core icon](https://i.ibb.co/KzmZp3t/fresh-core.png)

# Fresh Core

#### Common Fresh Core Source for the Exynos 9610 Platform

Supports the following devices:
- Samsung Galaxy M30s

Looking for the Linux kernel readme? [Click here.](https://github.com/TenSeventy7/android_kernel_samsung_exynos9610_fresh/blob/staging/README_Kernel)

## Source References and Contributors

This will **never** be possible without the unwavering work of these awesome people. I have tried my best to keep their authorships on the commit history. Thank you very much!

 - [Cruel Kernel for the Galaxy S10/Note10](https://github.com/CruelKernel/samsung-exynos9820/) (@evdenis)
 - [ThunderStorms Kernel for the Galaxy S10/Note10](https://github.com/ThunderStorms21th/Galaxy-S10) (@ThunderStorms21th)
 - [Destrictize Project](https://github.com/DestrictizeProject/Destrictize_9611) (@DestrictizeProject)
 - [Quantum Kernel](https://github.com/prashantpaddune/android_kernel_samsung_a50dd) (@prashantpaddune)
 - [Custom Galaxy A51 Kernel](https://github.com/ianmacd/a51xx) (@ianmacd)
 

## About

This is an optimized kernel source based on Samsung's open-source drop of the SM-A505F based on Android 11. Additional features include:

 - Added additional I/O schedulers, `anxiety` is set as default.
 - Various kernel and performance improvements.
 - Backported some changes from S10/Note10.
 - Releases are compiled with [GCC 11.1](https://github.com/CruelKernel/aarch64-cruel-elf).
 - Replaced kernel RNG (HWRandom) with SRandom.
 - Magisk installed by default.
 - State notifier support for various kernel drivers.
 - Uses `FLATMEM` instead of `SPARSEMEM`.
 - Disabled basic Samsung hardening (Knox, etc) by default.
 - DriveDroid support
 - WireGuard support
 - Gentle Fair sleepers support
 - Support for storages formatted with NTFS

## How to Install

**The device must have an unlocked bootloader**, as well as TWRP, OrangeFox Recovery, or any recovery of your choice installed.

 1. Download latest available release from GitHub Releases.
 2. Copy the ZIP file to your SD card if necessary.
 3. Reboot to recovery.
 4. Flash downloaded ZIP.
 5. Reboot to **System**  instead of recovery.
 6. ???
 7. Profit

If you ever experience issues with lock screen and/or Samsung Account, see below.

## Device Locked Out?

If you ever experience being locked out after installing any build, with SystemUI restarting after entering your password/PIN, and Samsung Account showing a `Samsung Account logged out` notification, you may have been experiencing the "Pin Problem".

Several builds of TWRP and OrangeFox has a security patch level (SPL) of 2099-12 (December 2099), but this kernel follows the latest SPL available to the device. This causes the device to act up once booted into the kernel.

Please note that it is **intended behavior** by the system and is **not** a bug.

You can flash PassReset, or wipe your device to fix it.

See [here](https://github.com/CruelKernel/samsung-exynos9820/#pin-problem-cant-login) for more information.

 
## Building Locally

Local builds of Fresh Core are built using **Ubuntu 20.04 LTS**.  These prerequisites are needed to build with this source:

 - build-essential
 - libelf-dev
 - kernel-package
 - bzip2
 - lib32stdc++6
 - libc6-dev-i386
 - git

Once you have the prerequisites installed, simply run this on the Terminal.

`./build.sh (device) [dirty]`

**Device options:**

```
- a50 # For Samsung Galaxy A50
- a50s # For Samsung Galaxy A50s
```

You can add the 'dirty' flag so it won't rebuild all components again. The script will download all it needs (including the toolchain) and builds a new kernel build for you.
