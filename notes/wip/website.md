# WIP: website (owner request 2026-11-11: a small Sly-themed site to advertise the port and offer the launcher)

**Status**: published 2026-11-11 (owner's OK): pushed to main, GitHub Pages enabled (Settings > Pages, branch `main`,
folder `/docs`) -> https://tanihad.github.io/Sly-2-Native-Pc-port/ . README links to it.

**Repository renamed**: GitHub now reports `tanihad/Sly-2-Native-Pc-port` (the old `tanihad/sly2-decomp` URLs redirect,
pushes included). The page's links use the new name. The launcher's update check, CLAUDE.md and the local remote still
use the old name and work through the redirect, as long as nobody creates a new repository called `sly2-decomp`.

## What there is
- `docs/index.html`: one self-contained page (inline CSS, SVG, a little JS; fonts from Google Fonts: Bangers, Barlow,
  Barlow Condensed). `docs/.nojekyll` so GitHub Pages serves the folder as it is.
- Sections: hero (download button -> `releases/latest/download/Sly2-Setup.exe`), The gear (features), Pick a disguise
  (Retro / Faithful / Reimagined), The plan (install steps), Tools of the trade (requirements), questions, legal footer.
- The version and size next to the button come from the GitHub API (`releases/latest`) when the page loads; the text
  in the page is the fallback.
- Recon photos: the section stays hidden until pictures named `docs/shots/1.jpg` ... `6.jpg` exist (the owner's own
  screenshots; 16:9).
- Preview: `.claude/launch.json` "website" (python http.server on 127.0.0.1:8377, folder docs; not committed).

## Decisions
- Sly themed with artwork drawn for the site (Paris rooftops, moon, blue job beacons, tightrope with sparkles, the
  cane, a calling card, comic lettering, heist wording). No official logo, character art or game assets in the page:
  those belong to Sony / Sucker Punch, and a public advertising page is where a takedown would start.
- Facts on the page come from README.md, notes/install.md, notes/controls.md and the 1.1 release notes.

## Next (optional, owner's call)
- Done 2026-11-11: the repository's "About" box links to the site and describes the PC port.
- Screenshots for the Recon photos section (`docs/shots/1.jpg` ...).
