#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "pmix_message.h"

pmix_message_t *PMIx_message_new(void)
{
      pmix_message_inst_t *msg = malloc(sizeof(pmix_message_inst_t));
      if( NULL == msg ){
          return NULL;
      }
      memset(msg, 0, sizeof(*msg));
      return (pmix_message_t *)msg;
}

uint32_t PMIx_message_hdr_size(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg){
        return 0;
    }
    return sizeof(msg->hdr);
}

void *PMIx_message_hdr_ptr(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg){
        return NULL;
    }
    return &(msg->hdr);
}

int PMIx_message_hdr_fix(pmix_message_t *msg_opaq, char *hdr)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    msg->hdr_rcvd = 1;
    msg->payload = malloc(msg->hdr.nbytes);
    if( NULL == msg->payload ){
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    return PMIX_SUCCESS;
}

uint32_t PMIx_message_pay_size(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg || !msg->hdr_rcvd ){
        return 0;
    }
    return msg->hdr.nbytes;
}

void *PMIx_message_pay_ptr(pmix_message_t *msg_opaq, char *pay)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg || !msg->hdr_rcvd ){
        return NULL;
    }
    return msg->payload;
}

void PMIx_message_free(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg ){
        return;
    }
    if( msg->hdr_rcvd && (NULL != msg->payload) ){
        free(msg->payload);
    }
    free(msg);
}
