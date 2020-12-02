# Set the environment variable PMIX_INSTALLDIR to point to your local PMIx install directory
ifeq ($(strip $(PMIX_INSTALLDIR)),)
$(info ******** Set PMIX_INSTALLDIR environment variable to point to PMIx install directory ********)
$(info ******** Defaulting to $(HOME)/pmix-install ********)
PMIX_INSTALLDIR = $(HOME)/pmix-install
endif

INCLUDES = -I$(PMIX_INSTALLDIR)/include -I. -I../../include -I../.. -I../../src/include
vpath %.h .:$(PMIX_INSTALLDIR)/include:../../include:../..:../../src/include
LDIR = $(PMIX_INSTALLDIR)/lib

CC = gcc
CFLAGS = $(INCLUDES) -fno-omit-frame-pointer -g -Wfatal-errors #-Wall -Wextra -Wpedantic -Wconversion -Wshadow
# Use CFLAGS below instead if you want to run with Sanitizer
#CFLAGS = $(INCLUDES) -fno-omit-frame-pointer -fsanitize=address -fsanitize=leak -fsanitize=undefined -g -Wfatal-errors -Wall
LDFLAGS = -Wl,-rpath,$(LDIR)
LIBS =-lpmix -lpthread -levent # -lefence # <-- can link against electric fence for memory checking

# Executables
EXE = pmix_test test_init_fin test_helloworld test_get_basic test_get_peers

# List of all .c source files.
PCFILES = pmix_test.c test_common.c cli_stages.c test_server.c server_callbacks.c base64_enc_dec.c
TCCOMMONFILES = test_common.c test_clientapp_funcs.c base64_enc_dec.c
TC1FILES = test_init_fin.c $(TCCOMMONFILES)
TC2FILES = test_helloworld.c $(TCCOMMONFILES)
TC3FILES = test_get_basic.c $(TCCOMMONFILES)
TC4FILES = test_get_peers.c $(TCCOMMONFILES)
TCFILES = $(TC1FILES) $(TC2FILES) $(TC3FILES) $(TC4FILES)
CFILES = $(PCFILES) $(TCFILES)

OBJ = $(CFILES:%.c=%.o)
POBJ = $(PCFILES:%.c=%.o)
T1OBJ = $(TC1FILES:%.c=%.o)
T2OBJ = $(TC2FILES:%.c=%.o)
T3OBJ = $(TC3FILES:%.c=%.o)
T4OBJ = $(TC4FILES:%.c=%.o)

all: pmix_test test_init_fin test_helloworld test_get_basic test_get_peers
.PHONY: all

clean:
	rm -f $(EXE) $(OBJ) $(DEP)
.PHONY: clean

pmix_test: $(POBJ)
	$(CC) -o $@ $^ $(CFLAGS) -L$(LDIR) $(LDFLAGS) $(LIBS)
test_init_fin: $(T1OBJ)
	$(CC) -o $@ $^ $(CFLAGS) -L$(LDIR) $(LDFLAGS) $(LIBS)
test_helloworld: $(T2OBJ)
	$(CC) -o $@ $^ $(CFLAGS) -L$(LDIR) $(LDFLAGS) $(LIBS)
test_get_basic: $(T3OBJ)
	$(CC) -o $@ $^ $(CFLAGS) -L$(LDIR) $(LDFLAGS) $(LIBS)
test_get_peers: $(T4OBJ)
	$(CC) -o $@ $^ $(CFLAGS) -L$(LDIR) $(LDFLAGS) $(LIBS)

%.o : %.c
	$(CC) -c -o $@ $< $(CFLAGS)

