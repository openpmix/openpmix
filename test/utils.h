#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "src/util/argv.h"
#include "test_common.h"

void fill_seq_ranks_array(size_t nprocs, char **ranks);
void set_namespace(int nprocs, char *ranks, char *name);
void set_client_argv(test_params *params, char ***argv);

