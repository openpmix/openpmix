/*
 * This file is derived from the sse package - per the BSD license, the following
 * info is copied here:
 ***********************
 * This file is part of the sse package, copyright (c) 2011, 2012, @radiospiel.
 * It is copyrighted under the terms of the modified BSD license, see LICENSE.BSD.
 *
 * For more information see https://https://github.com/radiospiel/sse.
 ***********************
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

%option always-interactive

%{

#include "sse.h"
#include "sse_internal.h"

#ifndef NDEBUG
#define YYDEBUG 1
#endif

/* === data ======================================================== */

static char* data_buf = 0;
static const char* data = 0;
static char* reply_url = 0;
static void *usercbdata = NULL;

static void set_reply_url(const char* url) {
  free(reply_url);
  reply_url = url ? strdup(url) : 0;
}

/*
 * skip the first character in string, if there is one;
 * then replace the string in \a *pTarget with this string;
 * return a pointer to the string.
 */
static void data_add(const char* string)
{
  size_t new_len = (data_buf ? strlen(data_buf) : 0) + strlen(string) + 1;

  data_buf = realloc(data_buf, new_len + 1);
  strcat(data_buf, "\n");
  strcat(data_buf, string);

  data = data_buf;
}

static void data_reset(void)
{
  if(data_buf)
    *data_buf = 0;

  data = NULL;
}

/* === headers ===================================================== */

static char* headers[PMIX_COMMON_SSE_MAX_HEADERS] = { 0 };
static char** header_ptr = headers;

static void headers_reset(void) {
  char** ph = headers;
  while(*ph) {
    free(*ph);
    *ph++ = 0;
  }

  header_ptr = headers;
}

static void header_add(const char* s) {
  if(header_ptr - headers < PMIX_COMMON_SSE_MAX_HEADERS - 1) {
    *header_ptr++ = strdup(s);
    *header_ptr = NULL;
  }
}

static void header_add_from_line(char* line)
{
  char* colon = strchr(line, ':');
  if(!colon || colon == line) return;

  *colon = '=';

  /*
   * upcase all chars until colon
   */
  {
    char* s;
    for(s = line; s < colon; ++s) {
      *s = toupper(*s);
    }
  }

  /*
   * if the value starts with a space we'll have to skip that. We do
   * this by memmoving the name one character to the right.
   */

  if(isspace(colon[1])) {
    memmove(line+1, line, colon - line);
    colon[1] = '=';
    line += 1;
  }

  header_add(line);
}

/* === flush the event ============================================= */

static void flush(void)
{
  /*
   * If neither headers nor data are set, then we flush after some
   * keep-alive traffic (or some other traffic that does not conform
   * to SSE)
   */
  if(*headers || (data && *data)) {
    pmix_common_on_sse_event(headers, data ? data : "", reply_url, usercbdata);
  }

  set_reply_url(0);
  data_reset();
  headers_reset();
}

%}

%%

                  /* the data attribute is special: multiple lines are concatenated */
data:\ .*         data_add(yytext + 6);
data:.*           data_add(yytext + 5);

                  /* the data attribute is special: multiple lines are concatenated */
reply:\ .*        set_reply_url(yytext + 7);
reply:.*          set_reply_url(yytext + 6);

                  /* all other attributes should appear only one */

[-_0-9a-z]+:\ .*  header_add_from_line(yytext);
[-_0-9a-z]+:.*    header_add_from_line(yytext);

                  /* ignore lines starting with a colon. */
:.*               ;

                  /* ignore lines without colon IN VIOLATION WITH THE SPECS. */
.+                ;

\n\n              flush();
\n                /* no yet flushing - this is within an event */

%%

/*
 * curl callback to feed some data into the lexer.
 */
void pmix_common_sse_parse_sse(char *ptr, size_t size, void *cbdata) {
  (void)yyunput;
  usercbdata = cbdata;

  YY_BUFFER_STATE buffer = yy_scan_bytes(ptr, size);
  while(yylex());
  yy_delete_buffer(buffer);
}

int yywrap() {
  return 1;
}

