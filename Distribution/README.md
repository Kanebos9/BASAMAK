# BASAMAK — Distribution

How the installable packages are produced for each OS. End-user install steps
are in **[INSTALL.txt](INSTALL.txt)**.

## What's here

```
Distribution/
  INSTALL.txt              end-user install instructions (all 3 OSes)
  scripts/
    install-macos.command / install-windows.bat / install-linux.sh
    build-macos.sh        / build-windows.bat    / build-linux.sh
    package-macos.sh      builds a clean release + assembles the macOS zip
  BASAMAK-macOS.zip       <- current, ready-to-ship macOS installer (v1.0.0)
```

Each install package contains: the plugin binary, the factory **Samples/**
folder, the double-click installer for that OS, `INSTALL.txt`, and a `VERSION`
file. Installing seeds the samples into `Documents/BASAMAK/Samples` and never
touches the user's existing library.

## Building each platform's package

A plugin binary can only be built **on its own OS** — you cannot cross-compile
the Windows or Linux build from a Mac. Two ways to get all three:

### Option A — GitHub Actions (recommended)

Push the repo to GitHub. [`.github/workflows/build.yml`](../.github/workflows/build.yml)
builds macOS, Windows and Linux on every push and uploads installable artifacts
(`BASAMAK-macOS`, `BASAMAK-Windows`, `BASAMAK-Linux`). Tag a commit `v1.0.0`
to also draft a release. This is the easiest way to keep all three platforms in
sync with the latest code.

### Option B — build on each machine

| OS | Build | Then package |
|----|-------|--------------|
| macOS | `bash Distribution/scripts/package-macos.sh` | produces `BASAMAK-macOS.zip` |
| Windows | `Distribution\scripts\build-windows.bat` | zip the `BASAMAK.vst3` together with `install-windows.bat` + `INSTALL.txt` + a `Samples/` folder |
| Linux | `bash Distribution/scripts/build-linux.sh` | zip the `BASAMAK.vst3` together with `install-linux.sh` + `INSTALL.txt` + a `Samples/` folder |

> The factory **Samples** library is not committed to the repo (≈37 MB).
> `package-macos.sh` pulls it from `~/Documents/DrumSequencer Samples` by default
> (override with `SAMPLES_SRC=/path`). For CI to bundle samples, add them to the
> repo (e.g. a `factory-samples/` folder, ideally via Git LFS) and copy them in
> the workflow's "Stage package" step.

## Versioning & updates

The version comes from `project(DrumSequencer VERSION x.y.z)` in `CMakeLists.txt`
(shown next to the `BASAMAK` logo). To ship an update: bump that version,
rebuild/repackage, and ship the new zips. Because the user's library lives in
`Documents/BASAMAK` — **separate from the plugin binary** — installing a new
version simply replaces the binary and leaves all samples, sounds and
presets intact. The installers report the version they replace.
