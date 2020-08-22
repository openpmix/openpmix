configure: builddir: /home/centos/IBM/openpmix
configure: srcdir: /home/centos/IBM/openpmix

============================================================================
== Configuring PMIx
============================================================================
checking build system type... powerpc64le-unknown-linux-gnu
checking host system type... powerpc64le-unknown-linux-gnu
checking target system type... powerpc64le-unknown-linux-gnu
checking for a BSD-compatible install... /usr/bin/install -c
checking whether build environment is sane... yes
checking for a thread-safe mkdir -p... /usr/bin/mkdir -p
checking for gawk... gawk
checking whether make sets $(MAKE)... yes
checking whether make supports nested variables... yes
checking whether make supports nested variables... (cached) yes
checking whether make supports the include directive... yes (GNU style)
checking for gcc... gcc
checking whether the C compiler works... yes
checking for C compiler default output file name... a.out
checking for suffix of executables... 
checking whether we are cross compiling... no
checking for suffix of object files... o
checking whether we are using the GNU C compiler... yes
checking whether gcc accepts -g... yes
checking for gcc option to accept ISO C89... none needed
checking whether gcc understands -c and -o together... yes
checking dependency style of gcc... gcc3
checking how to run the C preprocessor... gcc -E
checking for grep that handles long lines and -e... /usr/bin/grep
checking for egrep... /usr/bin/grep -E
checking for ANSI C header files... yes
checking for sys/types.h... yes
checking for sys/stat.h... yes
checking for stdlib.h... yes
checking for string.h... yes
checking for memory.h... yes
checking for strings.h... yes
checking for inttypes.h... yes
checking for stdint.h... yes
checking for unistd.h... yes
checking minix/config.h usability... no
checking minix/config.h presence... no
checking for minix/config.h... no
checking whether it is safe to define __EXTENSIONS__... yes
checking if want wrapper compiler rpath support... yes
checking if want wrapper compiler runpath support... yes
checking for ar... ar
checking the archiver (ar) interface... ar
checking for flex... flex
checking lex output file root... lex.yy
checking lex library... none needed
checking whether yytext is a pointer... no
checking if want dlopen support... yes
checking if embedded mode is enabled... no
checking if want developer-level compiler pickyness... no
--> developer override: enable picky compiler by default
checking if want developer-level debugging code... no
--> developer override: enable debugging code by default
checking if want to install project-internal header files... no
checking if tests and examples are to be installed... yes
checking if want pretty-print stacktrace... yes
checking if want dstore pthread-based locking... yes
checking if want ident string... 
checking if want developer-level timing support... no
checking if want backward compatibility for PMI-1 and PMI-2... yes
checking if want to disable binaries... yes
checking if want install Python bindings... no
checking if want to support dlopen of non-global namespaces... yes
checking if want pty support... yes
checking if want build psec/dummy_handshake... no
checking for prefix by checking for pmix_clean... no
installing to directory "/usr/local"
checking how to print strings... printf
checking for a sed that does not truncate output... /usr/bin/sed
checking for fgrep... /usr/bin/grep -F
checking for ld used by gcc... /usr/bin/ld
checking if the linker (/usr/bin/ld) is GNU ld... yes
checking for BSD- or MS-compatible name lister (nm)... /usr/bin/nm -B
checking the name lister (/usr/bin/nm -B) interface... BSD nm
checking whether ln -s works... yes
checking the maximum length of command line arguments... 1572864
checking how to convert powerpc64le-unknown-linux-gnu file names to powerpc64le-unknown-linux-gnu format... func_convert_file_noop
checking how to convert powerpc64le-unknown-linux-gnu file names to toolchain format... func_convert_file_noop
checking for /usr/bin/ld option to reload object files... -r
checking for objdump... objdump
checking how to recognize dependent libraries... pass_all
checking for dlltool... no
checking how to associate runtime and link libraries... printf %s\n
checking for archiver @FILE support... @
checking for strip... strip
checking for ranlib... ranlib
checking command to parse /usr/bin/nm -B output from gcc object... ok
checking for sysroot... no
checking for a working dd... /usr/bin/dd
checking how to truncate binary pipes... /usr/bin/dd bs=4096 count=1
checking for mt... no
checking if : is a manifest tool... no
checking for dlfcn.h... yes
checking for objdir... .libs
checking if gcc supports -fno-rtti -fno-exceptions... no
checking for gcc option to produce PIC... -fPIC -DPIC
checking if gcc PIC flag -fPIC -DPIC works... yes
checking if gcc static flag -static works... no
checking if gcc supports -c -o file.o... yes
checking if gcc supports -c -o file.o... (cached) yes
checking whether the gcc linker (/usr/bin/ld) supports shared libraries... yes
checking whether -lc should be explicitly linked in... no
checking dynamic linker characteristics... GNU/Linux ld.so
checking how to hardcode library paths into programs... immediate
checking whether stripping libraries is possible... yes
checking if libtool supports shared libraries... yes
checking whether to build shared libraries... yes
checking whether to build static libraries... no

*** C compiler and preprocessor
checking for gcc... (cached) gcc
checking whether we are using the GNU C compiler... (cached) yes
checking whether gcc accepts -g... (cached) yes
checking for gcc option to accept ISO C89... (cached) none needed
checking whether gcc understands -c and -o together... (cached) yes
checking dependency style of gcc... (cached) gcc3
checking for BSD- or MS-compatible name lister (nm)... (cached) /usr/bin/nm -B
checking the name lister (/usr/bin/nm -B) interface... (cached) BSD nm
checking __NetBSD__... no
checking __FreeBSD__... no
checking __OpenBSD__... no
checking __DragonFly__... no
checking __386BSD__... no
checking __bsdi__... no
checking __APPLE__... no
checking __linux__... yes
checking __sun__... no
checking __sun... no
checking netdb.h usability... yes
checking netdb.h presence... yes
checking for netdb.h... yes
checking netinet/in.h usability... yes
checking netinet/in.h presence... yes
checking for netinet/in.h... yes
checking netinet/tcp.h usability... yes
checking netinet/tcp.h presence... yes
checking for netinet/tcp.h... yes
checking for struct sockaddr_in... yes
configure: pmix builddir: /home/centos/IBM/openpmix
configure: pmix srcdir: /home/centos/IBM/openpmix
checking for pmix version... 4.0.0a1
checking if want pmix maintainer support... disabled
checking for pmix directory prefix... (none)
checking for extra lib... no
checking for extra ltlib... no
checking if want package/brand string... PMIx centos@snjkmr32.novalocal Distribution

============================================================================
== Compiler and preprocessor tests
============================================================================
checking if gcc requires a flag for C11... no
configure: verifying gcc supports C11 without a flag
checking if gcc  supports C11 _Thread_local... yes
checking if gcc  supports C11 atomic variables... yes
checking if gcc  supports C11 _Atomic keyword... yes
checking if gcc  supports C11 _Generic keyword... yes
checking if gcc  supports C11 _Static_assert... yes
checking if gcc  supports C11 atomic_fetch_xor_explicit... yes
configure: no flag required for C11 support
checking for gcc option to add a directory only to the search path for the quote form of include... -iquote
checking if gcc  supports __thread... yes
checking if gcc  supports C11 _Thread_local... yes
checking for the C compiler vendor... gnu
checking for ANSI C header files... (cached) yes
checking if gcc supports -Wno-long-double... no
checking if gcc supports -finline-functions... yes
checking if gcc supports -fno-strict-aliasing... yes
checking if gcc supports __builtin_expect... yes
checking if gcc supports __builtin_prefetch... yes
checking if gcc supports __builtin_clz... yes
checking for C optimization flags... -g -Wall -Wundef -Wno-long-long -Wsign-compare -Wmissing-prototypes -Wstrict-prototypes -Wcomment -pedantic -Werror-implicit-function-declaration -finline-functions -fno-strict-aliasing
checking for int8_t... yes
checking for uint8_t... yes
checking for int16_t... yes
checking for uint16_t... yes
checking for int32_t... yes
checking for uint32_t... yes
checking for int64_t... yes
checking for uint64_t... yes
checking for __int128... yes
checking for uint128_t... no
checking for long long... yes
checking for intptr_t... yes
checking for uintptr_t... yes
checking for ptrdiff_t... yes
checking size of _Bool... 1
checking size of char... 1
checking size of short... 2
checking size of int... 4
checking size of long... 8
checking size of long long... 8
checking size of float... 4
checking size of double... 8
checking size of void *... 8
checking size of size_t... 8
checking size of ptrdiff_t... 8
checking size of wchar_t... 4
checking size of pid_t... 4
checking alignment of bool... 1
checking alignment of int8_t... 1
checking alignment of int16_t... 2
checking alignment of int32_t... 4
checking alignment of int64_t... 8
checking alignment of char... 1
checking alignment of short... 2
checking alignment of wchar_t... 4
checking alignment of int... 4
checking alignment of long... 8
checking alignment of long long... 8
checking alignment of float... 4
checking alignment of double... 8
checking alignment of void *... 8
checking alignment of size_t... 8
checking for C bool type... no
checking size of _Bool... (cached) 1
checking for inline... __inline__

*** Compiler characteristics
checking for __attribute__... yes
checking for __attribute__(aligned)... yes
checking for __attribute__(always_inline)... yes
checking for __attribute__(cold)... yes
checking for __attribute__(const)... yes
checking for __attribute__(deprecated)... yes
checking for __attribute__(deprecated_argument)... yes
checking for __attribute__(format)... yes
checking for __attribute__(format_funcptr)... yes
checking for __attribute__(hot)... yes
checking for __attribute__(malloc)... yes
checking for __attribute__(may_alias)... yes
checking for __attribute__(no_instrument_function)... yes
checking for __attribute__(nonnull)... yes
checking for __attribute__(noreturn)... yes
checking for __attribute__(noreturn_funcptr)... yes
checking for __attribute__(packed)... yes
checking for __attribute__(pure)... yes
checking for __attribute__(sentinel)... yes
checking for __attribute__(unused)... yes
checking for __attribute__(visibility)... yes
checking for __attribute__(warn_unused_result)... yes
checking for __attribute__(destructor)... yes
checking for __attribute__(optnone)... no
checking for __attribute__(extension)... yes
checking for compiler familyid... 1
checking for compiler familyname... GNU
checking for compiler version... 525057
checking for compiler version_str... 8.3.1

*** Assembler
checking dependency style of gcc... gcc3
checking for perl... /usr/bin/perl
checking for atomic_compare_exchange_strong_16... yes
checking if atomic_compare_exchange_strong_16() gives correct results... yes
checking if C11 __int128 atomic compare-and-swap is always lock-free... yes
checking for 32-bit GCC built-in atomics... yes
checking for 64-bit GCC built-in atomics... yes
checking if 64-bit GCC built-in atomics are lock-free... yes
checking for __atomic_compare_exchange_n... yes
checking if __atomic_compare_exchange_n() gives correct results... yes
checking if __int128 atomic compare-and-swap is always lock-free... yes
checking for atomic_compare_exchange_strong_16... yes
checking if atomic_compare_exchange_strong_16() gives correct results... yes
checking if C11 __int128 atomic compare-and-swap is always lock-free... yes
checking if .proc/endp is needed... no
checking directive for setting text section... .text
checking directive for exporting symbols... .globl
checking for objdump... objdump
checking if .note.GNU-stack is needed... yes
checking suffix for labels... :
checking prefix for global symbol labels... 
checking prefix for lsym labels... .L
checking prefix for function in .type... @
checking if .size is needed... yes
checking if .align directive takes logarithmic value... yes
checking if PowerPC registers have r prefix... no
checking if gcc supports GCC inline assembly... yes
checking for assembly format... default-.text-.globl-:--.L-@-1-1-0-1-1
checking for assembly architecture... POWERPC64
checking for builtin atomics... BUILTIN_C11

============================================================================
== Header file tests
============================================================================
checking arpa/inet.h usability... yes
checking arpa/inet.h presence... yes
checking for arpa/inet.h... yes
checking fcntl.h usability... yes
checking fcntl.h presence... yes
checking for fcntl.h... yes
checking ifaddrs.h usability... yes
checking ifaddrs.h presence... yes
checking for ifaddrs.h... yes
checking for inttypes.h... (cached) yes
checking libgen.h usability... yes
checking libgen.h presence... yes
checking for libgen.h... yes
checking net/uio.h usability... no
checking net/uio.h presence... no
checking for net/uio.h... no
checking for netinet/in.h... (cached) yes
checking for stdint.h... (cached) yes
checking stddef.h usability... yes
checking stddef.h presence... yes
checking for stddef.h... yes
checking for stdlib.h... (cached) yes
checking for string.h... (cached) yes
checking for strings.h... (cached) yes
checking sys/ioctl.h usability... yes
checking sys/ioctl.h presence... yes
checking for sys/ioctl.h... yes
checking sys/param.h usability... yes
checking sys/param.h presence... yes
checking for sys/param.h... yes
checking sys/select.h usability... yes
checking sys/select.h presence... yes
checking for sys/select.h... yes
checking sys/socket.h usability... yes
checking sys/socket.h presence... yes
checking for sys/socket.h... yes
checking sys/sockio.h usability... no
checking sys/sockio.h presence... no
checking for sys/sockio.h... no
checking stdarg.h usability... yes
checking stdarg.h presence... yes
checking for stdarg.h... yes
checking for sys/stat.h... (cached) yes
checking sys/time.h usability... yes
checking sys/time.h presence... yes
checking for sys/time.h... yes
checking for sys/types.h... (cached) yes
checking sys/un.h usability... yes
checking sys/un.h presence... yes
checking for sys/un.h... yes
checking sys/uio.h usability... yes
checking sys/uio.h presence... yes
checking for sys/uio.h... yes
checking sys/wait.h usability... yes
checking sys/wait.h presence... yes
checking for sys/wait.h... yes
checking syslog.h usability... yes
checking syslog.h presence... yes
checking for syslog.h... yes
checking time.h usability... yes
checking time.h presence... yes
checking for time.h... yes
checking for unistd.h... (cached) yes
checking dirent.h usability... yes
checking dirent.h presence... yes
checking for dirent.h... yes
checking crt_externs.h usability... no
checking crt_externs.h presence... no
checking for crt_externs.h... no
checking signal.h usability... yes
checking signal.h presence... yes
checking for signal.h... yes
checking ioLib.h usability... no
checking ioLib.h presence... no
checking for ioLib.h... no
checking sockLib.h usability... no
checking sockLib.h presence... no
checking for sockLib.h... no
checking hostLib.h usability... no
checking hostLib.h presence... no
checking for hostLib.h... no
checking limits.h usability... yes
checking limits.h presence... yes
checking for limits.h... yes
checking sys/fcntl.h usability... yes
checking sys/fcntl.h presence... yes
checking for sys/fcntl.h... yes
checking sys/statfs.h usability... yes
checking sys/statfs.h presence... yes
checking for sys/statfs.h... yes
checking sys/statvfs.h usability... yes
checking sys/statvfs.h presence... yes
checking for sys/statvfs.h... yes
checking for netdb.h... (cached) yes
checking ucred.h usability... no
checking ucred.h presence... no
checking for ucred.h... no
checking zlib.h usability... yes
checking zlib.h presence... yes
checking for zlib.h... yes
checking sys/auxv.h usability... yes
checking sys/auxv.h presence... yes
checking for sys/auxv.h... yes
checking sys/sysctl.h usability... yes
checking sys/sysctl.h presence... yes
checking for sys/sysctl.h... yes
checking termio.h usability... yes
checking termio.h presence... yes
checking for termio.h... yes
checking termios.h usability... yes
checking termios.h presence... yes
checking for termios.h... yes
checking pty.h usability... yes
checking pty.h presence... yes
checking for pty.h... yes
checking libutil.h usability... no
checking libutil.h presence... no
checking for libutil.h... no
checking util.h usability... no
checking util.h presence... no
checking for util.h... no
checking grp.h usability... yes
checking grp.h presence... yes
checking for grp.h... yes
checking sys/cdefs.h usability... yes
checking sys/cdefs.h presence... yes
checking for sys/cdefs.h... yes
checking utmp.h usability... yes
checking utmp.h presence... yes
checking for utmp.h... yes
checking stropts.h usability... no
checking stropts.h presence... no
checking for stropts.h... no
checking sys/utsname.h usability... yes
checking sys/utsname.h presence... yes
checking for sys/utsname.h... yes
checking for sys/mount.h... yes
checking for sys/sysctl.h... (cached) yes
checking for net/if.h... yes
checking stdbool.h usability... yes
checking stdbool.h presence... yes
checking for stdbool.h... yes
checking if <stdbool.h> works... yes

============================================================================
== Type tests
============================================================================
checking for socklen_t... yes
checking for struct sockaddr_in... (cached) yes
checking for struct sockaddr_un... yes
checking for struct sockaddr_in6... yes
checking for struct sockaddr_storage... yes
checking whether AF_UNSPEC is declared... yes
checking whether PF_UNSPEC is declared... yes
checking whether AF_INET6 is declared... yes
checking whether PF_INET6 is declared... yes
checking if SA_RESTART defined in signal.h... yes
checking for struct sockaddr.sa_len... no
checking for struct dirent.d_type... yes
checking for siginfo_t.si_fd... yes
checking for siginfo_t.si_band... yes
checking for struct statfs.f_type... yes
checking for struct statfs.f_fstypename... no
checking for struct statvfs.f_basetype... no
checking for struct statvfs.f_fstypename... no
checking for struct ucred.uid... yes
checking for struct ucred.cr_uid... no
checking for struct sockpeercred.uid... no
checking for pointer diff type... ptrdiff_t (size: 8)
checking the linker for support for the -fini option... yes

============================================================================
== Library and Function tests
============================================================================
checking for library containing openpty... -lutil
checking for library containing gethostbyname... none required
checking for library containing socket... none required
checking for library containing dirname... none required
checking for library containing ceil... -lm
checking for library containing clock_gettime... none required
checking for asprintf... yes
checking for snprintf... yes
checking for vasprintf... yes
checking for vsnprintf... yes
checking for strsignal... yes
checking for socketpair... yes
checking for strncpy_s... no
checking for usleep... yes
checking for statfs... yes
checking for statvfs... yes
checking for getpeereid... no
checking for getpeerucred... no
checking for strnlen... yes
checking for posix_fallocate... yes
checking for tcgetpgrp... yes
checking for setpgid... yes
checking for ptsname... yes
checking for openpty... yes
checking for setenv... yes
checking for fork... yes
checking for execve... yes
checking for waitpid... yes
checking for htonl define... no
checking for htonl... yes
checking whether va_copy is declared... yes
checking whether __va_copy is declared... yes
checking whether __func__ is declared... yes

============================================================================
== System-specific tests
============================================================================
checking whether byte ordering is bigendian... no
checking for broken qsort... no
checking if C compiler and POSIX threads work as is... no
checking if C compiler and POSIX threads work with -Kthread... no
checking if C compiler and POSIX threads work with -kthread... no
checking if C compiler and POSIX threads work with -pthread... yes
checking for PTHREAD_MUTEX_ERRORCHECK_NP... yes
checking for PTHREAD_MUTEX_ERRORCHECK... yes
checking for working POSIX threads package... yes
checking if threads have different pids (pthreads on linux)... no
checking whether ln -s works... yes
checking for grep that handles long lines and -e... (cached) /usr/bin/grep
checking for egrep... (cached) /usr/bin/grep -E

============================================================================
== Symbol visibility feature
============================================================================
checking if gcc supports -fvisibility=hidden... yes
checking whether to enable symbol visibility... yes (via -fvisibility=hidden)

============================================================================
== Event libraries
============================================================================
checking --with-libev value... simple ok (unspecified)
checking --with-libev-libdir value... simple ok (unspecified)
checking will libev support be built... no
checking for libevent in... (default search paths)
looking for header in /usr/include
checking event.h usability... no
checking event.h presence... no
checking for event.h... no
looking for header in /usr/include/include
checking event.h usability... no
checking event.h presence... no
checking for event.h... no
checking for event_getcode4name in -levent... no
checking will libevent support be built... no
