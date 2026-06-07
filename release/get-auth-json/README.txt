================================================================================
  VitaSpotify — create your auth.json (Spotify login file)
================================================================================

You do NOT need to download the VitaSpotify source code or use git.
This folder is all you need on your computer.

Each folder (mac / linux / windows) includes a small helper program next to
the script you run. Nothing else to install. Read WHY-A-SMALL-HELPER.txt if
you wonder why a plain .bat is not enough by itself.

WHAT YOU NEED
  • Spotify Premium
  • Phone or computer with the Spotify app
  • Same Wi-Fi for your computer and phone (and your PS Vita)
  • The PS Vita will get the file later via USB or FTP (VitaShell)

  NOTE: The Mac helper is built for Apple Silicon (M1/M2/M3 Macs). If you have
  an older Intel Mac, ask for an Intel build or use the Linux steps in WSL.

--------------------------------------------------------------------------------
  MAC
--------------------------------------------------------------------------------

1. Open the "mac" folder.
2. Double-click:  Get VitaSpotify Login.command
   • If macOS says it cannot be opened: right-click the file → Open → Open again.
3. A Terminal window opens with step-by-step text. Leave it open.
4. On your phone (or another computer), open Spotify.
5. Play any song → tap the speaker / "Connect to a device" icon.
6. Pick **CSpot player** and log in if Spotify asks.
7. When the Terminal shows success, press any key to close it.
8. On your Desktop you will find:  vitaspotify auth.json

--------------------------------------------------------------------------------
  LINUX
--------------------------------------------------------------------------------

1. Open a terminal in the "linux" folder.
2. Run:
     chmod +x get-vitaspotify-login.sh
     ./get-vitaspotify-login.sh
3. Follow the on-screen steps (same Spotify Connect flow as Mac).
4. Output file:  ~/Desktop/vitaspotify auth.json
   (or ~/vitaspotify auth.json if you have no Desktop folder)

--------------------------------------------------------------------------------
  WINDOWS
--------------------------------------------------------------------------------

1. Open the "windows" folder.
2. Double-click:  Get VitaSpotify Login.bat
3. Follow the on-screen steps (same Spotify Connect flow as Mac).
4. Output file on your Desktop:  vitaspotify auth.json

--------------------------------------------------------------------------------
  COPY THE FILE TO YOUR PS VITA
--------------------------------------------------------------------------------

1. On the Vita, open VitaShell.
2. Enable USB or FTP (Start → Settings).
3. Connect from your computer.
4. Create this folder if it does not exist:
     ux0:data/vitaspotify/
5. Copy your file there and rename it to exactly:
     auth.json
   Full path on Vita:  ux0:data/vitaspotify/auth.json
6. Open VitaSpotify on the Vita → tap **Log in (auth.json)**.

--------------------------------------------------------------------------------
  TROUBLESHOOTING
--------------------------------------------------------------------------------

  "CSpot player" never appears in Spotify
    → Same Wi-Fi; turn off VPN; allow the app through your firewall; try again.

  "Helper program not found" (Windows / Mac / Linux)
    → You need the FULL zip, not just the .bat or .sh alone. The zip must
      contain vitaspotify-auth-helper.exe (Windows) in the same folder as the
      script. Re-download from whoever shared VitaSpotify.

  Login fails on the Vita
    → Check the path is ux0:data/vitaspotify/auth.json (inside vitaspotify folder).
    → Install iTLS-Enso on the Vita for HTTPS.

  Keep auth.json private — do not post it online. It grants access to your account.

================================================================================
