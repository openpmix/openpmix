#ifndef PMI_INTERFACE_H
#define PMI_INTERFACE_H

#include <stdio.h>

void pmi_init(int *rank, int *size);
void pmi_get_local_ranks(int **local_ranks, int *local_cnt);
void pmi_put_key_loc(char *key, int *key_val, int key_size);
void pmi_put_key_rem(char *key, int *key_val, int key_size);
void pmi_commit();
void pmi_fence(int collect);
void pmi_fini();
void pmi_get_key_loc(int rank, char *key_name, int **key_val, int *key_size);
void pmi_get_key_rem(int rank, char *key_name, int **key_val, int *key_size);

#endif
