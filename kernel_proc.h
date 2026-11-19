#ifndef __KERNEL_PROC_H
#define __KERNEL_PROC_H

/**
  @file kernel_proc.h
  @brief The process table and process management.

  @defgroup proc Processes
  @ingroup kernel
  @brief The process table and process management.

  This file defines the PCB structure and basic helpers for
  process access.

  @{
*/ 

#include "tinyos.h"
#include "kernel_sched.h"

/**
  @brief PID state

  A PID can be either free (no process is using it), ALIVE (some running process is
  using it), or ZOMBIE (a zombie process is using it).
  */
typedef enum pid_state_e {
  FREE,   /**< @brief The PID is free and available */
  ALIVE,  /**< @brief The PID is given to a process */
  ZOMBIE  /**< @brief The PID is held by a zombie */
} pid_state;

/**
  @brief Process Control Block.

  This structure holds all information pertaining to a process.
 */
typedef struct process_control_block {
  pid_state  pstate;      /**< @brief The pid state for this PCB */

  PCB* parent;            /**< @brief Parent's pcb. */
  int exitval;            /**< @brief The exit value of the process */

  Task main_task;         /**< @brief The main thread's function */
  int argl;               /**< @brief The main thread's argument length */
  void* args;             /**< @brief The main thread's argument string */

  rlnode children_list;   /**< @brief List of children */
  rlnode exited_list;     /**< @brief List of exited children */

  rlnode children_node;   /**< @brief Intrusive node for @c children_list */
  rlnode exited_node;     /**< @brief Intrusive node for @c exited_list */

  CondVar child_exit;     /**< @brief Condition variable for @c WaitChild. 

                             This condition variable is  broadcast each time a child
                             process terminates. It is used in the implementation of
                             @c WaitChild() */

  FCB* FIDT[MAX_FILEID];  /**< @brief The fileid table of the process */

  rlnode ptcb_list;       /**< @brief List of the PTCBs of this process's threads */
  int thread_count;       /**< @brief Number of live (non-exited) threads */

} PCB;

/**
  @brief Process-Thread Control Block.

  Per-thread information at the process level. The typedef @c PTCB
  is declared in util.h.
 */
struct process_thread_control_block {
  TCB* tcb;               /**< @brief The thread's TCB */

  Task task;              /**< @brief The thread's start function */
  int argl;               /**< @brief Length of the argument buffer */
  void* args;             /**< @brief The argument buffer.
                               ΠΡΟΣΟΧΗ: το PTCB ΔΕΝ είναι ιδιοκτήτης αυτής της μνήμης.
                               Για το main thread δείχνει στο pcb->args (το ελευθερώνει
                               η διεργασία), για τα υπόλοιπα threads στον buffer του χρήστη.
                               Ποτέ free(ptcb->args). */

  int exitval;            /**< @brief The thread's exit value */

  int exited;             /**< @brief 1 if the thread has exited */
  int detached;           /**< @brief 1 if the thread is detached */
  CondVar exit_cv;        /**< @brief Broadcast when the thread exits or is detached */

  int refcount;           /**< @brief Number of threads waiting in ThreadJoin */

  rlnode ptcb_list_node;  /**< @brief Intrusive node for @c PCB.ptcb_list */
};


/**
  @brief Initialize the process table.

  This function is called during kernel initialization, to initialize
  any data structures related to process creation.
*/
void initialize_processes();

/**
  @brief Get the PCB for a PID.

  This function will return a pointer to the PCB of 
  the process with a given PID. If the PID does not
  correspond to a process, the function returns @c NULL.

  @param pid the pid of the process 
  @returns A pointer to the PCB of the process, or NULL.
*/
PCB* get_pcb(Pid_t pid);

/**
  @brief Get the PID of a PCB.

  This function will return the PID of the process 
  whose PCB is pointed at by @c pcb. If the pcb does not
  correspond to a process, the function returns @c NOPROC.

  @param pcb the pcb of the process 
  @returns the PID of the process, or NOPROC.
*/
Pid_t get_pid(PCB* pcb);

/**
  @brief Δημιουργία νέου thread μαζί με το PTCB του.

  Δεσμεύει και αρχικοποιεί ένα PTCB, δημιουργεί το TCB με @c spawn_thread,
  συνδέει τα δύο μεταξύ τους, προσθέτει το PTCB στο @c pcb->ptcb_list και
  αυξάνει το @c pcb->thread_count.

  ΔΕΝ ξυπνά το thread: ο caller πρέπει να καλέσει @c wakeup(ptcb->tcb)
  αφού ολοκληρώσει όλη την αρχικοποίηση.

  Καλείται με κλειδωμένο το kernel_mutex.

  @param pcb   η διεργασία στην οποία ανήκει το νέο thread
  @param task  η συνάρτηση χρήστη που θα εκτελέσει το thread
  @param argl  το μήκος των ορισμάτων
  @param args  τα ορίσματα (δεν αντιγράφονται, βλ. PTCB.args)
  @param func  η συνάρτηση εκκίνησης του kernel (π.χ. @c start_main_thread)
  @returns το νέο PTCB (ποτέ NULL: αν αποτύχει η malloc, FATAL)
*/
PTCB* spawn_ptcb(PCB* pcb, Task task, int argl, void* args, void (*func)());

/**
  @brief Αποδέσμευση ενός PTCB.

  Βγάζει το PTCB από το @c ptcb_list της διεργασίας του και μετά το
  ελευθερώνει. Είναι ο ΜΟΝΑΔΙΚΟΣ τρόπος να ελευθερωθεί ένα PTCB, ώστε να
  ισχύει πάντα το αναλλοίωτο:

      ένα PTCB είναι στο ptcb_list  <=>  δεν έχει ελευθερωθεί.

  Έτσι το @c cleanup_process (που ελευθερώνει ό,τι βρει στη λίστα) δεν
  μπορεί να κάνει double free, και το @c rlist_find των Join/Detach
  απορρίπτει όσα tid έχουν ήδη ελευθερωθεί.

  ΔΕΝ ελευθερώνει το @c ptcb->args (δεν ανήκει στο PTCB, βλ. PTCB.args).
  Μετά την κλήση, ο caller δεν πρέπει να κάνει κανένα deref του @c ptcb.

  Καλείται με κλειδωμένο το kernel_mutex, μόνο όταν
  @c ptcb->exited==1 και @c ptcb->refcount==0.

  @param ptcb το PTCB που ελευθερώνεται
*/
void release_PTCB(PTCB* ptcb);

/**
  @brief Καθαρισμός μιας διεργασίας όταν τερματίσει το τελευταίο της thread.

  Αν η διεργασία είναι το init (pid 1), περιμένει να τελειώσουν όλα τα παιδιά της.
  Αλλιώς δίνει τα παιδιά της στο init και μπαίνει στη λίστα exited του parent.
  Μετά ελευθερώνει τα args, κλείνει τα αρχεία του FIDT, ελευθερώνει όσα PTCB
  έμειναν στο @c ptcb_list (με @c release_PTCB) και κάνει τη διεργασία ZOMBIE.

  ΔΕΝ κοιμίζει το τρέχον thread: ο caller (sys_ThreadExit) καλεί μετά
  @c kernel_sleep(EXITED, SCHED_USER).

  Καλείται με κλειδωμένο το kernel_mutex, μόνο όταν @c pcb->thread_count==0.

  @param pcb η διεργασία που τερματίζει
*/
void cleanup_process(PCB* pcb);

/** @} */

#endif
