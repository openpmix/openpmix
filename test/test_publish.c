#include "test_publish.h"

static int test_publish(char *my_nspace, int my_rank)
{
    int rc;
    pmix_info_t info;
    char data[512];
    
    PMIX_INFO_CONSTRUCT(&info);
    (void)snprintf(info.key, PMIX_MAX_KEYLEN, "%s:%d", my_nspace, my_rank);
    (void)snprintf(data, 512, "data from proc %s:%d", my_nspace, my_rank);
    info.value.type = PMIX_STRING;
    info.value.data.string = strdup(data);
    rc = PMIx_Publish(PMIX_UNIVERSAL, PMIX_PERSIST_INDEF, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

static int test_lookup(char *my_nspace, int my_rank)
{
    int rc;
    pmix_info_t info;
    char data[512];

    (void)snprintf(data, 512, "data from proc %s:%d", my_nspace, my_rank);
    PMIX_INFO_CONSTRUCT(&info);
    (void)snprintf(info.key, PMIX_MAX_KEYLEN, "%s:%d", my_nspace, my_rank);
    if (PMIX_SUCCESS != (rc = PMIx_Lookup(PMIX_UNIVERSAL, &info, 1, NULL))) {
        PMIX_INFO_DESTRUCT(&info);
        return rc;
    }

    if (PMIX_STRING != info.value.type ||
        NULL == info.value.data.string) {
        PMIX_INFO_DESTRUCT(&info);
        return PMIX_ERR_NOT_FOUND;
    }
    
    if (strncmp(data, info.value.data.string, strlen(data))) {
        PMIX_INFO_DESTRUCT(&info);
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

static int test_unpublish(char *my_nspace, int my_rank)
{
    int rc;
    char *keys[2];

    keys[0] = (char*)malloc(PMIX_MAX_KEYLEN * sizeof(char));
    (void)snprintf(keys[0], PMIX_MAX_KEYLEN, "%s:%d", my_nspace, my_rank);
    keys[1] = NULL;
    
    rc = PMIx_Unpublish(PMIX_UNIVERSAL, keys);
    free(keys[0]);
    return rc;
}

int test_publish_lookup(char *my_nspace, int my_rank)
{
    int rc;
    rc = test_publish(my_nspace, my_rank);
    if (PMIX_SUCCESS != rc) {
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: PMIX_Publish succeeded.", my_nspace, my_rank));

    rc = test_lookup(my_nspace, my_rank);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: PMIX_Lookup test failed.", my_nspace, my_rank));
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: PMIX_Lookup test succeeded.\n", my_nspace, my_rank));

    rc = test_unpublish(my_nspace, my_rank);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: PMIX_Unpublish test failed.", my_nspace, my_rank));
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: PMIX_Unpublish test succeeded.", my_nspace, my_rank));

    rc = test_lookup(my_nspace, my_rank);
    if (PMIX_ERR_NOT_FOUND != rc) {
        TEST_ERROR(("%s:%d: PMIX_Lookup function returned %d instead of PMIX_ERR_NOT_FOUND.", my_nspace, my_rank, rc));
        return PMIX_ERROR;
    }
    
    return PMIX_SUCCESS;
}
    
