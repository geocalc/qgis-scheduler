/*
 * fcgi_state.c
 *
 *  Created on: 08.01.2016
 *      Author: jh
 */

/*
    Fast CGI state tracker.
    Parses the information flow during a fcgi session. Tracks the state.
    Gives information about the session state.

    Copyright (C) 2015,2016  Jörg Habenicht (jh@mwerk.net)

    This file is part of qgis-server-scheduler

    qgis-server-scheduler is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    qgis-server-scheduler is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/


#include "fcgi_state.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/queue.h>
#include <fastcgi.h>

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define _STR(x)	# x
#define STR(x)	_STR(x)

#define ASSEMBLE_FCGI_NUMBERS16(variable) \
    ( (( variable ## B1 ) << 8) + ( variable ## B0 ))
#define ASSEMBLE_FCGI_NUMBERS32(variable) \
    ( (( variable ## B3 ) << 24) + (( variable ## B2 ) << 16) + (( variable ## B1 ) << 8) + ( variable ## B0 ))

#define WRITE_FCGI_NUMBER16(variable, value) \
    ({ ( variable ## B0 ) = ( value ) & 0xff; \
       ( variable ## B1 ) = ( value ) >> 8; })
#define WRITE_FCGI_NUMBER32(variable, value) \
    ({ ( variable ## B0 ) = ( value ) & 0xff; \
       ( variable ## B1 ) = (( value ) >> 8) & 0xff; \
       ( variable ## B2 ) = (( value ) >> 16) & 0xff; \
       ( variable ## B3 ) = (( value ) >> 24) & 0xff; })

#define ASSEMBLE_PARAM_LENGTH32( b0, b1, b2, b3 ) \
    ( ((b3 & 0x7f) << 24) + (b2 << 16) + (b1 << 8) + b0 )

#define MAX_MESSAGE_PRINT_LEN	20




struct fcgi_param_s
{
    char *name;
    char *value;
};

struct fcgi_param_list_iterator_s
{
    TAILQ_ENTRY(fcgi_param_list_iterator_s) entries;          /* Linked list prev./next entry */
    struct fcgi_param_s param;
};

struct fcgi_param_list_s
{
    TAILQ_HEAD(param_listhead_s, fcgi_param_list_iterator_s) head;	/* Linked list head */
};

struct fcgi_message_s
{
    int bytes_read;
    uint16_t contentLength;
    int parse_header_done;
    //int parse_body_done;
    int parse_done;
    struct {
    FCGI_Header header;
    union {
	FCGI_UnknownTypeBody unknowntypebody;
	FCGI_EndRequestBody endrequestbody;
	FCGI_BeginRequestBody beginrequestbody;
    };
    } message;
    char *content;
};

struct fcgi_message_list_iterator_s
{
    TAILQ_ENTRY(fcgi_message_list_iterator_s) entries;          /* Linked list prev./next entry */
    struct fcgi_message_s *mess;
};

struct fcgi_message_list_s
{
    TAILQ_HEAD(message_listhead_s, fcgi_message_list_iterator_s) head;	/* Linked list head */
    int bytes_written;	/* count of bytes written in fcgi_message_list_write() buffer */
    //pthread_rwlock_t rwlock;	/* lock used to protect list structures (add, remove, find, ..) */
};

struct fcgi_session_s
{
    enum fcgi_session_state_e state;
    int bytes_received; // tracks how much bytes we got. This is reset, if bytes_received==contentLength
    uint16_t requestId;
    int keep_messages;
    struct fcgi_message_list_s *messlist;
    struct fcgi_param_list_s *paramlist;
};


static struct fcgi_param_list_s *fcgi_param_list_new(void);
static void fcgi_param_list_delete(struct fcgi_param_list_s *paramlist);
static int fcgi_param_list_parse(struct fcgi_param_list_s *paramlist, struct fcgi_message_s *message);

static struct fcgi_message_list_s *fcgi_message_list_new(void);
static void fcgi_message_list_delete(struct fcgi_message_list_s *messlist);
static void fcgi_message_list_add_message(struct fcgi_message_list_s *messlist, struct fcgi_message_s *message);
static struct fcgi_message_s *fcgi_message_list_get_last_message(struct fcgi_message_list_s *messlist);
static struct fcgi_message_list_iterator_s *fcgi_message_list_get_iterator(struct fcgi_message_list_s *list);
static struct fcgi_message_s *fcgi_message_list_get_next_message(struct fcgi_message_list_iterator_s **iterator);
static void fcgi_message_list_return_iterator(struct fcgi_message_list_s *list);


int fcgi_param_parse(struct fcgi_param_s *param, const unsigned char *buffer, int len)
{
    int dataread = 0;

    assert(param);
    if ( param && buffer && len>=2 )
    {
	int nameLen = *buffer++;
	len--;
	dataread++;
	if (nameLen>>7)
	{
	    /* 32 bit number. 3 more bytes following */
	    nameLen &= 0x7f;
	    nameLen<<=8;
	    nameLen += *buffer++;
	    nameLen<<=8;
	    nameLen += *buffer++;
	    nameLen<<=8;
	    nameLen += *buffer++;
	    len -= 3;
	    dataread += 3;
	}

	if (len < 0)
	{
	    return 0;
	}

	int valueLen = *buffer++;
	len--;
	dataread++;
	if (valueLen>>7)
	{
	    /* 32 bit number. 3 more bytes following */
	    valueLen &= 0x7f;
	    valueLen<<=8;
	    valueLen += *buffer++;
	    valueLen<<=8;
	    valueLen += *buffer++;
	    valueLen<<=8;
	    valueLen += *buffer++;
	    len -= 3;
	    dataread += 3;
	}

	if (len < 0)
	{
	    return 0;
	}

	if ( nameLen + valueLen > len )
	{
	    return 0;
	}

	param->name = malloc(nameLen+1);
	memcpy(param->name, buffer, nameLen);
	param->name[nameLen] = '\0';
	buffer += nameLen;

	param->value = malloc(valueLen+1);
	memcpy(param->value, buffer, valueLen);
	param->value[valueLen] = '\0';

	dataread += (nameLen + valueLen);
    }

    return dataread;
}


struct fcgi_param_list_s *fcgi_param_list_new(void)
{
    struct fcgi_param_list_s *list = calloc(1, sizeof(*list));
    assert(list);
    if ( !list )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    return list;
}


void fcgi_param_list_delete(struct fcgi_param_list_s *paramlist)
{
    if (paramlist)
    {
	while (paramlist->head.tqh_first != NULL)
	{
	    struct fcgi_param_list_iterator_s *entry = paramlist->head.tqh_first;

	    TAILQ_REMOVE(&paramlist->head, paramlist->head.tqh_first, entries);
	    free(entry->param.name);
	    free(entry->param.value);
	    free(entry);
	}

	free(paramlist);
    }
}

void fcgi_param_list_add_param(struct fcgi_param_list_s *paramlist, struct fcgi_param_s param)
{
    assert(paramlist);
    if(paramlist)
    {
	struct fcgi_param_list_iterator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}

	entry->param = param;

	/* if list is empty we have to insert at beginning,
	 * else insert at the end.
	 */
	if (paramlist->head.tqh_first)
	    TAILQ_INSERT_TAIL(&paramlist->head, entry, entries);      /* Insert at the end. */
	else
	    TAILQ_INSERT_HEAD(&paramlist->head, entry, entries);
    }

}

int fcgi_param_list_parse(struct fcgi_param_list_s *paramlist, struct fcgi_message_s *message)
{
    assert(paramlist);
    assert(message);

    if (paramlist && message)
    {
	struct fcgi_param_s param;

	int contentLength = message->contentLength;
	unsigned char *content = (unsigned char *)message->content;
	if (content)
	{
	    while (contentLength > 0)
	    {
		int retval = fcgi_param_parse(&param, content, contentLength);
		if (retval > 0)
		{
		    fcgi_param_list_add_param(paramlist, param);
		    content += retval;
		    contentLength -= retval;
		}
		else
		{
		    break;
		}
	    }
	}

	return 0;
    }

    return -1;
}

int fcgi_param_list_print(struct fcgi_param_list_s *paramlist)
{
    int bytes_printed = 0;

    assert(paramlist);
    if (paramlist)
    {
	struct fcgi_param_list_iterator_s *it = paramlist->head.tqh_first;
	for (it = paramlist->head.tqh_first; it != NULL; it = it->entries.tqe_next)
	{
	    int retval = fprintf(stderr, "%s=%s\n", it->param.name, it->param.value);
	    if (-1 == retval)
	    {
		perror("error fprintf");
		exit(EXIT_FAILURE);
	    }
	    bytes_printed += retval;
	}
    }

    return bytes_printed;
}


const char *fcgi_param_list_find(struct fcgi_param_list_s *paramlist, const char *name)
{
    const char *value = NULL;

    assert(paramlist);
    assert(name);
    if (paramlist && name)
    {
	struct fcgi_param_list_iterator_s *it = paramlist->head.tqh_first;
	for (it = paramlist->head.tqh_first; it != NULL; it = it->entries.tqe_next)
	{

	    int retval = strcmp(name, it->param.name);
	    if ( !retval )
	    {
		value = it->param.value;
		break;
	    }
	}

    }

    return value;
}


struct fcgi_message_s *fcgi_message_new(void)
{
    struct fcgi_message_s *message = calloc(1, sizeof(*message));
    assert(message);
    if ( !message )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    return message;
}

void fcgi_message_delete(struct fcgi_message_s *message)
{
    if (message)
    {
	free(message->content);
	free(message);
    }
}

int fcgi_message_parse(struct fcgi_message_s *message, const char *data, int len)
{
    assert(message);
    if (!message)
	return -1;

    if (message->parse_done)
	return 0;

    int dataread = 0;

    /* if we know the content length of the message,
     * read until all bytes are parsed and the message is complete
     *
     * else read until we know the content length and then go on
     * read the rest of the message.
     */

    if ( message->bytes_read < sizeof(message->message.header))
    {
	dataread = min(len, sizeof(message->message.header)-message->bytes_read);
	memcpy((&message->message.header)+message->bytes_read, data, dataread );

	/* did we read enough bytes? then go on parsing
	 * else wait for the next call
	 */
	message->bytes_read += dataread;

	if (message->bytes_read < sizeof(message->message.header))
	{
	    return dataread;
	}

	data += dataread;	// move content pointer to next content
	message->contentLength = ASSEMBLE_FCGI_NUMBERS16(message->message.header.contentLength);
	message->parse_header_done = 1;

	/* depending on the FCGI message type we need to allocate some content memory */
	unsigned char messageType = message->message.header.type;
	switch (messageType)
	{
	case FCGI_BEGIN_REQUEST:	// fall through
	case FCGI_ABORT_REQUEST:	// fall through
	case FCGI_END_REQUEST:
	    break;

	case FCGI_PARAMS:	// fall through
	case FCGI_STDIN:	// fall through
	case FCGI_STDOUT:	// fall through
	case FCGI_STDERR:	// fall through
	case FCGI_DATA:
	    if (message->contentLength > 0)
	    {
		message->content = calloc(1, message->contentLength);
		assert(message->content);
		if ( !message->content )
		{
		    perror("could not allocate memory");
		    exit(EXIT_FAILURE);
		}
	    }

	    break;

//	case FCGI_GET_VALUES:
//	case FCGI_GET_VALUES_RESULT:
//	case FCGI_UNKNOWN_TYPE:
	default:
	    fprintf(stderr, "error: unknown request id in message: %d\n", fcgi_message_get_requestid(message));
	    exit(EXIT_FAILURE);

	}
    }

    /* over here we have message->parse_header_done == 1 */
    assert(message->parse_header_done);

    len -= dataread;
    assert( len >= 0 );

    /* depending on the FCGI message type we copy into different memory */
    /* TODO: if we delete the union beginrequestbody and copy the content into
     *       *content then we can omit this switch() over here
     */
    unsigned char messageType = message->message.header.type;
    switch (messageType)
    {
    case FCGI_BEGIN_REQUEST:	// fall through
    case FCGI_END_REQUEST:
    {
	/* here we copy the remaining data into the message body.
	 * no content copy needed
	 */
	assert(message->contentLength == sizeof(message->message.beginrequestbody));

	int copylen = min(sizeof(message->message.beginrequestbody), len);
	if (copylen > 0)
	{
	    memcpy(&message->message.beginrequestbody, data, copylen);
	    message->bytes_read += copylen;	// this is the amount we virtually read in this function call
	    dataread += copylen;
	    len -= copylen;
	}
	break;
    }
    case FCGI_ABORT_REQUEST:
	assert(0 == message->contentLength);
	break;

    case FCGI_PARAMS:	// fall through
    case FCGI_STDIN:	// fall through
    case FCGI_STDOUT:	// fall through
    case FCGI_STDERR:	// fall through
    case FCGI_DATA:
    {
	// note: contentLength is defined as the length of the body, i.e. message struct - header struct.
	int remaining_length = sizeof(message->message.header) + message->contentLength - message->bytes_read; // this amount needs to be read to entirely fill this message
	assert(remaining_length >= 0);
	int content_read = min(len, remaining_length);	// this amount is available to be read
	assert(content_read >= 0);
	if (content_read > 0)
	{
	    memcpy(message->content, data, content_read);	// copy content into message buffer
	    message->bytes_read += content_read;	// this is the amount we virtually read in this function call
	    dataread += content_read;	// this is the amount we virtually read in this function call
	    len -= content_read;
	}
	break;
    }
//	case FCGI_GET_VALUES:
//	case FCGI_GET_VALUES_RESULT:
//	case FCGI_UNKNOWN_TYPE:
    default:
	fprintf(stderr, "error: unknown request id in message: %d\n", fcgi_message_get_requestid(message));
	exit(EXIT_FAILURE);

    }


    /* if the old data read plus the current data read
     * is the same as content length
     * we are done
     */
    if (message->bytes_read >= message->contentLength)
	message->parse_done = 1;

    return dataread;
}


int fcgi_message_get_parse_done(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    return message->parse_done?1:0;
}


int fcgi_message_get_requestid(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return 0;

    return ASSEMBLE_FCGI_NUMBERS16(message->message.header.requestId);
}


/* return: message type or -1 in case of error */
int fcgi_message_get_type(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return 0;

    return message->message.header.type;
}


int fcgi_message_get_role(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return 0;

    if ( FCGI_BEGIN_REQUEST != message->message.header.type )
	return 0;

    return ASSEMBLE_FCGI_NUMBERS16(message->message.beginrequestbody.role);
}


int fcgi_message_get_flag(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return -1;

    return message->message.beginrequestbody.flags;
}


int fcgi_message_set_flag(struct fcgi_message_s *message, unsigned char flags)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return -1;

    message->message.beginrequestbody.flags = flags;

    return 0;
}


static int fcgi_message_get_size(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return 0;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return 0;


    return sizeof(message->message.header) + message->contentLength;
}



/* writes message into buffer of size len.
 * returns the bytes written.
 */
int fcgi_message_write(unsigned char *buffer, int len, const struct fcgi_message_s *message)
{
    assert(buffer);
    if ( !buffer )
	return -1;
    assert(message);
    if ( !message )
	return 0;

    int written = min(fcgi_message_get_size(message), len);
    unsigned char messageType = message->message.header.type;
    switch (messageType)
    {
    case FCGI_BEGIN_REQUEST:	// fall through
    case FCGI_ABORT_REQUEST:	// fall through
    case FCGI_END_REQUEST:
	// here I assume that the ABORT_REQUEST has contentLength = 0
	memcpy(buffer, &message->message, written);
	break;

    case FCGI_PARAMS:	// fall through
    case FCGI_STDIN:	// fall through
    case FCGI_STDOUT:	// fall through
    case FCGI_STDERR:	// fall through
    case FCGI_DATA:
	if (written <= sizeof(message->message.header))
	{
	    memcpy(buffer, &message->message.header, written);
	}
	else
	{
	    memcpy(buffer, &message->message.header, sizeof(message->message.header));
	    memcpy(buffer, message->content, written-sizeof(message->message.header));
	}

	break;

//    case FCGI_GET_VALUES:
//    case FCGI_GET_VALUES_RESULT:
//    case FCGI_UNKNOWN_TYPE:
    default:
	fprintf(stderr, "error: unknown request id in message: %d\n", fcgi_message_get_requestid(message));
	exit(EXIT_FAILURE);
    }

    return written;
}


/* prints message to stderr
 * return: number of bytes printed
 */
int fcgi_message_print(const struct fcgi_message_s *message)
{
    int bytes_printed = 0;

    assert(message);
    assert(message->parse_header_done);
    if ( message && message->parse_header_done )
    {
	int requestId = fcgi_message_get_requestid(message);
	    unsigned char messageType = message->message.header.type;
	    switch (messageType)
	    {
	    case FCGI_BEGIN_REQUEST:
	    {
		bytes_printed += fprintf(stderr, "{FCGI_BEGIN_REQUEST, %d, {", requestId);
		int role = fcgi_message_get_role(message);
		switch (role)
		{
		case FCGI_RESPONDER:
		    bytes_printed += fprintf(stderr, "FCGI_RESPONDER");
		    break;
		case FCGI_AUTHORIZER:
		    bytes_printed += fprintf(stderr, "FCGI_AUTHORIZER");
		    break;
		case FCGI_FILTER:
		    bytes_printed += fprintf(stderr, "FCGI_FILTER");
		    break;
		default:
		    fprintf(stderr, "error: unknown role %d\n", role);
		    break;
		}
		bytes_printed += fprintf(stderr, ", 0x%02x}}\n", message->message.beginrequestbody.flags);
		break;
	    }
	    case FCGI_ABORT_REQUEST:
		bytes_printed += fprintf(stderr, "{FCGI_ABORT_REQUEST, %d}\n", requestId);
		break;
	    case FCGI_END_REQUEST:
	    {
		int appStatus = ASSEMBLE_FCGI_NUMBERS32(message->message.endrequestbody.appStatus);
		bytes_printed += fprintf(stderr, "{FCGI_END_REQUEST, %d, { %d, \n", requestId, appStatus);
		switch (message->message.endrequestbody.protocolStatus)
		{
		case FCGI_REQUEST_COMPLETE:
		    bytes_printed += fprintf(stderr, "FCGI_REQUEST_COMPLETE}}\n");
		    break;
		case FCGI_CANT_MPX_CONN:
		    bytes_printed += fprintf(stderr, "FCGI_CANT_MPX_CONN}}\n");
		    break;
		case FCGI_OVERLOADED:
		    bytes_printed += fprintf(stderr, "FCGI_OVERLOADED}}\n");
		    break;
		case FCGI_UNKNOWN_ROLE:
		    bytes_printed += fprintf(stderr, "FCGI_UNKNOWN_ROLE}}\n");
		    break;
		default:
		    fprintf(stderr, "error: unknown protocol status %d\n", message->message.endrequestbody.protocolStatus);
		    break;
		}
		break;
	    }
	    case FCGI_PARAMS:
		bytes_printed += fprintf(stderr, "{FCGI_PARAMS, %d, { \"%."STR(MAX_MESSAGE_PRINT_LEN)"s\"%s = %u}\n", requestId, (message->content?message->content:""), (message->contentLength>MAX_MESSAGE_PRINT_LEN?"...":""), message->contentLength);
		break;
	    case FCGI_STDIN:
		bytes_printed += fprintf(stderr, "{FCGI_STDIN, %d, { \"%."STR(MAX_MESSAGE_PRINT_LEN)"s\"%s = %u}\n", requestId, (message->content?message->content:""), (message->contentLength>MAX_MESSAGE_PRINT_LEN?"...":""), message->contentLength);
		break;
	    case FCGI_STDOUT:
		bytes_printed += fprintf(stderr, "{FCGI_STDOUT, %d, { \"%."STR(MAX_MESSAGE_PRINT_LEN)"s\"%s = %u}\n", requestId, (message->content?message->content:""), (message->contentLength>MAX_MESSAGE_PRINT_LEN?"...":""), message->contentLength);
		break;
	    case FCGI_STDERR:
		bytes_printed += fprintf(stderr, "{FCGI_STDERR, %d, { \"%."STR(MAX_MESSAGE_PRINT_LEN)"s\"%s = %u}\n", requestId, (message->content?message->content:""), (message->contentLength>MAX_MESSAGE_PRINT_LEN?"...":""), message->contentLength);
		break;
	    case FCGI_DATA:
		bytes_printed += fprintf(stderr, "{FCGI_DATA, %d, { \"%."STR(MAX_MESSAGE_PRINT_LEN)"s\"%s = %u}\n", requestId, (message->content?message->content:""), (message->contentLength>MAX_MESSAGE_PRINT_LEN?"...":""), message->contentLength);
		break;

	//    case FCGI_GET_VALUES:
	//    case FCGI_GET_VALUES_RESULT:
	//    case FCGI_UNKNOWN_TYPE:
	    default:
		fprintf(stderr, "error: unknown request id in message: %d\n", fcgi_message_get_requestid(message));
		exit(EXIT_FAILURE);
	    }

    }

    return bytes_printed;
}


struct fcgi_message_list_s *fcgi_message_list_new(void)
{
    struct fcgi_message_list_s *messlist = calloc(1, sizeof(*messlist));
    assert(messlist);
    if ( !messlist )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    return messlist;
}



void fcgi_message_list_delete(struct fcgi_message_list_s *messlist)
{
    if (messlist)
    {
	while (messlist->head.tqh_first != NULL)
	{
	    struct fcgi_message_list_iterator_s *entry = messlist->head.tqh_first;

	    TAILQ_REMOVE(&messlist->head, messlist->head.tqh_first, entries);
	    fcgi_message_delete(entry->mess);
	    free(entry);
	}

	free(messlist);
    }
}



void fcgi_message_list_add_message(struct fcgi_message_list_s *messlist, struct fcgi_message_s *message)
{
    assert(messlist);
    assert(message);
    if(messlist && message)
    {
	struct fcgi_message_list_iterator_s *entry = malloc(sizeof(*entry));
	assert(entry);
	if ( !entry )
	{
	    perror("could not allocate memory");
	    exit(EXIT_FAILURE);
	}

	entry->mess = message;

	/* if list is empty we have to insert at beginning,
	 * else insert at the end.
	 */
	if (messlist->head.tqh_first)
	    TAILQ_INSERT_TAIL(&messlist->head, entry, entries);      /* Insert at the end. */
	else
	    TAILQ_INSERT_HEAD(&messlist->head, entry, entries);
    }
}


struct fcgi_message_s *fcgi_message_list_get_last_message(struct fcgi_message_list_s *messlist)
{
    struct fcgi_message_s *message = NULL;
    assert(messlist);
    if(messlist)
    {
	if (messlist->head.tqh_last)
	{
	    struct fcgi_message_list_iterator_s *np = *messlist->head.tqh_last;

	    message = np->mess;
	}
    }

    return message;
}


struct fcgi_message_list_iterator_s *fcgi_message_list_get_iterator(struct fcgi_message_list_s *list)
{
    assert(list);
    if (list)
    {
	return list->head.tqh_first;
    }

    return NULL;
}


struct fcgi_message_s *fcgi_message_list_get_next_message(struct fcgi_message_list_iterator_s **iterator)
{
    assert(iterator);
    if (iterator)
    {
	if (*iterator)
	{
	    struct fcgi_message_s *proc = (*iterator)->mess;
	    *iterator = (*iterator)->entries.tqe_next;
	    return proc;
	}
    }

    return NULL;
}


void fcgi_message_list_return_iterator(struct fcgi_message_list_s *list)
{

}




//int fcgi_message_list_write(unsigned char *buffer, int len, const struct fcgi_message_list_s *messlist)
//{
//    assert(messlist);
//    if (messlist)
//    {
//	assert(messlist->bytes_written >= 0);
//	int byteswritten = messlist->bytes_written;
//	int bufferwritten = 0;
//
//	struct fcgi_message_s *message;
//	struct fcgi_message_list_iterator *np;
//
//	for (np = messlist->head.lh_first; np != NULL; np = np->entries.le_next)
//	{
//	    message = np->mess;
//	    assert(message);
//
//	    int towrite = min(len, fcgi_state_get_message_size(message));
//	    byteswritten -= towrite;
//	    if (byteswritten < 0)
//	    {
//		towrite = -byteswritten;
//
//		bufferwritten += towrite;
//		byteswritten = 0;
//	    }
//
//	}
//
//
//	bufferwritten -= messlist->bytes_written;
//	messlist->bytes_written += bufferwritten;
//	return bufferwritten;
//    }
//
//    return -1;
//}





struct fcgi_session_s *fcgi_session_new(int keep_messages)
{
    struct fcgi_session_s *session = calloc(1, sizeof(*session));
    assert(session);
    if ( !session )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    session->keep_messages = keep_messages;
    session->messlist = fcgi_message_list_new();

    return session;
}

void fcgi_session_delete(struct fcgi_session_s *session)
{
    if (session)
    {
	fcgi_message_list_delete(session->messlist);
	fcgi_param_list_delete(session->paramlist);
	free(session);
    }
}

int fcgi_session_parse(struct fcgi_session_s *session, const char *data, int len)
{
    /* copy the data into the session header.
     * if not enough data is provided, wait for the next data.
     */
    assert(session);
    assert(session->messlist);
    int dataread = 0;

    if (session && session->messlist)
    {
	int bytes_read = 0;

	struct fcgi_message_s *message = fcgi_message_list_get_last_message(session->messlist);
	if ( message && !fcgi_message_get_parse_done(message) )
	{
	    bytes_read = fcgi_message_parse(message, data, len);
	    assert(bytes_read); // if message parse is not done it should read at least one byte
	    len -= bytes_read;
	    dataread += bytes_read;
	    data += bytes_read;
	    if (fcgi_message_get_parse_done(message))
	    {
		//fcgi_message_print(message);
		if ( FCGI_PARAMS == fcgi_message_get_type(message) )
		{
		    if ( !session->paramlist )
			session->paramlist = fcgi_param_list_new();

		    fcgi_param_list_parse(session->paramlist, message);
		}
	    }
	}

	if ( len > 0 )
	{
	    // if bytes available assure the last parse has finished
	    if (message)
		assert(fcgi_message_get_parse_done(message));

	    while ( len > 0 )
	    {
		message = fcgi_message_new();

		int readbytes = fcgi_message_parse(message, data, len);
		assert(readbytes);	// if bytes to read are available, the function should read them
		len -= readbytes;
		assert(len>=0);		// functions should not read more bytes than available
		dataread += readbytes;
		data += readbytes;
		fcgi_message_list_add_message(session->messlist, message);

		if (!fcgi_message_get_parse_done(message))
		    break;

		//fcgi_message_print(message);
		if ( FCGI_PARAMS == fcgi_message_get_type(message) )
		{
		    if ( !session->paramlist )
			session->paramlist = fcgi_param_list_new();

		    fcgi_param_list_parse(session->paramlist, message);
		}
	    }
	}
    }


    return dataread;

}


int fcgi_session_need_more_data(struct fcgi_session_s *session)
{
    assert(session);
    assert(session->messlist);
    if (session && session->messlist)
    {
	struct fcgi_message_s *message = fcgi_message_list_get_last_message(session->messlist);

	if ( !message )
	    return 0;	// if there is no message we don't need more data

	int parse_done = fcgi_message_get_parse_done(message);
	if (parse_done >= 0)
	    return !parse_done;
    }

    return -1;
}


//int fcgi_state_get_session_id(const struct fcgi_session_s *session)
//{
//
//}


int fcgi_session_print(const struct fcgi_session_s *session)
{
    int bytes_printed = 0;

    assert(session);
    assert(session->messlist);
    if (session && session->messlist)
    {
	struct fcgi_message_list_iterator_s *it = fcgi_message_list_get_iterator(session->messlist);
	struct fcgi_message_s *message;
	while ((message = fcgi_message_list_get_next_message(&it)) != NULL)
	{
	    bytes_printed += fcgi_message_print(message);
	}
	fcgi_message_list_return_iterator(session->messlist);

	if (session->paramlist)
	{
	    fcgi_param_list_print(session->paramlist);
	}
    }

    return bytes_printed;
}


const char *fcgi_session_get_param(const struct fcgi_session_s *session, const char *name)
{
    const char *retval = NULL;

    assert(session);
    if (session && session->paramlist)
    {
	retval = fcgi_param_list_find(session->paramlist, name);
    }

    return retval;
}


enum fcgi_session_state_e fcgi_session_get_state(const struct fcgi_session_s *session)
{
    assert(session);
    return session?session->state:FCGI_SESSION_STATE_ERROR;
}


struct fcgi_message_s *fcgi_message_new_endrequest(uint16_t requestId, uint32_t appStatus, unsigned char protocolStatus)
{
    struct fcgi_message_s *message = calloc(1, sizeof(*message));
    assert(message);
    if ( !message )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    message->message.header.version = FCGI_VERSION_1;
    message->message.header.type = FCGI_END_REQUEST;
    WRITE_FCGI_NUMBER16(message->message.header.contentLength, sizeof(message->message.endrequestbody));
    WRITE_FCGI_NUMBER16(message->message.header.requestId, requestId);
    WRITE_FCGI_NUMBER32(message->message.endrequestbody.appStatus, appStatus);
    message->message.endrequestbody.protocolStatus = protocolStatus;

    return message;
}


