OpenPMIx |opmix_ver|
====================

The charter of the PMIx community is to:

#. Develop an open source (non-copy-left licensed) and independent
   (i.e., not affiliated with any specific programming model code
   base) standalone library to support application interactions with
   Resource Managers (RMs).

#. Retain transparent compatibility with the existing PMI-1 and PMI-2
   definitions, and any future PMI releases.

#. Support the *Instant On* initiative for rapid startup of
   applications at exascale and beyond.

#. Work with the HPC community to define and implement new APIs that
   support evolving programming model requirements for application-RM
   interactions.

PMIx is designed to be particularly easy for resource managers to
adopt, thus facilitating a rapid uptake into that community for
application portability. Both client and server libraries are
included, along with reference examples of client usage and
server-side integration. A list of supported environments and versions
is provided [here](etc) - please check regularly as the list is
changing!

.. error:: The above paragraph refers to a list of supported
   environments and versions, but I don't know what it is referring
   to.  This text / link should be fixed.

PMIx targets support for the Linux operating system.  A reasonable
effort is made to support all major, modern Linux distributions;
however, validation is limited to the most recent 2-3 releases of
RedHat Enterprise Linux (RHEL), Fedora, CentOS, and SUSE Linux
Enterprise Server (SLES). Support for vendor-specific operating
systems is included as provided by the vendor.

Table of contents
=================

.. toctree::
   :maxdepth: 2
   :numbered:

   quickstart
   history
   release-notes
   exceptions
   getting-help
   install
   versions
   developers/index
   contributing
   license
   man/index
