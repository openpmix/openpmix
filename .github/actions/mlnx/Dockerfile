FROM almalinux:9

RUN \
    dnf install -y perl perl-Data-Dumper \
        python3 python3-pip zlib \
        automake libtool flex make bzip2 git which rpm-build libevent-devel hwloc hwloc-devel

COPY entrypoint.sh /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]

