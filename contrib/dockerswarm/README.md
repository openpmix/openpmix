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
| `run-python.sh` | Runs the **Python bindings**: the standalone unit suite, the connected client/server round-trip, and Python PMIx clients spread across nodes. Same `linux`/`macos` modes. See §10. |
| `python/` | The swarm's own Python clients (`swarm_client.py`, `swarm_group.py`) — launched by `prterun` as ordinary PMIx clients. |
| `Dockerfile` | Base image: toolchain, PRRTE master *source* (autogen'd), SSH wiring, node entrypoint. It contains **no** PMIx and **no** built PRRTE. |
| `docker-compose.yml` | The ten nodes `pmix-node1`..`pmix-node10`, each mounting the shared `pmix-build` volume. |

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
./run-python.sh linux      # Python bindings: units, round-trip, multi-node

# ---- native macOS (single host) ----
./build.sh macos           # native PMIx + PRRTE build under vpath-macos-*
./run-tests.sh macos       # single-host group smoke
./run-group-events.sh macos # single-host dynamic-group smoke
./run-python.sh macos      # single-host bindings smoke (most checks SKIP)
```

Rebuild after editing PMIx: rerun `./build.sh` (incremental). No image rebuild
and no `docker compose` restart needed — the nodes read the shared volume.

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
stale state on **every** node between DVM runs:

```sh
for n in $(seq 1 10); do
  docker exec pmix-node$n sh -c 'pkill -9 -x prted; pkill -9 -x prte;
    pkill -9 prterun; rm -rf /tmp/prte.* /tmp/prun.session.* /tmp/pmix*; true'
done
```

## 8. Rebuilding / resetting

| Want to… | Do |
|----------|----|
| pick up a PMIx source edit | `./build.sh` (incremental into the volume) |
| force a clean rebuild | `docker volume rm pmix-build && ./build.sh` |
| rebuild the base image (new PRRTE branch) | `PRRTE_REF=v3.0.x ./build.sh image` |
| tear down the swarm | `docker compose down` (the `pmix-build` volume persists) |

## 9. Topology reference

| Container | hostname | role |
|-----------|----------|------|
| `pmix-node1` | node1 | head node — start the DVM here, run all tools here |
| `pmix-node2`..`pmix-node10` | node2..node10 | additional daemon nodes |

Network: bridge `dvm`. All nodes mount the shared `pmix-build` volume read-only
at `/opt/prte`, where `build.sh` installs PMIx (`/opt/prte/pmix`), PRRTE
(`/opt/prte/prte`), the group clients (`/opt/prte/tests`), the Python bindings
and their scripts (`/opt/prte/tests-python`), and writes `/opt/prte/env.sh`. To
add or remove nodes, copy or delete a service block in `docker-compose.yml`
(and adjust the `seq 1 10` loops in `run-tests.sh`).

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

### Known failing check: locality-string round trip

`swarm_cpuset.py` currently reports one failure, and it is a **real library
defect this harness found**, not a harness problem. Do not silence it:

- `PMIx_server_generate_locality_string` emits a bare string,
  `SK0:L20:L10:CR0:HT0:NM0` — with no `hwloc:` prefix. PRRTE stores exactly
  that as `PMIX_LOCALITY_STRING`.
- `PMIx_Get_relative_locality` *requires* a `hwloc:` prefix
  (`pmix_hwloc_get_relative_locality` checks it and otherwise returns
  `PMIX_ERR_TAKE_NEXT_OPTION`).
- But [`include/pmix.h`](../../include/pmix.h) documents that function's
  arguments as *"String returned by the `PMIx_server_generate_locality_string`
  API"*.

So a caller following the documented contract gets `-1366`
(`PMIX_ERR_TAKE_NEXT_OPTION`) and no locality bits at all. Prepending
`hwloc:` by hand makes it return the correct sharing bits, which is how you
can confirm the diagnosis. The in-tree unit test misses this because it uses
hand-written `"hwloc:NM0:SK0:CR0:HT0"` literals rather than the generator's
output.

Either the generator should emit the prefix or the consumer should tolerate
its absence; the latter is the compatible fix, since changing the emitted
string changes what every existing host environment already stores.

> **Note:** adding Python to the image changed the `Dockerfile`, so the first
> run after picking this up needs `./build.sh image` (or any `./build.sh` on a
> machine with no `pmix-swarm` image yet). That re-clones PRRTE and takes a
> while; subsequent builds are incremental as before.
