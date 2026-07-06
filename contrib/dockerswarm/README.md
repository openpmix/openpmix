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
| `run-tests.sh` | Runs the group example programs and reports PASS/FAIL: full multi-node suite on Linux, single-host subset on macOS. |
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

# ---- native macOS (single host) ----
./build.sh macos           # native PMIx + PRRTE build under vpath-macos-*
./run-tests.sh macos       # single-host group smoke
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
(`/opt/prte/prte`), the group clients (`/opt/prte/tests`), and writes
`/opt/prte/env.sh`. To add or remove nodes, copy or delete a service block in
`docker-compose.yml` (and adjust the `seq 1 10` loops in `run-tests.sh`).
