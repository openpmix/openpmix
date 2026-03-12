.. _man1-pmixcc:

PMIx Wrapper Compiler
=====================

.. include_body

pmixcc |mdash| PMIx wrapper compiler

SYNTAX
------

``pmixcc [--showme | --showme:compile | --showme:link] ...``


OPTIONS
-------

The options include:

* ``--showme``: This option comes in several different variants (see
  below). None of the variants invokes the underlying compiler; they
  all provide information on how the underlying compiler would have
  been invoked had ``--showme`` not been used. The basic ``--showme``
  option outputs the command line that would be executed to compile
  the program.

  .. note:: If a non-filename argument is passed on the command line,
            the ``--showme`` option will *not* display any additional
            flags. For example, both ``"pmixcc --showme`` and
            ``pmixcc --showme my_source.c`` will show all the
            wrapper-supplied flags. But ``pmixcc
            --showme -v`` will only show the underlying compiler name
            and ``-v``.

* ``--showme:compile``: Output the compiler flags that would have been
  supplied to the underlying compiler.

* ``--showme:link``: Output the linker flags that would have been
  supplied to the underlying compiler.

* ``--showme:command``: Outputs the underlying compiler
  command (which may be one or more tokens).

* ``--showme:incdirs``: Outputs a space-delimited (but otherwise
  undecorated) list of directories that the wrapper compiler would
  have provided to the underlying compiler to indicate
  where relevant header files are located.

* ``--showme:libdirs``: Outputs a space-delimited (but otherwise
  undecorated) list of directories that the wrapper compiler would
  have provided to the underlying linker to indicate where relevant
  libraries are located.

* ``--showme:libs`` Outputs a space-delimited (but otherwise
  undecorated) list of library names that the wrapper compiler would
  have used to link an application. For example: ``pmix util``.

* ``--showme:version``: Outputs the version number of PMIx.

* ``--showme:help``: Output a brief usage help message.

See the man page for your underlying compiler for other options that
can be passed through pmixcc.


DESCRIPTION
-----------

Conceptually, the role of these commands is quite simple:
transparently add relevant compiler and linker flags to the user's
command line that are necessary to compile / link PMIx-based programs,
and then invoke the underlying compiler to actually perform the
command.

As such, these commands are frequently referred to as "wrapper"
compilers because they do not actually compile or link applications
themselves; they only add in command line flags and invoke the
back-end compiler.


Overview
--------

``pmixcc`` is a convenience wrapper for the underlying C compiler.
Translation of a PMIx-based program requires the linkage of the
PMIx-specific libraries which may not reside in one of the standard
search directories of ``ld(1)``. It also often requires the inclusion
of header files what may also not be found in a standard location.

``pmixcc`` passes its arguments to the underlying C compiler along with
the ``-I``, ``-L`` and ``-l`` options required by PMIx-based programs.

The PMIx Team *strongly* encourages using the wrapper compiler
instead of attempting to link to the PMIx library manually. This
allows the specific implementation of PMIx to change without
forcing changes to linker directives in users' Makefiles. Indeed, the
specific set of flags and libraries used by the wrapper compiler
depends on how PMIx was configured and built; the values can change
between different installations of the same version of PMIx.

Indeed, since the wrappers are simply thin shells on top of an
underlying compiler, there are very, very few compelling reasons *not*
to use PMIx's wrapper compiler. When it is not possible to use
the wrapper directly, the ``--showme:compile`` and ``--showme:link``
options should be used to determine what flags the wrapper would have
used. For example:

.. code:: sh

   shell$ cc -c file1.c `pmixcc --showme:compile`

   shell$ cc -c file2.c `pmixcc --showme:compile`

   shell$ cc file1.o file2.o `pmixcc --showme:link` -o my_program

.. _man1-pmix-wrapper-compiler-files:

FILES
-----

The strings that the wrapper compiler inserts into the command line
before invoking the underlying compiler are stored in a text file
created by PMIx and installed to
``$pkgdata/pmixcc-wrapper-data.txt``, where:

* ``$pkgdata`` is typically ``$prefix/share/pmix``
* ``$prefix`` is the top installation directory of PMIx

It is rarely necessary to edit this file, but it can be examined to
gain insight into what flags the wrapper is placing on the command
line.


ENVIRONMENT VARIABLES
---------------------

By default, the wrapper uses the compiler that was selected when
PMIx was configured. This compiler was either found
automatically by PMIx's "configure" script, or was selected by
the user via the ``CC`` environment variable
before ``configure`` was invoked. Additionally, other arguments specific
to the compiler may have been selected by configure.

These values can be selectively overridden by either editing the text
files containing this configuration information (see the :ref:`FILES
<man1-pmix-wrapper-compiler-files>` section), or by setting selected
environment variables of the form ``pmix_value``.

Valid value names are:

* ``CPPFLAGS``: Flags added when invoking the preprocessor

* ``LDFLAGS``: Flags added when invoking the linker

* ``LIBS``: Libraries added when invoking the linker

* ``CC``: C compiler

* ``CFLAGS``: C compiler flags
