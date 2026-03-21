.. _man3-PMIx_Log:

PMIx_Log
========

.. include_body

``PMIx_Log``, ``PMIx_Log_nb`` |mdash| Log data to a set of logging channels.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Log(const pmix_info_t data[], size_t ndata,
                          const pmix_info_t directives[], size_t ndirs);

   pmix_status_t PMIx_Log_nb(const pmix_info_t data[], size_t ndata,
                             const pmix_info_t directives[], size_t ndirs,
                             pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the data is a list of Python ``pmix_info_t`` dictionaries
  pydata = [{'key': PMIX_LOG_STDERR,
             'value': {'value': "log this string", 'val_type': PMIX_STRING}}]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = []
  rc = foo.log(pydata, pydirs)


INPUT PARAMETERS
----------------

* ``data``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs. Each
  entry names a logging *channel* (via its key) and carries the message to be logged on that
  channel (via its value).
* ``ndata``: Number of elements in the ``data`` array.
* ``directives``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs containing
  instructions on requested markups (e.g., timestamp) and routing of the logged information.
* ``ndirs``: Number of elements in the ``directives`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be executed once the operation is
  complete.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

The ``PMIx_Log`` function logs data subject to the services offered by the local PMIx server library
and its host environment. The ``data`` array specifies the information that is to be logged, while
the ``directives`` array controls formatting, routing, and other output options. Examples of messages
sometimes logged via this API include timestamped progress reports and error outputs.

``PMIx_Log`` is the blocking form: it does not return until the request has been handed off to every
servicing channel (or has failed). ``PMIx_Log_nb`` is the non-blocking form: it returns immediately,
and the provided ``cbfunc`` is invoked with the final status once the request completes. Callers of
the non-blocking form **must** keep the ``data`` and ``directives`` arrays valid until ``cbfunc`` is
invoked.

.. note::

  It is strongly recommended that the ``PMIx_Log`` API not be used by applications for streaming data,
  as it is not a "performant" transport and can perturb the application since it involves the local
  PMIx server and host daemon.

  Also note that a return of ``PMIX_SUCCESS`` only denotes that the data was successfully handed to the
  appropriate system call (for local channels) or the host environment, and does not indicate receipt
  at the final destination. For example, a return of ``PMIX_SUCCESS`` from a request to send an email
  message would only indicate that the message was passed to the local email server, and not that it
  had been delivered to the specified recipient.

Channels
^^^^^^^^

Each ``data`` element specifies the logging "channel" to be used (its key) plus the message to be
output via that channel (its value). The channel is selected by the *key* of the data item; a caller
never names a component directly. Supported channels include:

* ``PMIX_LOG_STDERR`` |mdash| output the message on the ``stderr`` file descriptor.
* ``PMIX_LOG_STDOUT`` |mdash| output the message on the ``stdout`` file descriptor.
* ``PMIX_LOG_SYSLOG`` |mdash| output the message to syslog. Defaults to ``ERROR`` priority. Will log
  to the global syslog if available, otherwise to the local syslog.
* ``PMIX_LOG_LOCAL_SYSLOG`` |mdash| output the message to the local syslog. Defaults to ``ERROR``
  priority.
* ``PMIX_LOG_GLOBAL_SYSLOG`` |mdash| forward the message to the system "master" and output it to that
  node's syslog. Defaults to ``ERROR`` priority.
* ``PMIX_LOG_EMAIL`` |mdash| send the message via email. The email parameters (recipient, sender,
  subject, server, and port) are provided using the ``PMIX_LOG_EMAIL_*`` attributes.

Additional channels defined for future use, whose availability depends on host environment support,
include:

* ``PMIX_LOG_JOB_RECORD`` |mdash| insert the message in the resource manager's job record. Many host
  environments maintain a database or other log that includes data on job start/end times, resources
  used, and other statistics, and may support injecting application-provided messages into that record
  for historical purposes.
* ``PMIX_LOG_GLOBAL_DATASTORE`` |mdash| record the message in a host-provided global datastore.

Channel availability
^^^^^^^^^^^^^^^^^^^^^

A requested channel may be unavailable for two distinct reasons:

* **Build configuration.** A channel is serviced by a ``plog`` framework component, and a component may
  be omitted at build time when its dependency is missing (for example, the ``smtp`` component that
  services ``PMIX_LOG_EMAIL`` requires ``libesmtp``, and the ``syslog`` component requires
  ``syslog.h``).
* **Selection criteria.** A component that is present may still be excluded at run time by the MCA
  parameters described under `MCA PARAMETERS`_ below, or the channel may only be serviceable by the
  host environment rather than by the library.

By default, a data element whose channel cannot be serviced is simply skipped. A caller that requires a
particular channel to be serviced |mdash| and wants an error if it cannot be |mdash| marks that data
element as *required* by setting the ``PMIX_INFO_REQD`` flag defined in
:ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>`. See `RETURN VALUE`_ for the resulting
status codes.

Directives
^^^^^^^^^^

The ``directives`` array qualifies how the request is handled. Commonly used directives include:

* ``PMIX_LOG_ONCE`` (bool) |mdash| log the request via only the first channel that can successfully
  service it, rather than via every matching channel. Channel order is significant only in this case
  (see ``plog_base_order`` under `MCA PARAMETERS`_).
* ``PMIX_LOG_GENERATE_TIMESTAMP`` (bool) |mdash| generate a timestamp for the log entry.
* ``PMIX_LOG_TIMESTAMP`` (time_t) |mdash| use the provided timestamp for the log entry.
* ``PMIX_LOG_TAG_OUTPUT`` (bool) |mdash| label the output with its channel name (e.g., ``stdout``).
* ``PMIX_LOG_TIMESTAMP_OUTPUT`` (bool) |mdash| prefix the printed output with a timestamp.
* ``PMIX_LOG_XML_OUTPUT`` (bool) |mdash| emit the output in XML format.
* ``PMIX_LOG_SYSLOG_PRI`` (int) |mdash| set the syslog priority level for syslog channels.

It is possible to log messages to different channels using a single call to
:ref:`PMIx_Log <man3-PMIx_Log>`. In this case, only directives applicable to a given channel are used
when outputting to that channel. For example, a request that included a ``PMIX_LOG_TIMESTAMP`` directive
and an email message would cause the email to be time-stamped; if that same request also included a
``PMIX_LOG_LOCAL_SYSLOG`` element with a ``PMIX_LOG_SYSLOG_PRI`` directive, the syslog-related directive
would apply only to the syslog channel (though the timestamp would apply to both).

Multiple instances of the same attribute may be included in the ``data`` array |mdash| e.g., to send
different emails to various recipients, or to log messages to both the local and global syslog channels.

Ordering of output within a single channel is preserved in the order the corresponding data elements are
presented. Ordering is **not** guaranteed between different channels, nor between ``PMIx_Log`` output and
other output streams (for example, a ``PMIX_LOG_STDOUT`` request and a direct ``printf`` to ``stdout``
may interleave in either order).


MCA PARAMETERS
--------------

The following MCA parameters influence the behavior of ``PMIx_Log``. The complete, authoritative list of
parameters (with current values) can be displayed with ``pmix_info``.

* ``plog_base_order=<list>``, where ``<list>`` is a comma-delimited, prioritized list of logging channel
  names. Log requests are offered to channels in the given order. Order matters only when
  ``PMIX_LOG_ONCE`` is included in the ``directives``. A channel name may carry a ``:req`` (or
  ``:required``) suffix to mark it mandatory; if a required channel has no servicing component, channel
  selection fails.

* ``plog=<list>``, where ``<list>`` is a comma-delimited list of ``plog`` components to use. Note that
  "components" differ from "channels": a single component may service multiple channels.

* ``pmix_log_host_only=<true|false>``. When set to ``true``, the PMIx server library passes all log
  requests to its host environment for processing (via the ``pmix_server_log2_fn_t`` server module
  entry) and does not use the ``plog`` framework to service them.

.. caution::

  Host environments that set ``pmix_log_host_only`` are responsible for preventing an infinite loopback
  of logging requests, which can occur if the host itself calls ``PMIx_Log`` |mdash| such requests would
  be returned to the host via the ``pmix_server_log2_fn_t`` server module entry.


RETURN VALUE
------------

Both forms return one of the following status codes. For ``PMIx_Log_nb``, a return of ``PMIX_SUCCESS``
indicates only that the request was accepted; the final status is delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the request was successfully handed off to every channel that could service
  it. When required data elements are present, this also indicates that all required channels were
  serviced.

* ``PMIX_ERR_PARTIAL_SUCCESS`` |mdash| at least one, but not all, of the requested channels serviced the
  request. This is returned when some data elements could be serviced and any remaining, unserviced
  elements were not marked required.

* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| none of the requested channels could service the request, or a
  channel required by the caller (via ``PMIX_INFO_REQD``) was not available. A client that receives this
  status from its server will retry the request against its own local channels before reporting the
  error.

* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``data`` array was ``NULL`` or ``ndata`` was zero.

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.


EXAMPLES
--------

Log a message to ``stderr`` and, in the same call, request that it be inserted into the syslog with a
generated timestamp:

.. code-block:: c

    #include <pmix.h>

    pmix_info_t data[2];
    pmix_info_t directive;
    pmix_status_t rc;

    /* the data array names the channels and carries the messages */
    PMIx_Info_load(&data[0], PMIX_LOG_STDERR,
                   "unable to open input file", PMIX_STRING);
    PMIx_Info_load(&data[1], PMIX_LOG_SYSLOG,
                   "job 12345: unable to open input file", PMIX_STRING);

    /* the directive array qualifies how the logging is done */
    PMIx_Info_load(&directive, PMIX_LOG_GENERATE_TIMESTAMP,
                   NULL, PMIX_BOOL);

    rc = PMIx_Log(data, 2, &directive, 1);
    if (PMIX_SUCCESS != rc && PMIX_ERR_PARTIAL_SUCCESS != rc) {
        /* not even one channel could service the request */
        fprintf(stderr, "PMIx_Log failed: %s\n", PMIx_Error_string(rc));
    }

    PMIx_Info_destruct(&data[0]);
    PMIx_Info_destruct(&data[1]);
    PMIx_Info_destruct(&directive);


.. seealso::
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
