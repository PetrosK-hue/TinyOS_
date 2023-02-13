#ifndef __KERNEL_PIPE_H
#define __KERNEL_PIPE_H


#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_dev.h"
#include "kernel_cc.h"
#include "kernel_streams.h"



typedef struct pipe_control_block{

    FCB *reader;
    FCB *writer;
	CondVar has_space;    /* For blocking writer if no space is available */
    CondVar has_data;     /* For blocking reader until data are available */
	
    int w_position, r_position;  
    char *BUFFER;   /* bounded (cyclic) byte buffer */
    int buf_size;   /* the size of buffer */

}Pipe_cb;


//General Funcs for Pipes
int sys_Pipe(pipe_t* pipe);
Pipe_cb* create_pipe_cb(FCB* reader, FCB* writer);//Initialize pipe control block
int Pipe_Close(Fid_t end_fd); 


// Reader's Functions declaration
int reader_pipe_Read(void* pipe, char *buf, unsigned int size);
int reader_pipe_Write(void* pipe, const char* buf, unsigned int size);
int reader_pipe_Close(void* pipe);


// Writer's Functions declaration
int writer_pipe_Read(void* pipe, char *buf, unsigned int size);
int writer_pipe_Write(void* pipe, const char* buf, unsigned int size);
int writer_pipe_Close(void* pipe);



#endif