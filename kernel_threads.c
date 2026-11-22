/*
  kernel_threads.c — Πολυνηματικές διεργασίες (Μέρος 1, Tickets 1–5)

  System calls: CreateThread, ThreadSelf, ThreadJoin, ThreadDetach, ThreadExit.

  Locking: όλα τα sys_* καλούνται από τα wrappers του kernel_sys.c (μακροεντολή
  SYSCALL), που καλούν kernel_lock() πριν και kernel_unlock() μετά. Πρόκειται
  για ένα big kernel lock: σημαφόρος kernel_sem με αρχική τιμή 1, χτισμένος
  πάνω στο kernel_mutex (kernel_cc.c). Στα σχόλια λέμε απλώς «kernel_mutex».
  Άρα μέσα στα sys_* τρέχει ένα μόνο thread του kernel κάθε φορά. Οι αναμονές
  γίνονται με kernel_wait/kernel_sleep, που αφήνουν το lock όσο κοιμόμαστε.
  Εξαίρεση: το start_thread τρέχει σε κώδικα χρήστη, χωρίς το lock.

  Tid_t = (Tid_t) PTCB*. Ένα Tid_t από τον χρήστη μπορεί να είναι οτιδήποτε,
  οπότε πριν από κάθε deref ελέγχουμε ότι είναι στο CURPROC->ptcb_list
  (rlist_find, που συγκρίνει μόνο διευθύνσεις). Έτσι απορρίπτονται τα σκουπίδια,
  τα threads άλλης διεργασίας και όσα PTCB έχουν ήδη ελευθερωθεί.

  Ποιος ελευθερώνει τι:
    TCB         ο scheduler (gain -> release_TCB), μετά το kernel_sleep(EXITED)
    PTCB        release_PTCB, όταν exited==1 && refcount==0, από:
                  - τον τελευταίο joiner (sys_ThreadJoin)
                  - το ίδιο το thread αν είναι detached (sys_ThreadExit)
                  - το cleanup_process (όσα έμειναν στο ptcb_list)
    ptcb->args  ΠΟΤΕ από εδώ: ανήκει στον χρήστη ή στο PCB (pcb->args)

  Πλήρης κύκλος ζωής του PTCB: βλ. kernel_proc.h, struct process_thread_control_block.
*/

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

	/* Αναμονή. Το refcount μετρά πόσοι βρίσκονται μέσα σε Join γι' αυτό το
	   thread: όσο είναι > 0, το PTCB ΔΕΝ ελευθερώνεται. Ξυπνάμε από το broadcast
	   του ThreadExit ή του ThreadDetach. Το kernel_wait αφήνει το kernel_mutex
	   όσο κοιμόμαστε και το ξαναπαίρνει πριν επιστρέψει. Ο while (και όχι if)
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

	/* ΓΙΑΤΙ το refcount-- είναι ΜΕΤΑ το kernel_wait (και πρέπει να μείνει εκεί):
	   1. Πρέπει πρώτα να έχουμε διαβάσει ό,τι χρειαζόμαστε από το PTCB
	      (detached, exitval) παραπάνω.
	   2. Το ίδιο το exit_cv βρίσκεται ΜΕΣΑ στο PTCB. Μετά το ξύπνημα, ο κώδικας
	      του cv_wait (kernel_cc.c) κλειδώνει το cv->waitset_lock, δηλαδή γράφει
	      στη μνήμη του PTCB, ΠΡΙΝ ξαναπάρουμε το kernel_mutex. Όσο το refcount
	      μας μετράει, κανείς δεν μπορεί να ελευθερώσει το PTCB, άρα το
	      exit_cv είναι έγκυρο. Αν το refcount-- γινόταν πριν από το
	      kernel_wait, ο ThreadExit (detached && refcount==0) ή κάποιος άλλος
	      joiner θα μπορούσε να κάνει free ενώ εμείς είμαστε ακόμα μέσα στο
	      cv_wait: use-after-free. */
	ptcb->refcount--;

	/* Ο τελευταίος που "κρατά" ένα τερματισμένο thread το ελευθερώνει.
	   Ισχύει ΚΑΙ όταν ret==-1: αν το thread έγινε detached όσο περιμέναμε και
	   μετά τερμάτισε, ο ThreadExit του δεν το ελευθέρωσε (το refcount ήταν > 0
	   τότε), άρα αν δεν το κάνουμε εμείς θα έχουμε leak.
	   Αν !exited, δεν το αγγίζουμε: θα το ελευθερώσει ο ThreadExit (αν είναι
	   detached), ένας μελλοντικός joiner ή το cleanup_process. */
	if(ptcb->exited && ptcb->refcount == 0)
		release_PTCB(ptcb);

	/* Από εδώ και πέρα ΚΑΝΕΝΑ deref του ptcb: μπορεί να έχει ελευθερωθεί.
	   Ένα επόμενο Join με το ίδιο tid θα αποτύχει στο rlist_find (-1). */
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

	/* (α) Αν είμαστε detached, κανείς δεν θα κάνει Join για να ελευθερώσει
	   το PTCB μας, άρα το ελευθερώνουμε εμείς. Αλλά μόνο αν refcount==0: αν
	   κάποιος joiner ξύπνησε από το Detach και δεν έχει τρέξει ακόμα, το PTCB
	   το ελευθερώνει εκείνος (βλ. sys_ThreadJoin).
	   Αν ΔΕΝ είμαστε detached, το PTCB μένει: μπορεί να έρθει Join αργότερα. */
	if(ptcb->detached && ptcb->refcount == 0)
		release_PTCB(ptcb);

	/* Από εδώ και πέρα ΚΑΝΕΝΑ deref του ptcb (μπορεί να έχει ελευθερωθεί).
	   Μηδενίζουμε και τον δείκτη στο TCB, ώστε να μη μείνει dangling. */
	ptcb = NULL;
	cur_thread()->ptcb = NULL;

	/* (β) Αν ήμασταν το τελευταίο thread, καθαρίζουμε τη διεργασία. Το
	   cleanup_process ελευθερώνει όσα PTCB έμειναν στη λίστα.
	   ΑΠΟΦΥΓΗ DOUBLE FREE: η σειρά (α) πριν από (β) είναι υποχρεωτική. Αν
	   είμαστε detached, το (α) μας έβγαλε ήδη από τη λίστα (release_PTCB),
	   άρα το (β) δεν μας ξαναβλέπει. Αν δεν είμαστε, το (α) δεν έκανε τίποτα
	   και μας ελευθερώνει μόνο το (β). Με την αντίστροφη σειρά, το
	   ptcb->detached του (α) θα ήταν ήδη use-after-free. */
	if(curproc->thread_count == 0)
		cleanup_process(curproc);

	/* ΠΡΟΣΟΧΗ: μετά το kernel_sleep(EXITED) ο scheduler ελευθερώνει το TCB
	   (release_TCB), άρα αν το PTCB μας ζει ακόμα (μη detached, περιμένει
	   Join), το ptcb->tcb του είναι dangling pointer και κανείς δεν πρέπει
	   να το διαβάσει. */
	kernel_sleep(EXITED, SCHED_USER);
}

