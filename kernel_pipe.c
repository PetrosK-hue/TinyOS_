
#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_pipe.h"


static file_ops reader_pipe_ops = {
	//.Open = rpipe_Open,
	.Read = reader_pipe_Read,
	.Write = reader_pipe_Write,
	.Close = reader_pipe_Close
};
static file_ops writer_pipe_ops = {
	//.Open = rpipe_Open,
	.Read = writer_pipe_Read,
	.Write = writer_pipe_Write,
	.Close = writer_pipe_Close
};

Pipe_cb* create_pipe_cb(FCB* reader, FCB* writer){

	Pipe_cb* pipe_cb = (Pipe_cb*)xmalloc(sizeof(Pipe_cb));

	pipe_cb->BUFFER = (char *)malloc(PIPE_BUFFER_SIZE*sizeof(char));
	pipe_cb->buf_size = PIPE_BUFFER_SIZE;
	pipe_cb->reader= reader;
	pipe_cb->writer= writer;
	pipe_cb->w_position = 0; //pipe_cb->BUFFER[0]
	pipe_cb->r_position = 0; //pipe_cb->BUFFER[0]

	pipe_cb->has_space = COND_INIT;
    pipe_cb->has_data = COND_INIT;

	pipe_cb->reader->streamobj = pipe_cb;		//set pipe reader object
	pipe_cb->reader->streamfunc = &reader_pipe_ops;  
	pipe_cb->writer->streamobj = pipe_cb;       //set pipe writer object
	pipe_cb->writer->streamfunc = &writer_pipe_ops;  
	
	return pipe_cb;



}

int sys_Pipe(pipe_t* pipe)
{
	
	//FCB_reserve 
	Fid_t fidt_buffer[2];
	FCB*  fcb_buffer[2];
	int reserve_flag;

	reserve_flag = FCB_reserve(2, fidt_buffer, fcb_buffer);
	if(reserve_flag == 0){
		return -1;
	}

	pipe->read =fidt_buffer[1];
	pipe->write =fidt_buffer[0];

	// Creating Pipe_cb with the FCBs
	create_pipe_cb(fcb_buffer[1], fcb_buffer[0]);

	return 0;
	
}

int Pipe_close(Fid_t close_f){

	int writer_flag = 0;
	int reader_flag = 0;

	FCB* close_fcb = get_fcb(close_f);

	Pipe_cb* pipe_cb = close_fcb->streamobj;

	// Wake up pipe_writer and pipe_reader
	kernel_broadcast(&pipe_cb->has_space);
	kernel_broadcast(&pipe_cb->has_data);
	

	if(close_fcb->refcount == 0) {
		free(close_fcb);
	}
	if(pipe_cb->reader == NULL && pipe_cb->writer == NULL) {
		free(pipe_cb);
	}
	// If pipe reader  has no more dependencies close it
	if(pipe_cb->reader->refcount == 0) {
		reader_pipe_Close(pipe_cb);
		reader_flag = 1;
	}
	// If pipe writer  has no more dependencies close it
	if(pipe_cb->writer->refcount == 0) {
		writer_pipe_Close(pipe_cb);
		writer_flag = 1;
	}
	//Eventually,if pipe's reader & writer are no more, free pipe
	if(writer_flag == 1 && reader_flag == 1) {
		free(pipe_cb);
		return 0;
	}

	return -1;

}


//______________  PIPEEE_FUNCTIONSSSS   _____________//
//=====================================================
//______________ READER_PIPE_FUNCTIONS  _____________//

int reader_pipe_Read(void* pipecb_t, char *buf, unsigned int n){

	if(pipecb_t == NULL ){
		return -1;
	}
	if(buf == NULL ){
		return -1;
	}
	if(n < 0 ){
		return -1;
	}
	Pipe_cb* pipe_cb = (Pipe_cb*)pipecb_t;

	int writer_exists; 	   //data able to read.
 	int has_read = 0;      //index pos in Reader's Buffer.

	if (pipe_cb == NULL){
		return -1;
	}

	if(pipe_cb->writer == NULL){
		writer_exists = 0;
	}else{
		writer_exists = 1;
	}
	//While Buffer is Empty 
	while(pipe_cb->w_position == pipe_cb->r_position){
		if(!writer_exists){
			return 0;
		}else
		{
			kernel_wait(&pipe_cb->has_data,SCHED_PIPE);
		}
	}

	//Read Data From Buffer.
	do {
		buf[has_read++] = pipe_cb->BUFFER[pipe_cb->r_position];
		pipe_cb->r_position = (pipe_cb->r_position + 1) % PIPE_BUFFER_SIZE;
	} while((pipe_cb->r_position != pipe_cb->w_position) && (has_read < n));

	////wake up all the writers waiting
	kernel_broadcast(&pipe_cb->has_space);
	return has_read;


}

int reader_pipe_Write(void* pipecb_t, const char* buf, unsigned int n){
	return -1; //reader can not write
}

int reader_pipe_Close(void* pipecb_t){

	if(pipecb_t == NULL){
		return -1;
	}
	Pipe_cb* pipe_cb = (Pipe_cb*)pipecb_t;

	if(pipe_cb!=NULL){
		//wake upp all the writer/readers before closing this pipe
		kernel_broadcast(&pipe_cb->has_data); 
		pipe_cb->reader = NULL; 
		return 0; 
	}
	return -1; 

}
//=====================================================
//______________ WRITER_PIPE_FUNCTIONS  _____________//
//=====================================================

int writer_pipe_Read(void* pipecb_t, char *buf, unsigned int n){
	return -1;//writer can not read
}

int writer_pipe_Write(void* pipecb_t, const char* buf, unsigned int n){

	if(pipecb_t == NULL) {
		return -1;
	}
	Pipe_cb* pipe_cb = (Pipe_cb*)pipecb_t;
	int has_write = 0;  //Position of index in Reader's Buffer.

	if(pipe_cb == NULL ){
		return -1;
	}
	if(pipe_cb->reader == NULL || pipe_cb->writer == NULL){
		return -1;
	}
	if(buf == NULL){
		return -1;
	}
	if(n<0){
		return -1;
	}

	while(pipe_cb->r_position == (pipe_cb->w_position+1)%PIPE_BUFFER_SIZE){  
		//wait until has data flowing on Stream.
		kernel_wait(&pipe_cb->has_space,SCHED_PIPE);
	}

	//Write data 
	do {
		pipe_cb->BUFFER[pipe_cb->w_position] = buf[has_write++];
		pipe_cb->w_position = (pipe_cb->w_position + 1) % PIPE_BUFFER_SIZE;
	} while(pipe_cb->r_position != (pipe_cb->w_position + 1) % PIPE_BUFFER_SIZE && (has_write < n));

	//wake up all the readers waiting 
	kernel_broadcast(&pipe_cb->has_data);
	return has_write;

}

int writer_pipe_Close(void* pipecb_t){

	if(pipecb_t == NULL){
		return -1;
	}
	Pipe_cb* pipe_cb = (Pipe_cb*) pipecb_t;

	if(pipe_cb!=NULL){
		//wake upp all the writer/readers before closing this pipe
		kernel_broadcast(&pipe_cb->has_space); 
		pipe_cb->writer = NULL; 
		return 0; 
	}
	return -1; 

}







