# N.O.V.A. Legacy Windows port

Runs the Android game N.O.V.A. Legacy (5.8.4a, ARM64) natively on Windows, offline, without an emulator.

No game files are included. You need your own copy of the APK.

## How it works

The game's native library, `libNOVA.so`, is loaded unmodified into a Windows process.

- **CPU:** ARM64 code is translated to x64 with [dynarmic](https://github.com/azahar-emu/dynarmic), with a small interpreter in front for code that runs only a few times.
- **Android:** everything the library imports is replaced with host code: libc, pthreads, JNI and the Java classes the game calls, assets and file access.
- **Graphics:** OpenGL ES calls go to ANGLE, which draws with Direct3D.
- **Audio and input:** OpenSL ES is mapped to SDL2. Keyboard and mouse are turned into touches on the game's on-screen controls.
- **Online:** the game's servers are gone, so network calls fail and the game runs offline.

## How to run it yourself

You need Windows 10/11 x64, Visual Studio 2022 Community with the C++ workload, Git and Python 3.

1. Clone this repository and put your N.O.V.A. Legacy 5.8.4a APK in its root folder.
2. Unpack the APK:
   ```
   python tools\unpack.py
   ```
3. Fetch the libraries and build:
   ```
   powershell -ExecutionPolicy Bypass -File port\setup.ps1
   port\build.bat
   ```
4. Start the game:
   ```
   run.bat
   ```

The first start takes longer while the game extracts its data. `Tab` switches between the menu mouse and in-game controls. Control positions, offline rewards and reward packs can be changed in `controls.cfg`, `offline.cfg` and `packs.cfg`.
