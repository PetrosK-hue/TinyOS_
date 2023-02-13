#include "kernel_pipe.h"
#include "kernel_socket.h"
#include "kernel_streams.h"
#include "kernel_sys.h"
#include "kernel_dev.h"
#include "util.h"


static file_ops socket_file_ops = {
	.Read  = socket_Read,
	.Write = socket_Write,
	.Close = socket_Close
};

//Initialize Socket.
Socket_cb* initialize_socket_cb(FCB* FCB,port_t port){

	Socket_cb* socket_cb = (Socket_cb*)xmalloc(sizeof(Socket_cb));
	socket_cb->ref_count = 1;
	socket_cb->fcb = FCB;
	socket_cb->fcb->streamobj = socket_cb;
	socket_cb->fcb->streamfunc = &socket_file_ops;
	socket_cb->type = UNBOUND;
	socket_cb->port = port;
	socket_cb->socket_kind.ko_peer = (Peer_cb*)xmalloc(sizeof(Peer_cb));
	socket_cb->socket_kind.ko_listener = (Listener_cb*)xmalloc(sizeof(Listener_cb));
	return socket_cb;

}
//Initialize Requests.
ConReq_cb* initialize_request_cb(Socket_cb* socket_cb){

	ConReq_cb* request = (ConReq_cb*)xmalloc(sizeof(ConReq_cb));
	request->scb = socket_cb;
	request->connected_cv = COND_INIT;
	request->admitted = 0;
	rlnode_init(&request->queue_node,request);

	return request;
}

//Socket Read.
int socket_Read(void* socketcb_t, char *buf, unsigned int n){
if(socketcb_t == NULL || buf == NULL || n < 0 ){
		return -1;
	}
	//instance of socket
	Socket_cb* socketcb = (Socket_cb*)socketcb_t;

	if(socketcb->type != PEER){
		return -1;
	}
	// peer reader
	if(socketcb->socket_kind.ko_peer->read_pipe!= NULL){ 
		int k;
		k = reader_pipe_Read(socketcb->socket_kind.ko_peer->read_pipe,buf,n);
		return k;
	}
	return -1;
}

//Socket Write.
int socket_Write(void* socketcb_t, const char* buf, unsigned int n){
	if(socketcb_t == NULL || buf == NULL || n<0 ){
		return -1;
	}

	//instance of socket	
	Socket_cb* socketcb = (Socket_cb*)socketcb_t;

	
	if(socketcb->type != PEER){
		return -1;
	}
	// peer writer
	if(socketcb->socket_kind.ko_peer->write_pipe != NULL){
		int k;
		k = writer_pipe_Write(socketcb->socket_kind.ko_peer->write_pipe,buf,n);
		return k;
	}
	return -1;

}
//Socket Close.
int socket_Close(void* socketcb_t){

	//instance of socket
	Socket_cb* socketcb = (Socket_cb*)socketcb_t;

	//check for listener to wake
	if(socketcb->type == LISTENER){
		kernel_broadcast(&socketcb->socket_kind.ko_listener->req_available);//wake up all its peers
		PORT_MAP[socketcb->port] = NULL;

	} // else free sockets' pipes
	else if(socketcb->type ==  PEER){
		reader_pipe_Close(socketcb->socket_kind.ko_peer->read_pipe);
		writer_pipe_Close(socketcb->socket_kind.ko_peer->write_pipe);
		decrscb_refcount(socketcb);
	}

	return 1;

}

//delete socket.
void scb_delete(Socket_cb* socketcb_t){
	assert(socketcb_t != NULL);
	free(socketcb_t);
	return ;
}

void incrscb_refcount(Socket_cb* socketcb_t){
    socketcb_t->ref_count++;
}
void decrscb_refcount(Socket_cb* socketcb_t){
    socketcb_t->ref_count--;
    if(socketcb_t->ref_count == 0){
        scb_delete(socketcb_t);
    }
}

//=======================================================================================
//=======================================================================================

Fid_t sys_Socket(port_t port)
{
	//Given port can not be illegal
	if(port < NOPORT || port > MAX_PORT ) {
		return NOFILE;
	}
	//If port is legal make File ID and File Control Block reservation.
    Fid_t socket_fidt;
	FCB * FCB;
	int ret = FCB_reserve(1, &socket_fidt, &FCB);
	if(ret == 0) {
		return NOFILE;
	}

	// Initialize the new Socket CB
	initialize_socket_cb(FCB,port);

	return socket_fidt;
}


int sys_Listen(Fid_t sock)
{
	//Given socket can not be illegal
	if(sock > MAX_FILEID || sock == NOFILE) {
		return -1;
	}

	//save the instance to use below.
	FCB * sFCB = get_fcb(sock);

	// if the fcb does not exist
	if(sFCB==NULL){
		return -1;
	}

	//take the socket cb indicated from the FCB
	Socket_cb* rcb_socket_cb = sFCB->streamobj;

	//if the SOCKET CB IS NOT INITIALIZED/EXISTS
	if(rcb_socket_cb == NULL){
		return -1;
	}
	//Can not be peered with listener
	if(rcb_socket_cb->type != UNBOUND){
		return -1;
	}
	//If the socket is not be bounded 
	if(rcb_socket_cb->port == NOPORT){
		return -1;
	}
	//if the port is holded from another socket
	if(PORT_MAP[rcb_socket_cb->port] != NULL){
		return -1;
	}

	//Make socket as a Listener
	rcb_socket_cb->type = LISTENER;
	
	rcb_socket_cb->socket_kind.ko_listener = (Listener_cb*)xmalloc (sizeof(Listener_cb));
	rlnode_init(&rcb_socket_cb->socket_kind.ko_listener->queue ,NULL);	
	rcb_socket_cb->socket_kind.ko_listener->req_available = COND_INIT;
	//Listener socket to  the Port Map
	PORT_MAP[rcb_socket_cb->port] = rcb_socket_cb;
		

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
// If Listener socket File Id given is not legal, return error
	if(lsock >= MAX_FILEID || lsock <= NOFILE) {
		return NOFILE;
	}

	FCB* sFCB = get_fcb(lsock);	//IMPORTANT

	// if the fcb does not exist
	if(sFCB==NULL){
		return NOFILE;
	}

	
	// Get the Listener socket CB
	Socket_cb* listener_cb = sFCB->streamobj;

	// If listener CB is non existent, return error
	if(listener_cb == NULL || listener_cb->type != LISTENER) {
		return NOFILE;
	}
	incrscb_refcount(listener_cb);

	// Wait if Listener socket has no requests to serve
	while(is_rlist_empty(&listener_cb->socket_kind.ko_listener->queue))  {
		kernel_wait(&listener_cb->socket_kind.ko_listener->req_available, SCHED_IO);
		
		// If Listener gets closed before Accept we must return error
		if(PORT_MAP[listener_cb->port] == NULL) {
			return NOFILE;
		}
	}

	// Accept the first request in the Listener Request Queue 
	rlnode* cur_node = rlist_pop_front(&listener_cb->socket_kind.ko_listener->queue);
	ConReq_cb* cur_request = cur_node->obj;	// Request to serve

	// Make socket that is bound to this request a peer
	cur_request->scb->type = PEER;
	Socket_cb* sock_cb2 = cur_request->scb; 

	Fid_t fid3; 
	Socket_cb* sock_cb3;
	FCB* sock_fcb3;
	

	// Reserve memory and create the second peer
	int ret = FCB_reserve(1, &fid3, &sock_fcb3);
	if (ret == 0) {
		return NOFILE;
	}
	// Initialize the new peer (FID3)
	sock_cb3 = initialize_socket_cb(sock_fcb3, listener_cb->port);
	sock_cb3->type = PEER;
	sock_cb3->socket_kind.ko_peer = (Peer_cb*)xmalloc(sizeof(Peer_cb));
	

	// Establish peer-peer connection with two pipes
	Pipe_cb* pipe1;
	Pipe_cb* pipe2;
	
	// Initialize the two pipes
	pipe1 = create_pipe_cb(sock_cb2->fcb, sock_cb3->fcb);
	pipe2 = create_pipe_cb(sock_cb3->fcb, sock_cb2->fcb);
	
	// Set socket CBs as pipes' stream objects
	pipe1->reader->streamobj = sock_cb2;
	pipe1->writer->streamobj = sock_cb3;
	pipe2->reader->streamobj = sock_cb3;
	pipe2->writer->streamobj = sock_cb2;

	// Set socket/pipe stream functions
	pipe1->reader->streamfunc = &socket_file_ops;
	pipe1->writer->streamfunc = &socket_file_ops;
	pipe2->reader->streamfunc = &socket_file_ops;
	pipe2->writer->streamfunc = &socket_file_ops;

	// Make connections between the two peers
	sock_cb2->socket_kind.ko_peer->read_pipe = pipe1;
	sock_cb2->socket_kind.ko_peer->write_pipe = pipe2;

	sock_cb3->socket_kind.ko_peer->read_pipe = pipe2;
	sock_cb3->socket_kind.ko_peer->write_pipe = pipe1;
	
	// Change the request admittion flag
	cur_request->admitted = 1;
	// Wake up Connect that waits for admittion
	kernel_signal(&cur_request->connected_cv);	

	decrscb_refcount(listener_cb);
	// Returns the new peer File ID, for user to use
	return fid3;


}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	//If port is illegal/or not exist, return error
	if(PORT_MAP[port] == NULL || port <= NOPORT || port > MAX_PORT) {
		return -1;
	}
	//if socket in this port is not a Listener, return error
	if(PORT_MAP[port]->type != LISTENER) {
		return -1;
	}
    
	//save the instance to use below.
	FCB* sFCB = get_fcb(sock);
	// if the fcb does not exist
	if(sFCB==NULL){
		return -1;
	}
	Socket_cb* scb = sFCB->streamobj;
	
	if(scb == NULL){
		return -1;
	}
	if(scb->type != UNBOUND){
		return -1;
	}
    incrscb_refcount(scb);
  
	//get the Listener Socket CB
	Socket_cb* listener_cb = PORT_MAP[port];

	//create and initialize a new request
	ConReq_cb* request = initialize_request_cb(scb);
	//peer the new scb
	request->scb->socket_kind.ko_peer = (Peer_cb*)xmalloc(sizeof(Peer_cb));
	request->scb->type = PEER;

	//push back request to Listener Requests Queue
	rlist_push_back(&listener_cb->socket_kind.ko_listener->queue,&request->queue_node);

	//wake up the Listener from accept
	kernel_signal(&listener_cb->socket_kind.ko_listener->req_available);

	//wait for Accept to admit the connection, time out after 1000 * timeout
	while(request->admitted==0){
    	int done_admit = kernel_timedwait(&request->connected_cv, SCHED_IO, 500);
		if(!done_admit){
			return -1;
		};
	}
	
	if(request->admitted == 0) {
		return -1;
	}

    decrscb_refcount(scb); 

	return 0;

}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{	
	FCB *sFCB = get_fcb(sock);
	Socket_cb *rcb_socket_cb = sFCB->streamobj;

	if(rcb_socket_cb == NULL || rcb_socket_cb->type == LISTENER || rcb_socket_cb->type == UNBOUND) {
			return -1;
		}
			switch (how)
			{
				case SHUTDOWN_READ: 
					// Close socket read 
					rcb_socket_cb->socket_kind.ko_peer->read_pipe->reader = NULL;
					rcb_socket_cb->socket_kind.ko_peer->read_pipe->writer = NULL;
					rcb_socket_cb->socket_kind.ko_peer->read_pipe = NULL;

					break; 
				
				case SHUTDOWN_WRITE: 
					// Close socket write 
					rcb_socket_cb->socket_kind.ko_peer->write_pipe->reader = NULL;
					rcb_socket_cb->socket_kind.ko_peer->write_pipe->writer = NULL;
					rcb_socket_cb->socket_kind.ko_peer->write_pipe = NULL;
					break;
				
				case SHUTDOWN_BOTH: 
					//Close both socket receiver,sender
					rcb_socket_cb->socket_kind.ko_peer->read_pipe->reader = NULL;
					rcb_socket_cb->socket_kind.ko_peer->read_pipe->writer = NULL;
					rcb_socket_cb->socket_kind.ko_peer->read_pipe = NULL;

					rcb_socket_cb->socket_kind.ko_peer->write_pipe->reader = NULL;
					rcb_socket_cb->socket_kind.ko_peer->write_pipe->writer = NULL;
					rcb_socket_cb->socket_kind.ko_peer->write_pipe = NULL;
					break;
					
				default:
					return -1;
			}
		return 0;
		
}

