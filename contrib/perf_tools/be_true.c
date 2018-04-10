#include <stdio.h>
#include <unistd.h>
#include "memory.h"
int main (int argc, char *argv[])
{
    sleep(3);
    get_memory_usage_kb(&meminfo);
    print_info();

    return 0;
}
