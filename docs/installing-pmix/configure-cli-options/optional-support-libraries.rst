.. _label-building-pmix-cli-options-optional-support-libraries:

CLI Options for optional support libraries
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The following ``configure`` command line options are for PMIx's
:ref:`optional support libraries
<label-install-optional-support-libraries>` |mdash| libraries PMIx will
use if it finds them, and build without if it does not.

Compression
-----------

* ``--with-zstd[=VALUE]``:
* ``--with-zlibng[=VALUE]``:
* ``--with-zlib[=VALUE]``:

  These options specify where to find the headers and libraries for
  `Zstandard <https://facebook.github.io/zstd/>`_, `zlib-ng
  <https://github.com/zlib-ng/zlib-ng>`_ and `zlib <https://zlib.net/>`_
  respectively. Each builds the matching ``pcompress`` component.

  Only one component is active at run time |mdash| the highest priority
  one built (``zstd``, then ``zlibng``, then ``zlib``) |mdash| so there
  is no need to provide more than one, and no harm in providing several.

  A build with none of them works, but compresses nothing: expect longer
  start-up times and larger memory footprints at scale, and a warning to
  that effect from servers and tools.

Other capabilities
------------------

* ``--with-munge[=VALUE]``:

  Specifies where to find `MUNGE <https://dun.github.io/munge/>`_, and
  builds the ``psec/munge`` component, which authenticates PMIx
  connections using MUNGE credentials.

* ``--with-smtp[=VALUE]``:

  Specifies where to find `libesmtp
  <https://libesmtp.github.io/>`_, and builds the ``plog/smtp``
  component, which can emit log messages by email.

* ``--with-libltdl[=VALUE]``:

  Specifies where to find `libltdl
  <https://www.gnu.org/software/libtool/>`_, and builds the
  ``pdl/plibltdl`` component for loading DSO components. The
  ``pdl/pdlopen`` component is preferred where both are available.

Probed but unused
-----------------

* ``--with-jansson[=VALUE]``:
* ``--with-curl[=VALUE]``:

  These are accepted and reported in the configure summary, but no code
  in ``libpmix`` uses either library; Jansson is used only by the
  ``test/sshot`` test programs. They are listed here so their appearance
  in ``configure --help`` is not mistaken for a dependency.

Permitted values, and one important behavior
--------------------------------------------

  The following ``VALUE``\s are permitted for all of the options above:

  * ``DIR``: Specify the location of a specific installation to use.
    ``configure`` will abort if it cannot find suitable header files
    and libraries under ``DIR``.

.. important:: **Naming one of these libraries makes it mandatory.**
   Passing ``--with-FOO``, with or without a path, tells ``configure``
   that you want it |mdash| so if it cannot find a usable copy it will
   **abort** rather than quietly build without it. Omit the option
   entirely to get a best-effort search that silently continues on
   failure. This is why a build that "should have had compression" and
   does not is almost always one where the option was left off.

* ``--with-zstd-libdir=LIBDIR``:
* ``--with-zlibng-libdir=LIBDIR``:
* ``--with-zlib-libdir=LIBDIR``:
* ``--with-munge-libdir=LIBDIR``:
* ``--with-smtp-libdir=LIBDIR``:
* ``--with-libltdl-libdir=LIBDIR``:
* ``--with-jansson-libdir=LIBDIR``:
* ``--with-curl-libdir=LIBDIR``:
  :ref:`See the configure CLI
  options conventions <building-pmix-cli-options-conventions-label>`
  for a description of these options.
