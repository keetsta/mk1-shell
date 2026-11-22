#!/usr/bin/env bash
set -euo pipefail

version='^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
tag=${1-}
if [[ ! $tag =~ $version ]]; then
    echo 'expected a firmware tag such as v0.1.2' >&2
    exit 1
fi

commit=$(git rev-parse --verify "refs/tags/$tag^{commit}")
tags=$(git tag --merged "$commit" --list 'v[0-9]*')
matches=()
while IFS= read -r candidate; do
    if [[ $candidate != "$tag" && $candidate =~ $version ]]; then
        matches+=(--match "$candidate")
    fi
done <<< "$tags"

range=$commit
if (( ${#matches[@]} )); then
    previous=$(git describe --tags --abbrev=0 "${matches[@]}" "$commit")
    range="refs/tags/$previous..$commit"
fi

git log --topo-order --reverse --format='- %s' "$range" --
