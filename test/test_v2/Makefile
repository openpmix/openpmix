#Fix below to be your local openpmix installation
INSTALLDIR = /opt/cps/pmix-install
INCLUDES = -I. -I$(INSTALLDIR)/include -I../../include -I../.. -I../../src/include
vpath %.h .:$(INSTALLDIR)/include:../../include:../..:../../src/include
LDIR = $(INSTALLDIR)/lib

CC = gcc
CFLAGS = $(INCLUDES) -g -Wfatal-errors #-Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS = -Wl,-rpath,$(LDIR)
LIBS =-lpmix -lpthread -levent

# Executables
EXE = pmix_test test_init_fin test_helloworld test_get_basic

# List of all .c source files.
PCFILES = pmix_test.c test_common.c cli_stages.c test_server.c server_callbacks.c base64_enc_dec.c
TC1FILES = test_init_fin.c test_common.c base64_enc_dec.c
TC2FILES = test_helloworld.c test_common.c base64_enc_dec.c
TC3FILES = test_get_basic.c test_common.c base64_enc_dec.c
TCFILES = $(TC1FILES) $(TC2FILES) $(TC3FILES)
CFILES = $(PCFILES) $(TCFILES)

OBJ = $(CFILES:%.c=%.o)
POBJ = $(PCFILES:%.c=%.o)
T1OBJ = $(TC1FILES:%.c=%.o)
T2OBJ = $(TC2FILES:%.c=%.o)
T3OBJ = $(TC3FILES:%.c=%.o)

all: pmix_test test_init_fin test_helloworld test_get_basic
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

%.o : %.c
	$(CC) -c -o $@ $< $(CFLAGS)

