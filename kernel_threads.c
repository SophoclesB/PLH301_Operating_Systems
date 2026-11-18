
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"   /* kernel_broadcast, kernel_sleep, kernel_wait */

/*
  Συνάρτηση εκκίνησης (του kernel) για κάθε thread που δημιουργεί το CreateThread.
  Είναι το αντίστοιχο του start_main_thread για τα δευτερεύοντα threads.

  Διαβάζει από το PTCB του τρέχοντος thread τι πρέπει να εκτελέσει.
  Αυτό γίνεται χωρίς kernel_mutex, και είναι ασφαλές: τα task/argl/args
  γράφτηκαν πριν από το wakeup και δεν αλλάζουν ποτέ μετά. Επίσης το PTCB
  δεν μπορεί να ελευθερωθεί όσο το ίδιο το thread ζει.
*/
static void start_thread()
{
	PTCB* ptcb = cur_thread()->ptcb;

	Task task = ptcb->task;
	int argl = ptcb->argl;
	void* args = ptcb->args;

	/* Αν η task επιστρέψει, η τιμή της γίνεται το exitval του thread (βλ. tinyos.h) */
	int exitval = task(argl, args);

	/* ΠΡΟΣΟΧΗ: εδώ ThreadExit (το wrapper) και ΟΧΙ sys_ThreadExit.
	   Τρέχουμε σε κώδικα "χρήστη", ΧΩΡΙΣ να κρατάμε το kernel_mutex. Το wrapper
	   κλειδώνει το kernel_mutex και μετά καλεί το sys_ThreadExit, που προϋποθέτει
	   ότι το mutex είναι κλειδωμένο. Είναι η αντίθετη περίπτωση από το sys_Exit,
	   που ήδη κρατά το mutex και γι' αυτό καλεί απευθείας το sys_ThreadExit.
	   Δεν επιστρέφει ποτέ. */
	ThreadExit(exitval);
}


/** 
  @brief Create a new thread in the current process.

  Τα argl και args περνούν ΑΥΤΟΥΣΙΑ στο νέο thread και ΔΕΝ αντιγράφονται
  (σε αντίθεση με το Exec). Σύμφωνα με το tinyos.h, δεν χρειάζεται να ορίζουν
  buffer μήκους argl: μπορεί να είναι άσχετα μεταξύ τους (π.χ. το symposium.c
  περνά δείκτη σε struct). Ένα memcpy(args, argl) θα ήταν λάθος και μπορεί να
  διάβαζε εκτός ορίων (undefined behavior). Τα threads μοιράζονται τη μνήμη της
  διεργασίας, οπότε είναι ευθύνη του προγραμματιστή να μένει έγκυρη η μνήμη
  στην οποία δείχνει το args όσο τη χρειάζεται το thread.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
	/* Χωρίς συνάρτηση δεν υπάρχει τι να εκτελέσει το thread */
	if(task == NULL)
		return NOTHREAD;

	/* Δημιουργία PTCB + TCB, σύνδεση με τη διεργασία, thread_count++.
	   Το thread είναι ακόμα σε κατάσταση INIT και δεν τρέχει. */
	PTCB* ptcb = spawn_ptcb(CURPROC, task, argl, args, start_thread);

	/* Το wakeup γίνεται ΤΕΛΕΥΤΑΙΟ: μόλις ξυπνήσει, το thread μπορεί να τρέξει
	   αμέσως σε άλλο core και να διαβάσει το PTCB του. Άρα όλα πρέπει να είναι
	   ήδη έτοιμα (αυτό το εξασφαλίζει το spawn_ptcb πριν επιστρέψει). */
	wakeup(ptcb->tcb);

	/* Tid_t = η διεύθυνση του PTCB */
	return (Tid_t) ptcb;
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

