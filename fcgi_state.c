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
#include <fastcgi.h>

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })


#define ASSEMBLE_FCGI_NUMBERS16(variable) \
    ( (( variable ## B1 ) << 8) + ( variable ## B0 ))
#define ASSEMBLE_FCGI_NUMBERS32(variable) \
    ( (( variable ## B3 ) << 32) + (( variable ## B2 ) << 16) + (( variable ## B1 ) << 8) + ( variable ## B0 ))

#define WRITE_FCGI_NUMBER16(variable, value) \
    ({ ( variable ## B0 ) = ( value ) & 0xff; \
       ( variable ## B1 ) = ( value ) >> 8; })
#define WRITE_FCGI_NUMBER32(variable, value) \
    ({ ( variable ## B0 ) = ( value ) & 0xff; \
       ( variable ## B1 ) = (( value ) >> 8) & 0xff; \
       ( variable ## B2 ) = (( value ) >> 16) & 0xff; \
       ( variable ## B3 ) = (( value ) >> 24) & 0xff; })



struct fcgi_session_s
{
    enum fcgi_state_e state;
    int bytes_received; // tracks how much bytes we got. This is reset, if bytes_received==contentLength
    uint16_t requestId;
    uint16_t contentLength;
    FCGI_Header header;
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
};

struct fcgi_message_s *fcgi_state_new_message(void)
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

void fcgi_state_delete_message(struct fcgi_message_s *message)
{
    free(message);
}

int fcgi_state_parse_message(struct fcgi_message_s *message, const char *data, int len)
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

    if ( message->bytes_read < sizeof(message->message))
    //if ( !message->parse_header_done )
    {
	dataread = min(len, sizeof(message->message)-message->bytes_read);
	memcpy((&message->message)+message->bytes_read, data, dataread );

	/* did we read enough bytes? then go on parsing
	 * else wait for the next call
	 */
	message->bytes_read += dataread;

	if (message->bytes_read < sizeof(message->message))
	{
	    return dataread;
	}

	message->contentLength = ASSEMBLE_FCGI_NUMBERS16(message->message.header.contentLength);
	message->parse_header_done = 1;
    }

    /* over here we have message->parse_header_done == 1 */
    assert(message->parse_header_done);

    len -= dataread;
    assert( len >= 0 );

    // note: contentLength is defined as the length of the body, i.e. message struct - header struct.
    int remaining_length = sizeof(message->message.header) + message->contentLength - message->bytes_read; // this amount needs to be read
    int content_read = min(len, remaining_length);
    message->bytes_read += content_read;	// this is the amount we virtually read in this function call
    dataread += content_read;	// this is the amount we virtually read in this function call

    /* if the old data read plus the current data read
     * is the same as content length
     * we are done
     */
    if (message->bytes_read >= message->contentLength)
	message->parse_done = 1;

    return dataread;
}

int fcgi_state_get_message_parse_done(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    return message->parse_done?1:0;
}


int fcgi_state_get_message_requestid(const struct fcgi_message_s *message)
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
int fcgi_state_get_message_type(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return 0;

    return message->message.header.type;
}


int fcgi_state_get_message_role(const struct fcgi_message_s *message)
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


int fcgi_state_get_message_flag(const struct fcgi_message_s *message)
{
    assert(message);
    if ( !message )
	return -1;

    assert(message->parse_header_done);
    if ( !message->parse_header_done )
	return -1;

    return message->message.beginrequestbody.flags;
}


int fcgi_state_set_message_flag(struct fcgi_message_s *message, unsigned char flags)
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


int fcgi_state_message_write(unsigned char *buffer, int len, const struct fcgi_message_s *message)
{
    assert(buffer);
    if ( !buffer )
	return -1;
    assert(message);
    if ( !message )
	return 0;

    int written = min(sizeof(message->message), len);
    memcpy(buffer, &message->message, written);

    return written;
}



struct fcgi_session_s *fcgi_state_new_session(void)
{
    struct fcgi_session_s *session = calloc(1, sizeof(*session));
    assert(session);
    if ( !session )
    {
	perror("could not allocate memory");
	exit(EXIT_FAILURE);
    }

    return session;
}

void fcgi_state_delete_session(struct fcgi_session_s *session)
{
    free(session);
}

int fcgi_state_parse(struct fcgi_session_s *session, const char *data, int len)
{
    /* copy the data into the session header.
     * if not enough data is provided, wait for the next data.
     */
    assert(session);
    int dataread = 0;

    if (session)
    {
	if (session->bytes_received < sizeof(session->header))
	{
	    /* parse the header information */
	    FCGI_Header header;
	    int bytes_received;

	    /* save already received information in local structure,
	     * append new data and test for valid data
	     */
	    memcpy(&header, &session->header, session->bytes_received);
	    dataread = min(len, sizeof(header)-session->bytes_received);
	    memcpy((&header)+session->bytes_received, data, dataread );
	    bytes_received = session->bytes_received + dataread;

	    /* not enough data?
	     * save already received data and
	     * return to caller to provide more information
	     */
	    if (bytes_received < sizeof(header))
	    {
		session->bytes_received = bytes_received;
		memcpy(&session->header, &header, bytes_received);

		return dataread;
	    }

	    /* enough data to parse the header */
	    uint16_t requestId = ASSEMBLE_FCGI_NUMBERS16(header.requestId);

	    /* wrong session ? */
	    if ((FCGI_STATE_RUNNING == session->state) && (requestId != session->requestId))
	    {
		return -1;
	    }

	    /* if we start a new session, save the parsed data */
	    if (FCGI_STATE_RUNNING != session->state)
	    {
		session->state = FCGI_STATE_RUNNING;
		session->requestId = requestId;
		session->bytes_received = bytes_received;
	    }

	    len -= dataread;
	    assert(len >= 0);

	    if (len == 0)
		return dataread;

	    //uint16_t contentLength = ASSEMBLE_FCGI_NUMBERS16(header.contentLength);

	    switch(header.type)
	    {
	    case FCGI_BEGIN_REQUEST:
	    {
		FCGI_BeginRequestBody body;
		int databodylen = min(len, sizeof(body));
		memcpy((&body), data+dataread, databodylen );

		break;
	    }
	    case FCGI_ABORT_REQUEST:
	    case FCGI_END_REQUEST:
	    case FCGI_PARAMS:
	    case FCGI_STDIN:
	    case FCGI_STDOUT:
	    case FCGI_STDERR:
	    case FCGI_DATA:
	    case FCGI_GET_VALUES:
	    case FCGI_GET_VALUES_RESULT:
	    case FCGI_UNKNOWN_TYPE:
		// TODO: please help yourself if you need other protocol types over here
		break;
	    default:
		return -1;
	    }
	}
    }


    return dataread;

}


struct fcgi_message_s *fcgi_state_new_endrequest_message(uint16_t requestId, uint32_t appStatus, unsigned char protocolStatus)
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


