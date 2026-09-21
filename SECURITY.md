# OpenPMIx Security Policy

Historically, PMIx has operated as a library at the user level in relatively
controlled environments — the typical high-performance computing cluster
embedded in a protected network, and thus not exposed to the general Internet.
That situation has evolved over time, as system management stack packages and
other software operating at privileged levels have begun to operate PMIx
servers, and with the advent of containers inside which users operate as
"root". As a result, the PMIx library now sometimes finds itself in situations
where its communications are between entities at different privilege levels
(e.g., between root and user), and the head node it operates on sometimes has
a direct connection to more exposed networks.

The PMIx community takes security associated with use of its library
seriously, recognizing that our ability to respond to concerns is bound by our
limited access to volunteer resources. We deeply appreciate coordinated
efforts done in partnership with our reporters, as these have the highest
probability of a successful and satisfactory resolution. Reports that simply
state something is wrong while providing no assistance in triaging the problem
or developing the solution will be treated seriously, but with correspondingly
longer response times.

PMIx does not have formal, contractual relationships with its users. Instead,
we have informal relationships with downstream packagers (e.g., Debian,
Fedora, SUSE), resource managers (e.g., Slurm, PBS, PALS), and libraries
(e.g., Open MPI, MPICH, OpenSHMEM, PGAS). This quite frequently takes the form
of individual rather than organizational contacts. It is therefore not
possible for the PMIx community to offer any guarantee as to the breadth or
immediacy of notification for security issues.

While we recognize that reporters may have a preferred process for dealing
with security-related issues, our limited available resources restricts us
to the somewhat informal process described below. We appreciate your
understanding.

## Reporting a vulnerability

**Report potential security issues privately, through GitHub, using the
"Report a vulnerability" form:**

> <https://github.com/openpmix/openpmix/security/advisories/new>

You can also reach the form from the repository's **Security** tab: choose
**Report a vulnerability** under *Advisories*. GitHub's own instructions for
this are at
<https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/report-privately>.

This opens a private conversation visible only to you and the OpenPMIx
maintainers. Please do **not** open a public GitHub issue, post to a mailing
list, or submit a public pull request for a suspected vulnerability before we
have had a chance to respond — a pull request is public the moment it exists,
including its diff and its commit messages.

A report is most useful when it contains:

* the output of `pmix_info --all` from the affected build, or, failing that,
  the PMIx version, how it was configured, and the hwloc and libevent versions
  it was built against;
* the environment: operating system, resource manager, which `psec` component
  is in use, and whether the PMIx server in question runs as a user, as a
  system daemon, or as root;
* a minimal reproducer — the program or command lines, any MCA parameters, and
  the client/server or tool/server arrangement needed to demonstrate the
  problem;
* what an attacker gains, and what access they need to begin with (an
  unprivileged local account on the same node, a different user on the same
  node, an unauthenticated peer on the network, and so on);
* any suggested fix, and how you would like to be credited — the commit
  that carries the fix is where that credit appears.

## What happens after you report

We handle vulnerabilities with the same small, volunteer-driven process we use
for every other defect, because that is what our resources support. In
particular, **OpenPMIx does not operate a formal notification, embargo, and
private-release procedure**, as earlier versions of this policy described. We
are not able to maintain a vetted list of embargoed consumers, coordinate a
multi-party disclosure timetable, or produce private pre-release tarballs, and
we would rather say so plainly than promise a process we cannot execute.

What we do instead:

1. **Triage in the private advisory thread.** We work with you to confirm the
   issue, understand its scope, and judge its severity. Expect questions; the
   conversation stays private throughout.
2. **Develop the fix on a private branch.** Work proceeds out of public view,
   so the defect is not advertised by its own repair. We may ask you to review
   or test a candidate patch.
3. **Ship the fix in the next release.** The fix merges to the supported
   branch and is included in the next release of that series. OpenPMIx
   releases are infrequent and are driven primarily by accumulated bug fixes,
   so a fix may sit on the branch for some time before a release carries it. A
   sufficiently severe issue can prompt a release sooner than the normal
   rhythm, but we cannot commit in advance to a schedule.

If the reported behavior turns out not to be a vulnerability, we will say so
and explain why, and — where there is a real defect underneath it — move the
discussion to a public issue and fix it there like any other bug.

## Supported versions

Support for OpenPMIx is limited to infrequent releases driven primarily by
collected bug fixes.

| Series | Status |
|--------|--------|
| v7.0.x | Supported once released — fixes land here |
| earlier series | End of life; no fixes, including security fixes |

**Fixes are not backported to earlier release series.** If you are running an
older series, the remedy for a security issue is to upgrade. Two properties of
PMIx make that easier than it may sound, and both are described in
[Version Numbers and Binary Compatibility](https://docs.openpmix.org/en/latest/versions.html):

* **Forward ABI compatibility within a series.** An application built against
  an earlier release of a given series runs against a later one, so a
  fix release can be dropped in without rebuilding what links to it.
* **Cross-version compatibility between client and server.** A client and a
  server need not be the same PMIx version, so the side you can update need
  not wait for the side you cannot.

## Scope

This policy covers the PMIx library itself — the client, server, and tool
libraries, the PMI-1 and PMI-2 compatibility interfaces, the tools shipped in
this repository, and the components under `src/mca`.

Issues in closely related components belong to their own projects, and we will
happily help you find the right contacts and coordinate:

* **PRRTE** (the PMIx Reference RunTime Environment): report it privately
  through that repository's own form,
  https://github.com/openpmix/prrte/security/advisories/new, under the policy
  in [`prrte/SECURITY.md`](https://github.com/openpmix/prrte/blob/master/SECURITY.md),
  which mirrors this one. PRRTE consumes a great deal of PMIx internal
  machinery, so if you are unsure which side of that boundary a problem lies
  on, report it to either and say so; we will route it.
* **libevent, hwloc, MUNGE, resource managers** (Slurm, PBS, LSF, PALS, Flux),
  and third-party `psec`, `ptl`, or other MCA plugins: these are separate
  upstreams. Raise the issue with us if you cannot tell where it belongs, and
  we will work out where the fix has to be written.

A few behaviors are intentional and are not, by themselves, vulnerabilities:

* **Authentication is opt-outable.** The `psec/none` component validates any
  credential as good. It refuses to be selected unless the deployment names it
  explicitly, which is the point: an environment that asks PMIx to skip
  connection authentication gets what it asked for. A way to reach `none` — or
  to have a credential accepted — *without* that explicit request is very much
  a vulnerability.
* **Channels are authenticated, not encrypted.** `psec` authenticates a
  connection while it is being established; the traffic that follows is not
  encrypted. A party that already holds the access needed to read a PMIx
  channel is outside the model, rather than a finding on its own. A way to
  obtain that access — to reach another user's rendezvous socket, session
  directory, or shared-memory segment — is in scope.
* **A namespace is a trust domain.** A client that legitimately authenticates
  may ask its server for the things its namespace is entitled to. What *is* a
  vulnerability is a client reaching across that line: reading another
  namespace's or another user's data, or driving an operation on their behalf.
* **Running a PMIx server as root.** A server started as root with paths it
  was handed by a user can damage what those paths point at; that risk belongs
  to whoever configured it that way. A privilege escalation reachable by an
  unprivileged client against a correctly configured root server is a
  vulnerability.

If you are unsure whether what you have found is in scope, report it. We would
much rather read a report that turns out to be benign than miss one that is
not.

## Software authenticity and integrity

The authenticity and integrity of PMIx software should always be confirmed by
computing the checksum of the downloaded archive and comparing it against the
value listed on the
[GitHub release page](https://github.com/openpmix/openpmix/releases). Assuming
you downloaded the file `pmix-7.0.0.tar.bz2`, you can run the `sha1sum`
command like this:

```sh
shell$ sha1sum pmix-7.0.0.tar.bz2
```

Check that the output matches what is printed in the release announcement,
which may look like this:

```sh
b4e1cb79dfd94c1b9db8eaba02f725c07ef9df2b  pmix-7.0.0.tar.bz2
```

To avoid having to compare the string by eye, use `sha1sum -c`:

```sh
shell$ echo 'b4e1cb79dfd94c1b9db8eaba02f725c07ef9df2b  pmix-7.0.0.tar.bz2' | sha1sum -c
```

Note that a checksum confirms only that you received the archive the release
page describes. It says nothing about a snapshot tarball built from an
arbitrary Git commit, and nothing about a tree you assembled yourself — for
those, verify the Git history you built from.
