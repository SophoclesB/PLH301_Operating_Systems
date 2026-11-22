
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

 Αλλαγές για τις πολυνηματικές διεργασίες (Μέρος 1):
 - spawn_ptcb / release_PTCB: δημιουργία και αποδέσμευση PTCB. Κάθε thread,
   και το main thread, έχει PTCB (το παλιό pcb->main_thread καταργήθηκε).
 - sys_Exit: γράφει το exitval της διεργασίας και μετά τερματίζει ΜΟΝΟ το
   τρέχον thread (sys_ThreadExit). Η διεργασία τελειώνει όταν βγει και το
   τελευταίο της thread.
 - cleanup_process: ο καθαρισμός που παλιά έκανε το sys_Exit. Τρέχει από το
   τελευταίο thread που κάνει ThreadExit (thread_count == 0).

 Ροή τερματισμού:
   Exit(v) -> sys_Exit: pcb->exitval = v -> sys_ThreadExit
   ThreadExit(v) -> sys_ThreadExit: ptcb->exitval = v, thread_count--
       αν thread_count == 0 -> cleanup_process (ZOMBIE, ξύπνημα του parent)
       kernel_sleep(EXITED) -> ο scheduler ελευθερώνει το TCB

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

  rlnode_init(& pcb->ptcb_list, NULL);
  pcb->thread_count = 0;
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

    /* Τα PCB ξαναχρησιμοποιούνται. Το cleanup_process της προηγούμενης
       διεργασίας πρέπει να άφησε το PCB χωρίς PTCB και χωρίς threads. */
    assert(is_rlist_empty(& pcb->ptcb_list));
    assert(pcb->thread_count == 0);

    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
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

	Διαφορά από το start_thread (kernel_threads.c): όταν επιστρέψει η main
	συνάρτηση, καλείται Exit (όχι ThreadExit). Έτσι η τιμή επιστροφής της main
	γίνεται exitval της ΔΙΕΡΓΑΣΙΑΣ, όπως στο C. Τα υπόλοιπα threads συνεχίζουν
	μέχρι να τερματίσουν κι αυτά (βλ. test_main_exit_cleanup).
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


/*
  Δημιουργεί ένα thread της διεργασίας pcb μαζί με το PTCB του.
  Βλ. την τεκμηρίωση στο kernel_proc.h.

  Must be called with kernel_mutex held
*/
PTCB* spawn_ptcb(PCB* pcb, Task task, int argl, void* args, void (*func)())
{
  /* Η malloc ΔΕΝ μηδενίζει τη μνήμη: κάθε πεδίο πρέπει να αρχικοποιηθεί
     ρητά παρακάτω, αλλιώς θα περιέχει "σκουπίδια". */
  PTCB* ptcb = (PTCB*) malloc(sizeof(PTCB));
  if(ptcb == NULL)
    FATAL("spawn_ptcb: out of memory for PTCB");

  /* Τι θα εκτελέσει το thread. Τα args ΔΕΝ αντιγράφονται και ΔΕΝ ανήκουν
     στο PTCB (για το main thread ανήκουν στο PCB), άρα ποτέ free(ptcb->args). */
  ptcb->task = task;
  ptcb->argl = argl;
  ptcb->args = args;

  /* Κατάσταση τερματισμού / join / detach */
  ptcb->exitval = 0;
  ptcb->exited = 0;
  ptcb->detached = 0;
  ptcb->exit_cv = COND_INIT;
  ptcb->refcount = 0;

  /* Ο κόμβος της λίστας δείχνει πίσω στο ίδιο το PTCB (node->ptcb) */
  rlnode_init(& ptcb->ptcb_list_node, ptcb);

  /* Δημιουργία του TCB (σε κατάσταση INIT, δεν τρέχει ακόμα)
     και αμφίδρομη σύνδεση TCB <-> PTCB */
  TCB* tcb = spawn_thread(pcb, func);
  ptcb->tcb = tcb;
  tcb->ptcb = ptcb;

  /* Καταχώριση στη διεργασία */
  rlist_push_back(& pcb->ptcb_list, & ptcb->ptcb_list_node);
  pcb->thread_count++;

  /* ΔΕΝ κάνουμε wakeup εδώ: το κάνει ο caller όταν όλα είναι έτοιμα,
     γιατί μόλις ξυπνήσει το thread μπορεί να τρέξει αμέσως σε άλλο core. */
  return ptcb;
}


/*
  Αποδέσμευση PTCB. Βλ. την τεκμηρίωση στο kernel_proc.h.

  Must be called with kernel_mutex held
*/
void release_PTCB(PTCB* ptcb)
{
  /* Προϋποθέσεις (βλ. kernel_proc.h): το thread έχει τερματίσει και κανείς
     δεν βρίσκεται μέσα σε Join γι' αυτό. Αλλιώς κάποιος θα διάβαζε το PTCB
     (ή το exit_cv του) μετά το free: use-after-free. */
  assert(ptcb->exited == 1);
  assert(ptcb->refcount == 0);

  /* ΠΡΩΤΑ βγαίνει από τη λίστα, ΜΕΤΑ free. Με την αντίστροφη σειρά, το
     rlist_remove θα έγραφε σε ελευθερωμένη μνήμη (undefined behavior). */
  rlist_remove(& ptcb->ptcb_list_node);
  free(ptcb);
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

  /* Αρχική τιμή του κωδικού εξόδου. Χρειάζεται γιατί μια διεργασία μπορεί να
     τελειώσει χωρίς κανένα thread της να καλέσει Exit (όλα καλούν ThreadExit):
     τότε κανείς δεν γράφει το exitval. Επειδή τα PCB ξαναχρησιμοποιούνται
     (το initialize_PCB τρέχει μόνο στην εκκίνηση), χωρίς αυτή τη γραμμή ο parent
     θα έπαιρνε στο WaitChild το exitval της ΠΡΟΗΓΟΥΜΕΝΗΣ διεργασίας του ίδιου PCB. */
  newproc->exitval = 0;

  /*
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
    /* Το main thread αποκτά κι αυτό PTCB. Τα args δείχνουν στο αντίγραφο
       που ανήκει στη διεργασία (newproc->args), όχι σε νέο αντίγραφο. */
    PTCB* ptcb = spawn_ptcb(newproc, call, newproc->argl, newproc->args, start_main_thread);
    wakeup(ptcb->tcb);
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


/*
  Καθαρισμός της διεργασίας pcb. Βλ. την τεκμηρίωση στο kernel_proc.h.

  Είναι ο κώδικας που παλιά βρισκόταν στο sys_Exit, μεταφερμένος αυτούσιος.
  Καλείται ΜΟΝΟ από το τελευταίο thread της διεργασίας (thread_count==0),
  μέσα από το sys_ThreadExit. ΔΕΝ κοιμίζει το thread: το kernel_sleep(EXITED)
  το κάνει ο caller, αφού επιστρέψει αυτή η συνάρτηση.

  Must be called with kernel_mutex held
*/
void cleanup_process(PCB* pcb)
{
  /*
    Αν είμαστε το init (pid 1), πρέπει να περιμένουμε να τελειώσουν
    όλα τα παιδιά μας πριν τερματίσουμε.
    Προσοχή: το sys_WaitChild μπορεί να κοιμηθεί (kernel_wait), οπότε
    προσωρινά αφήνει το kernel_mutex. Δεν πειράζει, γιατί κανένα άλλο
    thread της διεργασίας δεν είναι ζωντανό (thread_count==0).
   */
  if(get_pid(pcb)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  } else {

    /* Τα παιδιά της διεργασίας που τερματίζει υιοθετούνται από το init */
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& pcb->children_list)) {
      rlnode* child = rlist_pop_front(& pcb->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    /* Τα παιδιά που έχουν ήδη τερματίσει (zombies) πάνε στη λίστα
       exited του init, και το ξυπνάμε για να τα μαζέψει */
    if(!is_rlist_empty(& pcb->exited_list)) {
      rlist_append(& initpcb->exited_list, &pcb->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    /* Μπαίνουμε στη λίστα exited του parent και τον ξυπνάμε (WaitChild) */
    rlist_push_front(& pcb->parent->exited_list, &pcb->exited_node);
    kernel_broadcast(& pcb->parent->child_exit);

  }

  assert(is_rlist_empty(& pcb->children_list));
  assert(is_rlist_empty(& pcb->exited_list));


  /*
    Υπόλοιπος καθαρισμός: ορίσματα, αρχεία κλπ.
   */

  /* Αποδέσμευση του αντιγράφου των args (το έφτιαξε το sys_Exec).
     Το PTCB του main thread δείχνει κι αυτό εδώ, αλλά δεν το διαβάζει
     πια κανείς: η διεργασία δεν έχει ζωντανά threads. */
  if(pcb->args) {
    free(pcb->args);
    pcb->args = NULL;
  }

  /* Κλείσιμο των αρχείων της διεργασίας (FIDT) */
  for(int i=0;i<MAX_FILEID;i++) {
    if(pcb->FIDT[i] != NULL) {
      FCB_decref(pcb->FIDT[i]);
      pcb->FIDT[i] = NULL;
    }
  }

  /* Αποδέσμευση όσων PTCB έμειναν στη λίστα: όσα δεν έγιναν ποτέ Join,
     και (αν δεν είναι detached) αυτό του τρέχοντος thread.
     Όσα ελευθερώθηκαν νωρίτερα (από joiner ή από detached thread στο
     sys_ThreadExit) έχουν ήδη βγει από τη λίστα, άρα δεν τα ξαναβλέπουμε
     εδώ: έτσι δεν γίνεται double free.
     Το μπλοκ είναι στο τέλος, αφού το init έχει ήδη τελειώσει την αναμονή
     στο sys_WaitChild παραπάνω. */
  while(!is_rlist_empty(& pcb->ptcb_list)) {
    PTCB* ptcb = pcb->ptcb_list.next->ptcb;

    /* Όποιος περιμένει σε Join είναι ζωντανό thread αυτής της διεργασίας.
       Αφού thread_count==0, δεν περιμένει κανείς. */
    assert(ptcb->refcount == 0);

    release_PTCB(ptcb);
  }

  /* Το thread_count είναι ήδη 0 (γι' αυτό καλεστήκαμε). Δεν το μηδενίζουμε
     εδώ: μια ανάθεση θα έκρυβε ένα bug, ενώ το assert θα το εντοπίσει. */
  assert(pcb->thread_count == 0);

  /* Η διεργασία γίνεται zombie: το PCB μένει μέχρι να το μαζέψει
     ο parent με WaitChild (cleanup_zombie -> release_PCB). Ο parent
     δεν μπορεί να το κάνει πριν αφήσουμε το kernel_mutex, δηλαδή
     πριν το kernel_sleep(EXITED) του caller. */
  pcb->pstate = ZOMBIE;
}


/*
  Exit: τερματισμός της διεργασίας με κωδικό exitval.

  Γράφει τον κωδικό εξόδου της ΔΙΕΡΓΑΣΙΑΣ και μετά τερματίζει το τρέχον
  thread όπως το ThreadExit. Η διεργασία τελειώνει πραγματικά όταν βγει
  και το τελευταίο της thread (αυτό κάνει το cleanup_process).
  Αν πολλά threads καλέσουν Exit, μετράει η τιμή του τελευταίου.
*/
void sys_Exit(int exitval)
{
  /* Κωδικός εξόδου της διεργασίας (τον διαβάζει ο parent στο WaitChild) */
  CURPROC->exitval = exitval;

  /* ΠΡΟΣΟΧΗ: sys_ThreadExit και ΟΧΙ ThreadExit. Το ThreadExit είναι το
     wrapper του system call που ξανακλειδώνει το kernel_mutex, το οποίο
     ήδη κρατάμε: θα είχαμε deadlock. Το sys_ThreadExit δεν επιστρέφει. */
  sys_ThreadExit(exitval);
}



Fid_t sys_OpenInfo()
{
	return NOFILE;
}

