# Installing dwn (for people who do not write code)

dwn is two pieces that work together:

- a **browser extension** — the button and the panel you click,
- a small **program on your computer** (the *host*) — it runs `yt-dlp` to fetch the
  formats and save the file.

The extension alone cannot download anything, so both must be installed. Expect about
ten minutes, most of it waiting.

## Before you start

- Linux only (the host is not built for Windows or macOS).
- The browser must be Brave, Chrome or Chromium.
- You will type commands into a terminal. If that sounds like too much, see
  [the last section](#if-you-cannot-use-a-terminal).

## Step 1 — open a terminal

| Desktop | How |
|---|---|
| KDE (Plasma) | Menu → **Terminal** |
| GNOME | `Ctrl`+`Alt`+`T` |
| XFCE | Menu → **Terminal Emulator** |
| Others | search for “Terminal” in your applications |

## Step 2 — install the build tools

`dwn` is not shipped pre-built, so your computer needs a compiler and a few helpers.
Copy the line for your distribution and press Enter. It will ask for your password.

**Arch / EndeavourOS / Manjaro**
```bash
sudo pacman -S --needed base-devel cmake git
```
**Fedora**
```bash
sudo dnf install gcc-c++ cmake git
```
**Ubuntu / Debian / Mint**
```bash
sudo apt install build-essential cmake git
```
**openSUSE**
```bash
sudo zypper install gcc-c++ cmake git
```

While you are there, add the two programs dwn drives:

```bash
# Arch / Fedora / openSUSE
sudo pacman -S yt-dlp ffmpeg      # or:  sudo dnf install yt-dlp ffmpeg
# Debian / Ubuntu / Mint
sudo apt install yt-dlp ffmpeg
```

`ffmpeg` is used to combine the video and audio tracks and to save audio as `.opus`
or `.aac`. If it is missing, everything except those two features still works.

## Step 3 — get dwn

```bash
git clone <the-url-you-were-given> dwn
cd dwn
```

If you were sent a zip or tarball instead, unpack it, then `cd` into the `dwn`
folder inside it. You can check you are in the right place with `ls` — you should
see a folder called `extension` and a file called `README.md`.

## Step 4 — install it

```bash
./scripts/setup.sh
```

One command that:

1. checks the tools from step 2 are present,
2. downloads a current `yt-dlp` if yours is older than two weeks,
3. builds the host program,
4. runs its tests,
5. registers the host with your browser.

It prints what it is doing as it goes. It ends with:

```
==> Done
    host: /home/you/dwn/build/dwn-host
```

**It never asks for administrator rights after step 2.** Everything it writes goes
into the `dwn` folder and your home directory.

> The tests in step 4 need to reach the internet the first time (they download a few
> small source files). That can take a minute on a slow connection. The whole script
> takes roughly 2–5 minutes depending on your processor.

## Step 5 — load the extension

1. Open your browser.
2. Go to `brave://extensions`, `chrome://extensions` or `edge://extensions`.
3. Turn on **Developer mode** (top right on Brave/Chrome).
4. Click **Load unpacked**.
5. Choose the `extension` folder inside your `dwn` folder — the one containing
   `manifest.json`, *not* the `dwn` folder itself.
6. You should see a card named **dwn** with a blue download icon. Pin it to the
   toolbar if you like.

## Step 6 — restart the browser

**Fully close it** (not just the window) and open it again. Your browser reads the
list of installed programs only at startup, so nothing works until you do this.

## Step 7 — use it

1. Open a YouTube video.
2. Click the dwn icon. (In Brave you can also open the side panel directly with
   `Ctrl`+`Shift`+`D`.)
3. Pick a quality, choose where to save, press **Download**.

The **Queue** tab shows what is happening, with a progress bar.

## If something goes wrong

**The panel says “host not found”**
You skipped step 6. Restart the browser, fully.

**The icon does nothing on a page**
The extension only attaches to video sites (YouTube, X, TikTok, Instagram, Reddit).
Nowhere else to download from.

**“Sign in to confirm you're not a bot”**
YouTube is asking for proof. Open a video in your browser first so it sets its
cookies, then fetch again.

**“Requested format is not available”**
`yt-dlp` is too old. Run `./scripts/setup.sh` again, or update it with your
package manager.

**The extension disappeared after a browser update**
Unpacked extensions are removed by some updates. Load it again (step 5). Your
downloads are not affected.

**Check what the host is doing**
```bash
./scripts/debug-host.sh          # logs everything, then look at /tmp/dwn-host.log
```

## If you cannot use a terminal

This installation is not ready for you yet, and pretending otherwise would waste your
afternoon. dwn currently has to be **compiled on the machine it runs on**, and there
is no pre-built package or single-file AppImage to download.

The fix is packaging work already sketched in `docs/portability.md`: an AppImage
(one file, no installation, no compiler) plus a `.deb`/`.rpm` for the common
distributions. It is a contained piece of work — the program already builds into a
single static binary — but until it exists, this guide is the honest limit.

## Updating later

```bash
cd dwn
git pull
./scripts/setup.sh        # rebuilds and re-registers
```

Then reload the extension (step 5, without “Load unpacked” — the *reload* button on
its card) and restart the browser.