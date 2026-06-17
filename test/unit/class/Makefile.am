#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

check_PROGRAMS = class_object class_list class_bitmap class_hash_table \
    class_pointer_array class_ring_buffer class_value_array class_hotel

TESTS = class_object class_list class_bitmap class_hash_table \
    class_pointer_array class_ring_buffer class_value_array class_hotel

class_object_SOURCES = class_object.c
class_object_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_object_LDADD = \
    $(top_builddir)/src/libpmix.la

class_list_SOURCES = class_list.c
class_list_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_list_LDADD = \
    $(top_builddir)/src/libpmix.la

class_bitmap_SOURCES = class_bitmap.c
class_bitmap_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_bitmap_LDADD = \
    $(top_builddir)/src/libpmix.la

class_hash_table_SOURCES = class_hash_table.c
class_hash_table_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_hash_table_LDADD = \
    $(top_builddir)/src/libpmix.la

class_pointer_array_SOURCES = class_pointer_array.c
class_pointer_array_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_pointer_array_LDADD = \
    $(top_builddir)/src/libpmix.la

class_ring_buffer_SOURCES = class_ring_buffer.c
class_ring_buffer_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_ring_buffer_LDADD = \
    $(top_builddir)/src/libpmix.la

class_value_array_SOURCES = class_value_array.c
class_value_array_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_value_array_LDADD = \
    $(top_builddir)/src/libpmix.la

class_hotel_SOURCES = class_hotel.c
class_hotel_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
class_hotel_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f class_object class_list class_bitmap class_hash_table \
	    class_pointer_array class_ring_buffer class_value_array class_hotel
