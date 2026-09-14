# CODM Tutorial Skip (Zygisk / KernelSU)

Target package: `com.garena.game.codm`
ABI: `arm64-v8a`

## Build
Push repo ke GitHub, lalu buka **Actions > Build Module > Run workflow**.
Artifact: `CODM-Tutorial-Skip.zip`

## Install
Butuh KernelSU + ZygiskNext atau implementasi Zygisk kompatibel.
Install ZIP dari manager, reboot, lalu buka CODM.

## Log
```sh
su -c 'logcat -s CODM-TutorialSkip'
```

RVA di `jni/main.cpp` harus sesuai versi game/library yang sedang dipakai.
Default library: `libunity.so`.
# CODM-Tutorial-Skip
