.. _label-building-pmix-cli-options-required-support-libraries:

CLI Options for required support libraries
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The following ``configure`` command line options are for PMIx's
:ref:`required support libraries
<label-install-required-support-libraries>`

* ``--with-hwloc[=VALUE]``:
* ``--with-libevent[=VALUE]``:

These  options specify where to find
  the headers and libraries for the `Hwloc
  <https://www.open-mpi.org/projects/hwloc/>`_ and `Libevent
  <https://libevent.org/>`_ libraries, respectively.

  The following ``VALUE``\s are permitted:

  * ``DIR``: Specify the location of a specific installation to use.
    ``configure`` will abort if it cannot find suitable header files
    and libraries under ``DIR``.

* ``--with-hwloc-libdir=LIBDIR``:
* ``--with-libevent-libdir=LIBDIR``:
  :ref:`See the configure CLI
  options conventions <building-pmix-cli-options-conventions-label>`
  for a description of these options.
