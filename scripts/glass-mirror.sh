#!/bin/bash
# glass-mirror.sh: this desktop's Claude Code session as readable text, for
# a console inside the Glass screen (scripts/glass-screen.sh): the newest
# session transcript (~/.claude/projects/<dir>/*.jsonl) followed live, the
# assistant's replies printed as they land, the reply delimiters and rule
# lines dropped, one blank line between replies. Nothing is sent anywhere.
#   glass-mirror.sh [TRANSCRIPT.jsonl]     default: the newest in the project
#   scripts/glass-screen.sh run konsole -e scripts/glass-mirror.sh
set -u
dir=$HOME/.claude/projects/-home-lasimeri
f=${1:-}
if [ -z "$f" ]; then
    f=$(ls -t "$dir"/*.jsonl 2> /dev/null | head -n 1)
fi
[ -n "$f" ] && [ -f "$f" ] || { echo "glass-mirror: no transcript in $dir" >&2; exit 1; }
command -v jq > /dev/null || { echo "glass-mirror: jq is not installed" >&2; exit 1; }
printf '\033]30;claude on the glasses\007'
echo "glass-mirror: following $(basename "$f")"
# The last reply first (the tail of the file), then everything new.
tail -n 200 -f "$f" |
    jq --unbuffered -r 'select(.type == "assistant") | .message.content[]? | select(.type == "text") | .text, "\u0000"' |
    sed -u -e '/^<•••>$/d' -e '/^<\/•••>$/d' -e '/^---$/d' |
    awk '{ if ($0 == "\0") { print ""; fflush(); next } print; fflush() }'
