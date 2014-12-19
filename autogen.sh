#!/bin/sh

# Generate the version checking script with autom4te
echo "==> Generating pmix_get_version.sh";
CWD=`pwd`
cd $CWD/config
autom4te --language=m4sh pmix_get_version.m4sh -o pmix_get_version.sh
cd $CWD

# Run all the rest of the Autotools
echo "==> Running autoreconf";
autoreconf ${autoreconf_args:-"-ivf"}
