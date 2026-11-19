
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

  Περιμένει να τερματίσει το thread tid και γράφει το exitval του στο *exitval
  (αν δεν είναι NULL). Επιστρέφει -1 αν το tid δεν είναι thread αυτής της
  διεργασίας, αν είναι το τρέχον thread, ή αν το thread είναι (ή γίνει όσο
  περιμένουμε) detached. Στο *exitval γράφουμε ΜΟΝΟ σε επιτυχία: σε σφάλμα ο
  δείκτης μπορεί να είναι άκυρος (π.χ. το test join_illegal_tid περνά (int*)-1).
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
	/* Μόνο cast, ΟΧΙ deref ακόμα: το tid μπορεί να είναι οτιδήποτε */
	PTCB* ptcb = (PTCB*) tid;

	/* Εγκυρότητα: το rlist_find συγκρίνει μόνο διευθύνσεις, οπότε είναι ασφαλές
	   και για σκουπίδια (NOTHREAD, τυχαίοι αριθμοί, thread άλλης διεργασίας).
	   ΠΡΕΠΕΙ να γίνει πριν από οποιοδήποτε deref του ptcb. */
	if(rlist_find(& CURPROC->ptcb_list, ptcb, NULL) == NULL)
		return -1;

	/* Join στον εαυτό: θα περιμέναμε για πάντα */
	if(ptcb == cur_thread()->ptcb)
		return -1;

	/* Από εδώ και πέρα το deref είναι ασφαλές. Σε detached thread δεν γίνεται Join. */
	if(ptcb->detached)
		return -1;

	/* Αναμονή. Το refcount μετρά πόσοι περιμένουν σε Join γι' αυτό το thread
	   (θα το χρειαστεί το free του Ticket 5). Ξυπνάμε από το broadcast του
	   ThreadExit ή του ThreadDetach. Το kernel_wait αφήνει το kernel_mutex όσο
	   κοιμόμαστε και το ξαναπαίρνει πριν επιστρέψει. Ο while (και όχι if)
	   ξαναελέγχει τη συνθήκη μετά από κάθε ξύπνημα. */
	ptcb->refcount++;
	while(!ptcb->exited && !ptcb->detached)
		kernel_wait(& ptcb->exit_cv, SCHED_USER);

	/* Ένα thread μπορεί να έχει ΚΑΙ detached==1 ΚΑΙ exited==1 (Detach όσο ζούσε
	   και μετά ThreadExit). Γι' αυτό ελέγχουμε μόνο το detached: αν είναι 1, ο
	   joiner παίρνει -1 ανεξάρτητα από το exited. Αν είναι exited && !detached,
	   η κατάσταση δεν αλλάζει πια, γιατί το Detach απορρίπτει όσα έχουν τερματίσει. */
	int ret = -1;
	if(!ptcb->detached) {
		if(exitval != NULL)
			*exitval = ptcb->exitval;
		ret = 0;
	}

	/* Διαβάσαμε ό,τι χρειαζόμασταν από το PTCB ΠΡΙΝ το refcount--. Στο Ticket 5,
	   αμέσως μετά το refcount--, αν exited && refcount==0, ο joiner θα ελευθερώνει
	   το PTCB (rlist_remove + free). Μετά από αυτό κανένα deref του ptcb. */
	ptcb->refcount--;

	return ret;
}

/**
  @brief Detach the given thread.

  Κάνει το thread tid detached: δεν μπορεί πια να γίνει Join. Επιστρέφει -1 αν
  το tid δεν είναι thread αυτής της διεργασίας ή αν το thread έχει τερματίσει.
  Ένα thread μπορεί να κάνει Detach τον εαυτό του.
  */
int sys_ThreadDetach(Tid_t tid)
{
	/* Μόνο cast, ΟΧΙ deref ακόμα (βλ. sys_ThreadJoin) */
	PTCB* ptcb = (PTCB*) tid;

	if(rlist_find(& CURPROC->ptcb_list, ptcb, NULL) == NULL)
		return -1;

	/* Ένα thread που έχει τερματίσει δεν μπορεί να γίνει detached (tinyos.h) */
	if(ptcb->exited)
		return -1;

	/* Idempotent: αν ήταν ήδη detached (και ζωντανό), το ξαναγράφουμε 1 και
	   επιστρέφουμε 0 (βλ. test multiple_detach) */
	ptcb->detached = 1;

	/* Ξυπνάμε όσους περιμένουν σε Join, ώστε να δουν detached==1 και να
	   επιστρέψουν -1 */
	kernel_broadcast(& ptcb->exit_cv);

	return 0;
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

