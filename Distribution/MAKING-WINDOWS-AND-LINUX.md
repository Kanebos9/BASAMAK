# Making the Windows and Linux versions (plain-English guide)

**Short version:** the macOS version is finished (`BASAMAK-macOS.zip`). A plugin
can only be *compiled* on the same kind of computer it runs on, so the Windows
and Linux versions have to be built on a Windows / Linux machine. There is **no
shortcut on the Mac** — this is normal, every plugin company does it this way.

You have two ways to do it. Pick ONE.

---

## Way 1 — Let GitHub build them for you (no Windows PC needed) ⭐ recommended

This also gives other people a place to see and change the code, which is your
other goal. You don't need to know any commands.

1. Make a free account at **github.com**.
2. Install the free app **GitHub Desktop** (desktop.github.com).
3. In GitHub Desktop: **File → Add Local Repository →** choose your
   `DrumSequencer` folder. If it asks to "create a repository here", say yes.
4. Click **Publish repository** (you can keep it Private if you like).
5. Wait a few minutes. On the github.com page for your project, click the
   **Actions** tab. You'll see a build running for **macOS, Windows and Linux**.
6. When it finishes (green check), click that run and scroll to **Artifacts**.
   Download **BASAMAK-Windows** and **BASAMAK-Linux** — those are the installers
   for those systems.

Every time you change the code and click **Push** in GitHub Desktop, it rebuilds
all three automatically. Anyone you share the project link with can download it
or suggest changes.

---

## Way 2 — Build on a Windows PC yourself

Only if you have (or can borrow) a Windows computer.

1. Copy your whole `DrumSequencer` project folder onto the Windows PC (USB
   stick, cloud drive, whatever).
2. Install three free tools on that PC:
   - **Visual Studio 2022 Community** (during setup tick *"Desktop development
     with C++"*) — visualstudio.microsoft.com
   - **CMake** — cmake.org/download (tick *"Add CMake to PATH"* during install)
   - **Git** — git-scm.com/download/win
3. Open the folder, go into `Distribution\scripts`, and **double-click
   `build-windows.bat`**. It downloads JUCE and compiles (takes a few minutes).
4. Then **double-click `install-windows.bat`** to install it into that PC's DAW.

For Linux it's the same idea with `build-linux.sh` then `install-linux.sh`
(the script header lists the packages to install first).

---

## Sharing the code with other developers

Two options:

- **Easiest to edit together:** put it on GitHub (Way 1 above) and share the
  link. People can view it, download it, or "fork" it to make their own changes.
- **Just hand them the files:** give them **`Distribution/BASAMAK-Source.zip`**.
  They unzip it, open the folder in **VS Code**, and read `README.md` and
  `docs/ARCHITECTURE.md` to understand how everything works. The project already
  includes the VS Code setup (in the hidden `.vscode` folder).

> **Can't see the `.vscode` folder in Finder?** It's hidden because its name
> starts with a dot. In Finder press **Command + Shift + . (period)** to show
> hidden files (press again to hide them). It's there — it just doesn't show by
> default. (Inside VS Code it's always visible.)
