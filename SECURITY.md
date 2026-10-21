# Security Policy

Angels95 / AngelTech Engine is a hobby-grade multiplayer game project.
Security issues are still taken seriously — especially in the dedicated
server (`AngelServ`), which accepts untrusted network traffic.

## Supported Versions

Only the latest tagged release (`b*` tags) is supported for security fixes.

| Version | Supported          |
|---------|--------------------|
| Latest `b*` tag   | :white_check_mark: |
| Older `b*` tags   | :x:                |

## Reporting a Vulnerability

**Please do not open a public issue for a security vulnerability.**

To report privately:

1. Go to **https://github.com/TribeWarez/Angels-95/security** and
   click **Report a vulnerability**, or
2. Open a GitHub issue with a `[SECURITY]` prefix and *request it be made
   private* if you are unable to use the Security tab.

Include in your report:

- The affected version (commit hash or tag).
- A minimal description of the vulnerability (type, impact, affected
  component — server, client, editor, packer).
- Reproduction steps or a Proof of Concept where possible.

You will receive an acknowledgment within **3 business days**. We will work
on a fix before publicly disclosing the issue.

## Scope

In scope: `AngelServ` (UDP/hTTP/XML handling, GameState), client/server
packet parsing, `OzoneParser` (malformed map files), `OzPack`.

Out of scope: modified binaries, third-party dependencies (raylib, raygui,
pl_mpeg), and locally-invented or deliberately malformed content used
offline in single-player/editor contexts.

## Safe handling notes

- The server binds game data from `GameData/`; only serve trusted worlds.
- Save files (`*.sav`) and formats parsed by `OzoneParser` are
  treated as untrusted input where possible — report any crash/corruption
  triggered by malformed files as a vulnerability.