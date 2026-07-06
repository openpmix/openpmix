# Base image for the PMIx group-test "swarm" (see README.md).
#
# This harness is PMIx-centric: the code under test is *your live openpmix
# working tree*, which build.sh bind-mounts into a builder container and
# compiles out-of-tree (VPATH) into a shared volume mounted at /opt/prte.  PMIx
# is therefore NOT baked into this image.
#
# PRRTE is only the launcher we drive the tests through, so its *source* (the
# master branch) is baked in here and autogen'd, but it is NOT built at image
# time -- PRRTE must link the PMIx we are testing, so build.sh configures and
# builds the baked PRRTE source against your freshly-built PMIx at run time.
#
# This image therefore provides:
#
#   * the build toolchain (so the builder container can compile PMIx + PRRTE),
#   * PRRTE master source pre-cloned and autogen'd in /src/prrte,
#   * passwordless SSH between the "nodes", and
#   * an entrypoint that makes the shared-volume libraries loadable.
#
# Build it with ./build.sh (which also drives the PMIx + PRRTE build); a bare
# `docker build` just produces this base.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make \
        autoconf automake libtool m4 flex \
        perl python3 git \
        libevent-dev libhwloc-dev zlib1g-dev \
        openssh-server openssh-client \
        iproute2 iputils-ping procps less vim ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ---- baked PRRTE source (the launcher, built at run time) ----
# Clone PRRTE master with submodules so autogen.pl runs normally, and autogen
# it now (autogen needs no PMIx).  We deliberately do NOT configure/build here:
# PRRTE must be built against the PMIx under test, which build.sh bind-mounts
# and installs at run time.  Override the branch/repo with --build-arg.
ARG PRRTE_REPO=https://github.com/openpmix/prrte.git
ARG PRRTE_REF=master
RUN git clone --recursive -b "$PRRTE_REF" "$PRRTE_REPO" /src/prrte \
    && cd /src/prrte \
    && ./autogen.pl

# ---- passwordless SSH between the container "nodes" ----
RUN mkdir -p /var/run/sshd /root/.ssh \
    && ssh-keygen -t ed25519 -N "" -f /root/.ssh/id_ed25519 \
    && cp /root/.ssh/id_ed25519.pub /root/.ssh/authorized_keys \
    && printf 'Host *\n    StrictHostKeyChecking no\n    UserKnownHostsFile /dev/null\n    LogLevel ERROR\n' \
         > /root/.ssh/config \
    && chmod 600 /root/.ssh/* \
    && sed -i 's/#\?PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config \
    && printf '\nAcceptEnv *\n' >> /etc/ssh/sshd_config

# make the run-time install discoverable for any login shell (build.sh writes
# /opt/prte/env.sh with PATH/LD_LIBRARY_PATH once it knows the PMIx layout)
RUN echo '[ -f /opt/prte/env.sh ] && . /opt/prte/env.sh' > /etc/profile.d/prte.sh

# ---- node entrypoint ----
# The PMIx/PRRTE install lives in the shared /opt/prte volume, which is not on
# the image's default PATH or ld.so search path.  Before starting sshd:
#   * register the install's lib dirs with ldconfig so libpmix/libprrte load,
#     and
#   * symlink the install's binaries onto the default PATH (/usr/local/bin) so
#     that `prted` resolves in the non-login shell that plm/ssh uses to launch
#     daemons on the other nodes -- without this, a multi-node launch fails with
#     "prted: command not found".
RUN printf '#!/bin/sh\n\
for d in /opt/prte/prte/lib /opt/prte/pmix/lib; do\n\
    [ -d "$d" ] && echo "$d" >> /etc/ld.so.conf.d/prte.conf\n\
done\n\
ldconfig\n\
for b in /opt/prte/prte/bin/*; do\n\
    [ -e "$b" ] && ln -sf "$b" /usr/local/bin/\n\
done 2>/dev/null\n\
exec /usr/sbin/sshd -D -e\n' > /usr/local/bin/node-entrypoint.sh \
    && chmod +x /usr/local/bin/node-entrypoint.sh

EXPOSE 22
CMD ["/usr/local/bin/node-entrypoint.sh"]
