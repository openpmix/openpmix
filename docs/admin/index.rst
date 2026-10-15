.. _label-sysadmin:

System Administration
=====================

This section is for the people who install PMIx and run it on a
system. It covers:

* which PMIx release to install, and why one is enough;
* how PMIx and PRRTE releases pair up, including the series that do
  not work together;
* how PMIx authenticates connections, and how to control it.

For building and installing PMIx itself, see
:ref:`building-and-installing-section-label`. For how to set run-time
parameters, see :doc:`/mca`.

Developers of resource managers and launchers should also read
:ref:`label-host-env`, which describes what such a program must do to
host the PMIx server.

.. toctree::
   :maxdepth: 2

   releases
   prrte
   authentication
