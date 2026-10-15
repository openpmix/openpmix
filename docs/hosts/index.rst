.. _label-host-env:

Host Environments
=================

A *host environment* is the program that embeds the PMIx server
library: a resource manager, launcher, scheduler, or other daemon that
calls :ref:`PMIx_server_init(3) <man3-PMIx_server_init>` and supplies
the up-calls in
:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`. PRRTE is
the reference host.

This section is for **developers of host environments**, who need to
know what the PMIx library does for them, and what it expects them to
do in return. System administrators will find what they need in
:ref:`label-sysadmin`.


The partnership in brief
------------------------

PMIx and its host share the work of every operation. The split is the
same throughout:

* **PMIx handles what it can decide locally.** It authenticates each
  connection, checks that a request is well formed, checks that a
  process taking part in a collective is a participant, applies the
  access rule to data it answers itself, and assembles the local part
  of every collective.
* **The host handles everything else.** It decides whether a client or
  tool may connect at all, and whether a requester may perform an
  operation the library passes up. It runs every collective across
  nodes. It launches, signals, monitors, and allocates. It supplies the
  information that PMIx serves to applications.

PMIx's checks are an **initial triage**: inexpensive checks made as
early as possible, so that obviously invalid requests never reach the
host. They are not a complete authorization system, and are not meant
to be. Authorization policy needs knowledge PMIx does not have - who
is allowed to do what, on which resources, under which site rules. That
knowledge lives in the host, so **the host is the final arbiter** of
every connection and of every operation passed up to it.

To make that possible, PMIx tells the host who is asking on every
up-call it makes for a client or tool, using the identity it
established when that process connected.


Pages in this section
---------------------

.. toctree::
   :maxdepth: 2

   partnership
   connections
   authorization
   access-control
   operations

