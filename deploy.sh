#!/usr/bin/env bash
# Deploy thebitcointerminal.com — run after `pio run` cuts a release.
#
# Server layout: this repo is cloned at $REMOTE_DIR on $HOST with Apache's
# docroot pointed at $REMOTE_DIR/site. The site and flasher update via
# `git pull` (site/flash is a symlink to ../webflash); the firmware builds
# are gitignored, so they only reach the server via the rsync below.
set -euo pipefail
cd "$(dirname "$0")"

HOST=lightningpay-web
REMOTE_DIR=/var/www/thebitcointerminal.com/Bitcoin-ESP32-POS-Terminal

if [ -n "$(git status --porcelain)" ]; then
  echo "WARNING: uncommitted local changes — the server only receives what is pushed." >&2
fi

echo "== push local commits =="
git push origin master

echo "== update site + flasher on $HOST =="
ssh "$HOST" "set -e; cd '$REMOTE_DIR'; git pull --ff-only; \
  [ -e site/flash ] || ln -s ../webflash site/flash"

echo "== sync firmware builds + manifest =="
rsync -av --delete firmware/ "$HOST:$REMOTE_DIR/site/firmware/"

echo
echo "== verify live =="
for u in "" flash/ firmware/manifest.json; do
  printf "  https://thebitcointerminal.com/%s => " "$u"
  curl -s -o /dev/null -w "%{http_code}\n" --max-time 15 "https://thebitcointerminal.com/$u"
done
echo "Latest in manifest:"
curl -s --max-time 15 https://thebitcointerminal.com/firmware/manifest.json \
  | python3 -c "import json,sys; b=json.load(sys.stdin)['builds'][0]; print('  v%s @%s (%s)' % (b['version'], b['commit'], b['date']))" \
  || echo "  (could not read manifest)"
