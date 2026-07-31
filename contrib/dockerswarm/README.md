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
| `run-class-tests.sh` | Runs `test/unit/class` in the two configurations a developer's own `make check` does not cover: **Linux**, and **`--disable-debug`** with default symbol visibility. Deliberately *not* a multi-node test — see §11. |
| `swarm-common.sh` | Sourced by all six scripts above: which swarm to drive (`PMIX_SWARM`), how to reach a node, and how to clean one. The three runners each carried their own copy of that once, and the copies drifted. |
| `python/` | The swarm's own Python clients (`swarm_client.py`, `swarm_group.py`, `swarm_cpuset.py`). |
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

# ---- native macOS (single host) ----
./build.sh macos           # native PMIx + PRRTE build under vpath-macos-*
./run-tests.sh macos       # single-host group smoke
./run-group-events.sh macos # single-host dynamic-group smoke
./run-topology.sh macos    # single-host topology-handoff smoke
./run-python.sh macos      # single-host bindings smoke (most checks SKIP)
./run-client-tests.sh macos # single-host client smoke (one server, not the point)
```

Rebuild after editing PMIx: rerun `./build.sh` (incremental). No image rebuild
and no `docker compose` restart needed — the nodes read the shared volume.

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
`gds/shmem2` can build them inside an mmap'd segment a server shares with
its local clients — and that path is exercised by `run-tests.sh`, whose
group cases drive a real datastore on each node.

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

A final wide `dmodex` run (10 ranks over 5 nodes) drives the pending-request
list deep enough for the coalescing to matter.

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
