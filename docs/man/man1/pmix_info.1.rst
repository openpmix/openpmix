.. _man1-pmix_info:

pmix_info
=========

.. include_body

pmix_info |mdash| Display information about the PMIx installation

SYNOPSIS
--------

``pmix_info [options]``


DESCRIPTION
-----------

``pmix_info`` provides detailed information about the PMIx
installation. It can be useful for at least three common scenarios:

#. Checking local configuration and seeing how PMIx was installed.

#. Submitting bug reports / help requests to the PMIx community
   (see :doc:`Getting help </getting-help>`).

#. Seeing a list of installed PMIx plugins and querying what MCA
   parameters they support.


OPTIONS
-------

``pmix_info`` accepts the following options:

* ``-h`` | ``--help <arg0>``: Show help message. If the optional
  argument is not provided, then a generalized help message similar
  to the information provided here is returned. If an argument is
  provided, then a more detailed help message for that specific
  command line option is returned.

* ``-v`` | ``--verbose``: Enable debug output.

* ``-V`` | ``--version``: Print version and exit.

* ``--pmixmca <key> <value>``: Pass a PMIx MCA parameter, where ``key`` is the
  parameter name and ``value`` is the parameter value.

* ``-a`` | ``--all``: Show all configuration options and MCA
  parameters.

* ``--arch``: Show architecture on which PMIx was compiled.

* ``-c`` | ``--config``: Show configuration options

* ``--hostname``: Show the hostname on which PMIx was configured
  and built.

* ``--internal``: Show internal MCA parameters (not meant to be
  modified by users).

* ``--param <framework>[:<component>[,<component>...]]``: Show MCA
  parameters of the named framework, optionally limited to a
  comma-delimited list of its components (all of its components if none
  are given, or if the list is ``all``). The keyword ``all`` in place of
  a framework shows every parameter. The option may be repeated.

* ``--params <framework>[:<component>[,<component>...]]``: Synonym for
  ``--param``.

* ``--type <arg0>``: Show MCA parameters of the type specified in the argument.
  Accepts: ``unsigned_int``, ``unsigned_long``, ``unsigned_long_long``,
  ``size_t``, ``string``, ``version_string``, ``bool``, and ``double``.

* ``--show-version [<what> [<part>]]``: Show the version of PMIx or a
  component. ``<what>`` can be the keywords ``pmix`` or ``all``, a
  framework name (all components in a framework), or a
  ``framework:component`` string. ``<part>`` can be one of: ``full``
  (the default), ``major``, ``minor``, ``release``, ``greek``, ``repo``.
  Given no argument, the version of PMIx and of every component is
  shown.

* ``--path <type>``: Show paths that PMIx was configured
  with. Accepts ``all`` or one of: ``prefix``, ``exec_prefix``,
  ``bindir``, ``sbindir``, ``libdir``, ``incdir``, ``mandir``,
  ``pkglibdir``, ``libexecdir``, ``datarootdir``, ``datadir``,
  ``sysconfdir``, ``sharedstatedir``, ``localstatedir``, ``infodir``,
  ``pkgdatadir``, ``pkgincludedir``. The option may be repeated.

* ``--pretty-print``: When used in conjunction with other parameters, the output is
  displayed in "prettyprint" format (default)

* ``--parsable``: When used in conjunction with other parameters, the output is
  displayed in a machine-parsable format

* ``--parseable``: Synonym for ``--parsable``

* ``--color <hue>``: Control color coding: auto (default), never, always

* ``--show-failed``: Show the components that failed to load along with the reason why they failed

* ``--selected-only``: Show only variables from selected components.


EXIT STATUS
-----------

Returns 0 if successful, non-zero if an error is encountered

EXAMPLES
--------

Examples of using this command.

Show the default output of options and listing of installed
components in a human-readable / prettyprint format:

.. code-block::

   pmix_info

Show the default output of options and listing of installed components
in a machine-parsable format:

.. code-block::

   pmix_info --parsable

Show the MCA parameters of the "linux" PIF component in a
human-readable / prettyprint format:

.. code-block::

   pmix_info --param pif:linux

Show the "bindir" that PMIx was configured with:

.. code-block::

   pmix_info --path bindir

Show the version of PMIx version numbers in a prettyprint format:

.. code-block::

   pmix_info --version

Show *all* information about the PMIx installation, including all
components that can be found, all the MCA parameters that they support,
versions of PMIx and the components, etc.:

.. code-block::

   pmix_info --all

.. seealso::
   :ref:`openpmix(5) <man5-openpmix>`
