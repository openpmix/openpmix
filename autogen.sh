#!/bin/sh

# Run all the rest of the Autotools
echo "==> git"
git status
if [ -f config/autogen_found_items.m4 ]; then
  echo "==> removing useless config/autogen_found_items.m4"
  rm -f config/autogen_found_items.m4
fi
echo "==> find"
find . -type f
echo "==> find | ls"
find . -type f | xargs ls -l
echo "==> Running autoreconf";
autoreconf ${autoreconf_args:-"-ivf"}
