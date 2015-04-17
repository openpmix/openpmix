#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "src/util/argv.h"
#include "test_common.h"

int launch_clients(test_params params, char *** client_env, char ***client_argv);
