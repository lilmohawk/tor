/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

extern or_options_t options; /* command-line and config-file options */

static int connection_ap_handshake_process_socks(connection_t *conn);
static int connection_ap_handshake_send_begin(connection_t *ap_conn, circuit_t *circ,
                                              char *destaddr, uint16_t destport);
static int connection_ap_handshake_socks_reply(connection_t *conn, char result);

static int connection_exit_begin_conn(cell_t *cell, circuit_t *circ);

#define SOCKS4_REQUEST_GRANTED          90
#define SOCKS4_REQUEST_REJECT           91
#define SOCKS4_REQUEST_IDENT_FAILED     92
#define SOCKS4_REQUEST_IDENT_CONFLICT   93

int connection_edge_process_inbuf(connection_t *conn) {

  assert(conn);
  assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  if(conn->inbuf_reached_eof) {
#ifdef HALF_OPEN
    /* eof reached; we're done reading, but we might want to write more. */ 
    conn->done_receiving = 1;
    shutdown(conn->s, 0); /* XXX check return, refactor NM */
    if (conn->done_sending)
      conn->marked_for_close = 1;

    /* XXX Factor out common logic here and in circuit_about_to_close NM */
    circ = circuit_get_by_conn(conn);
    if (!circ)
      return -1;

    memset(&cell, 0, sizeof(cell_t));
    cell.command = CELL_RELAY;
    cell.length = RELAY_HEADER_SIZE;
    SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_END);
    SET_CELL_STREAM_ID(cell, conn->stream_id);
    cell.aci = circ->n_aci;

    if (circuit_deliver_relay_cell(&cell, circ, CELL_DIRECTION(conn->type), conn->cpath_layer) < 0) {
      log_fn(LOG_WARNING,"(fd %d) circuit_deliver_relay_cell failed. Closing.", conn->s);
      circuit_close(circ);
    }
    return 0;
#else 
    /* eof reached, kill it. */
    log_fn(LOG_INFO,"conn (fd %d) reached eof. Closing.", conn->s);
    return -1;
#endif
  }

  switch(conn->state) {
    case AP_CONN_STATE_SOCKS_WAIT:
      return connection_ap_handshake_process_socks(conn);
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      if(connection_package_raw_inbuf(conn) < 0)
        return -1;
      return 0;
    case EXIT_CONN_STATE_CONNECTING:
      log_fn(LOG_INFO,"text from server while in 'connecting' state at exit. Leaving it on buffer.");
      return 0;
  }

  return 0;
}

int connection_edge_send_command(connection_t *fromconn, circuit_t *circ, int relay_command) {
  cell_t cell;
  int cell_direction;

  if(!circ) {
    log_fn(LOG_WARNING,"no circ. Closing.");
    return -1;
  }

  memset(&cell, 0, sizeof(cell_t));
  if(fromconn && fromconn->type == CONN_TYPE_AP) {
    cell.aci = circ->n_aci;
    cell_direction = CELL_DIRECTION_OUT;
  } else {
    /* NOTE: if !fromconn, we assume that it's heading towards the OP */
    cell.aci = circ->p_aci;
    cell_direction = CELL_DIRECTION_IN;
  }

  cell.command = CELL_RELAY;
  SET_CELL_RELAY_COMMAND(cell, relay_command);
  if(fromconn)
    SET_CELL_STREAM_ID(cell, fromconn->stream_id);
  else
    SET_CELL_STREAM_ID(cell, ZERO_STREAM);

  cell.length = RELAY_HEADER_SIZE;
  log_fn(LOG_INFO,"delivering %d cell %s.", relay_command, cell_direction == CELL_DIRECTION_OUT ? "forward" : "backward");

  if(circuit_deliver_relay_cell(&cell, circ, cell_direction, fromconn ? fromconn->cpath_layer : NULL) < 0) {
    log_fn(LOG_WARNING,"circuit_deliver_relay_cell failed. Closing.");
    circuit_close(circ);
    return 0;
  }
  return 0;
}

/* an incoming relay cell has arrived. return -1 if you want to tear down the
 * circuit, else 0. */
int connection_edge_process_relay_cell(cell_t *cell, circuit_t *circ, connection_t *conn,
                                       int edge_type, crypt_path_t *layer_hint) {
  int relay_command;
  static int num_seen=0;

  assert(cell && circ);

  relay_command = CELL_RELAY_COMMAND(*cell);
//  log_fn(LOG_DEBUG,"command %d stream %d", relay_command, stream_id);
  num_seen++;
  log_fn(LOG_DEBUG,"Now seen %d relay cells here.", num_seen);

  /* either conn is NULL, in which case we've got a control cell, or else
   * conn points to the recognized stream. */

  if(conn && conn->state != AP_CONN_STATE_OPEN && conn->state != EXIT_CONN_STATE_OPEN) {
    if(conn->type == CONN_TYPE_EXIT && relay_command == RELAY_COMMAND_END) {
      log_fn(LOG_INFO,"Exit got end before we're connected. Marking for close.");
      conn->marked_for_close = 1;
      if(conn->state == EXIT_CONN_STATE_RESOLVING) {
        log_fn(LOG_INFO,"...and informing resolver we don't want the answer anymore.");
        dns_cancel_pending_resolve(conn->address, conn);
      }
    } else {
      log_fn(LOG_WARNING,"Got an unexpected relay cell, not in 'open' state. Dropping.");
    }
    return 0;
  }

  switch(relay_command) {
    case RELAY_COMMAND_BEGIN:
      if(edge_type == EDGE_AP) {
        log_fn(LOG_WARNING,"relay begin request unsupported at AP. Dropping.");
        return 0;
      }
      if(conn) {
        log_fn(LOG_WARNING,"begin cell for known stream. Dropping.");
        return 0;
      }
      return connection_exit_begin_conn(cell, circ);
    case RELAY_COMMAND_DATA:
      if((edge_type == EDGE_AP && --layer_hint->deliver_window < 0) ||
         (edge_type == EDGE_EXIT && --circ->deliver_window < 0)) {
        log_fn(LOG_WARNING,"(relay data) circ deliver_window below 0. Killing.");
        return -1;
      }
      log_fn(LOG_DEBUG,"circ deliver_window now %d.", edge_type == EDGE_AP ? layer_hint->deliver_window : circ->deliver_window);

      if(circuit_consider_sending_sendme(circ, edge_type, layer_hint) < 0)
        return -1;

      if(!conn) {
        log_fn(LOG_INFO,"relay cell dropped, unknown stream %d.",*(int*)conn->stream_id);
        return 0;
      }

      if(--conn->deliver_window < 0) { /* is it below 0 after decrement? */
        log_fn(LOG_WARNING,"(relay data) conn deliver_window below 0. Killing.");
        return -1; /* somebody's breaking protocol. kill the whole circuit. */
      }

//      printf("New text for buf (%d bytes): '%s'", cell->length - RELAY_HEADER_SIZE, cell->payload + RELAY_HEADER_SIZE);
      if(connection_write_to_buf(cell->payload + RELAY_HEADER_SIZE,
                                 cell->length - RELAY_HEADER_SIZE, conn) < 0) {
        conn->marked_for_close = 1;
        return 0;
      }
      if(connection_consider_sending_sendme(conn, edge_type) < 0)
        conn->marked_for_close = 1;
      return 0;
    case RELAY_COMMAND_END:
      if(!conn) {
        log_fn(LOG_INFO,"end cell dropped, unknown stream %d.",*(int*)conn->stream_id);
        return 0;
      }
      log_fn(LOG_INFO,"end cell for stream %d. Removing stream.",*(int*)conn->stream_id);

#ifdef HALF_OPEN
      conn->done_sending = 1;
      shutdown(conn->s, 1); /* XXX check return; refactor NM */
      if (conn->done_receiving)
        conn->marked_for_close = 1;
#endif
      conn->marked_for_close = 1;
      break;
    case RELAY_COMMAND_EXTEND:
      if(conn) {
        log_fn(LOG_WARNING,"'extend' for non-zero stream. Dropping.");
        return 0;
      }
      return circuit_extend(cell, circ);
    case RELAY_COMMAND_EXTENDED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_WARNING,"'extended' unsupported at exit. Dropping.");
        return 0;
      }
      log_fn(LOG_DEBUG,"Got an extended cell! Yay.");
      if(circuit_finish_handshake(circ, cell->payload+RELAY_HEADER_SIZE) < 0) {
        log_fn(LOG_WARNING,"circuit_finish_handshake failed.");
        return -1;
      }
      return circuit_send_next_onion_skin(circ);
    case RELAY_COMMAND_TRUNCATE:
      if(edge_type == EDGE_AP) {
        log_fn(LOG_WARNING,"'truncate' unsupported at AP. Dropping.");
        return 0;
      }
      if(circ->n_conn) {
        connection_send_destroy(circ->n_aci, circ->n_conn);
        circ->n_conn = NULL;
      }
      log_fn(LOG_DEBUG, "Processed 'truncate', replying.");
      return connection_edge_send_command(NULL, circ, RELAY_COMMAND_TRUNCATED);
    case RELAY_COMMAND_TRUNCATED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_WARNING,"'truncated' unsupported at exit. Dropping.");
        return 0;
      }
      return circuit_truncated(circ, layer_hint);
    case RELAY_COMMAND_CONNECTED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_WARNING,"'connected' unsupported at exit. Dropping.");
        return 0;
      }
      if(!conn) {
        log_fn(LOG_INFO,"connected cell dropped, unknown stream %d.",*(int*)conn->stream_id);
        break;
      }
      log_fn(LOG_INFO,"Connected! Notifying application.");
      if(connection_ap_handshake_socks_reply(conn, SOCKS4_REQUEST_GRANTED) < 0) {
        conn->marked_for_close = 1;
      }
      break;
    case RELAY_COMMAND_SENDME:
      if(!conn) {
        if(edge_type == EDGE_AP) {
          assert(layer_hint);
          layer_hint->package_window += CIRCWINDOW_INCREMENT;
          log_fn(LOG_DEBUG,"circ-level sendme at AP, packagewindow %d.", layer_hint->package_window);
          circuit_resume_edge_reading(circ, EDGE_AP, layer_hint);
        } else {
          assert(!layer_hint);
          circ->package_window += CIRCWINDOW_INCREMENT;
          log_fn(LOG_DEBUG,"circ-level sendme at exit, packagewindow %d.", circ->package_window);
          circuit_resume_edge_reading(circ, EDGE_EXIT, layer_hint);
        }
        return 0;
      }
      conn->package_window += STREAMWINDOW_INCREMENT;
      log_fn(LOG_DEBUG,"stream-level sendme, packagewindow now %d.", conn->package_window);
      connection_start_reading(conn);
      connection_package_raw_inbuf(conn); /* handle whatever might still be on the inbuf */
      break;
    default:
      log_fn(LOG_WARNING,"unknown relay command %d.",relay_command);
      return -1;
  }
  return 0;
}

int connection_edge_finished_flushing(connection_t *conn) {
  int e, len=sizeof(e);

  assert(conn);
  assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  switch(conn->state) {
    case EXIT_CONN_STATE_CONNECTING:
      if (getsockopt(conn->s, SOL_SOCKET, SO_ERROR, (void*)&e, &len) < 0)  { /* not yet */
        if(!ERRNO_CONN_EINPROGRESS(errno)) {
          /* yuck. kill it. */
          log_fn(LOG_DEBUG,"in-progress exit connect failed. Removing.");
          return -1;
        } else {
          log_fn(LOG_DEBUG,"in-progress exit connect still waiting.");
          return 0; /* no change, see if next time is better */
        }
      }
      /* the connect has finished. */

      log_fn(LOG_INFO,"Exit connection to %s:%u established.",
          conn->address,conn->port);

      conn->state = EXIT_CONN_STATE_OPEN;
      connection_watch_events(conn, POLLIN); /* stop writing, continue reading */
      if(connection_wants_to_flush(conn)) /* in case there are any queued relay cells */
        connection_start_writing(conn);
      return
        connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_CONNECTED) || /* deliver a 'connected' relay cell back through the circuit. */
        connection_process_inbuf(conn); /* in case the server has written anything */
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      connection_stop_writing(conn);
      return connection_consider_sending_sendme(conn, conn->type);
    default:
      log_fn(LOG_WARNING,"BUG: called in unexpected state.");
      return -1;
  }
  return 0;
}

int connection_package_raw_inbuf(connection_t *conn) {
  int amount_to_process;
  cell_t cell;
  circuit_t *circ;
 
  assert(conn);
  assert(!connection_speaks_cells(conn));
 
repeat_connection_package_raw_inbuf:
 
  circ = circuit_get_by_conn(conn);
  if(!circ) {
    log_fn(LOG_INFO,"conn has no circuits! Closing.");
    return -1;
  }
 
  if(circuit_consider_stop_edge_reading(circ, conn->type, conn->cpath_layer))
    return 0;
 
  if(conn->package_window <= 0) {
    log_fn(LOG_WARNING,"called with package_window 0. Tell Roger.");
    connection_stop_reading(conn);
    return 0;
  }
 
  amount_to_process = buf_datalen(conn->inbuf);
 
  if(!amount_to_process)
    return 0;
 
  /* Initialize the cell with 0's */
  memset(&cell, 0, sizeof(cell_t));
 
  if(amount_to_process > CELL_PAYLOAD_SIZE - RELAY_HEADER_SIZE) {
    cell.length = CELL_PAYLOAD_SIZE - RELAY_HEADER_SIZE;
  } else {
    cell.length = amount_to_process;
  }
 
  connection_fetch_from_buf(cell.payload+RELAY_HEADER_SIZE, cell.length, conn);
 
  log_fn(LOG_DEBUG,"(%d) Packaging %d bytes (%d waiting).",conn->s,cell.length,
         (int)buf_datalen(conn->inbuf));
 
  cell.command = CELL_RELAY;
  SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_DATA);
  SET_CELL_STREAM_ID(cell, conn->stream_id);
  cell.length += RELAY_HEADER_SIZE;
 
  if(conn->type == CONN_TYPE_EXIT) {
    cell.aci = circ->p_aci;
    if(circuit_deliver_relay_cell(&cell, circ, CELL_DIRECTION_IN, NULL) < 0) {
      log_fn(LOG_WARNING,"circuit_deliver_relay_cell (backward) failed. Closing.");
      circuit_close(circ);
      return 0;
    }
    assert(circ->package_window > 0);
    circ->package_window--;
  } else { /* send it forward. we're an AP */
    assert(conn->type == CONN_TYPE_AP);
    cell.aci = circ->n_aci;
    if(circuit_deliver_relay_cell(&cell, circ, CELL_DIRECTION_OUT, conn->cpath_layer) < 0) {
      log_fn(LOG_WARNING,"circuit_deliver_relay_cell (forward) failed. Closing.");
      circuit_close(circ);
      return 0;
    }
    assert(conn->cpath_layer->package_window > 0);
    conn->cpath_layer->package_window--;
  }
 
  assert(conn->package_window > 0);
  if(--conn->package_window <= 0) { /* is it 0 after decrement? */
    connection_stop_reading(conn);
    log_fn(LOG_DEBUG,"conn->package_window reached 0.");
    circuit_consider_stop_edge_reading(circ, conn->type, conn->cpath_layer);
    return 0; /* don't process the inbuf any more */
  }
  log_fn(LOG_DEBUG,"conn->package_window is now %d",conn->package_window);
 
  /* handle more if there's more, or return 0 if there isn't */
  goto repeat_connection_package_raw_inbuf;
}

int connection_consider_sending_sendme(connection_t *conn, int edge_type) {
  circuit_t *circ;
  cell_t cell;
 
  if(connection_outbuf_too_full(conn))
    return 0;
 
  circ = circuit_get_by_conn(conn);
  if(!circ) {
    /* this can legitimately happen if the destroy has already arrived and torn down the circuit */
    log_fn(LOG_INFO,"No circuit associated with conn. Skipping.");
    return 0;
  }
 
  memset(&cell, 0, sizeof(cell_t));
  cell.command = CELL_RELAY;
  SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_SENDME);
  SET_CELL_STREAM_ID(cell, conn->stream_id);
  cell.length += RELAY_HEADER_SIZE;
 
  if(edge_type == EDGE_EXIT)
    cell.aci = circ->p_aci;
  else
    cell.aci = circ->n_aci;
 
  while(conn->deliver_window < STREAMWINDOW_START - STREAMWINDOW_INCREMENT) {
    log_fn(LOG_DEBUG,"Outbuf %d, Queueing stream sendme.", conn->outbuf_flushlen);
    conn->deliver_window += STREAMWINDOW_INCREMENT;
    if(circuit_deliver_relay_cell(&cell, circ, CELL_DIRECTION(edge_type), conn->cpath_layer) < 0) {
      log_fn(LOG_WARNING,"circuit_deliver_relay_cell failed. Closing.");
      circuit_close(circ);
      return 0;
    }
  }
 
  return 0;
}

static int connection_ap_handshake_process_socks(connection_t *conn) {
  circuit_t *circ;
  char destaddr[200]; /* XXX why 200? but not 256, because it won't fit in a cell */
  uint16_t destport;

  assert(conn);

  log_fn(LOG_DEBUG,"entered.");

  switch(fetch_from_buf_socks(conn->inbuf,
                              destaddr, sizeof(destaddr), &destport)) {
    case -1:
      log_fn(LOG_WARNING,"Fetching socks handshake failed. Closing.");
      connection_ap_handshake_socks_reply(conn, SOCKS4_REQUEST_REJECT);
      return -1;
    case 0:
      log_fn(LOG_DEBUG,"Fetching socks handshake, not all here yet. Ignoring.");
      return 0;
    /* case 1, fall through */
  }

  /* find the circuit that we should use, if there is one. */
  circ = circuit_get_newest_open();

  if(!circ) {
    log_fn(LOG_INFO,"No circuit ready. Closing.");
    return -1;
  }

  circ->dirty = 1;

  /* add it into the linked list of streams on this circuit */
  log_fn(LOG_DEBUG,"attaching new conn to circ. n_aci %d.", circ->n_aci);
  conn->next_stream = circ->p_streams;
  circ->p_streams = conn;

  assert(circ->cpath && circ->cpath->prev);
  assert(circ->cpath->prev->state == CPATH_STATE_OPEN);
  conn->cpath_layer = circ->cpath->prev;

  if(connection_ap_handshake_send_begin(conn, circ, destaddr, destport) < 0) {
    circuit_close(circ);
    return -1;
  }

  return 0;
}

/* deliver the destaddr:destport in a relay cell */
static int connection_ap_handshake_send_begin(connection_t *ap_conn, circuit_t *circ,
                                              char *destaddr, uint16_t destport) {
  cell_t cell;
  memset(&cell, 0, sizeof(cell_t));

  cell.command = CELL_RELAY;
  cell.aci = circ->n_aci;
  SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_BEGIN);
  if(crypto_pseudo_rand(STREAM_ID_SIZE, ap_conn->stream_id) < 0)
    return -1;
  /* FIXME check for collisions */
  SET_CELL_STREAM_ID(cell, ZERO_STREAM);

  memcpy(cell.payload+RELAY_HEADER_SIZE, ap_conn->stream_id, STREAM_ID_SIZE);
  cell.length = 
    snprintf(cell.payload+RELAY_HEADER_SIZE+STREAM_ID_SIZE, CELL_PAYLOAD_SIZE-RELAY_HEADER_SIZE-STREAM_ID_SIZE,
             "%s:%d", destaddr, destport) + 
    1 + STREAM_ID_SIZE + RELAY_HEADER_SIZE;
  log_fn(LOG_DEBUG,"Sending relay cell (id %d) to begin stream %d.", *(int *)(cell.payload+1),*(int *)ap_conn->stream_id);
  if(circuit_deliver_relay_cell(&cell, circ, CELL_DIRECTION_OUT, ap_conn->cpath_layer) < 0) {
    log_fn(LOG_WARNING,"failed to deliver begin cell. Closing.");
    return -1;
  }
  ap_conn->package_window = STREAMWINDOW_START;
  ap_conn->deliver_window = STREAMWINDOW_START;
  ap_conn->state = AP_CONN_STATE_OPEN;
  log_fn(LOG_INFO,"Address/port sent, ap socket %d, n_aci %d",ap_conn->s,circ->n_aci);
  return 0;
}

static int connection_ap_handshake_socks_reply(connection_t *conn, char result) {
  char buf[SOCKS4_NETWORK_LEN];

  assert(conn);

  /* an inlined socks4_pack() */
  memset(buf,0,sizeof(buf));
  buf[1] = result; /* command */
  /* leave version, destport, destip zero */

  if(connection_write_to_buf(buf, sizeof(buf), conn) < 0)
    return -1;
  return connection_flush_buf(conn); /* try to flush it, in case we're about to close the conn */
}

static int connection_exit_begin_conn(cell_t *cell, circuit_t *circ) {
  connection_t *n_stream;
  char *colon;

  if(!memchr(cell->payload+RELAY_HEADER_SIZE+STREAM_ID_SIZE,0,cell->length-RELAY_HEADER_SIZE-STREAM_ID_SIZE)) {
    log_fn(LOG_WARNING,"relay begin cell has no \\0. Dropping.");
    return 0;
  }
  colon = strchr(cell->payload+RELAY_HEADER_SIZE+STREAM_ID_SIZE, ':');
  if(!colon) {
    log_fn(LOG_WARNING,"relay begin cell has no colon. Dropping.");
    return 0;
  }
  *colon = 0;

  if(!atoi(colon+1)) { /* bad port */
    log_fn(LOG_WARNING,"relay begin cell has invalid port. Dropping.");
    return 0;
  }

  log_fn(LOG_DEBUG,"Creating new exit connection.");
  n_stream = connection_new(CONN_TYPE_EXIT);

  memcpy(n_stream->stream_id, cell->payload + RELAY_HEADER_SIZE, STREAM_ID_SIZE);
  n_stream->address = strdup(cell->payload + RELAY_HEADER_SIZE + STREAM_ID_SIZE);
  n_stream->port = atoi(colon+1);
  n_stream->state = EXIT_CONN_STATE_RESOLVING;
  n_stream->receiver_bucket = -1; /* edge connections don't do receiver buckets */
  n_stream->bandwidth = -1;
  n_stream->s = -1; /* not yet valid */
  n_stream->package_window = STREAMWINDOW_START;
  n_stream->deliver_window = STREAMWINDOW_START;
  if(connection_add(n_stream) < 0) { /* no space, forget it */
    log_fn(LOG_WARNING,"connection_add failed. Dropping.");
    connection_free(n_stream);
    return 0;
  }

  /* add it into the linked list of streams on this circuit */
  n_stream->next_stream = circ->n_streams;
  circ->n_streams = n_stream;

  /* send it off to the gethostbyname farm */
  switch(dns_resolve(n_stream)) {
    case 1: /* resolve worked */
      if(connection_exit_connect(n_stream) >= 0)
        return 0;
      /* else fall through */
    case -1: /* resolve failed */
      log_fn(LOG_WARNING,"Couldn't queue resolve request.");
      connection_remove(n_stream);
      connection_free(n_stream);
    case 0: /* resolve added to pending list */
      ;
  }
  return 0;
}

int connection_exit_connect(connection_t *conn) {

  if(router_compare_to_exit_policy(conn) < 0) {
    log_fn(LOG_INFO,"%s:%d failed exit policy. Closing.", conn->address, conn->port);
    return -1;
  }

  switch(connection_connect(conn, conn->address, conn->addr, conn->port)) {
    case -1:
      return -1;
    case 0:
      connection_set_poll_socket(conn);
      conn->state = EXIT_CONN_STATE_CONNECTING;

      connection_watch_events(conn, POLLOUT | POLLIN | POLLERR);
      /* writable indicates finish, readable indicates broken link,
         error indicates broken link in windowsland. */
      return 0;
    /* case 1: fall through */
  }

  connection_set_poll_socket(conn);
  conn->state = EXIT_CONN_STATE_OPEN;
  if(connection_wants_to_flush(conn)) { /* in case there are any queued data cells */
    log_fn(LOG_WARNING,"tell roger: newly connected conn had data waiting!");
//    connection_start_writing(conn);
  }
//   connection_process_inbuf(conn);
  connection_watch_events(conn, POLLIN);

  /* also, deliver a 'connected' cell back through the circuit. */
  return connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_CONNECTED);
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/

