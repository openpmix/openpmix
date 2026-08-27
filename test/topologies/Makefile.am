#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Synthetic hwloc topology XML files used to exercise the src/hwloc
# datatype layer (pack/unpack/print/copy/size) against machine shapes
# other than the one the tests happen to run on. Consumed by the
# hwloc_datatype unit test (test/unit), which cycles over every file
# named here. These fixtures were imported from PRRTE's test suite.
#
# nvml-4gpu.xml is the shape that matters to device enumeration and is
# not synthetic: it is a reduced (16-PU) form of the machine reported in
# Open MPI issue #14169, gathered by an hwloc built with the CUDA and
# NVML backends. Each GPU function carries THREE OS devices - cuda0,
# opencl0d0 and nvml0 - and the vendor uuid is on nvml0 while the
# function is named by cuda0, so it is the fixture that proves the
# vendor identity is found across the function rather than on the
# device that names it.
#
# drm-4gpu.xml is that same machine again, reduced the same way, but as
# a distro hwloc saw it: DRM card and render nodes only, no vendor
# backend, no identity anywhere. It is the negative case - the GPUs are
# still found and still placed, they simply cannot be named - and it
# also carries a fifth card node (a BMC display adapter) that must NOT
# be counted as a GPU.
#
# intel-4gpu.xml and intel-flat-4gpu.xml are an Aurora node (Intel Data
# Center GPU Max 1550, two tiles to a card) cut down to four GPUs and
# eight PUs, read by an hwloc with the OpenCL and Level Zero backends.
# They are the SAME machine seen under the two Level Zero device
# hierarchy models, which is the distinction ZE_AFFINITY_MASK ordinals
# are relative to: under COMPOSITE (intel-4gpu) each card is one root
# device carrying its tiles as sub-devices, while under FLAT
# (intel-flat-4gpu) each tile is itself a root device, so one card is
# two ordinals rather than one. A mask computed under one model and
# read under the other silently hands the process half the hardware it
# was given, so both shapes have to be covered.
#
# A fixture here must be parseable by hwloc's MINIMALISTIC XML parser,
# not just by libxml2. hwloc uses whichever of the two it was built
# with, and a distro hwloc built without libxml2 - which is what CI
# runs - falls back to its own hand-rolled scanner. That scanner does
# not understand XML comments: a single <!-- --> anywhere in the file
# makes hwloc_topology_load() fail with "Failed to parse XML input with
# the minimalistic parser", and the test that reads it fails on Linux
# while passing on a machine whose hwloc has libxml2. So describe a
# fixture here, in this file, and never inside the fixture itself.
#
# unnamed-osdev.xml is not a machine anyone has: it is opa-hfi.xml with
# OS devices that carry no name at all, which hwloc's XML importer accepts
# and hwloc's own discovery never produces. It is here because a topology
# arrives at PMIx as XML from wherever it was exported - another node, an
# unpacked PMIX_TOPO off the wire - so the shape is caller-supplied data
# that the enumerator has to survive rather than trust.

EXTRA_DIST = test-topo.xml test-topo2.xml nvml-4gpu.xml drm-4gpu.xml \
        intel-4gpu.xml intel-flat-4gpu.xml opa-hfi.xml unnamed-osdev.xml
