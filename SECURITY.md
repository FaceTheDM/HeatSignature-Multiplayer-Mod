# Security

HS_Coop is a DLL that runs inside Heat Signature and talks to one other player over the network. This page explains
what it sends, what it trusts, and what it does not protect.

## Reporting a problem
Please don't open a public issue for a security problem. Use GitHub's **private vulnerability reporting** instead:
the "Security" tab of this repository, then "Report a vulnerability". This is a hobby project, so there's no promise
of a fix.

## What the mod does on your PC
- It runs only on Heat Signature patch 2021.1.20.1: it checks the game's SHA-256 and does nothing on any other version.
- **It never downloads or installs code.** There are no automatic updates. A new version only arrives when you
  download a zip and run Install.bat yourself.
- The installer downloads the Heat Signature Mod Loader from its author's GitHub release (v1.1.0) and refuses it unless
  both files match fixed SHA-256 checksums. It copies files only into the Heat Signature folder, and backs up anything
  it replaces. `Install.bat` runs `install.ps1` with PowerShell's execution policy bypassed for that one script; read it
  first if you like.
- The partner's game can send its galaxy save (a data file, not code) to the joining player. It is installed as a
  separate galaxy slot, and your own galaxy is kept.

## Connections
- **Steam lobby codes (the normal way):** the games talk through Steam's peer-to-peer networking, and no router port is
  opened. The mod doesn't show either player's IP address, but Steam may connect the two PCs directly, as it does for
  any peer-to-peer game.
- **IP-address codes (the fallback):** the code holds the host's addresses, a port and a random session key. Anyone
  holding the code can see the host's IP address, so share it only with your partner. The mod may ask your router to
  open UDP port 27015 (UPnP), and makes one request to a public STUN server (Google or Cloudflare) to learn your public
  address. Set `HS_COOP_NO_UPNP=1` or `HS_COOP_NO_STUN=1` to turn those off. A VPN such as Tailscale is the safer
  choice than forwarding a port.
- **Fixed setup (`coop.ini`):** the shared password (8 characters or more) is stored in plain text in `coop.ini` in
  the game folder. Don't reuse a password from anywhere else.
- Every packet is authenticated with HMAC-SHA256 using the session key or password, and packets that fail the check
  are dropped. Packets are **not encrypted**: game state isn't secret, but don't send anything private through the mod.
- Your partner's game is trusted with the shared game state (positions, items, guard hits, galaxy). Only play with
  people you trust.

## Logs and bug reports
`coop-logs` and `coop-reports` mask lobby codes and IP addresses (loopback stays, private network addresses keep their
first two parts, public ones become `<public-ip>`). Developer mode (`debug=1` in `coop.ini`) turns the masking off.
Screenshots in a report can still show a lobby code while you are hosting.

## Release zips
- Download releases only from this repository's Releases page.
- Windows SmartScreen warns about the zip because it isn't code-signed with a paid certificate.
