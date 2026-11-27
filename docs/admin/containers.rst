.. _label-sysadmin-containers:

Containers and Kubernetes
=========================

This page covers what a PMIx server needs from a container's file
system, and what to set when a container - a Kubernetes pod, in
particular - does not provide it. The settings here are the
**server's**: set them where the host environment's daemons (PRRTE's
``prted``, a resource manager's node daemon) run, not for the
applications. Clients and tools do not read them.


A server will not start with a world-writable ``/tmp``
------------------------------------------------------

**Symptom.** The host environment fails to start, or fails to launch
anything, and prints:

.. code-block:: text

   This PMIx server's session tmpdir

     /tmp

   is, or lies below, a directory another user could change:

     Directory:  /tmp
     Owner:      uid 0
     Mode:       0777
     Why:        others may write it, and it is not sticky

   That user could move this server's directories aside and put others at
   their names, and the server's clients would fail to connect. So the
   server will not start.

The tmpdir and directory named may differ (``system`` instead of
``session``, ``$TMPDIR`` instead of ``/tmp``), but the cause is the
same.

**Why.** A PMIx server makes its local socket, and the directories its
clients and tools find it by, in its temporary directories. Before it
makes anything there, it checks the path to each of them, as OpenSSH
does for ``~/.ssh``:

* every directory *above* a tmpdir must be owned by root, by the
  server's user, or by an id from outside the server's user namespace,
  and nobody else may write it - unless it has the sticky bit, as
  ``/tmp`` normally does (mode ``1777``);
* the tmpdir *itself* may be anyone's, provided nobody but its owner
  can write it - or it must pass the rule above.

A user who could write such a directory could rename the server's
directories and put others in their place. The server's clients would
then fail to connect.

A Kubernetes ``emptyDir`` volume is created owned by root with mode
``0777`` - world-writable, *without* the sticky bit. Mounted at
``/tmp``, or anywhere above ``$TMPDIR``, it fails the check, and the
server refuses to start. Other container runtimes' scratch volumes can
do the same. This is the check working as designed. On a shared node
such a directory would be a real problem, but inside a pod usually
nobody else is there to exploit it.

**What to do.** Any one of these:

#. **Give the volume the sticky bit.** This keeps the check. Run
   ``chmod 1777`` on the mount point from an init container. It must
   run as root, or as the volume's owner:

   .. code-block:: yaml

      spec:
        initContainers:
        - name: sticky-tmp
          image: busybox
          command: ["chmod", "1777", "/tmp"]
          securityContext:
            runAsUser: 0
          volumeMounts:
          - name: tmp
            mountPath: /tmp
        containers:
        - name: app
          # ...
          volumeMounts:
          - name: tmp
            mountPath: /tmp
        volumes:
        - name: tmp
          emptyDir: {}

#. **Put the server's tmpdirs elsewhere.** Point ``TMPDIR`` (or the
   host's ``PMIX_SERVER_TMPDIR`` / ``PMIX_SYSTEM_TMPDIR``) at a directory
   that passes - for example the image's own ``/tmp``, by not mounting
   the ``emptyDir`` there. A subdirectory of the ``emptyDir`` does
   *not* help: the volume is still a directory above it.

#. **Tell the server not to refuse.** Set the MCA parameter
   ``ptl_base_tmpdir_check``:

   * ``refuse`` - the default: report the directory and fail to start;
   * ``warn`` - report the directory, and start anyway;
   * ``silent`` - do not check.

   In the pod spec:

   .. code-block:: yaml

      env:
      - name: PMIX_MCA_ptl_base_tmpdir_check
        value: "warn"

   or in the image's ``$prefix/etc/pmix-mca-params.conf``:

   .. code-block:: text

      ptl_base_tmpdir_check = warn

   so that every daemon started from the image has it. See :doc:`/mca`
   for the other ways to set a parameter.

   Use ``warn`` or ``silent`` only where no other user shares the
   directory - a pod's own volume is such a directory. They cost only
   availability, not identity: a client still refuses a server whose
   uid is not the one it expects, whatever is at the socket's name.

The same parameter covers a tmpdir the check misjudges for other
reasons, such as a directory on an NFS export with ``root_squash``,
which the server sees as owned by ``nobody``.


Keep the temporary directory's path short
-----------------------------------------

The local socket is created under the server's temporary directory,
and a socket's path is limited to about 100 characters. A server whose
tmpdir path is too long fails to start. Volumes mounted at deep paths
can exceed it. Mount the volume at a short path, or set a short
``TMPDIR``.


User namespaces
---------------

In a container that runs in a user namespace (in Kubernetes,
``hostUsers: false``), a directory made outside the namespace - by the
node, or by the container's setup - can be owned by an id with no entry
in the container's ``/proc/self/uid_map``, which the kernel shows as
the overflow id (usually 65534, ``nobody``). The tmpdir check counts
such an owner as root, so these directories are not refused for their
owner. They are still refused if others may write them and they are
not sticky.

For more on PMIx temporary files, see "Temporary files" in
:doc:`authentication`. :doc:`/how-things-work/ptl` describes the check
in detail.
