
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
    rlnode_init(&pcb->ptcb_list, NULL);
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}

/* change */
void start_thread(){
  int exitval;
  
  Task call = cur_thread()->ptcb->task;
  int argl = cur_thread()->ptcb->argl;
  void *args = cur_thread()->ptcb->args;

  exitval = call(argl, args);
  ThreadExit(exitval);
}

/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
     /* vsam's copde newproc->main_thread = spawn_thread(newproc, start_main_thread); */
    PTCB* call_ptcb = initialize_ptcb(newproc, newproc->main_task, 
                                newproc->argl, newproc->args, start_main_thread);
    newproc->main_thread = call_ptcb->tcb;
    wakeup(newproc->main_thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  }
    sys_ThreadExit(exitval);

    
}

/* System Info Operations & Control Block */
/* Info CB stream functions */

/* Reader function to print system information */
int info_Read(void* info_cb, char *proc_info, unsigned int n) {
  
  Info_cb* info = (Info_cb*)info_cb;    // The info CB 
  int i = info->index_counter;            // Proccess table info counter

  // Copy system/proccess information in order to print them
  if(PT[i].pstate != FREE) {
    info->proc_info->pid = get_pid(&PT[i]);
    info->proc_info->ppid = get_pid(PT[i].parent);
    if(PT[i].pstate == ZOMBIE) {
        info->proc_info->alive = 0;
    } else {
        info->proc_info->alive = PT[i].pstate;
      }
    info->proc_info->thread_count = rlist_len(&PT[i].ptcb_list);
    info->proc_info->main_task = PT[i].main_task;
    info->proc_info->argl = PT[i].argl;
    memcpy(info->proc_info->args, PT[i].args, PT[i].argl);
    
    memcpy(proc_info, info->proc_info, n);
    info->index_counter++;
  } else {
    // If proccess is FREE do not display its info
    return -1;
  }
  
  // If all non-free proccess info is printed return
  if(info->index_counter==MAX_PROC) {
    return -1;
  }
  return n;
}
// Fake write function  errors (cannot write information)
int info_Write(void* info_cb, const char* buf, unsigned int n) {
  return n;
}
/* Function to free the Info CB */
int info_Close(void* info_cb) {
  Info_cb* info = (Info_cb*)info_cb;
  free(info);
  info = NULL;
  return 1;
}

static file_ops sys_info_ops = {
  .Read = info_Read,
  .Write = info_Write,
  .Close = info_Close
}; 

/* Function to initialize an Info CB*/
Fid_t sys_OpenInfo()
{
  Info_cb* info;
  Fid_t fd;
  FCB* fcb;
  int ret;

  ret = FCB_reserve(1,&fd,&fcb);
  if(ret == 0) {
    return NOFILE;
  }
  info = (Info_cb*)malloc(sizeof(Info_cb));
  info->info_fcb = fcb;
  info->info_fcb->streamobj = info;
  info->info_fcb->streamfunc = &sys_info_ops;
  info->proc_info = (procinfo*)malloc(sizeof(procinfo));
  info->index_counter = 1;

	return fd;
}



