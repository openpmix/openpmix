#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Point the tests at the components in the build tree so that "make
# check" does not require a prior "make install". See the comments in
# test/pmix_test_env.sh.in.
AM_TESTS_ENVIRONMENT = . $(abs_top_builddir)/test/pmix_test_env.sh;

check_PROGRAMS = mca_base_var mca_base_var_enum mca_base_component \
    mca_base_show_load_errors mca_base_framework mca_base_paramfile \
    mca_base_select

TESTS = mca_base_var mca_base_var_enum mca_base_component \
    mca_base_show_load_errors mca_base_framework mca_base_paramfile \
    mca_base_select

mca_base_var_SOURCES = mca_base_var.c
mca_base_var_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_var_LDADD = \
    $(top_builddir)/src/libpmix.la

mca_base_var_enum_SOURCES = mca_base_var_enum.c
mca_base_var_enum_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_var_enum_LDADD = \
    $(top_builddir)/src/libpmix.la

mca_base_component_SOURCES = mca_base_component.c
mca_base_component_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_component_LDADD = \
    $(top_builddir)/src/libpmix.la

mca_base_show_load_errors_SOURCES = mca_base_show_load_errors.c
mca_base_show_load_errors_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_show_load_errors_LDADD = \
    $(top_builddir)/src/libpmix.la

mca_base_framework_SOURCES = mca_base_framework.c
mca_base_framework_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_framework_LDADD = \
    $(top_builddir)/src/libpmix.la

mca_base_paramfile_SOURCES = mca_base_paramfile.c
mca_base_paramfile_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_paramfile_LDADD = \
    $(top_builddir)/src/libpmix.la

mca_base_select_SOURCES = mca_base_select.c
mca_base_select_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
mca_base_select_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f mca_base_var mca_base_var_enum mca_base_component \
	    mca_base_show_load_errors mca_base_framework mca_base_paramfile \
    mca_base_select
