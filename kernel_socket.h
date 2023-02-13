#ifndef __KERNEL_SOCKET_H
#define __KERNEL_SOCKET_H

#include "tinyos.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_pipe.h"
#include "kernel_dev.h"

//Socket's Types
typedef enum {
  LISTENER,
  UNBOUND,  
  PEER  
}socket_type;

//Peer
typedef struct peer_control_block{
	Pipe_cb* write_pipe; 
	Pipe_cb* read_pipe;  
}Peer_cb;

//Listener
typedef struct listener_control_block{
	rlnode queue;
  CondVar req_available;
}Listener_cb;

typedef struct socket_control_block {
  int ref_count;	
  socket_type type;
  port_t port;
  FCB * fcb;			
  union { 
   Listener_cb* ko_listener;
 	 Peer_cb* ko_peer; 
   }socket_kind;
} Socket_cb;

//Connection_Request 
typedef struct connection_request_control_block{
   int admitted;
	 Socket_cb* scb;
	 CondVar connected_cv;
   rlnode queue_node;
}ConReq_cb;

//Port Map the Listeners.
Socket_cb* PORT_MAP[MAX_PORT + 1] = { [0] = 0 };

//Implement socket functions:Read,Write,Close.
int socket_Read(void* socketcb_t, char *buf, unsigned int n);
int socket_Write(void* socketcb_t, const char* buf, unsigned int n);
int socket_Close(void* socketcb_t);


Fid_t sys_Socket(port_t port);
int sys_Listen(Fid_t sock);
Fid_t sys_Accept(Fid_t lsock);
int sys_Connect(Fid_t sock, port_t port, timeout_t timeout);
int sys_ShutDown(Fid_t sock, shutdown_mode how);


//Initialize Socket.
Socket_cb* initialize_socket_cb(FCB* sFCB,port_t port);

//Initialize Requests.
ConReq_cb* initialize_request_cb(Socket_cb* scb);

void scb_delete(Socket_cb* scb);
void incrscb_refcount(Socket_cb* s);
void decrscb_refcount(Socket_cb* s);


#endif