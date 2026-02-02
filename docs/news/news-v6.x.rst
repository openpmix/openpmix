PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

v6.1.0  - - TBD Feb 2026
------------------------
.. important:: This is the second release in the v6 family. This
               release includes the following noteworthy changes:

               * a fix for a bug that apparently went undetected
                 for a long time. The bug prevented selection of
                 the loopback interface on a node for use by the PMIx
                 messaging system, even when no remote connections
                 were allowed. This prevented PMIx from working
                 in an environment where only loopback devices
                 were available.
               * re-enabling of the compression support. An undetected
                 typo caused PMIx to disable compression even when
                 the supporting libz (or libzng) libraries were present
               * repair of the indirect debugger connection procedure.
                 Hardening of the authentication procedure inadvertently
                 prevented tools fork'd by PMIx itself from connecting
                 back to the server.
               * threadshifting of all API calls and server callbacks to
                 avoid touching global variables while in the user's
                 progress thread
               * addition of a new API to allow the host to direct that
                 the PMIx internal progress thread be stopped - helpful
                 for abnormal termination scenarios
               * added implementation of process monitoring statistics
                 collection
               * support specification of a comma-delimited list of ranges
                 for the listener thread port
               * numerous memory leak, code cleanup, and bug fixes


Detailed changes since master rebranch include:
 - Update NEWS and VERSION for release
 - `Cleanup init/finalize cycle <https://github.com/openpmix/openpmix/commit/2f04cfd9b9333646e86c672dcc9bc54a736bdc97>`_
 - `Enable cherry-pick requirements on this branch <https://github.com/openpmix/openpmix/commit/2c1ee9597e6b67c6c555622dab665edee92eaef1>`_

Commits from master rebranch:
 - `Correct cflags used for check_compiler_version.m4 <https://github.com/openpmix/openpmix/commit/f29cfc71074806695add4b1691026e5eb4ac835c>`_
 - `Correctly threadshift PMIx_IOF_push directives <https://github.com/openpmix/openpmix/commit/9b222f41f6f09b439397e013208d8600c63316a7>`_
 - `Cleanup and abstract pmix_info support <https://github.com/openpmix/openpmix/commit/23908e24018e719a532002b311988d526e4aee7b>`_
 - `Fully support return of static values <https://github.com/openpmix/openpmix/commit/679f6956050c23d8193e5a8cdb2a6a2af3d696e7>`_
 - `Improve description of PMIx_Compute_distances API <https://github.com/openpmix/openpmix/commit/b21daa0e6586b550caadc770c0f226d1e75c2b53>`_
 - `Fine-tune the show-version option <https://github.com/openpmix/openpmix/commit/ee1d6c1570c0adaadd42c5c6d91beebf0fc41279>`_
 - `Revamp the pmix_info support <https://github.com/openpmix/openpmix/commit/f8b75a23506b86599c4ea0b158203a4a9b8c77a0>`_
 - `Ensure to store group info in PMIx server <https://github.com/openpmix/openpmix/commit/c5e3c3fb3cc18e337bb3a2a5ab7305c228ca037b>`_
 - `Add capability: get number function available <https://github.com/openpmix/openpmix/commit/2967f4ab8c6e9c81b7c5bffe13623f6a53c1c80b>`_
 - `Protect callbacks from threadshift when progress thread is stopped <https://github.com/openpmix/openpmix/commit/5541eeb136547cc06bfeaccca583962baab30578>`_
 - `Allow passing of progress thread to stop - default NULL to all <https://github.com/openpmix/openpmix/commit/1eb961d0bb4faf97e407cd6759060125c3122f36>`_
 - `Implement new API to stop the progress thread <https://github.com/openpmix/openpmix/commit/2abc2bc5637ed7981de48ee0ad0517c8d2be5f18>`_
 - `Silence valgrind issues <https://github.com/openpmix/openpmix/commit/75711b629bca67f38ccea5c94c8d6d9d62ba2280>`_
 - `Stop the progress thread right away in server_finalize <https://github.com/openpmix/openpmix/commit/7704efaf865328234e3cb1f77ff393adc971c9fe>`_
 - `Update the monitor_multi example <https://github.com/openpmix/openpmix/commit/a80c055654cf3cdea8207d4b8191d2fb118a515d>`_
 - `Seal memory leak <https://github.com/openpmix/openpmix/commit/cbac91edf1be0f5e14444c4bc545d8e7b474578c>`_
 - `update-my-copyright.py: properly support git workspaces <https://github.com/openpmix/openpmix/commit/e238a6ed52a60216a13c4e196740a4a91eb2c90f>`_
 - `Update log support to conform to Standard <https://github.com/openpmix/openpmix/commit/539b8e155ccb0b7af860c98d356dc15ceb4f1292>`_
 - `Threadshift IOF API calls <https://github.com/openpmix/openpmix/commit/e37fa253bb55d1e6121e687e1254373698ad53bb>`_
 - `Update PMIx_Fence to fully conform to Standard <https://github.com/openpmix/openpmix/commit/0cca795f62e88e2c6a03eccf78beb9e5e7a4fbda>`_
 - `Silence some Coverity warnings <https://github.com/openpmix/openpmix/commit/5d7c80eb605b2e46c6426d893a6d06bbcf3523de>`_
 - `Potential double free and use after free (alerts 13,14) <https://github.com/openpmix/openpmix/commit/db5b5170a2e8620abde072df9948e51fa76c098b>`_
 - `Potentially overflowing call to snprintf (alert 11) <https://github.com/openpmix/openpmix/commit/b591fcb18064b6645f978dc1467dd2ea49952895>`_
 - `Workflow does not contain permissions <https://github.com/openpmix/openpmix/commit/77fc99839c6d63f0fb62ade7431145d95ba0ac8a>`_
 - `printf.c: fix off-by-one + underflow errors <https://github.com/openpmix/openpmix/commit/78af01eef89b6dde9b051c7f5ca295dae724191c>`_
 - `Use floating numbers for float/double comparisons <https://github.com/openpmix/openpmix/commit/9b5bd0eb80975dff6565b4a92a080c5f24cd051e>`_
 - `Extend debugger CI tests <https://github.com/openpmix/openpmix/commit/66711f22e0dfae4e7dbbb1a20b21b1b0f767855b>`_
 - `Fix indirect debugger launch <https://github.com/openpmix/openpmix/commit/667b23a771bc6746f1772dabeb40a4831bf4b36b>`_
 - `Fix refresh cache request <https://github.com/openpmix/openpmix/commit/bb34c7c5dc154321d048a3be3b1b710a9c97c847>`_
 - `Add refresh test <https://github.com/openpmix/openpmix/commit/f0f989714a1b6d973f7712de484a964c737bf326>`_
 - `Protect static variable <https://github.com/openpmix/openpmix/commit/49f97c441f29835761b1065dd2a528dc7b04b16b>`_
 - `Cleanup ready-for-debug announcement <https://github.com/openpmix/openpmix/commit/d076bf5a5ad0c62437db628109afece946c36db5>`_
 - `Fix bitmap mask literal size <https://github.com/openpmix/openpmix/commit/36df20027e01623567225a62ea238eb13063153f>`_
 - `Fix cmd line option checker <https://github.com/openpmix/openpmix/commit/8263a63b02c1457ac1ebfcad543a12267be6345a>`_
 - `Check if we fork'd the tool ourselves <https://github.com/openpmix/openpmix/commit/9717c2c6d7a46457471515ce8e69d1c751e57672>`_
 - `fix for failure to call MPI_Session_init twice <https://github.com/openpmix/openpmix/commit/d30c15e65f3e6f154807424dd428698fe9fbad3a>`_
 - `Protect against equal signs in option check <https://github.com/openpmix/openpmix/commit/3617afc4b0862a9b27eeb497d822f23b776a531d>`_
 - `Implement support for resource usage monitoring <https://github.com/openpmix/openpmix/commit/632bc703f9352655de70263313ffb77879ee4e37>`_
 - `Enable use of loopback interface <https://github.com/openpmix/openpmix/commit/baea134cf02b2921d99dc42910de0b9c43f4cba5>`_
 - `Add new PMIX_GROUP_FINAL_MEMBERSHIP_ORDER attribute <https://github.com/openpmix/openpmix/commit/06a3e488568323e3991ad4cc1a3cd96ae7f04c7b>`_
 - `Replace sprintf with snprintf <https://github.com/openpmix/openpmix/commit/1c63aeaad0911617d288a142e18a04785c4863d2>`_
 - `Put the sink cleanup in the sink destructor <https://github.com/openpmix/openpmix/commit/f04adb20d4405e210aa78bc3726dfaf9d69f5765>`_
 - `Flush namespace sinks' residuals before destroying them <https://github.com/openpmix/openpmix/commit/f7739479f832ab1549d72a7b55faeab927dfe94f>`_
 - `Replace int taint limits with defined names <https://github.com/openpmix/openpmix/commit/0ab43bde0cae9a578b3660dba9ef63c12a646617>`_
 - `Silence latest Coverity warning report <https://github.com/openpmix/openpmix/commit/e3a8c50e4ea6a2d19bd4a4a497e7a68ceda57704>`_
 - `Port bug fixes to zlibng component <https://github.com/openpmix/openpmix/commit/bd6846346f12ba3caa3b622293a0a77452cbf1d7>`_
 - `preg/compress parsing bugfix <https://github.com/openpmix/openpmix/commit/2dfa69c1f566464f5fa8409546d0b2b45386553c>`_
 - `bitmap num_set boundary condition bugfix <https://github.com/openpmix/openpmix/commit/08c22de3fd6ada70d2c22fca54be1782d5f162be>`_
 - `Silence Coverity warning 1 <https://github.com/openpmix/openpmix/commit/41002c98265643e87602aeb41d57a205df4a6ba1>`_
 - `Update the plog framework <https://github.com/openpmix/openpmix/commit/1eb743c287bf099e96a74e13bc9ed9e49cacd85b>`_
 - `Check only for existence of PMIx capability flag <https://github.com/openpmix/openpmix/commit/73960b2e855b94363285d8191615c752ea3b0d17>`_
 - `Update to reflect standard changes <https://github.com/openpmix/openpmix/commit/3b508ae118d435fc539ad51e42f26042d32f289c>`_
 - `Silence Coverity warnings 2 <https://github.com/openpmix/openpmix/commit/85937fa7bcfd2afb05bc35c0b6127aea1eba00c3>`_
 - `Remove unnecessary locks from munge psec module <https://github.com/openpmix/openpmix/commit/d567ef351ecab2ca23373d90613be20b121ae035>`_
 - `Avoid use of API in PMIx_Init <https://github.com/openpmix/openpmix/commit/66bae450798add0454bc9966332e654d468a2458>`_
 - `Silence Coverity warnings 3 <https://github.com/openpmix/openpmix/commit/7e60d5a80e8d2958b9e83ee846de4a39aba05f66>`_
 - `Silence Coverity warnings 4 <https://github.com/openpmix/openpmix/commit/08af67fb0e6426b8c5c94d87bbf4f46df9bb471e>`_
 - `Switch to atomics for tracking initialization <https://github.com/openpmix/openpmix/commit/56b3b87d562185cb069bfb6a315958db696ab015>`_
 - `Silence coverity warning 5 <https://github.com/openpmix/openpmix/commit/abfa084db71cc70b7ec68f4859f6c9fcfd15b6da>`_
 - `Change to using atomic for show_help_enabled <https://github.com/openpmix/openpmix/commit/c05db59d99d3da1977287021cb818fc059e1b80b>`_
 - `Silence Coverity warning 6 <https://github.com/openpmix/openpmix/commit/97af1a02e8a51b937aa85a06ecf7022ee379d0ed>`_
 - `Remove stale/unused tests <https://github.com/openpmix/openpmix/commit/a9ef77baa6c741d8ed898b742205425731957461>`_
 - `Silence more Coverity warnings 7 <https://github.com/openpmix/openpmix/commit/88de8e1b2f9429bed524aac89e28b869fadb6e35>`_
 - `Silence Coverity warnings 8 <https://github.com/openpmix/openpmix/commit/0d157efbfff83493346789ec8a5ef92af969811d>`_
 - `Silence dumb Coverity warning 9 <https://github.com/openpmix/openpmix/commit/496b18d59721e38de09987d536d4e238baa28368>`_
 - `Use the correct value for the number of info to unpack <https://github.com/openpmix/openpmix/commit/58f49a1f82cd9753c89d6c1c960c116fb9a22ba9>`_
 - `Silence more Coverity warnings 10 <https://github.com/openpmix/openpmix/commit/f37be14495e103dc6758f9d75ece987620a71aa9>`_
 - `Silence Coverity warnings 11 <https://github.com/openpmix/openpmix/commit/34fee3d1961121b1d5cfb6916944213df2e82c6f>`_
 - `Extend listener thread port specification to support ranges <https://github.com/openpmix/openpmix/commit/421f60b0e3f16379562690d8646ab917854d9dc5>`_
 - `Fix compression components <https://github.com/openpmix/openpmix/commit/0fe0c5951cf36bbed7f637daf2c4456af3f5cb8b>`_
 - `Add attribute to request reports be in physical CPU IDs <https://github.com/openpmix/openpmix/commit/435c44c22a351d95b7b67bd047d8b45da122d71c>`_
 - `Add set-env cmd line option definition <https://github.com/openpmix/openpmix/commit/acbed18eb398f24470719a0eb44d32a69e68dfcd>`_
 - `Minor change to thread construct/ops <https://github.com/openpmix/openpmix/commit/7c19d2a1a355465463e7aa4891912cfcf9661daf>`_
 - `Fix error code on blocking PMIx_Notify_event calls <https://github.com/openpmix/openpmix/commit/7b400db0686c3b3c903770b113bdfaab2a7e648a>`_
 - `Silence a few Coverity complaints <https://github.com/openpmix/openpmix/commit/849a9c17e4e677be54ffa17ab9bed18145293ae0>`_
 - `define default MAXPATHLEN if not defined by system <https://github.com/openpmix/openpmix/commit/32f1a464efd160187a83cd4847831fe250f46e4d>`_
 - `Improve selection of interfaces <https://github.com/openpmix/openpmix/commit/b5a5067dc56b2f990dc65b2945922f3739617c94>`_
 - `Avoid duplicate namespace entries <https://github.com/openpmix/openpmix/commit/a3103b86821e6d6a7b58faae5a9acbc00bdf914b>`_
 - `Cleanup a bit on connection handling <https://github.com/openpmix/openpmix/commit/947d75e14b7bdaec5db48da515a3066bde6a698b>`_
 - `Allow foreign tools by default <https://github.com/openpmix/openpmix/commit/ab85e1a5362187c3942d9e2e55a7144842aa7c1c>`_
 - `Do not remove nspace from global list on rejected connection <https://github.com/openpmix/openpmix/commit/12918e11421c4fcc8b03312d421de603f1371bd7>`_
 - `Correctly check length in pmix_bitmap_num_set_bits <https://github.com/openpmix/openpmix/commit/47b6634be858ca6e5915eed5fb2c50ceb0b4ece0>`_
 - `Fix the abort server upcall <https://github.com/openpmix/openpmix/commit/fbe94bcd6e52afb6e33d249f8a655c72be93a086>`_
 - `Update listener thread setting of permissions on connection files <https://github.com/openpmix/openpmix/commit/cc49332fdf1242669d08a63e48ffae28b48e04b9>`_
 - `Extend authentication support <https://github.com/openpmix/openpmix/commit/d5773968432d96cbc359d6e8a9c54c367209ff0b>`_
 - `Provide more info on connections <https://github.com/openpmix/openpmix/commit/3162ba55463fbcca3aade455917394b0845ab4d8>`_
 - `Pass the client's pid as well <https://github.com/openpmix/openpmix/commit/e0e2974788582c1406e4e3d35c4bc6099db4b001>`_
 - `ci: add group_bootstrap to CI <https://github.com/openpmix/openpmix/commit/d514cd9bc6f2130a75a0d3b076a3b539c63cb82b>`_
 - `Prevent memory overrun in regx calculation <https://github.com/openpmix/openpmix/commit/37ff4df80844fe4c479e4f2072ceea15ba3f76a5>`_
 - `Silence Coverity complaints <https://github.com/openpmix/openpmix/commit/b3b709b90e14688ac35dddb9a09e340697ee421f>`_
 - `(Should have been squashed into above commit) <https://github.com/openpmix/openpmix/commit/766e1cf1f1b172c84f54f605c6418aa3021f21b0>`_
 - `Revamp stats implementation to reflect Standard <https://github.com/openpmix/openpmix/commit/2c3c74c71704170bd9868a9575b236d8349ea4b5>`_
 - `Pass the uid/gid for client connections <https://github.com/openpmix/openpmix/commit/633068a906db22e1532c7cfff1fa4f74b52f9fed>`_
 - `Provide better FQDN support <https://github.com/openpmix/openpmix/commit/c36cae08d0e37ad93644bc82810c2a3052b886f1>`_
 - `IOF output file bugfix <https://github.com/openpmix/openpmix/commit/5995438c0925560ed2c9c43d9b6416e899f99ce4>`_
 - `Parameterize client finalize timeout <https://github.com/openpmix/openpmix/commit/85bc673d5860158623b3eaebf6996d2e2d59e860>`_
 - `Update termios right away <https://github.com/openpmix/openpmix/commit/93e8859a024696cc39a739f4b8cd34a68ecb88d1>`_
 - `Continue work on pty support <https://github.com/openpmix/openpmix/commit/893f12fda44dbd0d1c1d110e44f5f80aed42d9c2>`_
 - `Work on enabling "pty" behaviors <https://github.com/openpmix/openpmix/commit/5ee562a6e9342f9212b048b9035c8fd226f5544f>`_
 - `Always search help arrays if initialized <https://github.com/openpmix/openpmix/commit/e4161004a72944a6719f01108be436795b760226>`_
 - `Check return code for notify ready-for-debug <https://github.com/openpmix/openpmix/commit/69f7e7e22f01b4963e4fd24ea0eb82d7bd702013>`_
 - `Add debugger checks to CI <https://github.com/openpmix/openpmix/commit/9a0c18c2b2343c57b5f4fbde07fb3676f64516b2>`_
 - `Correct client notify of ready for debugger <https://github.com/openpmix/openpmix/commit/6eb0177c9a13ff94636db9f8711c21d3559e8e19>`_
 - `Only report bad prefix if verbose requested <https://github.com/openpmix/openpmix/commit/f28cdd74468bbd3299460b225aa78b0735e5ee3d>`_
 - `Change default show-load-errors to "none" <https://github.com/openpmix/openpmix/commit/3657e41340f934e0ff2dd5286b26e556fb1eda2c>`_
 - `Prevent show -help from using IOF too soon <https://github.com/openpmix/openpmix/commit/f305d269f63b865f55d8dfdd65c61be4222dd70f>`_
 - `Try reorganizing <https://github.com/openpmix/openpmix/commit/66743da9e6cbfa21c45a3ffcfea62451cf6b4600>`_
 - `Update to track changes in Standard <https://github.com/openpmix/openpmix/commit/2b3dd64e7f3fc9e9a161b8efdadae8e30ca08251>`_
 - `Cleanup some group docs <https://github.com/openpmix/openpmix/commit/255b55dc32347889664877a6c147a88315ab9e75>`_
 - `Update CI <https://github.com/openpmix/openpmix/commit/d8e842daa2ada47d7cdd345962ff1db176277e79>`_
 - `Ensure cleanup of allocated pmix_info_t <https://github.com/openpmix/openpmix/commit/51a4b6725450eb486df37587dacd60a3b6af8d74>`_
 - `Properly trigger the "keepalive failed" event <https://github.com/openpmix/openpmix/commit/57d71b6e748abb6dd403962da074e3666bdcaca3>`_
 - `Provide better singleton support and support blocking event notify <https://github.com/openpmix/openpmix/commit/b668a99213e173b8960426638c944ad3dc01b980>`_
 - `bindings: fix Python 3.x error for 'long' integer <https://github.com/openpmix/openpmix/commit/684e9282d2f963a87216defc93c923466ff604e1>`_
 - `github: add python -version checks for bindings <https://github.com/openpmix/openpmix/commit/c690de9a30ae73ba95bf49edbb8cee34a52a0626>`_
 - `Update OAC submodule <https://github.com/openpmix/openpmix/commit/12ec3d50d182f196e786d5f11014ac7af8ecaa5a>`_
 - `Handle some corner cases for data ops <https://github.com/openpmix/openpmix/commit/d738ddc3e29808685a93cf028498efec05d03fa9>`_
 - `Update the Data pack/unpack functions <https://github.com/openpmix/openpmix/commit/33caa9ed12543cd2c35f96ae0e8d62ae773f7cfe>`_
 - `First set of API updates <https://github.com/openpmix/openpmix/commit/f6c2c080de598f1ab55c28f728e1b352850ebb34>`_
 - `Check for pthread_np.h header <https://github.com/openpmix/openpmix/commit/829f0bb5b06e460633bff7572a17dd47b48d88da>`_
 - `Complete sweep of server upcall callback functions <https://github.com/openpmix/openpmix/commit/6d6fc9aac9d2a7baf987e8b3253f14ed15f1cf51>`_
 - `Add a PMIX_FWD_ENVIRONMENT attribute <https://github.com/openpmix/openpmix/commit/7e9b1cdca47cc03fbab058de8efc3715aa83b521>`_
 - `Threadshift the PMIx_Notify_event API <https://github.com/openpmix/openpmix/commit/336e4f5941643ebf32f8093416016a6b51241781>`_
 - `Delete built files on "make clean" <https://github.com/openpmix/openpmix/commit/cab097fcc585a3bbcd205c2a2368a5aaf7b4126c>`_
 - `Decrease min Py version to 3.6 <https://github.com/openpmix/openpmix/commit/b50dfb18ac6a28128a97b96f2ce0b4d86bf17ef8>`_
 - `Continue work on threadshifting all upcall callbacks 1 <https://github.com/openpmix/openpmix/commit/7865295777af42f23b94f0f811e4012d06eaaa3e>`_
 - `Continue work on threadshifting all upcall callbacks 2 <https://github.com/openpmix/openpmix/commit/e227fb931e0e6e637a169499bce1d33f9204b56f>`_
 - `Ensure more upcall cbfuncs threadshift <https://github.com/openpmix/openpmix/commit/f94172362997a295855dbd238a4ac65441be2fa3>`_
 - `Fix the wrapper compiler <https://github.com/openpmix/openpmix/commit/824058daae3449e02dacda9f4d0abe94db544d60>`_
 - `Ensure to threadshift callback functions <https://github.com/openpmix/openpmix/commit/32f9c9d1cfd0991b7e5255d74bc0e7c13e5d954a>`_
 - `Update news from release branches <https://github.com/openpmix/openpmix/commit/735762c05a4d8b180b14a16e470829e6cc25d036>`_


v6.0.0  - - 19 May 2025
-----------------------
.. important:: This is the first release in the v6 family. The intent
               for this series is to provide regular "reference tags",
               effectively serving as milestones for any development
               that might occur after the project achieved a stable
               landing zone at the conclusion of the v5 series. It
               is expected, therefore, that releases shall be infrequent
               and rare occurrences, primarily driven by the completion
               of some significant feature or some particularly
               critical bug fix.

               For this initial release, that feature is completion of
               the Group family of APIs. This includes support for all
               three of the group construction modes, including the new
               "bootstrap" method. A description of each mode can be
               found in the :ref:`Group Construction <group-construction-label>`
               section of the documentation.

               A few notes:

               (1) Proper execution of the various group construction
               modes requires that the host provide the necessary backend
               support. Please check with your host provider to ensure it
               is available, or feel free to use the PMIx Reference
               RunTime Environment (`PRRTE <https://github.com/openpmix/prrte/releases>`_)  - you will require v4.0 or above.

               (2) Starting with this release, PMIx requires
               Python >= v3.7 to build a Git clone (ie., not a tarball).
               Certain elements of the code base are constructed at build
               time, with the construction performed by Python script. The
               constructed elements are included in release tarballs.

               (3) PRRTE < v4.0 is not compatible with PMIx >= v6.0 due
               to internal changes (e.g., show-help messages are now
               contained in memory instead of on-disk files).

A full list of individual changes will not be provided here,
but will commence with the v6.0.1 release.
