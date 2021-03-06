/*

sshcrdown.c

  Authors:
        Tatu Ylonen <ylo@ssh.fi>
        Markku-Juhani Saarinen <mjos@ssh.fi>
        Timo J. Rinne <tri@ssh.fi>
        Sami Lehtinen <sjl@ssh.fi>

  Copyright (C) 1997-1998 SSH Communications Security Oy, Espoo, Finland
  All rights reserved.

Helper functions for the cross-layer protocol.  This file contains
functions to make it very easy to implement a cross layer stream (the
"Down" direction, which takes a downward going normal stream and gives a
nice packet based interface for the module using it).  Functions in this
module would be used in applications or protocol modules that sit above
a cross-layer based protocol module.

*/

#define ALLOW_AFTER_BUFFER_FULL         (10000 + 5)
#define BUFFER_MAX_SIZE                 50000

#include "sshincludes.h"
#include "sshbuffer.h"
#include "sshbufaux.h"
#include "sshgetput.h"
#include "sshstream.h"
#include "sshmsgs.h"
#include "sshencode.h"
#include "sshcross.h"

struct SshCrossDownRec
{
  /* The underlying stream going down.  This stream will be automatically
     closed when we are destroyed. */
  SshStream stream;

  /* SshBuffer for incoming data (downwards). */
  SshBuffer incoming;
  Boolean incoming_eof;
  
  /* SshBuffer for outgoing data (downwards). */
  SshBuffer outgoing;
  Boolean outgoing_eof;

  /* Flag indicating that ssh_cross_down_can_send has returned FALSE, and
     thus we should call the can_send callback when sending is again
     possible. */
  Boolean send_blocked;

  /* Flag indicating whether we can receive.  This flag can be set by
     the application using ssh_cross_down_can_receive. */
  Boolean can_receive;

  /* Flag indicating that we have been destroyed, but the destroy has been
     postponed until buffers have drained. */
  Boolean destroy_pending;

  /* If TRUE, we are in a callback in a situation where we cannot destroy
     immediately.  If this is true in a destroy, destroy_requested is set
     to TRUE, and destroy will be called when possible. */
  Boolean cannot_destroy;

  /* Set to TRUE to request immediate destroy after returning from a
     callback. */
  Boolean destroy_requested;
  
  /* Flag indicating that we have shortcircuited the stream.  If this is
     FALSE but shortcircuit_up_stream is non-NULL, we have a shortcircuit
     pending as soon as downward buffers have drained. */
  Boolean shortcircuited;

  /* The stream to which we have shortcircuited.  NULL if not shortcircuited
     and no shortcircuit pending. */
  SshStream shortcircuit_up_stream;
  
  /* Application callbacks. */
  SshCrossPacketProc received_packet;
  SshCrossEofProc received_eof;
  SshCrossCanSendNotify can_send;
  void *context;
};

/* Destroys the protocol context immediately.  Closes the downward stream
   and frees memory. */

void ssh_cross_down_destroy_now(SshCrossDown down)
{
  /* Close the downward stream. */
  ssh_stream_destroy(down->stream);

  /* Uninitialize buffers. */
  ssh_buffer_uninit(&down->incoming);
  ssh_buffer_uninit(&down->outgoing);

  /* Fill the context with 'F' to ease debugging, and free it. */
  memset(down, 'F', sizeof(*down));
  ssh_xfree(down);
}

/* This function outputs as much data from internal buffers to the downward
   stream.  This returns TRUE if something was successfully written. */

Boolean ssh_cross_down_output(SshCrossDown down)
{
  int len;
  Boolean return_value = FALSE;

  /* Loop while we have data to output.  When all data has been sent,
     we check whether we need to send EOF. */
  while (ssh_buffer_len(&down->outgoing) > 0)
    {
      /* Write as much data as possible. */
      len = ssh_stream_write(down->stream, ssh_buffer_ptr(&down->outgoing),
                             ssh_buffer_len(&down->outgoing));
      if (len < 0)
        return return_value;  /* Cannot write more now. */
      if (len == 0)
        {
          /* EOF on output; will not be able to write any more. */
          down->outgoing_eof = TRUE;
          ssh_buffer_clear(&down->outgoing);
          return TRUE;
        }
      
      /* Consume written data. */
      ssh_buffer_consume(&down->outgoing, len);

      /* We've done something, so return TRUE. */
      return_value = TRUE;
    }

  /* All output has drained.  There is no more buffered data. */
  if (down->send_blocked)
    {
      down->cannot_destroy = TRUE;
      if (down->can_send)
        (*down->can_send)(down->context);
      down->cannot_destroy = FALSE;
      if (down->destroy_requested)
        {
          ssh_cross_down_destroy(down);
          return FALSE;
        }
      down->send_blocked = FALSE;
    }
  
  /* If we should send EOF after output has drained, do it now. */
  if (down->outgoing_eof)
    ssh_stream_output_eof(down->stream);

  /* If we get here and the stream is shortcircuited, that means we had
     output data to drain before shortcircuiting. */
  if (down->shortcircuit_up_stream && !down->shortcircuited)
    {
      down->shortcircuited = TRUE;
      ssh_cross_up_shortcircuit_now(down->shortcircuit_up_stream,
                                    down->stream);
    }

  /* If there's a destroy pending (that is, waiting for buffers to drain),
     do the destroy now. */
  if (down->destroy_pending)
    {
      /* Destroy the context now.  This also closes the stream. */
      ssh_cross_down_destroy_now(down);

      /* Return FALSE to ensure that the loop in ssh_cross_down_callback
         exits without looking at the context again. */
      return FALSE;
    }

  return return_value;
}

/* Reads as much data as possible from the downward stream, assuming we can
   receive packets.  Passes any received packets to the appropriate callbacks.
   Returns TRUE if packets were successfully received. */

Boolean ssh_cross_down_input(SshCrossDown down)
{
  size_t data_to_read, data_read;
  int ret;
  unsigned char *ptr;
  SshCrossPacketType type;
  Boolean return_value = FALSE;

  for (;;)
    {
      /* If we cannot receive, return immediately. */
      if (!down->can_receive || down->incoming_eof || down->destroy_pending ||
          down->shortcircuit_up_stream != NULL)
        return return_value;

      /* Get length of data read so far. */
      data_read = ssh_buffer_len(&down->incoming);

      /* Add enough space to buffer for reading either header or
         entire packet.  This also sets `ptr' to point to the place
         where data should be read, and `data_to_read' to the number
         of bytes that should be there after reading (should read
         data_to_read - data_read bytes). */
      if (data_read < 4)
        {
          /* Packet header not yet in buffer.  Read only header. */
          data_to_read = 4;
          ssh_buffer_append_space(&down->incoming, &ptr, 4 - data_read);
        }
      else
        {
          /* Packet header already in buffer. */
          ptr = ssh_buffer_ptr(&down->incoming);
          data_to_read = 4 + SSH_GET_32BIT(ptr);
          assert(data_to_read > data_read);
          ssh_buffer_append_space(&down->incoming, &ptr, data_to_read - data_read);
        }
  
      /* Keep reading until entire packet read, or no more data available. */
      while (data_read < data_to_read)
        {
          /* Try to read the remaining bytes. */
          ptr = (unsigned char *)ssh_buffer_ptr(&down->incoming) + data_read;
          ret = ssh_stream_read(down->stream, ptr, data_to_read - data_read);
          if (ret < 0)
            {
              /* No more data available at this time.  Remove
                 allocated but unread space from end of buffer. */
              ssh_buffer_consume_end(&down->incoming, data_to_read - data_read);
              return return_value;
            }

          if (ret == 0)
            {
              /* EOF received. */
              ssh_buffer_consume_end(&down->incoming, data_to_read - data_read);
              down->incoming_eof = TRUE;
              
              /* Pass the EOF to the application callback. */
              down->cannot_destroy = TRUE;
              if (down->received_eof)
                (*down->received_eof)(down->context);
              down->cannot_destroy = FALSE;
              if (down->destroy_requested)
                {
                  ssh_cross_down_destroy(down);
                  return FALSE;
                }
              return TRUE;
            }

          if (data_read < 4 && data_read + ret >= 4)
            {
              /* Header has now been fully received.  Prepare to receive rest
                 of packet. */
              data_read += ret;
              ptr = ssh_buffer_ptr(&down->incoming);
              data_to_read = 4 + SSH_GET_32BIT(ptr);
              if (data_to_read > data_read)
                ssh_buffer_append_space(&down->incoming, &ptr,
                                    data_to_read - data_read);
            }
          else
            data_read += ret;
        }

      /* An entire packet has been received. */
      assert(ssh_buffer_len(&down->incoming) == data_to_read);
      
      /* Get packet type. */
      ptr = ssh_buffer_ptr(&down->incoming);
      type = (SshCrossPacketType)ptr[4];
      
      /* Call the application callback if set. */
      down->cannot_destroy = TRUE;
      if (down->received_packet)
        (*down->received_packet)(type, ptr + 5, data_to_read - 5,
                                 down->context);
      down->cannot_destroy = FALSE;
      if (down->destroy_requested)
        {
          ssh_cross_down_destroy(down);
          return FALSE;
        }
      ssh_buffer_clear(&down->incoming);
      
      return_value = TRUE;
    }
  /*NOTREACHED*/
}

/* Callback function for the lower-level stream.  This receives notifications
   when we can read/write data from the lower-level stream. */

void ssh_cross_down_callback(SshStreamNotification op, void *context)
{
  SshCrossDown down = (SshCrossDown)context;
  Boolean ret;
  
  ret = FALSE;
  
  /* Process the notification.  We loop between input and output
     operations until one returns FALSE (they return TRUE if the other
     operation should be performed). */
  do
    {
      switch (op)
        {
        case SSH_STREAM_CAN_OUTPUT:
          ret = ssh_cross_down_output(down);
          op = SSH_STREAM_INPUT_AVAILABLE;
          break;
          
        case SSH_STREAM_INPUT_AVAILABLE:
          ret = ssh_cross_down_input(down);
          op = SSH_STREAM_CAN_OUTPUT;
          break;
          
        case SSH_STREAM_DISCONNECTED:
          ssh_debug("ssh_cross_down_callback: disconnected");
          ret = FALSE;
          break;
          
        default:
          ssh_fatal("ssh_cross_down_callback: unknown op %d", (int)op);
        }
      /* Note: `down' might have been destroyed by now.  In that case
         `ret' is FALSE. */
    }
  while (ret == TRUE);
}

/* Creates a cross-layer packet handler for the stream going downwards.
   This makes it easy for applications and protocol levels to use the
   cross-layer interface.  This returns a context handle that should be
   destroyed with ssh_cross_down_destroy when the downward connection is
   to be closed.  The stream will be destroyed automatically when this is
   closed.  This will take control of the stream.
      `down_stream'          stream to lower-level protocol (or network)
      `received_packet'      called when a packet is received
      `received_eof'         called when EOF is received
      `can_send'             called when we can send after not being able to
      `context'              passed as argument to callbacks
   Any of the functions can be NULL if not needed. */

SshCrossDown ssh_cross_down_create(SshStream down_stream,
                                   SshCrossPacketProc received_packet,
                                   SshCrossEofProc received_eof,
                                   SshCrossCanSendNotify can_send,
                                   void *context)
{
  SshCrossDown down;

  down = ssh_xcalloc(1, sizeof(*down));
  down->stream = down_stream;
  ssh_buffer_init(&down->incoming);
  ssh_buffer_init(&down->outgoing);
  down->incoming_eof = FALSE;
  down->outgoing_eof = FALSE;
  down->send_blocked = TRUE;
  down->can_receive = FALSE;
  down->destroy_pending = FALSE;
  down->cannot_destroy = FALSE;
  down->destroy_requested = FALSE;
  down->shortcircuited = FALSE;

  /* Save the callback functions. */
  down->received_packet = received_packet;
  down->received_eof = received_eof;
  down->can_send = can_send;
  down->context = context;

  /* Set callback for the downward stream.  Note that this will also cause
     can_send to be called from the output callback. */
  ssh_stream_set_callback(down->stream, ssh_cross_down_callback, (void *)down);
  
  return down;
}

/* Closes and destroys the downward connection.  This automatically
   closes the underlying stream.  Any buffered data will be sent out
   before the stream is actually closed.  It is illegal to access the
   object after this has been called. */

void ssh_cross_down_destroy(SshCrossDown down)
{
  /* Clear the callbacks so that user functions are not called. */
  down->received_packet = NULL;
  down->received_eof = NULL;
  down->can_send = NULL;

  /* If we cannot destroy at this time, set the proper flag and return
     immediately without destroying.  This happens in some callbacks.
     The code after the callback will check for the flag and call destroy
     again if set. */
  if (down->cannot_destroy)
    {
      down->destroy_requested = TRUE;
      return;
    }

  down->destroy_pending = TRUE;

  if (ssh_buffer_len(&down->outgoing) == 0)
    ssh_cross_down_destroy_now(down);
}

/* Informs the packet code whether `received_packet' can be called.  This is
   used for flow control.  Initially, packets cannot be received. */

void ssh_cross_down_can_receive(SshCrossDown down, Boolean status)
{
  down->can_receive = status;
  if (status == TRUE)
    {
      /* Reset the callbacks to ensure that our callback gets called. */
      ssh_stream_set_callback(down->stream, ssh_cross_down_callback,
                              (void *)down);
    }
}

/* Sends EOF to the downward stream (after sending out any buffered data).
   It is illegal to send any packets after calling this. */

void ssh_cross_down_send_eof(SshCrossDown down)
{
  /* If EOF already sent, return immediately. */
  if (down->outgoing_eof)
    return;

  /* Otherwise, send EOF now. */
  down->outgoing_eof = TRUE;
  if (ssh_buffer_len(&down->outgoing) == 0)
    ssh_stream_output_eof(down->stream);
}

/* Returns TRUE if it is OK to send more data.  It is not an error to
   send small amounts of data (e.g. a disconnect) when this returns
   FALSE, but sending lots of data when this returns FALSE will
   eventually crash the system.  To give a specific value, it is OK to send
   10000 bytes after this starts returning FALSE. */

Boolean ssh_cross_down_can_send(SshCrossDown down)
{
  Boolean status;

  status = ssh_buffer_len(&down->outgoing) <
    BUFFER_MAX_SIZE - ALLOW_AFTER_BUFFER_FULL;

  /* If no more can be sent, mark that sending is blocked.  This will
     trigger a callback when data can again be sent. */
  if (!status)
    down->send_blocked = TRUE;

  return status;
}
  

/* Encodes and sends a packet as specified for ssh_encode_cross_packet. */

void ssh_cross_down_send_encode_va(SshCrossDown down,
                                   SshCrossPacketType type,
                                   va_list va)
{
  /* Wrap the data into a cross-layer packet and append to the outgoing
     stream. */
  ssh_cross_encode_packet_va(&down->outgoing, type, va);

  /* Reset the callback to ensure that our callback gets called. */
  ssh_stream_set_callback(down->stream, ssh_cross_down_callback, (void *)down);
}

/* Encodes and sends a packet as specified for ssh_encode_cross_packet. */

void ssh_cross_down_send_encode(SshCrossDown down,
                                SshCrossPacketType type,
                                ...)
{
  va_list va;

  va_start(va, type);
  ssh_cross_down_send_encode_va(down, type, va);
  va_end(va);
}

/* Sends the given packet down.  The packet may actually get buffered. */

void ssh_cross_down_send(SshCrossDown down, SshCrossPacketType type,
                         const unsigned char *data, size_t len)
{
  ssh_cross_down_send_encode(down, type,
                             SSH_FORMAT_DATA, data, len,
                             SSH_FORMAT_END);
}

/* Sends a disconnect message down.  However, this does not automatically
   destroy the object.  It is legal to destroy the object immediately
   after calling this; that will properly drain the buffers.  The message
   should not contain a newline.  Note that the format argument list is
   a va_list. */

void ssh_cross_down_send_disconnect_va(SshCrossDown down,
                                       Boolean locally_generated,
                                       unsigned int reason_code,
                                       const char *reason_format, va_list va)
{
  char buf[256];
  const char lang[] = "en";

  /* Format the reason string. */
  vsnprintf(buf, sizeof(buf), reason_format, va);

  /* Wrap the data into a cross layer packet and append to the outgoing
     stream. */
  ssh_cross_down_send_encode(down, SSH_CROSS_DISCONNECT,
                             SSH_FORMAT_BOOLEAN, locally_generated,
                             SSH_FORMAT_UINT32, (SshUInt32) reason_code,
                             SSH_FORMAT_UINT32_STR, buf, strlen(buf),
                             SSH_FORMAT_UINT32_STR, lang, strlen(lang),
                             SSH_FORMAT_END);
}

/* Sends a disconnect message down.  However, this does not automatically
   destroy the object.  It is legal to destroy the object immediately
   after calling this; that will properly drain the buffers.  The message
   should not contain a newline. */

void ssh_cross_down_send_disconnect(SshCrossDown down,
                                    Boolean locally_generated,
                                    unsigned int reason_code,
                                    const char *reason_format, ...)
{
  va_list va;

  /* Format the reason string. */
  va_start(va, reason_format);
  ssh_cross_down_send_disconnect_va(down, locally_generated, reason_code,
                                    reason_format, va);
}

/* Sends a debug message down.  The format is as in printf.  The message
   should not contain a newline.  Note that the argument list is a va_list. */

void ssh_cross_down_send_debug_va(SshCrossDown down, Boolean always_display,
                                  const char *format, va_list va)
{
  char buf[256];
  const char lang[] = "en";

  /* Format the message. */
  vsnprintf(buf, sizeof(buf), format, va);

  /* Wrap the data into a cross layer packet and append to the outgoing
     stream. */
  ssh_cross_down_send_encode(down, SSH_CROSS_DEBUG,
                             SSH_FORMAT_BOOLEAN, always_display,
                             SSH_FORMAT_UINT32_STR, buf, strlen(buf),
                             SSH_FORMAT_UINT32_STR, lang, strlen(lang),
                             SSH_FORMAT_END);
}

/* Sends a debug message down.  The format is as in printf.  The message
   should not contain a newline. */

void ssh_cross_down_send_debug(SshCrossDown down, Boolean always_display,
                               const char *format, ...)
{
  va_list va;

  /* Format the message. */
  va_start(va, format);
  ssh_cross_down_send_debug_va(down, always_display, format, va);
  va_end(va);
}

/* Causes any I/O requests from up to be directly routed to the lower
   level stream, without processing any more data on this level.  This
   will automatically allow sends/receives in each direction as
   appropriate.  Destroy is not shortcircuited, and the destroy
   callback should destroy the downward stream.  This can only be
   called from a SshCrossDown packet callback. */

void ssh_cross_shortcircuit(SshStream up_stream,
                            SshCrossDown down)
{
  /* Mark that the stream is shortcircuited. */
  down->shortcircuited = FALSE;
  down->shortcircuit_up_stream = up_stream;

#if 0 /* the packet is still in down->incoming when we call the callback */
  /* Sanity check: there must not be data in incoming buffer. */
  if (ssh_buffer_len(&down->incoming) != 0)
    ssh_fatal("ssh_cross_shortcircuit: incoming data in buffer; not set in packet callback");
#endif /* 0 */

  /* If there is no data to drain, shortcircuit output now. */
  if (ssh_buffer_len(&down->outgoing) == 0)
    {
      down->shortcircuited = TRUE;
      ssh_cross_up_shortcircuit_now(down->shortcircuit_up_stream,
                                    down->stream);
    }
}
