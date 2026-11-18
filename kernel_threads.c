
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"   /* kernel_broadcast, kernel_sleep, kernel_wait */

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
	return NOTHREAD;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	/* Tid_t = η διεύθυνση του PTCB του τρέχοντος thread */
	return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
	return -1;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
	return -1;
}

/**
  @brief Terminate the current thread.

  Τερματίζει ΜΟΝΟ το τρέχον thread. Αν είναι το τελευταίο ζωντανό thread
  της διεργασίας, κάνει και τον καθαρισμό της διεργασίας (cleanup_process).
  ΔΕΝ γράφει το pcb->exitval: αυτό το κάνει μόνο το Exit.
  Δεν επιστρέφει ποτέ.
  */
void sys_ThreadExit(int exitval)
{
	PTCB* ptcb = cur_thread()->ptcb;
	PCB* curproc = CURPROC;

	/* Κατάσταση τερματισμού του thread, για όσους κάνουν ThreadJoin */
	ptcb->exitval = exitval;
	ptcb->exited = 1;

	/* Ξυπνάμε όσους περιμένουν σε ThreadJoin γι' αυτό το thread.
	   Δεν θα τρέξουν πριν αφήσουμε το kernel_mutex στο kernel_sleep παρακάτω. */
	kernel_broadcast(& ptcb->exit_cv);

	/* Ένα ζωντανό thread λιγότερο στη διεργασία */
	curproc->thread_count--;

	/* Αν ήμασταν το τελευταίο thread, καθαρίζουμε τη διεργασία */
	if(curproc->thread_count == 0)
		cleanup_process(curproc);

	/* Το PTCB ΔΕΝ ελευθερώνεται εδώ (μπορεί να έρθει Join αργότερα· βλ. Ticket 5).
	   ΠΡΟΣΟΧΗ: μετά το kernel_sleep(EXITED) ο scheduler ελευθερώνει το TCB
	   (release_TCB), άρα από εκεί και πέρα το ptcb->tcb είναι dangling pointer
	   και κανείς δεν πρέπει να το διαβάσει. */
	kernel_sleep(EXITED, SCHED_USER);
}

