# PMIx group-test "swarm"

A small, self-contained harness for exercising **PMIx group construction**
across several container "nodes" — real, separate PMIx servers (one `prted` per
node) so the server-side local-completion and lost-connection accounting
actually run on more than one daemon. It is the quickest way to run the group
example programs the way they are meant to be run: distributed across nodes,
including the case where a participant dies mid-construct.

It is **not** a Docker Swarm in the orchestration sense — just ten plain
`ubuntu:24.04` containers on one bridge network, each acting as a node. "Swarm"
is only the nickname.

> This harness is adapted from PRRTE's `contrib/dockerswarm`, inverted to be
> **PMIx-centric**: the code under test is *your live openpmix working tree*
> (bind-mounted, built out-of-tree, never stale, no commit required). PRRTE is
> just the launcher — its master branch is baked into the image as source and
> built **against your PMIx** at run time, so the launcher's PMIx *server*
> library is the one you are testing.

---

## 1. What's here

| File | Purpose |
|------|---------|
| `build.sh` | Builds PMIx from your **live** tree (VPATH) plus PRRTE master against it: into a shared volume for the Linux swarm, or natively for macOS. Start here. |
| `run-tests.sh` | Runs the group **construction-method** example programs (plus the construct/connect loss and voluntary-leave cases) and reports PASS/FAIL: full multi-node suite on Linux, single-host subset on macOS. |
| `run-group-events.sh` | Runs the **dynamic, event-driven** group exercisers and their fault paths (invite/join, member lost during destruct, ...); kept separate from `run-tests.sh` so the event/fault matrix can grow independently. Same `linux`/`macos` modes. |
| `run-topology.sh` | Runs the **topology + locality** exerciser across real nodes: each rank loads the topology its *local* server published (hwloc shmem, with XML as the fallback) and compares `PMIX_LOCALITY_STRING` with every peer. The only place `src/hwloc` gets a multi-node answer -- on one host every peer is a node-mate, so a single-host run cannot tell a correct result from one that claims everything is local. Same `linux`/`macos` modes. |
| `run-python.sh` | Runs the **Python bindings**: the standalone unit suite, the connected client/server round-trip, and Python PMIx clients spread across nodes. Same `linux`/`macos` modes. See §10. |
| `run-client-tests.sh` | Runs the `src/client` API surface across the swarm, so the ranks sit behind **different** PMIx servers. This is the multi-node case for the client library: everything in `src/client` either answers locally or round-trips to a server, and a singleton exercises none of the second half. See §12. |
| `run-common-tests.sh` | Runs the `src/common` role-shared APIs (query, log, job control, allocation, monitoring, IOF) across the swarm. This is the multi-node case for the shared layer: like `src/client`, almost everything in `src/common` either answers locally or round-trips to a server or host, and the monitor's local/remote split cannot even be entered on one node. See §13. |
| `run-class-tests.sh` | Runs `test/unit/class` in the two configurations a developer's own `make check` does not cover: **Linux**, and **`--disable-debug`** with default symbol visibility. Deliberately *not* a multi-node test — see §11. |
| `run-mca-tests.sh` | Runs `test/unit/mca` plus the hostile MCA-parameter cases in the `--enable-mca-dso` configuration, where the component repository is actually exercised. See §14. |
| `run-bfrops-tests.sh` | Runs the `src/mca/bfrops` unit programs on Linux in an optimized `--enable-mca-dso` build, **and** moves one value of every PMIx data type between ranks on *different* nodes. The only place the peer-assigned bfrops module and the negotiated buffer type are observable at all. See §15. |
| `run-gds-tests.sh` | Runs the `src/mca/gds` datastore suite. See §16. |
| `run-ptl-tests.sh` | Runs `src/mca/ptl` -- the transport itself -- over real sockets between real hosts: interface selection, node-local rendezvous discovery, a tool attaching across nodes, the inbound message-size ceiling, and an exhausted listener port range. See §17. |
| `swarm-common.sh` | Sourced by all the scripts above: which swarm to drive (`PMIX_SWARM`), how to reach a node, and how to clean one. The three runners each carried their own copy of that once, and the copies drifted. |
| `python/` | The swarm's own Python clients (`swarm_client.py`, `swarm_group.py`, `swarm_cpuset.py`, `swarm_datatypes.py`, `swarm_nonblocking.py`). |
| `Dockerfile` | Base image: toolchain, PRRTE master *source* (autogen'd), SSH wiring, node entrypoint. It contains **no** PMIx and **no** built PRRTE. |
| `docker-compose.yml` | The ten nodes `pmix-node1`..`pmix-node10`, each mounting the shared `pmix-build` volume. Every one of those names derives from `$PMIX_SWARM`, so two clones can each run a swarm — see §4. |

## 2. How it works

```
        your live openpmix tree   (bind-mounted read-only)
                   │
   ┌───────────────┴───────────────┐
   │ build.sh linux                │ build.sh macos
   ▼                               ▼
 builder container              native on host
 PMIx  VPATH -> /opt/prte/pmix   PMIx  -> vpath-macos-pmix/install
 PRRTE VPATH -> /opt/prte/prte   PRRTE -> vpath-macos-prte/install
 group clients -> /opt/prte/tests   (PRRTE built --with-pmix=<your PMIx>)
   │                               │
   ▼                               ▼
 10 nodes mount /opt/prte:ro     run-tests.sh macos
 run-tests.sh linux            (single-host smoke)
```

- **PMIx is the tree under test.** PRRTE is built `--with-pmix` your PMIx, so
  `prted` (the PMIx server) links the library you just changed.
- **Never stale, no commit.** Edit a file, rerun `build.sh`, and the swarm runs
  your change (the build is incremental).
- **PRRTE is baked as source.** The image clones PRRTE `master` and runs
  `autogen.pl`; `build.sh` only configures + builds it (against your PMIx) into
  the shared volume. Override the branch with `PRRTE_REF=... ./build.sh image`.

## 3. Prerequisites

- Docker (with `docker compose`) and `git` for the Linux swarm.
- A working autotools toolchain (`autoconf`/`automake`/`libtool`/`perl`) on the
  host for `autogen.pl` and the macOS build.
- **Network access during the first image build** (clones PRRTE + submodules,
  installs apt packages).

## 4. Quick start

```sh
# from this directory (contrib/dockerswarm/)

# ---- Linux swarm ----
./build.sh                 # distclean+autogen your PMIx (once), build image,
                           #   build PMIx + PRRTE + group clients into the volume
docker compose up -d       # start pmix-node1 .. pmix-node10
./run-tests.sh linux       # multi-node group construction suite
./run-group-events.sh linux # dynamic invite/join + group fault paths
./run-topology.sh linux    # topology handoff + cross-node locality
./run-python.sh linux      # Python bindings: units, round-trip, multi-node
./run-client-tests.sh linux # src/client APIs across separate PMIx servers
./run-common-tests.sh linux # src/common role-shared APIs across separate servers

# ---- native macOS (single host) ----
./build.sh macos           # native PMIx + PRRTE build under vpath-macos-*
./run-tests.sh macos       # single-host group smoke
./run-group-events.sh macos # single-host dynamic-group smoke
./run-topology.sh macos    # single-host topology-handoff smoke
./run-python.sh macos      # single-host bindings smoke (most checks SKIP)
./run-client-tests.sh macos # single-host client smoke (one server, not the point)
./run-common-tests.sh macos # single-host common-API smoke (one server)
```

Rebuild after editing PMIx: rerun `./build.sh` (incremental). No image rebuild
and no `docker compose` restart needed — the nodes read the shared volume.

> **`./build.sh` distcleans your in-tree build every time it finds one — not
> just the first time.** The "(once)" above describes the *autogen*, not the
> distclean: the VPATH configure it runs cannot coexist with an in-tree
> build, so any tree you have configured for a native `make` / `make check`
> is torn down, Makefiles and all. The next plain `make` then fails with
> "No targets specified and no makefile found."
>
> This bites the ordinary review workflow, where you alternate between
> `make check` on the host and the swarm suites: every swarm rebuild costs
> you a `./configure && make` to get the host tree back. Nothing is lost —
> your source edits are untouched — but budget for it, and re-run configure
> with the *same* options (recover them with `./config.status --config`
> before you build, or from `config.log`, since config.status goes away
> too).

### Two clones on one host: `PMIX_SWARM`

Every global name this harness claims — the compose project, the ten container
names, the build volume, the docker network — is derived from `$PMIX_SWARM`, so
a second clone (or a second agent) can drive its own swarm:

```sh
export PMIX_SWARM=alt      # for the WHOLE shell; see the warning below
./build.sh                 # -> volume alt-build
docker compose up -d       # -> project altswarm, containers alt-node1..10
./run-tests.sh linux       #    on network altswarm_dvm
```

Unset, it is `pmix` and every name is exactly what it has always been: project
`pmixswarm`, containers `pmix-node1..10`, volume `pmix-build`.

Both swarms contain a container whose **hostname is `node1`**, and that is
fine: each user-defined bridge network runs its own embedded DNS serving only
the containers attached to it, so `node2` inside a swarm can only ever mean
that swarm's node2. No test needs changing — `--host node1:2,node2:2` means the
right machines in either.

> **Export it, don't prefix one command.** `docker compose` interpolates
> `docker-compose.yml` itself, so `PMIX_SWARM` has to be in *that* command's
> environment. A `docker compose up -d` without it quietly brings up the
> **default** swarm instead, against the default volume, and your build sits in
> `alt-build` unused. The runners say which swarm they looked for when they
> find nothing, and `build.sh` prints the exact next command including the
> variable.

The name is a host-wide namespace, and PRRTE's identical harness uses the same
mechanism (`PRTE_SWARM`) — so `PMIX_SWARM=alt` here and `PRTE_SWARM=alt` there
would both want a container called `alt-node1`. Pick something of your own
(your initials, the branch you are on) rather than a generic word.

**What is still shared, and what that costs.** The base image
(`pmix-swarm:latest`) is read-only to a running swarm and expensive to build —
it clones and autogens PRRTE — so both instances use the same one. Rebuilding
it moves it under the other swarm's feet, and that swarm's containers then fail
the "containers are running the current image" preflight until they are
recreated (§8). Nothing else crosses: separate containers, separate `/tmp`,
separate install, separate network.

**The macOS subsets need no such knob** — their build is already per-clone
(`vpath-macos-pmix`, `vpath-macos-prte`) — but they isolate the two things that
are not. They start `prterun` and every example by absolute path out of their
own install and match on that path when reaping strays (a bare `pkill -9
prterun` kills the other clone's launcher, and a bare `pgrep -x prte` is worse:
it reports the other clone's DVM as this one's and passes a case on it), and
they run PRRTE under a private `TMPDIR` they create and remove, so no session
directory they delete was ever anyone else's.

## 5. Driving it by hand

`run-tests.sh` automates this, but to poke at it yourself:

```sh
RUN='docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 pmix-node1 bash -lc'

# run a group example across two nodes (a group then spans two PMIx servers)
$RUN '. /opt/prte/env.sh; prterun --host node1:2,node2:2 -np 4 --map-by node group'

# the other construction methods
$RUN '. /opt/prte/env.sh; prterun --host node1:2,node2:2 -np 4 --map-by node group_bootstrap'
```

`. /opt/prte/env.sh` puts the shared-volume install on `PATH`/`LD_LIBRARY_PATH`
in a non-login `docker exec` shell (login shells get it automatically).

The group example programs (from the PMIx `examples/` directory) are built as
standalone clients into `/opt/prte/tests`:

| Program | Construction method exercised |
|---------|-------------------------------|
| `group` | basic `PMIx_Group_construct` over a proc subset, then destruct |
| `group_bootstrap` | bootstrap construction (members join a named group) |
| `group_dmodex` | group + direct modex data exchange |
| `group_lcl_cid` | local context-id assignment |
| `asyncgroup` | non-blocking `PMIx_Group_construct_nb` |
| `multi_nspace_group` | a group spanning more than one namespace |

## 6. What "success" looks like

Each group example prints `Group construct complete` on its constructing ranks
and exits cleanly, with no `prted` left behind on any node. The multi-node
launch spreads the ranks across daemons (`--map-by node`), so the group's
local phase is assembled and completed on more than one PMIx server.

**Participant death.** The `participant death during group construct` check
launches a variant in which one rank exits before calling the construct; the
group must still complete on the survivors rather than hang. A hang (the run
hitting `--timeout`) is the pre-fix failure mode — group collectives received
no lost-connection accounting at all, so the construct never reached its
expected count. This check is skipped until the `group_die` client is built
alongside the group loss-accounting fix.

## 7. Cleanup hygiene

`run-tests.sh` cleans up between tests; if you drive things by hand, clear
stale state on **every** node between DVM runs — the same sweep `cleanup_swarm`
does (`swarm-common.sh`):

```sh
for n in $(seq 1 10); do
  docker exec pmix-node$n sh -c '
    for t in prted prte prterun prun pterm; do pkill -9 -x $t; done
    rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/ompi.* /tmp/pmix*
    find /tmp -maxdepth 2 -name "pmix*" -prune -exec rm -rf {} +
    true'
done
```

**Every tool has its own session-dir prefix, and one missed prefix is enough.**
`prte.<pid>` is the HNP, `prtrn.<pid>` is `prterun`, `prted.<pid>` is a daemon
standing on its own, `prun.<pid>` is `prun` itself, `ompi.<pid>` is anything run
under the ompi personality. Each holds a `pmix.*` server rendezvous file, a
system-level server drops one straight into `/tmp`, and any one left behind
makes the next tool report *"multiple possible servers … connection handles have
been read from files named pmix.\*"* and fail to find the DVM. That is why the
sweep ends with a `find`: it catches the rendezvous file of a session dir whose
prefix this list has not heard of. (PRRTE hit exactly this —
[prrte#2526](https://github.com/openpmix/prrte/issues/2526) — where a
hand-driven `prterun` before a suite run made later cases fail for reasons that
had nothing to do with the code.)

The tools are killed, not just the daemons: a live `prun` or `pterm` holds a
rendezvous file of its own, and reaping it is the point of a teardown.

If you are running a second swarm (`PMIX_SWARM`, §4), the loop above is
per-swarm — use that swarm's container names. Nothing in it can reach the other
swarm's `/tmp`, because the containers are different containers.

## 8. Rebuilding / resetting

| Want to… | Do |
|----------|----|
| pick up a PMIx source edit | `./build.sh` (incremental into the volume) |
| force a clean rebuild | `docker volume rm pmix-build && ./build.sh` |
| move the baked PRRTE forward | `docker build --no-cache --build-arg PRRTE_REF=master -t pmix-swarm:latest .`, then wipe the volume's VPATH dirs and recreate the containers - see below |
| rebuild the base image (different PRRTE branch) | `PRRTE_REF=v3.0.x ./build.sh image` |
| tear down the swarm | `docker compose down` (the `pmix-build` volume persists) |
| run a second, independent swarm | `export PMIX_SWARM=alt` and repeat the quick start — see §4. Every command in this table then names that swarm's volume (`alt-build`) and containers (`alt-node*`), and **a `docker compose down` without the variable takes down the *default* swarm** |

### The image goes stale, and the containers go stale after it

Two traps, and they compound. Both were hit for real.

**`./build.sh image` does not necessarily move the baked PRRTE.** The
Dockerfile builds the launcher's source from

```dockerfile
RUN git clone --recursive -b "$PRRTE_REF" "$PRRTE_REPO" /src/prrte
```

and docker caches that layer by its *text*, not by what the remote now
contains - so a rebuild "succeeds" in seconds and bakes exactly the PRRTE
you were trying to leave behind. This matters here more than it would in
most images, because PRRTE master is what tracks new PMIx capability flags:
an old baked PRRTE fails to `configure` against your PMIx, or builds and
then cannot launch it.

**The ten nodes are long-lived containers, so a rebuilt image does not
reach them until they are recreated.** `build.sh` compiles PMIx and PRRTE
inside a *new* container and installs them into the volume the *old*
containers read, so the daemons load libraries built for a different
launcher. The symptom is an undefined symbol, or a launcher that does not
understand an option, in whichever case reaches it first - and nothing in
the output mentions containers. `run-tests.sh`'s preflight now compares
each container's image ID against `pmix-swarm:latest` and refuses to run
when they differ.

The full sequence when you want a current launcher:

```sh
docker build --no-cache --build-arg PRRTE_REF=master -t pmix-swarm:latest .
docker run --rm -v pmix-build:/opt/prte pmix-swarm:latest \
    rm -rf /opt/prte/vpath-pmix /opt/prte/vpath-prrte \
           /opt/prte/pmix /opt/prte/prte
./build.sh                                  # rebuild against the new image
docker compose up -d --force-recreate
```

The volume wipe is not optional: `build.sh` reconfigures when the configure
*arguments* change, and they do not change when only the image's PRRTE does.

**Where you run `docker compose` matters.** The project name defaults to the
directory name, `dockerswarm` - which is also the name of PRRTE's identical
harness directory. Run it against the wrong project and compose adopts the
other swarm: it stops those containers, renames these ones out from under
themselves, and then fails on the name collision, leaving this swarm on the
previous image. That is exactly how the state above was reached (from the
PRRTE side). `docker-compose.yml` now pins `name: pmixswarm`, so a plain
`docker compose` from this directory can only ever mean this swarm. Check by
hand with:

```sh
docker inspect pmix-node1 --format '{{.Image}}'
docker images --no-trunc --format '{{.ID}}' pmix-swarm:latest
```

## 9. Topology reference

| Container | hostname | role |
|-----------|----------|------|
| `pmix-node1` | node1 | head node — start the DVM here, run all tools here |
| `pmix-node2`..`pmix-node10` | node2..node10 | additional daemon nodes |

Network: bridge `pmixswarm_dvm`. All nodes mount the shared `pmix-build` volume read-only
at `/opt/prte`, where `build.sh` installs PMIx (`/opt/prte/pmix`), PRRTE
(`/opt/prte/prte`), the group clients (`/opt/prte/tests`), the Python bindings
and their scripts (`/opt/prte/tests-python`), and writes `/opt/prte/env.sh`. To
add or remove nodes, copy or delete a service block in `docker-compose.yml`
(and adjust the `seq 1 10` loops in `swarm-common.sh` and the runners).

The container, volume, and network names above are the `PMIX_SWARM=pmix`
default; under another name they all shift together (§4). The **hostnames**
never do — `node1`..`node10` is what the tests name, in every swarm.

---

## 10. Python bindings

`build.sh` configures PMIx with `--enable-python-bindings` and stages the built
extension, the maintained scripts from [`test/python/`](../../test/python/), and
the swarm's own clients from [`python/`](python/) into `/opt/prte/tests-python`.
`env.sh` puts that directory on `PYTHONPATH`, so any node can just
`import pmix`. Drive it with `./run-python.sh linux`.

**Why here and not only in `make check`.** Two gaps this closes:

- **Linux coverage.** The bindings are normally built only on a developer's
  machine and in CI. This gives them a real Linux build (Ubuntu 24.04, distro
  Cython) on every `./build.sh`.
- **Real multi-node clients.** The in-tree `server.py`/`client.py` round-trip
  runs a client and server in one process tree on one host, so every
  `PMIx_Get` is answered out of the local datastore. Under the swarm, a rank's
  peers sit behind a *different* `prted`, so a peer get is a genuine server
  round trip and travels through the bindings' unload path.

There is a third gap that only Linux can close: **several cpuset and topology
methods cannot run on macOS at all.** Darwin exposes no CPU-binding API, so
`hwloc_get_cpubind` fails and `get_cpuset` returns `PMIX_ERR_NOT_FOUND` while
`compute_distances` returns `PMIX_ERR_UNREACH` regardless of what the bindings
do. `swarm_client.py` exercises them for real here.

| Script | What it covers |
|--------|----------------|
| `test_bindings.py` (from `test/python/`) | the standalone unit suite — import, class hierarchy, constants, the stateless `*_string` and cpuset converters |
| `server.py` + `client.py` (from `test/python/`) | the connected round-trip; the only script driving the **server** bindings (`register_nspace`/`register_client`/`setup_fork`) and the upcall path |
| `python/swarm_client.py` | multi-node client: `put`/`commit`/`fence`/`get` of every peer's data across nodes, then the client-role topology calls — `load_topology`, `get_cpuset`, `compute_distances`, `parse_cpuset_string`, `get_relative_locality` |
| `python/swarm_cpuset.py` | the **server-role** cpuset calls against real hwloc — `generate_cpuset_string`, `generate_locality_string` — which a launched client rank cannot reach (they wrap `PMIx_server_generate_*`), plus their malformed-input guards. Runs standalone, no launcher |
| `python/swarm_group.py` | multi-node groups from Python, both `group_construct`/`_destruct` and the non-blocking `_nb` forms whose callbacks run on the progress thread |
| `python/swarm_datatypes.py` | every data type the conversion layer supports, written by one rank and read by a peer behind a different prted, plus a `data_pack`/`data_unpack` round trip of the same values. The in-tree unit suite runs the loader against the unloader, so a mistake *both* make — the element size of an array, the byte order of a coordinate — cancels out; the library's packer does not forgive it |
| `python/swarm_nonblocking.py` | the `_nb` family (fence, get, publish/lookup/unpublish, connect/disconnect, query, log, job_control) against a remote server. A request answered out of the local datastore completes before the method returns, so nothing the keepalive caddy holds has to survive — only a remote peer makes it outstanding |

Each swarm client prints one `PMIXPY <rank> <PASS|FAIL> <name>` line per check
and a final `PMIXPY <rank> DONE <nfail>`; a client exits non-zero if any check
on its rank failed.

**The `DONE` line is load-bearing.** A client that dies partway — an
exception, a segfault — simply stops printing, so a driver that counts only
PASS/FAIL lines reports a clean run for a client that never finished. That is
not hypothetical: it hid a `TypeError` in these very scripts. `run-python.sh`
requires one `DONE` per expected rank. Keep that contract if you add a client.

Build the bindings out with `PYTHON_BINDINGS=no ./build.sh` if you want a
faster PMIx build and do not care about them.

### A defect this harness found

`swarm_cpuset.py` is where a real library defect surfaced, and the checks
there are now its regression test:

- `PMIx_server_generate_locality_string` emits a bare string,
  `SK0:L20:L10:CR0:HT0:NM0` — with no `hwloc:` prefix. PRRTE stores exactly
  that as `PMIX_LOCALITY_STRING`.
- `PMIx_Get_relative_locality` *required* a `hwloc:` prefix and otherwise
  returned `PMIX_ERR_TAKE_NEXT_OPTION`.
- But [`include/pmix.h`](../../include/pmix.h) documents that function's
  arguments as *"String returned by the `PMIx_server_generate_locality_string`
  API"*.

So a caller following the documented contract got `-1366` and no locality
bits at all — every consumer of `PMIX_LOCALITY_STRING` saw unrelated
processes. Fixed by teaching the consumer to accept either spelling
(`pmix_hwloc_locality_payload`); see `src/hwloc/AGENTS.md` item 8.

The in-tree unit test had missed this for years because it used hand-written
`"hwloc:NM0:SK0:CR0:HT0"` literals rather than the generator's output. That
is the lesson worth keeping: **when testing a consumer, feed it the
producer's real output.** The multi-node harness caught it because the
Python clients do exactly that.

> **Note:** adding Python to the image changed the `Dockerfile`, so the first
> run after picking this up needs `./build.sh image` (or any `./build.sh` on a
> machine with no `pmix-swarm` image yet). That re-clones PRRTE and takes a
> while; subsequent builds are incremental as before.

---

## 11. The `src/class` unit suite (`run-class-tests.sh`)

```
./run-class-tests.sh linux    # both configurations, in the container
./run-class-tests.sh macos    # both configurations, natively
```

**This is not a multi-node test, and it is not trying to be.** Everything
in [`src/class`](../../src/class) — the object model, list, hash table,
pointer array, hotel, ring buffer, value array, bitmap — is a
single-process, in-memory data structure. Spreading a linked list over ten
containers tells you nothing that running it once does not. The distributed
dimension of these classes is *cross-process on one node*:
`pmix_hash_table_t` and `pmix_pointer_array_t` are TMA-aware precisely so
`gds/shmem3` can build them inside an mmap'd segment a server shares with
its local clients.

That cross-process dimension is covered directly by
`test/unit/class/class_tma_shared`, which this script picks up
automatically (the program list is read out of `Makefile.am`). It builds
those containers, and an object several processes retain and release at
once, inside a real `MAP_SHARED` segment, and verifies from a second
process that the storage and the reference count are genuinely shared —
the two claims `src/class/AGENTS.md` makes about the TMA path. It is also
the one member of the suite for which the ten-node contention pass below
is more than a load test. What it does *not* model is segment negotiation
and attach between **unrelated** processes: its second process comes from
`fork()`, so the segment lands at the same address and the embedded
`pmix_tma_t`'s function pointers stay valid. Real `gds/shmem3` peers do
neither, which is why `pmix_hash_table.c` skips its key-type assert for
TMA tables. That half stays with `run-tests.sh`, whose group cases drive a
real datastore on each node.

What this script is for is the two configurations `make check` on a
developer's machine does not produce:

1. **Linux.** The primary development host is macOS. This code has
   platform-sensitive corners: the width of `unsigned long` under a shift,
   C11 atomic codegen, what `calloc(0, n)` and `realloc(p, 0)` return,
   pthread scheduling.

2. **`--disable-debug`, default symbol visibility.** This is the bigger
   one. Several guards in `src/class` used to be compiled out unless
   `--enable-debug`, so the bugs they catch were only reachable in a build
   nobody tested — an un-init'd hash table divided by zero;
   `pmix_value_array_remove_item()` `memmove`'d a wrapped `size_t` length.
   And *every* tree in this workflow (yours, and `build.sh`'s) configures
   `--enable-debug`; the maintainer's also uses `--disable-visibility`.

### A defect this half found

The first run of the `--disable-debug`, default-visibility build failed to
link:

```
Undefined symbols for architecture arm64:
  "_pmix_ring_buffer_t_class", referenced from: _main in class_ring_buffer.o
```

`pmix_ring_buffer_t` was the one class in the directory whose descriptor
carried no `PMIX_EXPORT` — not on the declaration, not on the
`PMIX_CLASS_INSTANCE`. So `PMIX_NEW(pmix_ring_buffer_t)` cannot link
outside `libpmix` in any normal build.

That is not a theoretical break: this class's consumers are **downstream
projects building against PMIx's installed internal headers**, which is
exactly the population the missing export locks out. What kept it hidden
is that no code *inside* this repository uses the class, so the only
in-tree consumer able to catch it is `test/unit/class` — normally built
against a `--disable-visibility` tree, where nothing is hidden and it
links fine.

Two lessons, both worth carrying. The one from §10, a level down: **a
test that only ever runs in the maintainer's configuration only ever
tests the maintainer's configuration.** And: **an in-tree grep is not
evidence that an interface is unused** — `libpmix`'s internal headers
are installed, and things build against them.

### Two defects in the harness itself

Worth knowing about, because both hid behind a green-looking run.

**The ten-node contention pass had never run anything.** It staged the
programs with `cp test/unit/class/$p`, but in an uninstalled libtool build
that path is a `/bin/sh` **wrapper**; the executable is under `.libs/`.
The copied wrapper looked for `.libs/$p` relative to its new home, did not
find it, and exited 1 — on all ten nodes, every time, with a message the
runner discarded. It stages `.libs/$p` now (falling back to the plain
path). If you write a runner that ships `check_PROGRAMS` anywhere else,
remember this.

**A component rename wedges every pre-existing build directory.**
Automake records each component's `configure.m4` as a prerequisite of
`aclocal.m4`. Rename or delete a component directory — `gds/shmem2` →
`gds/shmem3` — and one of those prerequisites no longer exists, so GNU
make decides `aclocal.m4` must be remade, shells out to `aclocal-1.<n>`,
and dies: this image ships no autotools. The build directory is then
unusable for good, and the error ("`aclocal-1.18` is missing") points
nowhere near the cause. `build.sh` now checks the recorded prerequisites
and reconfigures from scratch when one has gone missing. If you hit this
in a tree `build.sh` does not manage, the fix is the same: delete
`config.status` and re-run `configure`.

> **Note on the `macos` half.** Autoconf refuses an out-of-tree build while
> the source directory itself holds a `config.status`. `build.sh` resolves
> that by running `make distclean` on your tree; a test script has no
> business doing that to a working build, so the optimized half *skips*
> with an explanation instead. Run `./build.sh macos` first (which
> distcleans), or use the `linux` half, which has no such restriction — the
> container builds both configurations against a read-only bind mount.

---

## 12. The `src/client` API suite (`run-client-tests.sh`)

```
./run-client-tests.sh linux    # across the swarm: several PMIx servers
./run-client-tests.sh macos    # natively: one server, a smoke pass
```

**This one *is* a multi-node test, and that is the whole point** — the
opposite of §11. Almost every function in [`src/client`](../../src/client)
is an instance of "the request could not be answered locally, so pack a
command and round-trip to the server". A singleton reaches none of it:
`PMIx_Fence` and `PMIx_Commit` short-circuit to success, `PMIx_Get` never
leaves the local datastore, and publish/lookup/spawn/connect all stop at
the "am I connected?" check. The in-tree
[`test/unit/client_api.c`](../../test/unit/client_api.c) covers what a
client can decide by itself; this covers what it cannot.

Spreading the ranks over several nodes matters because **each node runs its
own `prted`, hence its own PMIx server**. That is what makes these paths
real rather than loopback:

| Case | What it actually exercises |
|------|---------------------------|
| `client` | put/commit/fence/get where the answer lives behind another server |
| `dmodex` | direct modex: rank 0 delays its commit, so the other ranks' gets sit in the client's pending-request list — the coalescing path, where only the first requester sends and the rest are satisfied from the one reply |
| `nodeinfo` | the node/app/session realm directives, whose answers differ per node — the densest parser in the directory |
| `pub` | publish/lookup/unpublish resolving up through the daemons |
| `resolve` | `PMIx_Resolve_peers` / `PMIx_Resolve_nodes` with something to resolve that is not just "this node" |
| `dynamic` | `PMIx_Spawn` plus `PMIx_Connect`/`PMIx_Disconnect`, i.e. the multi-nspace job-info exchange in `PMIx_Connect_nb` |
| `spawn_reuse` | `PMIx_Spawn` carrying `PMIX_SETUP_APP_ENVARS`, which used to fail outright from a client with `PMIX_ERR_INIT` (the `pmdl` framework that does the harvest is opened only by the server and tool roles, and its "not open here" answer was treated as fatal). Also guards the caller's `const pmix_app_t` array against being edited in place, and a mistyped `PMIX_PARENT_ID` against being dereferenced |

`spawn_reuse` is here for a different reason from its neighbours. The others
need a server because the *data* they want lives behind one; this one needs a
real **launcher**. A singleton stops at the "am I connected?" check before it
reaches the directives under test, and `test/simple/simptest` cannot host it
either — its `spawn_fn` calls `PMIx_server_setup_application()` and then blocks
on the result while running *on* the server's progress thread, so the fake
launch deadlocks. No `make check` test spawns, which is why that has gone
unnoticed. See the third-sweep notes in
[`src/client/AGENTS.md`](../../src/client/AGENTS.md).

A final wide `dmodex` run (10 ranks over 5 nodes) drives the pending-request
list deep enough for the coalescing to matter.

**The group side of `src/client` is covered by `run-group-events.sh`, not
here.** That suite drives the invite/join negotiation, which is realized
entirely through cross-server event notification. Its
`group_invite_others` case belongs to this review: the leader invites the
other ranks and does *not* join, so the group forms on the invitees alone.
The library credits the leader's own answer against the membership — right
only when the leader is itself an invitee — and crediting it in this shape
resolves the invitation one answer early, marks the last accept a
non-responder, and aborts the (all-or-nothing) construct. Ranks are split
across two nodes precisely so the last accept has to cross a server
boundary to reach the leader, which is what makes it the late one.

Two more of its cases belong to the same review. `group_invite_suppress`
has the leader register an ordinary handler for
`PMIX_GROUP_INVITE_ACCEPTED` that ends the event chain — the documented
way for an application to say it handled an event — which used to
suppress the library's own answer counter and hang the invite forever
([openpmix#4059][i4059]); across servers each acceptance arrives as a
separate event chain, which is the case that matters.
`group_invite_nb` drives `PMIx_Group_invite_nb` and a real
`PMIx_Group_join_nb` callback: the non-blocking invite used to announce
nothing at all, so no group formed, and join used to complete before the
construct resolved and so returned no group data. Each invitee checks
that its callback carries the group id and the full membership, which on
this suite had to travel from the leader's daemon.

[i4059]: https://github.com/openpmix/openpmix/issues/4059

**What this suite still does not reach.** `PMIx_Fabric_*` needs a fabric
provider the swarm does not have, so the fabric register/update paths are
covered only by the parameter-validation cases in `client_api.c`.
`PMIx_Compute_distances`'s relay-to-the-server fallback only runs when the
client's own hwloc computation fails, which no test environment here can
arrange. Both gaps are recorded in
[`src/client/AGENTS.md`](../../src/client/AGENTS.md).

### Prerequisite: the clients must link *your* PMIx

The examples are **compiled in a throwaway builder container** and **run in
the long-lived node containers**. Only the shared volume is guaranteed to be
the same in both, so the script insists on `/opt/prte/pmix` — what
`./build.sh` installs — and refuses to fall back to a PMIx baked into the
image. It also repeats `run-tests.sh`'s "containers are running the current
image" preflight. Both failures are otherwise invisible: you get a missing
`pmix_common.h`, or an undefined symbol, and nothing in either message
mentions containers.

So the full sequence from a cold start is:

```sh
./build.sh                              # installs your PMIx into the volume
docker compose up -d --force-recreate   # if the nodes predate the image
./run-client-tests.sh linux
```

### macOS mode

`./run-client-tests.sh macos` compiles the same clients against the in-tree
library and runs them under a single local `prterun`. That still drives the
connected paths a singleton cannot reach — a real server, real
`PMIX_PTL_SEND_RECV` round-trips — but with one server it is a regression
smoke pass, not the multi-server case above. Use it when you have changed
`src/client` and want a quick answer before spending a swarm run.


## 13. The `src/common` API suite (`run-common-tests.sh`)

```
./run-common-tests.sh linux    # across the swarm: several PMIx servers
./run-common-tests.sh macos    # natively: one server, a smoke pass
```

`src/common` holds the public entry points shared by the client, server
and tool roles — query, log, job control, allocation, session control,
monitoring, credentials, IOF. Almost every one has the same shape: decide
whether the request can be answered here, and otherwise pack it and
round-trip it to a server or hand it up to the host.

[`test/unit/common_api.c`](../../test/unit/common_api.c) covers the first
half, because a singleton reaches nothing else — with no server,
`PMIx_Query_info` answers only the two ABI-version keys, `PMIx_Log` falls
back to the local `plog`, `PMIx_Job_control` and `PMIx_Allocation_request`
stop at the "am I connected?" check, and `PMIx_Process_monitor` never gets
as far as its local/remote split. This runner is the second half.

### What spreading the ranks buys

* **Query** — keys the local library does not hold take the full
  pack → `PMIX_PTL_SEND_RECV` → `query_cbfunc` → results-list path,
  including the "some of this resolved locally, ask for the rest" split
  in `pmix_parse_localquery`.
* **Log** — routes through the *server's* `plog` rather than the client's,
  which is the only way the `PMIX_ERR_NOT_AVAILABLE` fallback in
  `log_cbfunc` is ever taken.
* **Job control and allocation** — reach a host that actually implements
  them (prte does; `test/simple`'s `simptest` does not, which is why these
  cannot be `run_*.pl` cases), so the completion path runs against a real
  answer.
* **Monitoring** — this is the reason to spread out at all.
  `pmix_monitor_processing` sorts the requested targets into "local",
  "remote" or both, and merges `pstat` results with the host's. On one
  node every target is local, and the remote and mixed branches — where
  the host upcall and the result merge live — are never entered.
  `monitor_remote` and `monitor_multi` exist for exactly that.
* **IOF** — the forwarding path only exists once a server holds the read
  end of somebody else's stdout. The final case drives a job with
  `--output "file=DIR/%h/rank%R:pattern"`, which makes `pmix_iof_setup`
  expand the conversions and open per-rank sinks.

### Two things about the IOF case that will waste your time

**`PATTERN` is a qualifier on the `file` directive, not a directive.**
It attaches with a colon (`file=NAME:pattern`); comma-separating it is
rejected outright with "The specified output directive is not
recognized". And **without** it, a name containing `%` is not an error
and is not expanded — it is simply a stem with odd characters, and the
files come out as `%h-rank%R.<nspace>.<rank>.err`. That is correct
behavior, so a version of this case that omits the qualifier passes
against any library and proves nothing.

**`%h` names the host that WRITES the file, not the host the rank ran
on.** For a forwarded stream that is the daemon which received it, so a
four-rank job spread over two nodes can legitimately put every file under
one hostname directory. `src/common/pmix_iof.h` documents it that way;
PRRTE's `--help output` text says "the node the process ran on", which is
the looser reading. The test therefore asserts only that the conversions
were expanded at all — no `%` survives in any name — not which host they
named.

### macOS mode

`./run-common-tests.sh macos` compiles the same clients against the
in-tree library and runs them under a single local `prterun`. The
connected query/log/job-control paths are real, but with one server the
monitor's local/remote split collapses to "all local", so this is a
regression smoke pass rather than the multi-server case above.

## 14. The `src/mca/base` unit suite (`run-mca-tests.sh`)

```
./run-mca-tests.sh linux    # in the containers: static and --enable-mca-dso
./run-mca-tests.sh macos    # natively: in-tree, plus an --enable-mca-dso build
```

[`src/mca/base`](../../src/mca/base) is the MCA implementation itself —
the MCA variable registry, the component repository, and the framework
register/open/select/close lifecycle. Its unit suite lives in
[`test/unit/mca`](../../test/unit/mca) and runs under an ordinary
`make check`.

### This is not a multi-node test, and does not pretend to be

Everything in `src/mca/base` is process-local: it reads the environment
and parameter files, registers variables, loads plugins, and opens
frameworks. Nothing in it crosses a node boundary. The ten-node stage at
the end of the Linux run is a contention and load pass — ten independent
copies against one kernel and one CPU set — and confirms the staged
binaries work under the same runtime as the rest of the harness. It is
not a distributed test.

### What the swarm actually buys

**`--enable-mca-dso`, which is the whole reason this runner exists.**
Roughly half of `src/mca/base` is compiled but never executed in an
ordinary build:
[`pmix_mca_base_component_repository.c`](../../src/mca/base/pmix_mca_base_component_repository.c)
is entirely `#if PMIX_HAVE_PDL_SUPPORT`, and `find_dyn_components()` in
[`pmix_mca_base_component_find.c`](../../src/mca/base/pmix_mca_base_component_find.c)
does nothing at all when there are no DSOs to find. A static build walks
`framework_static_components` and stops — so the `project@path` parser,
the repository hash, the `dlopen`/`dlsym`/version-check sequence, the
per-file refcount, and the `show_load_errors` reporting wrapped around
them get **zero** coverage from `make check` on a normal tree. Two of the
memory errors the August 2026 review of that directory found were in
exactly that code.

Secondarily: Linux (the primary development host here is macOS, and this
directory reaches `getcwd`, `geteuid`, the home directory, `syslog` and
`dlopen`), and an optimized `--disable-debug` build, since
`register_variable()` and its neighbours guard developer errors behind
`assert()` and `#if PMIX_ENABLE_DEBUG`.

### The stages

1. **Static build, unit suite.** Against the tree `build.sh` already
   configured, as a baseline.
2. **`--enable-mca-dso` build, unit suite.** A separate prefix and VPATH
   directory, so this library never displaces the one the rest of the
   harness runs against.
3. **"The repository actually loaded components."** With DSOs, the only
   way `pmix_info --all` can name a component is by having `dlopen`ed
   it, so a count near zero means the dynamic path silently found
   nothing — precisely the failure a static build hides. The stage fails
   below ten.
4. **Malformed MCA parameters must not be memory errors.** Each case
   runs `pmix_info --all` with one hostile value and asserts the process
   **survives** — an exit status above 128 is a signal and fails. What it
   prints is deliberately not checked: a malformed MCA parameter is an
   ordinary user mistake, and the only contract is that it may not take
   the library down. The cases include an
   `mca_base_component_path` entry with no `@` delimiter and one with an
   over-long project name (both were a stack buffer overflow),
   `mca_base_verbose=syslogid:<str>` (a use-after-free), every accepted
   shape of `mca_base_component_show_load_errors` including the bare
   framework and `^` forms (a `NULL` dereference), and a deprecated
   synonym.
5. **Ten-node contention pass**, as described above.

The unit suite the first two stages run includes `mca_base_framework`,
whose subject is the register/open/close reference count. That one is
worth having in the DSO configuration specifically: a framework whose
open fails there has real components to tear down, where in the static
build the lists are empty.

### Things to know

**The build volume outlives your branch.** `test/unit/mca` is newer than
some trees sitting in `pmix-build`, and a `config.status` that predates a
directory has never heard of its `Makefile` — `make` then dies with
`test/unit/mca: No such file or directory`. The runner detects the
missing Makefile and re-runs `config.status --recheck` rather than
failing with a message that points nowhere. The same shape of problem,
from the other direction, is the `aclocal.m4` trap described in §8.

**`pmix_info` is used on purpose** for stages 3 and 4: it is the one
installed program that opens every framework, so a single run exercises
the repository across all of them rather than only the ones some client
happens to need.

### macOS mode

`./run-mca-tests.sh macos` runs the in-tree suite and then builds a
second, `--enable-mca-dso` tree out of tree and repeats stages 2–4
against it. It skips the second half if the source directory is
configured in place, because autoconf refuses a VPATH build alongside a
`config.status` in the srcdir — and a test script has no business running
`make distclean` on a working tree to get around that.

## 15. The `src/mca/bfrops` suite (`run-bfrops-tests.sh`)

```sh
./run-bfrops-tests.sh linux    # unit programs in two builds, then across nodes
./run-bfrops-tests.sh macos    # the single-process half only
```

`bfrops` is the serialization engine: everything that leaves a PMIx
process passes through it. The suite has two halves, and they are worth
separating because only one of them needs the swarm.

**The single-process half.** `test/unit/bfrops_darray` and
`test/unit/bfrops_malformed` (plus `bfrops_regex2`,
`bfrops_alloc_inherit` and `nested_darray`) pack and unpack inside one
process. Stages 1 and 2 build and run them twice: once in the tree
`build.sh` configured, and once in a separate `--enable-mca-dso`,
`--disable-debug` tree. That is not a multi-node claim — it is Linux, an
optimized build, and every component a real `dlopen`'d plugin, none of
which a developer on macOS gets.

**The cross-server half, which genuinely needs separate nodes.** Two
things about any pack are decided by the *peer*, not by the process
doing the packing:

- **which module encodes.** `PMIX_BFROPS_PACK` dereferences
  `peer->nptr->compat.bfrops` — the module
  `pmix_bfrops_base_assign_module()` picked from the version string the
  `ptl` handshake exchanged. Inside one process that is always this
  build's newest component, so the assignment never really happens.
- **whether the buffer is described.** Described vs. non-described is
  negotiated at connection time, and the pack and unpack drivers branch
  on it to decide whether each item carries a type tag.

A round trip inside one process is self-consistent under either choice
and therefore cannot fail on either. `examples/datatypes.c` publishes
one value of every interesting type — including nested data arrays and
the typeless array descriptor whose marker pack and unpack must spell
identically — and every rank verifies every *other* rank's copy. Run at
2, 4 and 8 nodes.

**One rank per node, deliberately.** The launches use
`--host node1:1,node2:1,... --map-by node`, not the `node1:2,node2:2`
the other runners use. A rank asking for a peer that shares its node is
answered out of the local datastore and never packs anything, so with
two ranks per node those pairs pass no matter what state the encoders
are in — the run can be green with the cross-node half broken.

### What a failure looks like

Every rank prints `datatypes: rank N: PASS` or names the type that came
back wrong. The runner counts the PASS lines and requires one per rank,
so a run in which some ranks never got that far fails rather than
reading as a pass. A stream desynchronization — the failure mode when
pack and unpack disagree about a marker — shows up as several
consecutive `MISMATCH` lines starting at the value after the one that
disagreed, not at the one that caused it.

### macOS mode

`./run-bfrops-tests.sh macos` runs the unit programs in the local tree
and then drives `examples/datatypes` through a single `simptest` server.
That is not a substitute for the swarm stage — one server means one peer
module and one negotiated buffer type — but it catches an outright break
before you spend ten containers on it.

---

## 16. The `src/mca/gds` suite (`run-gds-tests.sh`)

```sh
./run-gds-tests.sh linux    # ten-container swarm
./run-gds-tests.sh macos    # the single-host subset
```

`gds` is the datastore: everything a process `PMIx_Put`s, every
job-level value a launcher hands a server, and every remote value that
arrives through a fence lives in, and comes back out of, a `gds` module.
Two things about it are invisible to `make check`, and they are not the
same thing.

### Why this suite exists

**`gds/shmem3` is not built on macOS at all.** Its `configure.m4` gates
on a 64-bit, non-Apple host, and unlike the other environment-specific
components it carries no `|| test "$pmix_testbuild" = "1"` escape — so
`--enable-test-build` does not reach it either. A developer working on a
Mac gets *no* compiler coverage of that component: not a weaker test,
none. Since `shmem3` outbids `hash` wherever it runs, that is the module
serving most real Linux jobs. The first stage here simply compiles it,
in two configurations, with `--enable-devel-check` so a warning is an
error; everything downstream of that is a bonus.

**The datastore's job is to answer for processes that are not here.** A
rank asking for a value its own node already holds is served out of the
local tables and never touches the modex machinery. So a single-node run
is self-consistent under a datastore that drops every remote proc — and a
datastore that mishandles the modex is not hypothetical. `shmem3`'s
`store_modex` callback returns the unpack "ran off the end" code where
the base envelope walker expects `PMIX_SUCCESS`; the walker reads any
other status as a failure of the contribution it is walking, so it would
store the **first** proc of each server's contribution, discard the rest,
and report success. That particular one is dormant — `shmem3` is
excluded from storing modex data by design for now, because
`PMIX_GDS_STORE_MODEX` always resolves the local module and a server
assigns itself `hash` — but nothing in `test/unit` could see it either
way, and `hash` serves this path for real.

That is why every launch below uses one slot per host
(`--host node1:1,node2:1,… --map-by node`). With two ranks on a node,
half the gets are answered locally and the run can be green with the
remote half broken — the same reasoning as `run-bfrops-tests.sh` (§15).

### Stages

| Stage | What it covers |
|-------|----------------|
| build | The source is copied into a container, `autogen.pl`'d once, and configured twice — default and `--enable-mca-dso`. Each build must produce `shmem3` objects, list `shmem3` in `static-components.h`, and pass `gds_datastore`, `gds_fallback` and `proc_array_id`. |
| collective | `examples/datatypes` over 2, 4 and 8 separate PMIx servers: put, commit, fence with `PMIX_COLLECT_DATA`, then verify every peer's values. This is the modex path. |
| scopes | `examples/client` over the same geometry. `datatypes` puts everything at `PMIX_GLOBAL`; this puts at `PMIX_LOCAL` and `PMIX_REMOTE` separately and reads each peer back, so the scope routing in `store` and the scope filtering in `fetch` have to agree *across a server boundary*, not merely within one process. |
| hash | The same geometry with `PMIX_MCA_gds=hash`, so both shipped components are covered and a divergence between them shows up as one passing and the other failing. |
| direct | `examples/dmodex`: commit with no collective fence, so each get is answered by the owning proc's server on demand. That is the `assemb_kvs_req` / `accept_kvs_resp` path — the module slots `shmem3` leaves `NULL` and routes to the local module. |
| fallback | The collective run again with `PMIX_MCA_gds_shmem3_force_client_attach_failure=1`, so every client's fixed-address attach fails and it has to switch to the next module and re-request its job data. A regression here is a failed `PMIx_Init`, not a wrong answer. |

The build stage copies the tree into the container rather than
configuring a VPATH build against the read-only mount, which is what
`run-bfrops-tests.sh` does. That costs one `autogen.pl` per run and buys
not caring whether the developer's srcdir has been configured in place —
which most have, and which blocks a VPATH build two different ways (§15).
Skipping the headline stage on the ordinary developer configuration would
defeat the runner.

### The environment goes to two places, and they are different

An MCA parameter read by the *client* library has to reach the app
process, which is `prterun -x`. One that has to reach the PMIx server
inside each `prted` has to be in the environment `prterun` itself was
started with, because that is what PRRTE forwards to the daemons it
launches. `run_across_nodes()` sets both; dropping either gives a run
that looks configured and is not.

### What it deliberately does not cover, and one overlap

`examples/datatypes` is also driven cross-node by `run-bfrops-tests.sh`.
That is the same program with a different subject: there it is asked
whether every data type survives the *encoders*, here whether the
*datastore* keeps and returns what it was given — which is why this
runner repeats the geometry under three `gds` configurations and that one
does not. If you change `datatypes.c`, both suites are downstream.

`examples/client` runs only in the swarm stages, never against
`simptest`. It asks for `PMIX_LOCAL_PROCS`, which the **host** supplies
at `register_nspace` time and PMIx then stores and returns — PRRTE
provides it, `simptest` does not. Against a host that does not, the
client treats the `PMIX_ERR_NOT_FOUND` as fatal, jumps over the entire
put/fence/get section it exists for, and returns 0: success, having
tested nothing. That is why the runner grades it by counting its per-key
`returned correct` lines rather than trusting its exit status, and why
the macOS mode drives `datatypes` instead.

Nothing here covers the *cross-version* half of `shmem3`: a server and a
client built from different releases mapping the same segment. The
layout stamp (`PMIX_GDS_SHMEM3_LAYOUT_ID`) and the component rename
discipline that guard it are described in
[`src/mca/gds/shmem3/AGENTS.md`](../../src/mca/gds/shmem3/AGENTS.md); a
test for them needs two PMIx installs, which this harness does not build.

### macOS mode

`./run-gds-tests.sh macos` runs the three unit programs in the local tree
and drives `examples/datatypes` through a single `simptest` server. It
says up front that `shmem3` does not exist on the platform. One server
means every get is answered out of the local datastore, which is exactly
the case the multi-node stages exist to get past — so treat it as a
cheap "did I break it outright" pass, not as coverage.

---

## 17. The `src/mca/ptl` suite (`run-ptl-tests.sh`)

```sh
./run-ptl-tests.sh linux     # the swarm
./run-ptl-tests.sh macos     # single-host subset
```

`ptl` is the socket layer under every other suite here, which is exactly
why it needs one of its own: when the transport is wrong, what the other
runners report is a group that would not form or a datastore that
answered nothing, and the cause is several layers down.

### Why the swarm buys something here

A developer's own `make check` drives the PTL hard, but always over
loopback, on one host, between processes one server started. Four things
it therefore cannot reach:

* **Interface selection.** `pmix_ptl_base_setup_listener` defaults to a
  loopback device and only falls back to a public one when remote or
  tool connections were asked for. On one host the loopback branch is
  always taken, and the public branch — plus the `no-remote-interfaces`
  / `no-loopback-interfaces` / `no-available-interfaces` diagnostics
  guarding it — is dead code. A tool on another node can only reach a
  server that took the other branch.

* **Discovery is node-local, and nothing proves it.** The rendezvous
  files a server drops (`pmix.<host>.tool`, `pmix.<host>.tool.<pid>`,
  `pmix.<host>.tool.<nspace>`) live in that node's tmpdir and name that
  node's host. On one host "search the tmpdir" always succeeds, so the
  failure path is never entered — and that path used to dereference a
  URI it had not obtained whenever the caller passed
  `PMIX_TOOL_CONNECT_OPTIONAL`.

* **Partial writes.** `send_msg` carries a cursor across `writev()`
  because a socket buffer can fill mid-message, and `read_bytes` resumes
  a payload across `EAGAIN` for the same reason. A loopback socket
  between two processes on an idle machine rarely makes either happen; a
  real TCP connection carrying a modex between containers does.

* **The message-size ceiling.** `pmix_ptl_base.max_msg_size` is checked
  against every inbound header before the payload is allocated. Nothing
  in the tree sets that parameter, so the case where somebody does was
  never run — and "0 means no limit" used to leave the ceiling at zero,
  which refuses every message carrying a payload.

### The cases

| Case | What it pins |
|------|--------------|
| cross-node client | the baseline: put/get/fence between ranks behind two servers. Read this one first when several fail together. |
| direct modex | a real `PMIX_PTL_SEND_RECV` round trip to a peer behind another server — dynamic tag, header, payload |
| `max_msg_size=0` | zero is a "no practical limit" sentinel, not a zero-byte cap |
| `max_msg_size=32` | an explicit ceiling still passes traffic under it |
| tool by nspace | rendezvous-file discovery on the tool's own node |
| tool discovery is node-local | the same namespace from another node fails **promptly and cleanly** — not a hang, not a crash, and not a false success |
| tool by URI across nodes | the handshake over a public interface. Skipped, with the address named, when the DVM's listener bound loopback — whether PRRTE asked for remote connections is PRRTE's business, not the transport's |
| tool loses its server | the client half of `lost_connection`: the tool must notice the drop and unwind rather than block forever on a reply that will never come |
| exhausted port range | told to use one port and finding it taken, the listener must fail. It used to run off the end of its port loop, and `listen()` on an unbound socket happily gets a port of the kernel's choosing — so the server came up advertising an address that had nothing to do with what it was told to use |

### Things to know

**The DVM URI is parsed the way PMIx parses it.** The case that attaches
a tool across nodes splits `<nspace>.<rank>;tcp4://<addr>:<port>` from
the *back*, because a namespace may itself contain dots (PRRTE names one
after the host). Splitting from the front finds the wrong separator on
exactly the namespaces this harness produces.

**A loopback DVM is a skip, not a failure.** The remote-tool case
depends on the DVM having enabled remote connections. When it did not,
the address in the reported URI is `127.0.0.1` and the case says so and
skips: failing there would be blaming the transport for a launcher
configuration.

**The port-range case needs the port actually occupied.** It parks a
Python listener on node1 and *verifies* the port is taken before
launching; if it could not take the port, the case skips rather than
passing on a range that was never exhausted.

### macOS mode

`./run-ptl-tests.sh macos` is not a multi-node test and does not pretend
to be — it runs the baseline client and the two message-size-ceiling
cases against a single local server. The ceiling cases lose nothing by
being local: the parameter is read once, at framework registration, and
the defect it guards against made every payload-bearing message fail.
Everything else in the suite needs a second host by construction.
