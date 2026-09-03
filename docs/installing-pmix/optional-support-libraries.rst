.. _label-install-optional-support-libraries:

Optional support libraries
==========================

Beyond the :ref:`required support libraries
<label-install-required-support-libraries>`, PMIx will use a number of
further libraries **if it finds them at configure time**, and will build
without them otherwise. MUNGE is the one exception: it is searched for
only when ``--with-munge`` asks for it (see below).

None of these is needed to produce a working PMIx. Each one either
enables a capability that some sites want and others do not, or makes
something PMIx already does faster |mdash| and the distinction matters:
a missing library from the first group means a feature is simply absent,
while a missing library from the second group means PMIx still works but
does more of the work itself.

.. important:: The **selection happens at configure time, not run time.**
   A component is compiled only if its library was found, so an absent
   library is not something PMIx can report later or fall back from at
   run time |mdash| the code was never built. If you care whether a
   particular one was picked up, check the :ref:`configure output summary
   <label-install-configure-output-summary>` rather than assuming.

Recommended
-----------

These do not add features, but installing at least one of them is
worthwhile on any system that runs jobs at scale.

.. list-table::
   :header-rows: 1
   :widths: 12 10 30

   * - Library
     - ``configure`` option
     - What it does
   * - `Zstandard <https://facebook.github.io/zstd/>`_
     - ``--with-zstd``
     - Builds the ``pcompress/zstd`` component. **Preferred** where
       available: it is both faster and produces smaller output than the
       zlib components on the data PMIx actually compresses.
   * - `zlib-ng <https://github.com/zlib-ng/zlib-ng>`_
     - ``--with-zlibng``
     - Builds the ``pcompress/zlibng`` component |mdash| a faster
       implementation of the same DEFLATE format ``zlib`` produces.
   * - `LZ4 <https://lz4.org/>`_
     - ``--with-lz4``
     - Builds the ``pcompress/lz4`` component. The fastest of the four
       and the least dense, so it is not preferred over ``zstd`` by
       default; it is the one to select where daemon CPU matters more
       than bytes on the wire.
   * - `zlib <https://zlib.net/>`_
     - ``--with-zlib``
     - Builds the ``pcompress/zlib`` component. Almost universally
       available, which is why it is the usual fallback.

**What compression is for.** PMIx compresses large data objects before
they are shipped over the wire or stored |mdash| a job map, a modex blob,
a set of network endpoints. At scale those are the objects whose size
dominates job start-up, so a system with no compression library at all
will see longer start-up times and larger memory footprints. PMIx says so
once, at start-up, on servers and tools (clients stay silent); the warning
can be silenced with ``pcompress_base_silence_warning=1`` if the trade is
deliberate.

**You do not need more than one.** Exactly one component is active,
chosen by priority: ``zstd`` (90), then ``zlibng`` (75), then ``lz4``
(60), then ``zlib`` (50). Building several is harmless |mdash| the
highest-priority one wins and the others are simply never selected, and
any of them can be forced at run time with ``--pmixmca pcompress
<name>``.

.. important:: **You need the development package, not just the runtime
   library.** A ``pcompress`` component is compiled only if ``configure``
   finds the library's **headers** |mdash| ``zstd.h``, ``lz4frame.h``,
   ``zlib.h``. A system carrying ``libzstd.so`` but not ``libzstd-dev``
   builds no ``zstd`` component at all, and silently falls back to
   whichever lower-priority library *does* have headers installed. The
   distribution packages are typically ``libzstd-dev``, ``liblz4-dev``
   and ``zlib1g-dev`` (Debian/Ubuntu), or ``libzstd-devel``,
   ``lz4-devel`` and ``zlib-devel`` (RHEL/SUSE). Check the ``External
   Packages`` lines of the ``configure`` summary to see what was actually
   found.

.. warning:: **All processes in a job must use the same component.**
   ``zlib`` and ``zlib-ng`` both emit standard DEFLATE and are freely
   interchangeable, so a build that picks one on one node and the other
   elsewhere is fine. A ``zstd`` or ``lz4`` blob is **not** DEFLATE, and
   compressed data does cross nodes, so mixing either of those with each
   other or with a zlib-based installation within one job will not
   interoperate. Both refuse a blob that does not carry their own frame
   magic, so the mismatch surfaces as a clean error rather than as
   corrupted data |mdash| but it does not work. In practice this follows
   from a shared installation; it is worth knowing during a rolling
   upgrade.

Optional features
-----------------

Each of these enables something PMIx cannot do otherwise. Their absence
costs you that capability and nothing else.

.. list-table::
   :header-rows: 1
   :widths: 12 10 30

   * - Library
     - ``configure`` option
     - What it enables
   * - `MUNGE <https://dun.github.io/munge/>`_
     - ``--with-munge``
     - Builds the ``psec/munge`` component, which authenticates PMIx
       connections using MUNGE credentials. **Opt-in only:** unlike the
       rest of this table, an installed MUNGE is not detected on its own
       |mdash| the option must be given. Without it, the other ``psec``
       components (``native``, ``none``) remain available.
   * - `libesmtp <https://libesmtp.github.io/>`_
     - ``--with-smtp``
     - Builds the ``plog/smtp`` component, which can emit log messages by
       email. Without it, the other ``plog`` components are unaffected.
   * - `libltdl <https://www.gnu.org/software/libtool/>`_
     - ``--with-libltdl``
     - Builds the ``pdl/plibltdl`` component, an alternative to
       ``pdl/pdlopen`` for loading DSO components. ``pdlopen`` (priority
       80) is preferred over ``plibltdl`` (50) where both are built.

.. note:: **``dlopen`` is worth a word of its own.** It is not a package
   you install |mdash| it is part of the C library on essentially every
   supported platform |mdash| but it is what ``pdl/pdlopen`` needs, and
   ``pdl`` is what loads MCA components built as DSOs. A build with
   neither ``pdlopen`` nor ``plibltdl`` can still work, but only if every
   component is compiled statically into the library.

Specifying where to find them
-----------------------------

Every option above follows the same conventions as the required
libraries: ``--with-FOO=DIR`` names an installation prefix and
``--with-FOO-libdir=LIBDIR`` names the library directory when it is not
``DIR/lib``. They are listed in full under :ref:`CLI options for optional
support libraries
<label-building-pmix-cli-options-optional-support-libraries>`.

One behavior is worth stating explicitly, because it differs from a
plain search:

.. important:: **Naming a library makes it mandatory.** If you pass
   ``--with-zstd`` (with or without a path) and ``configure`` cannot find
   a usable Zstandard, it will **abort** rather than quietly build
   without it. That is deliberate |mdash| asking for something and
   silently not getting it is the failure mode these options exist to
   prevent. Omit the option entirely to get a best-effort search.

For example, to build against compression and MUNGE installations under
``/opt``:

.. code-block:: sh

   ./configure --prefix=$HOME/pmix-install \
       --with-zstd=/opt/zstd \
       --with-munge=/opt/munge ...
