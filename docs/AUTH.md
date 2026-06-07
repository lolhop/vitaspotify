# Create your `auth.json` (Spotify login for VitaSpotify)

VitaSpotify does **not** use your Spotify password on the Vita. You create a small file on your **computer**, copy it to the console, then tap **Log in (auth.json)** in the app.

**You need:** Spotify **Premium**, a jailbroken Vita, **iTLS-Enso**, and a computer on the **same Wi‑Fi** as your phone and Vita.

---

## For most users (no coding, no git)

Download the release bundle. You should have:

| File | What it is |
|------|------------|
| `vitaspotify.vpk` | Install on the Vita |
| `VitaSpotify-auth-helper.zip` | Creates `auth.json` on your computer |

Unzip **VitaSpotify-auth-helper.zip** and open **README.txt** inside.

### Quick steps

| OS | What to run |
|----|-------------|
| **Mac** | Double-click `mac/Get VitaSpotify Login.command` |
| **Linux** | Run `linux/get-vitaspotify-login.sh` |
| **Windows** | Double-click `windows/Get VitaSpotify Login.bat` (uses `vitaspotify-auth-helper.exe` in the same folder) |

Then in the **Spotify app** (phone or desktop, same Wi‑Fi):

1. Play any track.
2. Tap **Connect to a device** (speaker icon).
3. Choose **`CSpot player`**.
4. Wait until the helper says **SUCCESS**.

Your login file is saved as **`vitaspotify auth.json`** on your Desktop (or home folder on Linux).

### Copy to the Vita

1. VitaShell → enable **USB** or **FTP**.
2. Create folder: `ux0:data/vitaspotify/`
3. Copy the file there as **`auth.json`** (exact name).
4. VitaSpotify → **Log in (auth.json)**.

---

## Checklist

| Step | Action |
|------|--------|
| 1 | Install **iTLS-Enso** on the Vita |
| 2 | Install **vitaspotify.vpk** |
| 3 | Unzip **VitaSpotify-auth-helper.zip** on your PC |
| 4 | Run the helper for your OS (see table above) |
| 5 | Spotify → Connect → **CSpot player** |
| 6 | Copy `auth.json` → `ux0:data/vitaspotify/auth.json` |
| 7 | Vita → **Log in (auth.json)** |

---

## Troubleshooting

| Problem | What to try |
|---------|-------------|
| **CSpot player** never appears | Same Wi‑Fi; disable VPN; allow through firewall; run helper again |
| “Helper program not found” | Use the full zip — `vitaspotify-auth-helper.exe` must sit next to the `.bat` (see **WHY-A-SMALL-HELPER.txt**) |
| Mac won’t open `.command` file | Right-click → **Open** → **Open** again |
| Login fails on Vita | Path must be `ux0:data/vitaspotify/auth.json`; check **iTLS-Enso** |
| **Bad auth.json** | Generate a new file; don’t edit by hand |

**Keep `auth.json` private** — it grants access to your Spotify account.

---

## Offline without login

**Play offline** works without `auth.json` if you have downloaded tracks. Online streaming needs the file.

---

## Developers (building from source)

If you have the git repo, you can still use:

```bash
./scripts/pack-auth-helper.sh   # creates build/VitaSpotify-auth-helper.zip
```

Or the in-repo helper:

```bash
./scripts/get-auth-json.sh
```

That builds `cspotcli` locally — **not** required for end users who use the release zip.
