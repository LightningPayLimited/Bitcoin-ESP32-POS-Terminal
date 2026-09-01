#!/usr/bin/env bash
# Assemble the thebitcointerminal.com site into dist/ ready for any static host:
#
#   dist/index.html        <- site/index.html        (marketing site)
#   dist/flash/index.html  <- webflash/index.html    (Web Serial flasher)
#   dist/firmware/         <- firmware/              (builds + manifest.json)
#
# Upload dist/ to the host (e.g. `npx wrangler pages deploy dist` for
# Cloudflare Pages, or drag it into the dashboard). Re-run after each
# `pio run` release so the new build and manifest go up too.
set -euo pipefail
cd "$(dirname "$0")"

rm -rf dist
mkdir -p dist/flash
cp site/index.html dist/index.html
cp webflash/index.html dist/flash/index.html
cp -r firmware dist/firmware

echo "dist/ ready:"
find dist -type f | sort | sed 's/^/  /'
