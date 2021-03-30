#!/usr/bin/env bash

echo "Running clang-format on code base..."

files=($(git ls-tree -r master --name-only | grep -v -E 'contrib/' | grep -e '.*\.[ch]$' ))

for file in "${files[@]}" ; do
    if test "$1" = "-d" ; then
	echo "Would have formatted: ${file}"
    else
	clang-format --style=file --verbose -i "${file}"
    fi
done

echo "Done"
