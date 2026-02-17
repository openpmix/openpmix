---
name: Release checklist
about: Checklist for PMIx releases
title: 'vxxx: Release checklist'
labels: ''
assignees: rhc54

---

## Build the Release
- [] Verify the major,minor,release,greek version numbers in `VERSION`
- [] Update the `c:r:a` shared library version number(s) in `VERSION` [per the GNU Libtool shared library version number rules](https://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html)
   - **NOTE:** It may be helpful to use a command like `git checkout BRANCH; git pull --rebase; git log --stat --topo-order --decorate TAG_FROM_PREVIOUS_RELEASE..HEAD` to examine the Git logs and see what has changed.
   - [] *IF RELEVANT:* If this is a new backwards-compatible release on a single branch (i.e., this is `vx.y.z` where `z>1`), you probably want to examine `git log --stat --no-merges last_release_tag..this_branch_name` to see what source code files have changed (which directly impacts how to increment the `c:r:a` values).
  - [ ] *IF RELEVANT:* If this is a new release series (e.g., `vx.y.0`), set `r` to 0 and increase `c` values by 10 compared to the first release in the prior series (i.e., `vx.(y-1).0` or `v(x-1).0.0`, as relevant).
  - [ ] *IF RELEVANT:* If this is a new backwards-compatible release series (i.e., `vx.y.0`, where `y>1`), set`a` values to 10 so that the shared libraries will be ABI compatible with the prior release series.
  - [ ] *IF RELEVANT:* If this is a new backwards-**IN**compatible release series (i.e., `vx.0.0`), set `a` values to 0 so that there is no possibility of users accidentally mixing shared library versions.
- [] Update all documentation files (especially including dates and version numbers), including:
  - [] `README`: all relevant updates, build options, etc.  Be sure to update the date near the top of the file.
  - [] `NEWS`: List all user-noticeable changes.  Similar to setting shared library versions (above): 
       - Pro tip: if this is a new release on a single branch (i.e., this is `vx.y.z` where `z>1`), you probably want to examine `git log --stat --no-merges last_release_tag..this_branch_name` to see what has changed.
       - Pro tip: if this is a new release series (i.e., this is `vx.y.0` where `y>1`, or this is `vx.0.0`), you will need to be more creative in examining the git logs because this release is on a different branch than the prior release (`vx.(y-1).z`).  Hence, `git log ... last_release_tag..this_branch_name` will not necessarily give you need.  You may need to merge what has changed on your branch with what has changed on the prior release branch, depending on when the prior release branched from this branch.  Read the **SPECIFYING RANGES** section `gitrevisions(7)` for more details.
  - [] `LICENSE`: Update the years in the copyright notices 
 
## Publish pre-releases
- [] Create a tag for the pre-release, matching the version being released (ie, `git tag -a v3.0.1rc1 -m "v3.0.1rc1" <HASH>`).  Verify <HASH> points to the correct release point.  Push the tag for the release.
- [ ] Create a PR (e.g., tickle README.md) to trigger CI and verify cross-version compatibility
- [] PR release candidate to OMPI for MTT and verify passage (must run for at least one week for coverage)
- [] Build the pre-release tarball and post it on the GitHub tag
- [x] Send an email to the `pmix@googlegroups.com` and `pmix-testers@googlegroups.com` mailing lists announcing the pre-release candidate being available for evaluation. Body of message should include the link to the GitHub tag and a copy of the `NEWS` items relating to this candidate.

## Publish the release
* **DO NOT DO A FINAL RELEASE** if you are too close to Supercomputing and/or Christmas. If you release during these time periods, there can be a ~2 week delay while the developer community is not paying attention to their email (and will not be able to respond to the inevitable post-release user emails).
- [ ] Create a tag for the release, matching the version being released (ie, `git tag -a v3.0.1 -m "v3.0.1" <HASH>`).  Verify <HASH> points to the correct release point.  Push the tag for the release.
- [ ] Make the tarball for the release and the src rpm
- [ ] Post the tarballs and src rpm on the GitHub tag. Include a copy of the `NEWS` items relating to this release (see prior releases for an example)
- [ ] Point ReadTheDocs to the new release. Go to openpmix.docs.org and mark the new version as "active", then mark the prior version as "inactive"
- [ ] Close the relevant Github milestone in `openpmix/openpmix`
- [ ] Send an email to the `pmix@googlegroups.com` list announcing the release

## Prep for next release in series
- [ ] Ensure that new Github milestones exist in [GitHub](https://github.com/openpmix/openpmix/issues) for the next release
- [ ] Re-target (change milestone) all still-open issues for the new release to the next major or minor release of PMIx as appropriate
- [ ] Update the PMIx version number of the release branch in `VERSION` to `<NEXT_VERSION>` and set `greek` to `a1`
- [ ] Open a duplicate of this issue for the next release in this series, if one is anticipated
