#!/usr/bin/env bash
set -euo pipefail

tag=${1:?a firmware tag is required}
image=${2:?a UF2 file is required}
if [[ ! "$tag" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo 'expected a stable firmware tag' >&2
    exit 1
fi

name=${image##*/}
case "$name" in
    mk1.uf2|mk1-demo.uf2) ;;
    *)
        echo 'expected mk1.uf2 or mk1-demo.uf2' >&2
        exit 1
        ;;
esac

ref=refs/heads/firmware-assets
digest=$(sha256sum -- "$image")
asset="uf2/$tag/${digest%% *}/$name"
blob=$(git hash-object -w --no-filters -- "$image")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
export GIT_INDEX_FILE="$scratch/index"

for attempt in 1 2 3 4; do
    parents=()
    if git ls-remote --exit-code --heads origin "$ref" >/dev/null; then
        git fetch --no-tags origin "$ref"
        previous=$(git rev-parse FETCH_HEAD)
        if existing=$(git rev-parse --verify "$previous:$asset" 2>/dev/null); then
            if [[ "$existing" != "$blob" ]]; then
                echo 'the firmware asset already exists with different bytes' >&2
                exit 1
            fi
            exit 0
        fi
        parents=(-p "$previous")
        git read-tree "$previous"
    elif [[ $? -eq 2 ]]; then
        git read-tree --empty
    else
        exit 1
    fi

    git update-index --add --cacheinfo 100644 "$blob" "$asset"
    tree=$(git write-tree)
    commit=$(git -c user.name='github-actions[bot]' \
        -c user.email='41898282+github-actions[bot]@users.noreply.github.com' \
        commit-tree "$tree" "${parents[@]}" -m "Publish mk-1 firmware $tag")
    if git push origin "$commit:$ref"; then
        exit 0
    fi
    sleep "$attempt"
done

echo 'could not publish the browser firmware asset' >&2
exit 1
