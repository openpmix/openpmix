.. _man1-pattrs:

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

* ``-a``, ``--all``: Show all configuration options and MCA
  parameters. Also changes the default MCA parameter level to 9,
  unless ``--level`` is also specified.

* ``--arch``: Show architecture on which Open MPI was compiled.

* ``-c``, ``--config``: Show configuration options

* ``-gmca``, ``--gmca <param> <value>``: Pass global MCA parameters
  that are applicable to all contexts.

* ``-h``, ``--help``: Shows help / usage message.

* ``--hostname``: Show the hostname on which Open MPI was configured
  and built.

* ``--internal``: Show internal MCA parameters (not meant to be
  modified by users).

* ``-mca``, ``--mca <param> <value>``: Pass context-specific MCA
  parameters; they are considered global if ``--gmca`` is not used and
  only one context is specified.

* ``--param <type> <component>``: Show MCA parameters. The first
  parameter is the type of the component to display; the second
  parameter is the specific component to display (or the keyword
  ``all``, meaning "display all components of this type").

* ``-t``, ``--type``: Show MCA parameters of the type specified in the
  parameter. Accepts the following parameters: ``unsigned_int``,
  ``unsigned_long``, ``unsigned_long_long``, ``size_t``, ``string``,
  ``version_string``, ``bool``, ``double``. By default level is 1
  unless it is specified with ``--level``.

* ``--parsable``: When used in conjunction with other parameters, the
  output is displayed in a machine-parsable format ``--parseable``
  Synonym for ``--parsable``.

* ``--path <type>``: Show paths that Open MPI was configured
  with. Accepts the following parameters: ``prefix``, ``bindir``,
  ``libdir``, ``incdir``, ``pkglibdir``, ``sysconfdir``.

* ``--pretty``: When used in conjunction with other parameters, the output is
  displayed in "prettyprint" format (default)

* ``--selected-only``: Show only variables from selected components.

* ``-V``, ``--version``: Show version of Open MPI.

EXIT STATUS
-----------

Description of the various exit statuses of this command.

EXAMPLES
--------

Examples of using this command.

.. seealso::
   :ref:`openpmix(5) <man5-openpmix>`
