/* libebb web server library
 * Copyright 2008 ryah dahl, ry at tiny clouds punkt org
 *
 * This software may be distributed under the "MIT" license included in the
 * README
 */
#ifndef server_h
#define server_h

#include "ebb_request_parser.h"
#include <ev.h>
#include <gnutls/gnutls.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define EBB_MAX_CONNECTIONS 1024

#define EBB_AGAIN 0
#define EBB_STOP 1

typedef struct ebb_buf        ebb_buf;
typedef struct ebb_server     ebb_server;
typedef struct ebb_connection ebb_connection;

struct ebb_buf {
  size_t written; /* private */

  /* public */
  char *base;
  size_t len;
  void (*free)(ebb_buf*);
  void *data;
};

struct ebb_server {
  int fd;                                       /* ro */
  struct sockaddr_in sockaddr;                  /* ro */
  socklen_t socklen;                            /* ro */
  char port[6];                                 /* ro */
  struct ev_loop *loop;                         /* ro */
  unsigned listening:1;                         /* ro */
  unsigned secure:1;                            /* ro */
  gnutls_certificate_credentials_t credentials; /* private */
  ev_io connection_watcher;                     /* private */

  /* Public */

  /* Allocates and initializes an ebb_connection.  NULL by default. */
  ebb_connection* (*new_connection) (ebb_server*, struct sockaddr_in*);

  void *data;
};

void ebb_server_init( ebb_server *server, struct ev_loop *loop);
void ebb_secure_server_init(ebb_server *server, struct ev_loop *loop, 
                            const char *cert_file, const char *key_file);
int ebb_server_listen_on_port(ebb_server *server, const int port);
int ebb_server_listen_on_fd(ebb_server *server, const int sfd);
void ebb_server_unlisten(ebb_server *server);

struct ebb_connection {
  int fd;                      /* ro */
  struct sockaddr_in sockaddr; /* ro */
  socklen_t socklen;           /* ro */ 
  ebb_server *server;          /* ro */
  float timeout;               /* ro */
  char *ip;                    /* ro */
  unsigned open:1;             /* ro */
  ebb_buf *to_write;           /* ro */
  ev_io write_watcher;         /* private */
  ev_io read_watcher;          /* private */
  ev_io handshake_watcher;     /* private */
  ev_timer timeout_watcher;    /* private */
  ebb_request_parser parser;   /* private */

  gnutls_session_t session;    /* private */
  /* Public */

  ebb_request* (*new_request) (ebb_connection*); 

  /* The new_buf callback allocates and initializes an ebb_buf structure.
   * By default this is set to a simple malloc() based callback which always
   * returns 4 kilobyte bufs.  Write over it with your own to use your own
   * custom allocation
   *
   * new_buf is called each time there is data from a client connection to
   * be read. See on_readable() in server.c to see exactly how this is used.
   */
  ebb_buf* (*new_buf) (ebb_connection*); 

  /* Returns EBB_STOP or EBB_AGAIN 
   * NULL by default.
   */
  int (*on_timeout) (ebb_connection*); 

  /* The connection was closed */
  void (*on_close) (ebb_connection*); 

  /* Called when libebb will no longer use this structure. 
   * NULL by default.
   */
  void (*free) (ebb_connection*); 

  void *data;
};

typedef void (*ebb_connection_cb)(ebb_connection *connection, void *data);

void ebb_connection_init(ebb_connection *connection, float timeout);
void ebb_connection_close(ebb_connection *);
void ebb_connection_reset_timeout(ebb_connection *connection);
int ebb_connection_write(ebb_connection *connection, ebb_buf *buf);

#define ebb_request_connection(request) ((ebb_connection*)(request->parser->data))

#endif