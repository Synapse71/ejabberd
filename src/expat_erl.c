/* $Id$ */

#include <stdio.h>
#include <string.h>
#include <erl_driver.h>
#include <ei.h>
#include <expat.h>

#define EI_ENCODE_STRING_BUG

#ifdef EI_ENCODE_STRING_BUG

/*
 * Workaround for EI encode_string bug
 */

#define put8(s,n) do { \
  (s)[0] = (char)((n) & 0xff); \
  (s) += 1; \
} while (0) 

#define put16be(s,n) do { \
  (s)[0] = ((n) >>  8) & 0xff; \
  (s)[1] = (n) & 0xff; \
  (s) += 2; \
} while (0)

#define put32be(s,n) do {  \
  (s)[0] = ((n) >>  24) & 0xff; \
  (s)[1] = ((n) >>  16) & 0xff; \
  (s)[2] = ((n) >>  8) & 0xff;  \
  (s)[3] = (n) & 0xff; \
  (s) += 4; \
} while (0)

int ei_encode_string_len_fixed(char *buf, int *index, const char *p, int len)
{
    char *s = buf + *index;
    char *s0 = s;
    int i;

    if (len <= 0xffff) {
	if (!buf) s += 3;
	else {
	    put8(s,ERL_STRING_EXT);
	    put16be(s,len);
	    memmove(s,p,len);	/* unterminated string */
	}
	s += len;
    }
    else {
	if (!buf) s += 6 + (2*len);
	else {
	    /* strings longer than 65535 are encoded as lists */
	    put8(s,ERL_LIST_EXT);
	    put32be(s,len);

	    for (i=0; i<len; i++) {
		put8(s,ERL_SMALL_INTEGER_EXT);
		put8(s,p[i]);
	    }

	    put8(s,ERL_NIL_EXT);
	}
    }

    *index += s-s0; 

    return 0; 
}

int ei_encode_string_fixed(char *buf, int *index, const char *p)
{
    return ei_encode_string_len_fixed(buf, index, p, strlen(p));
}

int ei_x_encode_string_len_fixed(ei_x_buff* x, const char* s, int len)
{
    int i = x->index;
    ei_encode_string_len_fixed(NULL, &i, s, len);
    if (!x_fix_buff(x, i))
	return -1;
    return ei_encode_string_len_fixed(x->buff, &x->index, s, len);
}

int ei_x_encode_string_fixed(ei_x_buff* x, const char* s)
{
    return ei_x_encode_string_len_fixed(x, s, strlen(s));
}

#else

#define ei_encode_string_len_fixed(buf, index, p, len) \
        ei_encode_string_len(buf, index, p, len)
#define ei_encode_string_fixed(buf, index, p) \
        ei_encode_string(buf, index, p)
#define ei_x_encode_string_len_fixed(x, s, len) \
        ei_x_encode_string_len(x, s, len)
#define ei_x_encode_string_fixed(x, s) \
        ei_x_encode_string(x, s)

#endif

#define XML_START 0
#define XML_END   1
#define XML_CDATA 2
#define XML_ERROR 3


typedef struct {
      ErlDrvPort port;
      XML_Parser parser;
} expat_data;

void *erlXML_StartElementHandler(expat_data *d,
				 const XML_Char *name,
				 const XML_Char **atts)
{
   int i;
   ei_x_buff buf;
   
   ei_x_new_with_version(&buf);
   ei_x_encode_tuple_header(&buf, 2);
   ei_x_encode_long(&buf, XML_START);
   ei_x_encode_tuple_header(&buf, 2);
   ei_x_encode_string_fixed(&buf, name);
   
   for (i = 0; atts[i]; i += 2) {}

   ei_x_encode_list_header(&buf, i/2);
  
   for (i = 0; atts[i]; i += 2)
   {
      ei_x_encode_tuple_header(&buf, 2);
      ei_x_encode_string_fixed(&buf, atts[i]);
      ei_x_encode_string_fixed(&buf, atts[i+1]);
   }
   
   ei_x_encode_empty_list(&buf);
   
   driver_output(d->port, buf.buff, buf.index);
   ei_x_free(&buf);
   return NULL;
}

void *erlXML_EndElementHandler(expat_data *d,
			       const XML_Char *name)
{
   ei_x_buff buf;
   
   ei_x_new_with_version(&buf);
   ei_x_encode_tuple_header(&buf, 2);
   ei_x_encode_long(&buf, XML_END);
   ei_x_encode_string_fixed(&buf, name);

   driver_output(d->port, buf.buff, buf.index);
   ei_x_free(&buf);
   return NULL;
}

void *erlXML_CharacterDataHandler(expat_data *d,
				  const XML_Char *s,
				  int len)
{
   ei_x_buff buf;
   
   ei_x_new_with_version(&buf);
   ei_x_encode_tuple_header(&buf, 2);
   ei_x_encode_long(&buf, XML_CDATA);
   ei_x_encode_string_len_fixed(&buf, s, len);

   driver_output(d->port, buf.buff, buf.index);
   ei_x_free(&buf);
   return NULL;
}


static ErlDrvData expat_erl_start(ErlDrvPort port, char *buff)
{
   expat_data* d = (expat_data*)driver_alloc(sizeof(expat_data));
   d->port = port;
   d->parser = XML_ParserCreate("UTF-8");
   XML_SetUserData(d->parser, d);

   XML_SetStartElementHandler(
      d->parser, (XML_StartElementHandler)erlXML_StartElementHandler);
   XML_SetEndElementHandler(
      d->parser, (XML_EndElementHandler)erlXML_EndElementHandler);
   XML_SetCharacterDataHandler(
      d->parser, (XML_CharacterDataHandler)erlXML_CharacterDataHandler);


   return (ErlDrvData)d;
}

static void expat_erl_stop(ErlDrvData handle)
{
   XML_ParserFree(((expat_data *)handle)->parser);
   driver_free((char*)handle);
}

static void expat_erl_output(ErlDrvData handle, char *buff, int bufflen)
{
   expat_data* d = (expat_data*)handle;
   int res, errcode;
   char *errstring;
   ei_x_buff buf;

   res = XML_Parse(d->parser, buff, bufflen, 0);

   if(!res)
   {
      errcode = XML_GetErrorCode(d->parser);
      errstring = (char *)XML_ErrorString(errcode);

      ei_x_new_with_version(&buf);
      ei_x_encode_tuple_header(&buf, 2);
      ei_x_encode_long(&buf, XML_ERROR);
      ei_x_encode_tuple_header(&buf, 2);
      ei_x_encode_long(&buf, errcode);
      ei_x_encode_string_fixed(&buf, errstring);

      driver_output(d->port, buf.buff, buf.index);
      ei_x_free(&buf);
   }
   
   //driver_output(d->port, &res, 1);
}



ErlDrvEntry expat_driver_entry = {
   NULL,                       /* F_PTR init, N/A */
   expat_erl_start,          /* L_PTR start, called when port is opened */
   expat_erl_stop,           /* F_PTR stop, called when port is closed */
   expat_erl_output,         /* F_PTR output, called when erlang has sent */
   NULL,                       /* F_PTR ready_input, called when input descriptor ready */
   NULL,                       /* F_PTR ready_output, called when output descriptor ready */
   "expat_erl",              /* char *driver_name, the argument to open_port */
   NULL,                       /* F_PTR finish, called when unloaded */
   NULL,                       /* F_PTR control, port_command callback */
   NULL,                       /* F_PTR timeout, reserved */
   NULL                        /* F_PTR outputv, reserved */
};

DRIVER_INIT(expat_erl) /* must match name in driver_entry */
{
    return &expat_driver_entry;
}


