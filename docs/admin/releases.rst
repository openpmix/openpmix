.. _label-sysadmin-releases:

Choosing a Release
==================

Install only the latest release
-------------------------------

**Keep a single PMIx installation - the latest release - on each
system.** You do not need older releases alongside it.

PMIx is forward compatible. Every public API, macro, structure, and
attribute is kept forever. A program built against an earlier PMIx
release therefore builds and runs against a later one without change.
Applications, libraries, and tools do not need to be rebuilt when you
upgrade PMIx. (Two past exceptions, affecting programs built against
PMIx v5.0, are listed in :ref:`label-version-numbers`.)

Fixes, including security fixes, are made only to the current release
series (see :doc:`/security`). Staying on the latest release is how a
system receives them.


The exception: host environments
--------------------------------

A *host environment* - the resource manager or launcher that runs the
PMIx server - must be built against the PMIx release it runs with.
**When you upgrade PMIx, rebuild the host environment, or install its
vendor's build for the new release.**

The reason: a host gives the PMIx server library a table of functions
(its ``pmix_server_module_t``) by passing a pointer to it. The table
grows as PMIx adds operations, and the library cannot tell from a
pointer which version of the table it was given. It assumes the table
matches its own. See :doc:`/hosts/partnership`.

PRRTE is a host environment, and is tied more closely still: see
:doc:`prrte`.


Mixing versions at run time
---------------------------

Clients and servers of different PMIx releases work together:

* a server accepts clients and tools from PMIx v2.1 on;
* a client or tool connects to servers from PMIx v3.2 on.

This matters most for containers, which carry their own copy of the
PMIx client library. A container built with an older release runs
under a server from the latest one. See :ref:`label-version-numbers`.
