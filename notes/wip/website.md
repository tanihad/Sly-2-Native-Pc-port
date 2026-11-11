# WIP: website (owner request 2026-11-11: a small Sly-themed site to advertise the port and offer the launcher)

**Status**: page built and checked locally (desktop 1360 px, phone 375 px); committed on main, **not pushed, not
published**. Waiting for the owner's OK on hosting (GitHub Pages from `main` / `docs`).

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

## Next
- Owner OK -> push, enable Pages (Settings > Pages > main /docs) -> check https://tanihad.github.io/sly2-decomp/ ->
  add the link to README.md and the repository's "About".
