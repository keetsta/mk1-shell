#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
    echo 'usage: release-notices.sh <tag>' >&2
    exit 1
fi

commit=$(git rev-parse --verify "refs/tags/$1^{commit}")

print_notice() {
    git show "$commit:$1" | awk -v name="$1" '
        BEGIN { fence_length = 3 }
        {
            lines[NR] = $0
            line = $0
            while (match(line, /`+/)) {
                if (RLENGTH >= fence_length) fence_length = RLENGTH + 1
                line = substr(line, RSTART + RLENGTH)
            }
        }
        END {
            for (i = 0; i < fence_length; i++) fence = fence "`"
            printf "\n### `%s`\n\n%stext\n", name, fence
            for (i = 1; i <= NR; i++) print lines[i]
            print fence
        }
    '
}

printf '\n<!-- mk1:license-notices -->\n<details>\n<summary>License and third-party notices</summary>\n'
print_notice LICENSE
print_notice THIRD_PARTY_NOTICES.md
printf '\n</details>\n'
