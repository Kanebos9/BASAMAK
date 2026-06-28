# Getting Windows + Linux builds from GitHub (step by step)

This is the no-extra-computer way to produce the Windows and Linux installers.
GitHub will compile all three operating systems for you in the cloud. Follow it
slowly; it's click-by-click on purpose.

Last time you hit a screen that said *"create a workflow"* — that just means the
little instructions file (the "workflow") wasn't in your uploaded project yet.
**Part B below fixes exactly that** by pasting it in directly on the website,
which is the most reliable way.

---

## Part A — Put the project on GitHub (once)

1. Make a free account at **github.com** and sign in.
2. Install **GitHub Desktop** (desktop.github.com) and sign in there too.
3. In GitHub Desktop: **File → Add Local Repository** → choose your
   `DrumSequencer` folder. If it offers to "create a repository", click
   **Create a repository** → **Create Repository**.
4. Click **Publish repository** (top bar). Untick *"Keep this code private"* only
   if you want it public; either is fine. Click **Publish Repository**.

Your code is now on github.com. Open your browser to
`https://github.com/<your-username>/DrumSequencer` to see it.

---

## Part B — Add the build workflow on the website (reliable)

1. On your repo's GitHub page, click the **Actions** tab (top, between
   *Pull requests* and *Projects*).
2. You'll see either a list of "suggested workflows" or a link **"set up a
   workflow yourself"**. Click **set up a workflow yourself**.
   *(If instead it shows your build already running — great, skip to Part C.)*
3. It opens a text editor with a file named `main.yml` full of example text.
   **Select all of that example text and delete it.**
4. **Paste in the whole block below** (everything between the lines):

----------------------------------------------------------------------
```yaml
name: Build BASAMAK
on:
  push:
  workflow_dispatch:
env:
  JUCE_TAG: 8.0.4
jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - { os: macos-latest,   name: macOS }
          - { os: windows-latest, name: Windows }
          - { os: ubuntu-latest,  name: Linux }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - name: Install Linux dependencies
        if: matrix.name == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y libasound2-dev libjack-jackd2-dev \
            libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev libx11-dev \
            libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev \
            libcurl4-openssl-dev libwebkit2gtk-4.1-dev ninja-build
      - name: Get JUCE
        uses: actions/checkout@v4
        with:
          repository: juce-framework/JUCE
          ref: ${{ env.JUCE_TAG }}
          path: JUCE
      - name: Configure
        run: cmake -B build -DDAVULSEQ_RELEASE=ON -DJUCE_DIR="${{ github.workspace }}/JUCE" -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --config Release --parallel
      - name: Collect (mac/linux)
        if: matrix.name != 'Windows'
        shell: bash
        run: |
          OUT="BASAMAK-${{ matrix.name }}"; mkdir -p "$OUT"
          find build -name "BASAMAK.vst3" -maxdepth 8 -exec cp -R {} "$OUT/" \;
          find build -name "BASAMAK.component" -maxdepth 8 -exec cp -R {} "$OUT/" \;
          cp Distribution/INSTALL.txt "$OUT/" 2>/dev/null || true
          if [ "${{ matrix.name }}" = "macOS" ]; then
            cp "Distribution/scripts/install-macos.command" "$OUT/Install BASAMAK.command"
          else
            cp "Distribution/scripts/install-linux.sh" "$OUT/"
          fi
      - name: Collect (windows)
        if: matrix.name == 'Windows'
        shell: pwsh
        run: |
          $out = "BASAMAK-Windows"; New-Item -ItemType Directory -Force $out | Out-Null
          $vst3 = Get-ChildItem -Recurse -Directory build -Filter BASAMAK.vst3 | Select-Object -First 1
          Copy-Item $vst3.FullName "$out\BASAMAK.vst3" -Recurse
          Copy-Item Distribution/scripts/install-windows.bat "$out\"
          Copy-Item Distribution/INSTALL.txt "$out\" -ErrorAction SilentlyContinue
      - name: Upload
        uses: actions/upload-artifact@v4
        with:
          name: BASAMAK-${{ matrix.name }}
          path: BASAMAK-${{ matrix.name }}
```
----------------------------------------------------------------------

5. Top-right, click the green **Commit changes...** button, then **Commit
   changes** again in the popup. (This saves the file into your repo at
   `.github/workflows/main.yml`.)
6. The moment you commit, the build **starts automatically**. Click the
   **Actions** tab again — you'll see a run called **"Build BASAMAK"** with a
   yellow dot (in progress).

> If you bring updated code later via GitHub Desktop, just click **Push** and a
> fresh build starts on its own — you only do Part B once.

---

## Part C — Download the Windows / Linux installers

1. **Actions** tab → click the latest **Build BASAMAK** run.
2. Wait until the three jobs (macOS, Windows, Linux) finish — green ticks.
   First run takes ~5–10 minutes.
3. Scroll to the bottom of that run page to the **Artifacts** section.
4. Download **BASAMAK-Windows** and **BASAMAK-Linux** (they download as `.zip`).
   Inside each is the plugin plus its `install-…` script — exactly like the Mac
   package. Hand those to your Windows/Linux users (see `INSTALL.txt`).

> **Samples note:** the cloud build does not include the factory sample library
> (it's large and not stored on GitHub), so the Windows/Linux packages install
> the plugin but not the starter samples. Users can drop their own audio into
> `Documents/BASAMAK/Samples`, or you can add the samples to those zips before
> sharing. (The macOS zip I built locally *does* include the samples.)

---

## If something looks wrong

- **Actions tab still says "create a workflow"** → the file didn't save. Redo
  Part B and make sure you click the green **Commit changes**.
- **A job has a red ✗** → click it, open the failed step to read the error. The
  most common cause is a typo when pasting the YAML — re-paste the whole block
  exactly.
- **Nothing happens after Commit** → go to Actions, click **Build BASAMAK** on
  the left, then the **Run workflow** button on the right (the
  `workflow_dispatch` line lets you start it by hand).
