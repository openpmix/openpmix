#!/bin/sh

# Generate the version checking script with autom4te
echo "==> Generating pmix_get_version.sh";
pushd config 2>/dev/null 1>/dev/null
autom4te --language=m4sh pmix_get_version.m4sh -o pmix_get_version.sh
popd 2>/dev/null 1>/dev/null

# Run all the rest of the Autotools
echo "==> Running autoreconf";
autoreconf ${autoreconf_args:-"-ivf"}
