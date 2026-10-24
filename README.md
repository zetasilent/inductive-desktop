# Inductive desktop app

Inductive is a PCB design app built on [KiCad](https://www.kicad.org) 10.0.7, with a
design assistant beside the schematic and board editors. This repository is the source
of the desktop app: the changes it makes to KiCad and the editor-side code compiled
into it. Like KiCad, it is licensed under the GNU General Public License, version 3
or (at your option) any later version.

## What is here

| Path | What it is |
|---|---|
| `patches/` | The changes to KiCad 10.0.7, applied in order |
| `src/` | The sidebar, Home, sign-in, updates and the editor tools, compiled into KiCad |
| `web/` | The sidebar and Home pages |
| `examples/circuits/` | The reference circuits the app ships |
| `tools/` | Build, install and packaging scripts (macOS) |
| `brand/` | The app icon (see NOTICE: the name and logo are trademarks) |

## What is not here

The design assistant, board layout (placement, routing, pours and board setup) and
parts sourcing run on Inductive's servers and need an Inductive account. They are
separate programs, not part of this app, and are not in this repository. The app
talks to them over HTTPS.

## Building on macOS

1. Get KiCad 10.0.7: `git clone --branch 10.0.7 https://gitlab.com/kicad/code/kicad.git`
2. Apply the patches, in order: `for p in patches/*.patch; do git -C kicad apply "$p"; done`
3. Link this repository into it: `tools/install-into-kicad.sh /path/to/kicad`
4. Build KiCad as its documentation describes (on macOS with
   [kicad-mac-builder](https://gitlab.com/kicad/packaging/kicad-mac-builder)), then
   install the app with `tools/install-app.sh`.

The app reads its server settings from `~/.config/inductive/env`
(`INDUCTIVE_API_BASE`, `INDUCTIVE_GCP_PROJECT_ID`, `INDUCTIVE_FIREBASE_API_KEY`,
`INDUCTIVE_FIREBASE_AUTH_DOMAIN`). The UI uses the Satoshi typeface from
[Fontshare](https://www.fontshare.com/fonts/satoshi), which is not included: put
`Satoshi-Regular.woff2`, `Satoshi-Medium.woff2` and `Satoshi-Bold.woff2` in `web/fonts/`,
or the pages fall back to the system font.

## License

GPL-3.0-or-later; see `LICENSE`. KiCad's own licensing is described in its
[LICENSE.README](https://gitlab.com/kicad/code/kicad/-/blob/10.0.7/LICENSE.README).
