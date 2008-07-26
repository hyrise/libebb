/* libebb web server library
 * Copyright 2008 ryah dahl, ry at tiny clouds punkt org
 *
 * This software may be distributed under the "MIT" license included in the
 * README
 */
#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <netinet/in.h>  /* inet_ntoa */
#include <arpa/inet.h>   /* inet_ntoa */
#include <unistd.h>
#include <error.h>
#include <stdio.h>      /* perror */
#include <errno.h>      /* perror */
#include <stdlib.h> /* for the default methods */

#include <ev.h>
#include <gnutls/gnutls.h>

#include "ebb.h"
#include "ebb_request_parser.h"

#define TRUE 1
#define FALSE 0

#define FREE_CONNECTION_IF_CLOSED \
  if(!connection->open && connection->free) connection->free(connection);

#define GNUTLS_NEED_WRITE (gnutls_record_get_direction(connection->session) == 1)
#define GNUTLS_NEED_READ (gnutls_record_get_direction(connection->session) == 0)
#define CONNECTION_HAS_SOMETHING_TO_WRITE (connection->to_write != NULL)

static void set_nonblock (int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  int r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  assert(0 <= r && "Setting socket non-block failed!");
}

static ssize_t push(void *data, const void *buf, size_t len)
{
  ebb_connection *connection = data;
  return send(connection->fd, buf, len, MSG_NOSIGNAL);
}

static ssize_t pull(void *data, void *buf, size_t len)
{
  ebb_connection *connection = data;
  return recv(connection->fd, buf, len, 0);
}

/* Internal callback 
 * called by connection->timeout_watcher
 */
static void on_timeout(struct ev_loop *loop, ev_timer *watcher, int revents)
{
  ebb_connection *connection = watcher->data;

  //printf("on_timeout\n");

  /* if on_timeout returns true, we don't time out */
  if( connection->on_timeout 
   && connection->on_timeout(connection) != EBB_AGAIN
    ) 
  {
    ebb_connection_reset_timeout(connection);
    return;
  }

  ebb_connection_close(connection);
  FREE_CONNECTION_IF_CLOSED 
}

static void on_handshake(struct ev_loop *loop ,ev_io *watcher, int revents)
{
  ebb_connection *connection = watcher->data;

  //printf("on_handshake\n");

  assert(ev_is_active(&connection->timeout_watcher));
  assert(!ev_is_active(&connection->read_watcher));
  assert(!ev_is_active(&connection->write_watcher));

  if(EV_ERROR & revents) {
    error(0, 0, "on_handshake() got error event, closing connection.\n");
    goto error;
  }

  int r = gnutls_handshake(connection->session);
  if(r < 0) {
    if(gnutls_error_is_fatal(r)) goto error;
    if(r == GNUTLS_E_INTERRUPTED || r == GNUTLS_E_AGAIN)
      ev_io_set( watcher
               , connection->fd
               , EV_ERROR | (GNUTLS_NEED_WRITE ? EV_WRITE : EV_READ)
               );
    return;
  }

  ebb_connection_reset_timeout(connection);
  ev_io_stop(loop, watcher);

  ev_io_start(loop, &connection->read_watcher);
  if(CONNECTION_HAS_SOMETHING_TO_WRITE)
    ev_io_start(loop, &connection->write_watcher);

  return;
error:
  ebb_connection_close(connection);
  FREE_CONNECTION_IF_CLOSED 
}

/* Internal callback 
 * called by connection->write_watcher
 * Use ebb_connection_write to send data to this
 */
static void on_writable(struct ev_loop *loop ,ev_io *watcher, int revents)
{
  ebb_connection *connection = watcher->data;
  ebb_buf *buf = connection->to_write;
  ssize_t sent;
  
  //printf("on_writable\n");

  assert(buf != NULL);
  assert(buf->written <= buf->len);
  assert(ev_is_active(&connection->timeout_watcher));
  assert(!ev_is_active(&connection->handshake_watcher));

  if(connection->server->secure) {
    sent = gnutls_record_send( connection->session
                             , buf->base + buf->written
                             , buf->len - buf->written
                             ); 
    if(sent <= 0) {
      if(gnutls_error_is_fatal(sent)) goto error;
      if( (sent == GNUTLS_E_INTERRUPTED || sent == GNUTLS_E_AGAIN)
       && GNUTLS_NEED_READ
        ) ev_io_stop(loop, watcher);
      return; 
    }
  } else {
    sent = push(connection, buf->base + buf->written, buf->len - buf->written);
    if(sent < 0) goto error;
    if(sent == 0) return;
  }

  ebb_connection_reset_timeout(connection);

  buf->written += sent;

  if(buf->written == buf->len) {
    ev_io_stop(loop, watcher);
    connection->to_write = NULL;
    if(buf->free)
      buf->free(buf);
  }
  return;
error:
  error(0, 0, "close connection on write.\n");
  ebb_connection_close(connection);
  FREE_CONNECTION_IF_CLOSED 
}


/* Internal callback 
 * called by connection->read_watcher
 */
static void on_readable(struct ev_loop *loop, ev_io *watcher, int revents)
{
  ebb_connection *connection = watcher->data;
  ssize_t recved;

  //printf("on_readable\n");

  assert(ev_is_active(&connection->timeout_watcher));
  assert(!ev_is_active(&connection->handshake_watcher));

  if(EV_ERROR & revents) {
    error(0, 0, "on_readable() got error event, closing connection.\n");
    goto error;
  }

  ebb_buf *buf = NULL;
  if(connection->new_buf)
    buf = connection->new_buf(connection);
  if(buf == NULL) goto error; 

  if(connection->server->secure) {
    recved = gnutls_record_recv( connection->session
                               , buf->base
                               , buf->len
                               );
    if(recved <= 0) {
      if(gnutls_error_is_fatal(recved)) goto error;
      if( (recved == GNUTLS_E_INTERRUPTED || recved == GNUTLS_E_AGAIN)
       && GNUTLS_NEED_WRITE
        ) ev_io_start(loop, &connection->write_watcher);
      return; 
    } 
  } else {
    recved = push(connection, buf->base, buf->len);
    if(recved < 0) goto error;
    if(recved == 0) return;
  }

  ebb_connection_reset_timeout(connection);

  ebb_request_parser_execute(&connection->parser, buf->base, recved);
  /* parse error? just drop the client. screw the 400 response */
  if(ebb_request_parser_has_error(&connection->parser)) goto error;

  if(buf->free)
    buf->free(buf);

  FREE_CONNECTION_IF_CLOSED 
  return;

error:
  ebb_connection_close(connection);
  FREE_CONNECTION_IF_CLOSED 
}


/* Internal callback 
 * Called by server->connection_watcher.
 */
static void on_connection(struct ev_loop *loop, ev_io *watcher, int revents)
{
  ebb_server *server = watcher->data;

  //printf("on connection!\n");

  assert(server->listening);
  assert(server->loop == loop);
  assert(&server->connection_watcher == watcher);
  
  if(EV_ERROR & revents) {
    error(0, 0, "on_connection() got error event, closing server.\n");
    ebb_server_unlisten(server);
    return;
  }

  
  struct sockaddr_in addr; // connector's address information
  socklen_t addr_len = sizeof(addr); 
  int fd = accept( server->fd
                 , (struct sockaddr*) & addr
                 , & addr_len
                 );
  if(fd < 0) {
    perror("accept()");
    return;
  }

  ebb_connection *connection = NULL;
  if(server->new_connection)
    connection = server->new_connection(server, &addr);
  if(connection == NULL) {
    close(fd);
    return;
  } 
  
  set_nonblock(fd);
  connection->fd = fd;
  connection->open = TRUE;
  connection->server = server;
  memcpy(&connection->sockaddr, &addr, addr_len);
  if(server->port[0] != '\0')
    connection->ip = inet_ntoa(connection->sockaddr.sin_addr);  

  if(server->secure) {
    gnutls_init(&connection->session, GNUTLS_SERVER);
    gnutls_transport_set_lowat(connection->session, 0); 
    gnutls_set_default_priority(connection->session);
    gnutls_credentials_set(connection->session, GNUTLS_CRD_CERTIFICATE, connection->server->credentials);

    gnutls_transport_set_ptr(connection->session, (gnutls_transport_ptr) connection); 
    gnutls_transport_set_push_function(connection->session, push);
    gnutls_transport_set_pull_function(connection->session, pull);
  }

  ev_io_set(&connection->handshake_watcher, connection->fd, EV_READ | EV_WRITE | EV_ERROR);
  /* Note: not starting the write watcher until there is data to be written */
  ev_io_set(&connection->write_watcher, connection->fd, EV_WRITE);
  ev_io_set(&connection->read_watcher, connection->fd, EV_READ | EV_ERROR);
  /* XXX: seperate error watcher? */

  if(server->secure)
    ev_io_start(loop, &connection->handshake_watcher);
  else
    ev_io_start(loop, &connection->read_watcher);
  ev_timer_start(loop, &connection->timeout_watcher);
}

/**
 * Begin the server listening on a file descriptor.  This DOES NOT start the
 * event loop.  Start the event loop after making this call.
 */
int ebb_server_listen_on_fd(ebb_server *server, const int fd)
{
  assert(server->listening == FALSE);

  if (listen(fd, EBB_MAX_CONNECTIONS) < 0) {
    perror("listen()");
    return -1;
  }
  
  set_nonblock(fd); /* XXX superfluous? */
  
  server->fd = fd;
  server->listening = TRUE;
  
  ev_io_set (&server->connection_watcher, server->fd, EV_READ | EV_ERROR);
  ev_io_start (server->loop, &server->connection_watcher);
  
  return server->fd;
}


/**
 * Begin the server listening on a file descriptor This DOES NOT start the
 * event loop. Start the event loop after making this call.
 */
int ebb_server_listen_on_port(ebb_server *server, const int port)
{
  int fd = -1;
  struct linger ling = {0, 0};
  struct sockaddr_in addr;
  int flags = 1;
  
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket()");
    goto error;
  }
  
  flags = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
  setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

  /* TODO: Sending single byte chunks in a response body? Perhaps there is
   * a need to enable the Nagel algorithm dynamically. For now disabling.
   */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
  
  /* the memset call clears nonstandard fields in some impementations that
   * otherwise mess things up.
   */
  memset(&addr, 0, sizeof(addr));
  
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind()");
    goto error;
  }
  
  int ret = ebb_server_listen_on_fd(server, fd);
  if (ret >= 0) {
    sprintf(server->port, "%d", port);
  }
  return ret;
error:
  if(fd > 0) close(fd);
  return -1;
}

/**
 * Stops the server. Will not accept new connections.  Does not drop
 * existing connections.
 */
void ebb_server_unlisten(ebb_server *server)
{
  if(server->listening) {
    ev_io_stop(server->loop, &server->connection_watcher);
    close(server->fd);
    server->port[0] = '\0';
    server->listening = FALSE;
  }
}

/**
 * Initialize an ebb_server structure.  After calling ebb_server_init set
 * the callback server->new_connection and, optionally, callback data
 * server->data.  The new connection MUST be initialized with
 * ebb_connection_init before returning it to the server.
 *
 * @param server the server to initialize
 * @param loop a libev loop
 */
void ebb_server_init(ebb_server *server, struct ev_loop *loop)
{
  server->loop = loop;
  server->listening = FALSE;
  server->port[0] = '\0';
  server->fd = -1;
  server->connection_watcher.data = server;
  ev_init (&server->connection_watcher, on_connection);
  server->secure = FALSE;

  server->new_connection = NULL;
  server->data = NULL;
}

/* similar to server_init. 
 *
 * the user of secure server might want to set additional callbacks from
 * GNUTLS. In particular 
 * gnutls_global_set_mem_functions() 
 * gnutls_global_set_log_function()
 *
 * cert_file: the filename of a PEM certificate file
 *
 * key_file: the filename of a private key. Currently only PKCS-1 encoded
 * RSA and DSA private keys are accepted. 
 */
void ebb_secure_server_init(ebb_server *server, struct ev_loop *loop, 
                            const char *cert_file, const char *key_file)
{
  ebb_server_init(server, loop);
  server->secure = TRUE;
  gnutls_global_init();
  gnutls_certificate_allocate_credentials(&server->credentials);
  /* todo gnutls_certificate_free_credentials */
  int r = gnutls_certificate_set_x509_key_file( server->credentials
                                              , cert_file
                                              , key_file
                                              , GNUTLS_X509_FMT_PEM
                                              );
  assert(r >= 0 && "error loading certificates");
}

static ebb_buf* default_new_buf(ebb_connection *connection)
{
  static ebb_buf buf;
  static char base[TCP_MAXWIN];
  buf.base = base;
  buf.len = TCP_MAXWIN;
  buf.free = NULL;
  return &buf;
}

static ebb_request* new_request_wrapper(void *data)
{
  ebb_connection *connection = data;
  if(connection->new_request)
    return connection->new_request(connection);
  return NULL;
}

/**
 * Initialize an ebb_connection structure. After calling this function you
 * must setup callbacks for the different actions the server can take. See
 * server.h for which callbacks are availible. 
 * 
 * This should be called immediately after allocating space for a new
 * ebb_connection structure. Most likely, this will only be called within
 * the ebb_server->new_connection callback which you supply. 
 *
 * @param connection the connection to initialize
 * @param timeout    the timeout in seconds
 */
void ebb_connection_init(ebb_connection *connection, float timeout)
{
  connection->fd = -1;
  connection->server = NULL;
  connection->ip = NULL;
  connection->open = FALSE;
  connection->timeout = timeout;

  ebb_request_parser_init( &connection->parser );
  connection->parser.data = connection;
  connection->parser.new_request = new_request_wrapper;
  
  connection->write_watcher.data = connection;
  ev_init (&connection->write_watcher, on_writable);
  connection->to_write = NULL;

  connection->read_watcher.data = connection;
  ev_init(&connection->read_watcher, on_readable);

  connection->handshake_watcher.data = connection;
  ev_init(&connection->handshake_watcher, on_handshake);

  connection->timeout_watcher.data = connection;  
  ev_timer_init(&connection->timeout_watcher, on_timeout, timeout, timeout);

  connection->session = NULL;

  connection->new_buf = default_new_buf;
  connection->new_request = NULL;
  connection->on_timeout = NULL;
  connection->on_close = NULL;
  connection->free = NULL;
  connection->data = NULL;
}

void ebb_connection_close(ebb_connection *connection)
{
  if(connection->open) {
    ev_io_stop(connection->server->loop, &connection->read_watcher);
    ev_io_stop(connection->server->loop, &connection->write_watcher);
    ev_io_stop(connection->server->loop, &connection->handshake_watcher);
    ev_timer_stop(connection->server->loop, &connection->timeout_watcher);

    if(connection->session) {
      gnutls_deinit(connection->session);
    }
    int r =  close(connection->fd);
    assert(r == 0);
    
    connection->open = FALSE;


    if(connection->on_close)
      connection->on_close(connection);

  }
}

/* 
 * Resets the timeout to stay alive for another connection->timeout seconds
 */
void ebb_connection_reset_timeout(ebb_connection *connection)
{
  ev_timer_again( connection->server->loop
                , &connection->timeout_watcher
                );
}

/**
 * Writes a string to the socket. This is actually sets a watcher
 * which may take multiple iterations to write the entire string.
 *
 * The buf->free() callback will be made when the operation is complete.
 *
 * This can only be called once at a time. If you call it again
 * while the connection is writing another buffer the ebb_connection_write
 * will return FALSE and ignore the request.
 */
int ebb_connection_write(ebb_connection *connection, ebb_buf *buf)
{
  if(ev_is_active(&connection->write_watcher))
    return FALSE;
  assert(!CONNECTION_HAS_SOMETHING_TO_WRITE);
  assert(buf->len > 0);
  buf->written = 0;
  connection->to_write = buf;
  ev_io_start(connection->server->loop, &connection->write_watcher);
  return TRUE;
}