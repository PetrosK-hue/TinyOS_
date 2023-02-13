#include <assert.h>
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_sys.h"
#include "kernel_streams.h"

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  PTCB* ptcb = initialize_ptcb(CURPROC, task, argl, args, start_thread);
  wakeup(ptcb->tcb);
  return (Tid_t)ptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{ 
  if(tid==NOTHREAD){
    return -1;
  }
  PTCB* ptcb = (PTCB*) tid;
 
 PTCB* check_ptcb = (PTCB *)rlist_find(&CURPROC->ptcb_list, ptcb, NULL);
  if(check_ptcb == NULL) {
    return -1;
  }
 
  if(ptcb->tcb ==(Tid_t) cur_thread()) {
    return -1;
  }

  /*Thread to be joined owned by the same process as 
  the current thread and is not detached */
 ptcb->ref_count++;
 while(ptcb->detached !=1 && ptcb->exited !=1){
     kernel_wait(&(ptcb->exit_cv), SCHED_USER);  
 }
 ptcb->ref_count--;

 if(ptcb->detached == 1 ){
   return -1;
 }
  //Exit value parsing
  if(ptcb->exited == 1) {
    if(exitval != NULL) {
      *exitval = ptcb->exitval;
    }
  }

  if(ptcb->ref_count==0){
    rlist_remove(&ptcb->ptcb_node);
    free(ptcb);
  }

  return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  PTCB* ptcb = (PTCB*)tid;

  PTCB* find_ptcb =(PTCB*)rlist_find(&(CURPROC->ptcb_list),ptcb, NULL);
  
  if(find_ptcb == NULL /*&& tid!= (Tid_t) cur_thread()*/) {
    return -1;
    }
  if(ptcb->exited == 1 /*&& ptcb->tcb==NULL*/) {
     return -1;
  }

  ptcb->detached = 1;
  ptcb->ref_count = 0;
  kernel_broadcast(&(ptcb->exit_cv));
  return 0;
 
}
  


/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{ 
  PTCB* ptcb = cur_thread()->ptcb;

  assert(ptcb != NULL);
  ptcb->exitval = exitval;
  ptcb->exited = 1;
  PCB* curproc = CURPROC;


    kernel_broadcast(&(ptcb->exit_cv));

  curproc->thread_count = curproc->thread_count -1;

  if(curproc->thread_count==0){
    if(get_pid(curproc) !=1){
    /* Reparent any children of the exiting process to the 
       initial task */
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& curproc->children_list)) {
      rlnode* child = rlist_pop_front(& curproc->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    /* Add exited children to the initial task's exited list 
       and signal the initial task */
    if(!is_rlist_empty(& curproc->exited_list)) {
      rlist_append(& initpcb->exited_list, &curproc->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    /* Put me into my parent's exited list */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);

    }

  }

  assert(is_rlist_empty(& curproc->children_list));
  assert(is_rlist_empty(& curproc->exited_list));


  /* 
    Do all the other cleanup we want here, close files etc. 
   */

  /* Release the args data */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  rlnode* pop_ptcb;
  while(is_rlist_empty(&(CURPROC->ptcb_list)) != 0){

    pop_ptcb = rlist_pop_front(&(CURPROC->ptcb_list));

    free(pop_ptcb);
   }

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;
   
  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);
}








