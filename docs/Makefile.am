#
# Copyright (c) 2022 Cisco Systems, Inc.  All rights reserved.
#
# Copyright (c) 2022-2023 Intel, Inc.  All rights reserved.
# Copyright (c) 2023-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# We need this Makefile to be executed serially.  Below, we list all
# the man pages as the targets of the rule that invokes Sphinx for
# dependency/generation reasons.  But a *single* execution of the make
# target will generate *all* of the man pages and HTML files.  Hence,
# when "make" determines that none of the man page files exist, it
# should execute the Sphinx-invocation rule once, and then it will
# realize that all the man pages files exist.  More specifically: if
# someone invokes "make -j N", we need make to not execute the
# Sphinx-invocation rule multiple times simultaneously.  Both GNU Make
# and BSD Make will honor the .NOTPARALLEL target to disable all
# parallel invocation in this Makefile[.am].
#
# Note that even though we explicitly disable make's parallelism,
# we'll use Sphinx's internal parallelism via "-j auto" -- see
# SPHINX_OPTS.
.NOTPARALLEL:

OUTDIR             = _build
SPHINX_CONFIG      = conf.py
SPHINX_OPTS       ?= -W --keep-going -j auto

# Note: it is significantly more convenient to list all the source
# files here using wildcards (vs. listing every single .rst file).
# However, it is necessary to list $(srcdir) when using wildcards.

# if you have any .txt files, list them here
TEXT_SOURCE_FILES =

MARKDOWN_SOURCE_FILES = \
        $(top_srcdir)/.github/CODE_OF_CONDUCT.md


# list source example files here
CODE_SOURCE_FILES = \
        $(srcdir)/how-things-work/sets_groups/*.c


# list image files here
IMAGE_SOURCE_FILES  = \
        $(srcdir)/images/*.png \
        $(srcdir)/images/*.jpg


RST_SOURCE_FILES   = \
        $(srcdir)/*.rst \
        $(srcdir)/building-apps/*.rst \
        $(srcdir)/developers/*.rst \
        $(srcdir)/how-things-work/*.rst \
        $(srcdir)/how-things-work/schedulers/*.rst \
        $(srcdir)/how-things-work/sets_groups/*.rst \
        $(srcdir)/installing-pmix/*.rst \
        $(srcdir)/installing-pmix/configure-cli-options/*.rst \
        $(srcdir)/man/*.rst \
        $(srcdir)/man/man1/*.rst \
        $(srcdir)/man/man3/*.rst \
        $(srcdir)/man/man5/*.rst \
        $(srcdir)/news/*.rst \
        $(srcdir)/release-notes/*.rst


EXTRA_DIST         = \
        requirements.txt \
        generate-code-of-conduct-rst.py \
        $(SPHINX_CONFIG) \
        $(TEXT_SOURCE_FILES) \
        $(MARKDOWN_SOURCE_FILES) \
        $(IMAGE_SOURCE_FILES) \
        $(RST_SOURCE_FILES) \
        $(CODE_SOURCE_FILES)


###########################################################################

# Note: we list all the man pages explicitly (as opposed to using
# wildcards, like we do to list all the RST source files in
# EXTRA_DIST) because these files get installed by Automake.  There
# are less complications and portability issues (between GNU and BSD
# make(1), for example) if all the files to be installed are
# explicitly listed.

PMIX_MAN1 = \
        pmix_info.1 \
        palloc.1 \
        pattrs.1 \
        pevent.1 \
        plookup.1 \
        pmixcc.1 \
        pps.1 \
        pquery.1

PMIX_MAN3 = \
	PMIx_Abort.3 \
	PMIx_Init.3 \
	PMIx_Initialized.3 \
	PMIx_Finalize.3 \
	PMIx_Put.3 \
	PMIx_Commit.3 \
	PMIx_Fence.3 \
	PMIx_Get.3 \
	PMIx_Publish.3 \
	PMIx_Lookup.3 \
	PMIx_Unpublish.3 \
	PMIx_Spawn.3 \
	PMIx_Connect.3 \
	PMIx_Disconnect.3 \
	PMIx_Resolve_peers.3 \
	PMIx_Resolve_nodes.3 \
	PMIx_Query_info.3 \
	PMIx_Log.3 \
	PMIx_Allocation_request.3 \
	PMIx_Resource_block.3 \
	PMIx_Session_control.3 \
	PMIx_Job_control.3 \
	PMIx_Process_monitor.3 \
	PMIx_Heartbeat.3 \
	PMIx_Get_credential.3 \
	PMIx_Validate_credential.3 \
	PMIx_Group_construct.3 \
	PMIx_Group_invite.3 \
	PMIx_Group_join.3 \
	PMIx_Group_leave.3 \
	PMIx_Group_destruct.3 \
	PMIx_Register_event_handler.3 \
	PMIx_Deregister_event_handler.3 \
	PMIx_Notify_event.3 \
	PMIx_Fabric_register.3 \
	PMIx_Fabric_update.3 \
	PMIx_Fabric_deregister.3 \
	PMIx_Compute_distances.3 \
	PMIx_Load_topology.3 \
	PMIx_Parse_cpuset_string.3 \
	PMIx_Get_cpuset.3 \
	PMIx_Get_relative_locality.3 \
	PMIx_Progress.3 \
	PMIx_Progress_thread_stop.3 \
	PMIx_Error_string.3 \
	PMIx_Error_code.3 \
	PMIx_Proc_state_string.3 \
	PMIx_Scope_string.3 \
	PMIx_Persistence_string.3 \
	PMIx_Data_range_string.3 \
	PMIx_Data_type_string.3 \
	PMIx_Alloc_directive_string.3 \
	PMIx_Resource_block_directive_string.3 \
	PMIx_Alloc_inheritance_string.3 \
	PMIx_IOF_channel_string.3 \
	PMIx_Job_state_string.3 \
	PMIx_Get_attribute_string.3 \
	PMIx_Get_attribute_name.3 \
	PMIx_Link_state_string.3 \
	PMIx_Device_type_string.3 \
	PMIx_Value_comparison_string.3 \
	PMIx_Group_operation_string.3 \
	PMIx_Info_string.3 \
	PMIx_Value_string.3 \
	PMIx_Info_directives_string.3 \
	PMIx_App_string.3 \
	PMIx_Proc_string.3 \
	PMIx_Resource_unit_string.3 \
	PMIx_Get_version.3 \
	PMIx_Store_internal.3 \
	PMIx_Value_get_size.3 \
	PMIx_Info_get_size.3 \
	PMIx_Data_pack.3 \
	PMIx_Data_unpack.3 \
	PMIx_Data_copy.3 \
	PMIx_Data_print.3 \
	PMIx_Data_copy_payload.3 \
	PMIx_Data_unload.3 \
	PMIx_Data_load.3 \
	PMIx_Data_embed.3 \
	PMIx_Data_compress.3 \
	PMIx_Data_decompress.3 \
	PMIx_Data_buffer_construct.3 \
	PMIx_Data_buffer_destruct.3 \
	PMIx_Data_buffer_create.3 \
	PMIx_Data_buffer_release.3 \
	PMIx_Data_buffer_load.3 \
	PMIx_Data_buffer_unload.3 \
	PMIx_Info_construct.3 \
	PMIx_Info_create.3 \
	PMIx_Value_unload.3 \
	PMIx_Value_get_number.3


# The following section-3 pages each document both a blocking API and
# its non-blocking (_nb) counterpart on a single page. For each one,
# install a <name>_nb.3 symlink pointing at <name>.3 so that, e.g.,
# "man PMIx_Fence_nb" displays the page that documents both forms.
PMIX_MAN3_NB_ALIASES = \
	PMIx_Allocation_request \
	PMIx_Compute_distances \
	PMIx_Connect \
	PMIx_Disconnect \
	PMIx_Fabric_deregister \
	PMIx_Fabric_register \
	PMIx_Fabric_update \
	PMIx_Fence \
	PMIx_Get_credential \
	PMIx_Get \
	PMIx_Group_construct \
	PMIx_Group_destruct \
	PMIx_Group_invite \
	PMIx_Group_join \
	PMIx_Group_leave \
	PMIx_Job_control \
	PMIx_Log \
	PMIx_Lookup \
	PMIx_Process_monitor \
	PMIx_Publish \
	PMIx_Query_info \
	PMIx_Resource_block \
	PMIx_Spawn \
	PMIx_Unpublish \
	PMIx_Validate_credential

PMIX_MAN5 = \
        openpmix.5 \
        pmix_status_t.5 \
        pmix_value_t.5 \
        pmix_info_t.5 \
        pmix_info_directives_t.5 \
        pmix_byte_object_t.5

MAN_OUTDIR = $(OUTDIR)/man

PMIX_MAN1_RST = $(PMIX_MAN1:%.1=man/man1/%.1.rst)
PMIX_MAN1_BUILT = $(PMIX_MAN1:%.1=$(MAN_OUTDIR)/%.1)

PMIX_MAN3_RST = $(PMIX_MAN3:%.3=man/man3/%.3.rst)
PMIX_MAN3_BUILT = $(PMIX_MAN3:%.3=$(MAN_OUTDIR)/%.3)

PMIX_MAN5_RST = $(PMIX_MAN5:%.5=man/man5/%.5.rst)
PMIX_MAN5_BUILT = $(PMIX_MAN5:%.5=$(MAN_OUTDIR)/%.5)

EXTRA_DIST += \
        $(PMIX_MAN1_BUILT) \
        $(PMIX_MAN3_BUILT) \
        $(PMIX_MAN5_BUILT)

###########################################################################
if PMIX_BUILD_DOCS

include $(top_srcdir)/Makefile.openpmix-rules

# Generate this file from CODE_OF_CONDUCT.md as part of the Sphinx
# docs build.
CODE_OF_CONDUCT_RST = $(srcdir)/code-of-conduct.rst

# Have to not list these targets in EXTRA_DIST outside of the
# PMIX_BUILD_DOCS conditional because "make dist" will fail due to
# these missing targets (and therefore not run the "dist-hook" target
# in the top-level Makefile, which prints a pretty message about why
# "make dist" failed).
#
# We list the entire directory trees (html and man) to grab all
# generated files in them.
EXTRA_DIST += \
        $(OUTDIR)/html \
        $(OUTDIR)/man

ALL_MAN_BUILT = \
        $(PMIX_MAN1_BUILT) $(PMIX_MAN3_BUILT) $(PMIX_MAN5_BUILT)
$(ALL_MAN_BUILT): $(RST_SOURCE_FILES) $(IMAGE_SOURCE_FILES)
$(ALL_MAN_BUILT): $(TEXT_SOURCE_FILES) $(SPHINX_CONFIG)
$(ALL_MAN_BUILT): $(CODE_OF_CONDUCT_RST)

$(CODE_OF_CONDUCT_RST): $(top_srcdir)/.github/CODE_OF_CONDUCT.md
$(CODE_OF_CONDUCT_RST): generate-code-of-conduct-rst.py
	$(PMIX_V_GEN) python3 $(srcdir)/generate-code-of-conduct-rst.py \
		--input $(top_srcdir)/.github/CODE_OF_CONDUCT.md --output $@

# List both commands (HTML and man) in a single rule because they
# really need to be run in serial.  Specifically, if they were two
# different rules and someone ran "make -j", then both of them could
# be writing to $(OUTDIR)/doctrees simultaneously, which would be Bad.
# Use one of the man pages as a sentinel file to indicate whether all
# the HTML docs and man pages have been built.
$(ALL_MAN_BUILT):
	$(PMIX_V_SPHINX_HTML) $(SPHINX_BUILD) -M html "$(srcdir)" "$(OUTDIR)" $(SPHINX_OPTS)
	$(PMIX_V_SPHINX_MAN) $(SPHINX_BUILD) -M man "$(srcdir)" "$(OUTDIR)" $(SPHINX_OPTS)

# A useful rule to invoke manually to ensure that all of the external
# HTML links we have are valid.  Running this rule requires
# connectivity to the general internet.
linkcheck:
	$(SPHINX_BUILD) -M linkcheck "$(srcdir)" "$(OUTDIR)" $(SPHINX_OPTS)

.PHONY: linkcheck

maintainer-clean-local:
	$(SPHINX_BUILD) -M clean "$(srcdir)" "$(OUTDIR)" $(SPHINX_OPTS)
	rm -f $(CODE_OF_CONDUCT_RST)

# List all the built man pages here in the Automake BUILT_SOURCES
# macro.  This hooks into the normal Automake build mechanisms, and
# will ultimately cause the invocation of the above rule that runs
# Sphinx to build the HTML and man pages.
BUILT_SOURCES = $(CODE_OF_CONDUCT_RST) $(ALL_MAN_BUILT)

endif PMIX_BUILD_DOCS

###########################################################################
if PMIX_INSTALL_DOCS

man1_MANS = $(PMIX_MAN1_BUILT)
man3_MANS = $(PMIX_MAN3_BUILT)
man5_MANS = $(PMIX_MAN5_BUILT)

# We do not know the names of all the generated HTML files: we only
# know that the generated tree will of files be in _build/html/.
# Unfortunately, Automake's installation process only handles
# individual files -- not entire trees of files.  Hence, we use a
# "find ..." to copy the entire generated tree to the target
# directory.
#
# Note: we do not use "cp -r ..." because if the source directory is
# write-protected (e.g., during "make distcheck"), then the cp -r will
# also write-protect the target directory.  Instead, use the
# Automake-provided install macros to set desirable permissions on the
# target directories and files.
#
# Since this might be a VPATH build, first check to see if _build/html
# exists in the source tree.  If not, do the find+install from the
# build tree.
install-data-hook:
	$(MKDIR_P) $(DESTDIR)$(docdir)
	if test -d $(srcdir)/_build/html; then \
	    topdir=$(srcdir)/_build; \
	else \
	    topdir=_build; \
	fi; \
	cd $$topdir; \
	find html -type d -exec $(mkinstalldirs) $(DESTDIR)$(docdir)/{} \; ; \
	find html -type f -exec $(INSTALL_DATA) {} $(DESTDIR)$(docdir)/{} \;

# Install a <name>_nb.3 symlink alongside each base page that documents
# a non-blocking form, so "man PMIx_Foo_nb" resolves to the shared page.
	@list='$(PMIX_MAN3_NB_ALIASES)'; for base in $$list; do \
	    if test -f "$(DESTDIR)$(man3dir)/$$base.3"; then \
	        echo "  LN_S     $${base}_nb.3 -> $$base.3"; \
	        rm -f "$(DESTDIR)$(man3dir)/$${base}_nb.3"; \
	        (cd "$(DESTDIR)$(man3dir)" && $(LN_S) "$$base.3" "$${base}_nb.3"); \
	    fi; \
	done

uninstall-hook:
	rm -rf $(DESTDIR)$(docdir)
	@list='$(PMIX_MAN3_NB_ALIASES)'; for base in $$list; do \
	    rm -f "$(DESTDIR)$(man3dir)/$${base}_nb.3"; \
	done

endif PMIX_INSTALL_DOCS
