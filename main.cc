
#include <iostream>
#include <sstream>  
#include <list>
#include <iterator>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>

#define DEBUG

/*
This program does the following.
1) Create handlers for two signals.
2) Create an idle process which will be executed when there is nothing else to do.
3) Create a send_signals process that sends a SIG ALRM every so often.

If compiled with -DEBUG, when run it should produce the following
output (approximately):

$ ./a.out
state:		3
name:		 IDLE
pid:		  21145
ppid:		 21143
interrupts:   0
switches:	 0
started:	  0
in CPU.cc at 240 at beginning of send_signals getpid() = 21144
in CPU.cc at 245 sending signal = 14
in CPU.cc at 246 to pid = 21143
In ISR stopped:	 21145
---- entering scheduler
continuing	21145
---- leaving scheduler
in CPU.cc at 245 sending signal = 14
in CPU.cc at 246 to pid = 21143
In ISR stopped:	 21145
---- entering scheduler
continuing	21145
---- leaving scheduler
in CPU.cc at 245 sending signal = 14
in CPU.cc at 246 to pid = 21143
In ISR stopped:	 21145
---- entering scheduler
continuing	21145
---- leaving scheduler
in CPU.cc at 245 sending signal = 14
in CPU.cc at 246 to pid = 21143
In ISR stopped:	 21145
---- entering scheduler
continuing	21145
---- leaving scheduler
in CPU.cc at 245 sending signal = 14
in CPU.cc at 246 to pid = 21143
in CPU.cc at 250 at end of send_signals
In ISR stopped:	 21145
---- entering scheduler
continuing	21145
Terminated: 15
---------------------------------------------------------------------------
Add the following functionality.
1) Change the NUM_SECONDS to 20.

2) Take any number of arguments for executables and place each on the
   processes list with a state of NEW. The executables will not require
   arguments themselves.

3) When a SIGALRM arrives, scheduler() will be called; it currently simply
   restarts the idle process. Instead, do the following.
   a) Update the PCB for the process that was interrupted including the
	  number of context switches and interrupts it had and changing its
	  state from RUNNING to READY.
   b) If there are any NEW processes on processes list, change its state to
	  RUNNING, and fork() and execl() it.
   c) If there are no NEW processes, round robin the processes in the
	  processes queue that are READY. If no process is READY in the
	  list, execute the idle process.

4) When a SIGCHLD arrives notifying that a child has exited, process_done() is
   called. process_done() currently only prints out the PID and the status.
   a) Add the printing of the information in the PCB including the number
	  of times it was interrupted, the number of times it was context
	  switched (this may be fewer than the interrupts if a process
	  becomes the only non-idle process in the ready queue), and the total
	  system time the process took.
   b) Change the state to TERMINATED.
   c) Restart the idle process to use the rest of the time slice.
*/

#define NUM_SECONDS 10
#define EVER ;;

#define assertsyscall(x, y) if(!((x) y)){int err = errno; \
	fprintf(stderr, "In file %s at line %d: ", __FILE__, __LINE__); \
		perror(#x); exit(err);}

#ifdef DEBUG
#   define dmess(a) cout << "in " << __FILE__ << \
	" at " << __LINE__ << " " << a << endl;

#   define dprint(a) cout << "in " << __FILE__ << \
	" at " << __LINE__ << " " << (#a) << " = " << a << endl;

#   define dprintt(a,b) cout << "in " << __FILE__ << \
	" at " << __LINE__ << " " << a << " " << (#b) << " = " \
	<< b << endl
#else
#   define dmess(a)
#   define dprint(a)
#   define dprintt(a,b)
#endif

// positions of read/write file descriptors
// `int fd[2]` passed to pipe()
#define READ 0
#define WRITE 1

using namespace std;

// http://man7.org/linux/man-pages/man7/signal-safety.7.html

#define WRITES(a) { const char* foo = a; write(1, foo, strlen(foo)); }
#define WRITEI(a) { char buf[10]; assert(eye2eh(a, buf, 10, 10) != -1); WRITES(buf); }

enum STATE { NEW, RUNNING, WAITING, READY, TERMINATED };

struct PCB {
	STATE state;
	// Added toParent/toChild file descriptors	
	// To hold pipe fd information
	int toParent[2];	// pipe to parent
	int toChild[2];		// pipe to child
	const char* name;	// name of the executable
	int pid;			// process id from fork();
	int ppid;			// parent process id
	int interrupts;		// number of times interrupted
	int switches;		// may be < interrupts
	int started;		// the time this process started
	int cputime; 		// Total time in cpu
};

PCB* running;
PCB* idle;

struct sigaction* tick;
struct sigaction* child;
// Added this action struct to hold information about
// the callback registered for SIGTRAP
struct sigaction* check;
// Added this to hold pid of main process that reacts to signals
int schedulerPid;
int send_signals_pid;

// http://www.cplusplus.com/reference/list/list/
list<PCB*> new_list;
list<PCB*> processes;

int sys_time;

/*
** Async-safe integer to a string. i is assumed to be positive. The number
** of characters converted is returned; -1 will be returned if bufsize is
** less than one or if the string isn't long enough to hold the entire
** number. Numbers are right justified. The base must be between 2 and 16;
** otherwise the string is filled with spaces and -1 is returned.
*/
int eye2eh(int i, char* buf, int bufsize, int base) {
	if (i == 0) {
		for (int k = 0; k < bufsize - 2; k++) {
			buf[k] = ' ';
		}
		buf[bufsize-2] = '0';
		buf[bufsize-1] = '\0';
		return 1;
	}

	if (bufsize < 1) { return(-1); }
	buf[bufsize-1] = '\0';
	if (bufsize == 1) { return(0); }
	if (base < 2 || base > 16) {
		for(int j = bufsize-2; j >= 0; j--) {
			buf[j] = ' ';
		}
		return(-1);
	}
	
	int count = 0;
	const char* digits = "0123456789ABCDEF";
	for (int j = bufsize-2; j >= 0; j--) {
		if (i == 0) {
			buf[j] = ' ';
		} else {
			buf[j] = digits[i%base];
			i = i/base;
			count++;
		}
	}
	if (i != 0) { return(-1); }
	return(count);
}

/*
** a signal handler for those signals delivered to this process, but
** not already handled.
*/
void grab(int signum) { WRITEI(signum); WRITES("\n"); }

// c++decl> declare ISV as array 32 of pointer to function(int) returning void
void(*ISV[32])(int) = {
/*		00		01		02		03		04	05	06	07	08	09 */
/*  0 */grab, 	grab, 	grab, 	grab, grab, grab, grab, grab, grab, grab,
/* 10 */grab, 	grab, 	grab, 	grab, grab, grab, grab, grab, grab, grab,
/* 20 */grab, 	grab, 	grab, 	grab, grab, grab, grab, grab, grab, grab,
/* 30 */grab,	grab
};

/*
** stop the running process and index into the ISV to call the ISR
*/
void ISR(int signum) {
	if(signum != SIGCHLD) {
		if(kill(running->pid, SIGSTOP) == -1) {
			WRITES("In ISR kill returned: ");
			WRITEI(errno);
			WRITES("\n");
			return;
		}

		WRITES("In ISR stopped: ");
		WRITEI(running->pid);
		WRITES("\n");
		running->state = READY;
	}

	ISV[signum](signum);
}

/*
** an overloaded output operator that prints a PCB
*/
ostream& operator <<(ostream &os, struct PCB* pcb) {
	os << "state:       " << pcb->state << endl;
	os << "name:        " << pcb->name << endl;
	os << "pid:         " << pcb->pid << endl;
	os << "ppid:        " << pcb->ppid << endl;
	os << "interrupts:  " << pcb->interrupts << endl;
	os << "switches:    " << pcb->switches << endl;
	os << "started:     " << pcb->started << endl;
	os << "time in cpu: " << pcb->cputime << endl;
	return(os);
}

/*
** an overloaded output operator that prints a list of PCBs
*/
ostream& operator <<(ostream &os, list<PCB*> which) {
	list<PCB*>::iterator PCB_iter;
	for(PCB_iter = which.begin(); PCB_iter != which.end(); PCB_iter++) {
		os <<(*PCB_iter);
	}
	return(os);
}

/*
**  send signal to process pid every interval for number of times.
*/
void send_signals(int signal, int pid, int interval, int number) {
	dprintt("at beginning of send_signals", getpid());

	for(int i = 1; i <= number; i++) {
		WRITEI(i);
		WRITES("\n");
		assertsyscall(sleep(interval), == 0);
		dprintt("sending", signal);
		dprintt("to", pid);
		assertsyscall(kill(pid, signal), == 0)
	}

	WRITES("at end of send_signals\n");
}

struct sigaction* create_handler(int signum, void(*handler)(int)) {
	struct sigaction* action = new(struct sigaction);

	action->sa_handler = handler;

/*
**  SA_NOCLDSTOP
**  If  signum  is  SIGCHLD, do not receive notification when
**  child processes stop(i.e., when child processes  receive
**  one of SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU).
*/
	if(signum == SIGCHLD) {
		action->sa_flags = SA_NOCLDSTOP | SA_RESTART;
	} else {
		action->sa_flags =  SA_RESTART;
	}

	sigemptyset(&(action->sa_mask));
	assert(sigaction(signum, action, NULL) == 0);
	return(action);
}

int firstProcessWithState(STATE state) {
	list<PCB*>::iterator it = processes.begin();
	for (int i = 0; i < processes.size(); i++) {
		PCB* item = *it;
		if (item->state == state) { return i; }
		++it;
	}
	return -1;
}

PCB* getProcessByIndex(int index) {
	if (index < 0 || index > processes.size()) { return NULL; }
	list<PCB*>::iterator it = processes.begin();
	advance(it, index);
	return *it;
}

PCB* getProcessByPID(int pid) {
	list<PCB*>::iterator it = processes.begin();
	for (int i = 0; i < processes.size(); i++) {
		PCB* item = *it;
		if (item->pid == pid) { return item; }
		++it;	
	}
	return NULL;
}

void moveProcessToBack(int index) {
	if (index < 0 || index > processes.size()) { return; }
	list<PCB*>::iterator it = processes.begin();
	advance(it, index);
	PCB* item = *it;
	
	processes.erase(it);
	processes.push_back(item);
}




void scheduler(int signum) {
	WRITES("---- entering scheduler\n");
	assert(signum == SIGALRM);
	sys_time++;

	running->interrupts++;
	running->state = READY;

	PCB* tocont = idle;

	int anyNew = firstProcessWithState(NEW);
	int anyReady = firstProcessWithState(READY);

	if (anyNew >= 0) {
		// Initialize new process
		tocont = getProcessByIndex(anyNew);
		if ((tocont->pid = fork()) == 0) {
			char arg1[32]; snprintf(arg1, 32, "%d", tocont->toParent[READ]);
			char arg2[32]; snprintf(arg2, 32, "%d", tocont->toParent[WRITE]);
			char arg3[32]; snprintf(arg3, 32, "%d", tocont->toChild[READ]);
			char arg4[32]; snprintf(arg4, 32, "%d", tocont->toChild[WRITE]);

			// executes the other program
			// We pass in the #'s for the file descriptors
			// This is the only way we could figure out to enable
			// multiple child processes to work with their OWN pipes
			// instead of trying to access the same pipes as other children.
			//
			// For whatever reason
			// 		we need to provide the entire ARGV to execl
			// execl(path/to/binary, binary, [args,] NULL)
			// 		instead of just a single string
			execl(tocont->name, tocont->name, arg1, arg2, arg3, arg4, NULL);
			
			// Close pipes that will be unused in this process
			// after having forked the child
			assertsyscall(close(tocont->toParent[WRITE]), == 0);
			assertsyscall(close(tocont->toChild[READ]), == 0);
			
			//execl("/bin/sh", "sh", "-c", command, (char*)0)
			perror("execl");
		}
		tocont->started = sys_time;
		tocont->state = RUNNING;
	} else if (anyReady >= 0) {
		tocont = getProcessByIndex(anyReady);
		moveProcessToBack(anyReady);
	} else {
		// No processes to actually run.
		// WE DONE, or we just idle (default value of tocont)
	}

	tocont->cputime++;
	if (running != tocont) {
        /*
            This is done in ISV, though I don't think there's any harm in
            stopping an already-stopped process.
		kill(running->pid, SIGSTOP);
		WRITES("\nPausing ");
		WRITEI(running->pid);
		WRITES("/");
		WRITES(running->name);
		WRITES("\n");
        */
		running->switches++;
	}
	
	WRITES("continuing");
	WRITEI(tocont->pid);
	WRITES("/");
	WRITES(tocont->name);
	WRITES("\n");

	tocont->state = RUNNING;
	running = tocont;
	if(kill(tocont->pid, SIGCONT) == -1) {
		WRITES("in scheduler kill error: ");
		WRITEI(errno);
		WRITES("\n");
		return;
	}
	WRITES("---- leaving scheduler\n");
}

void process_done(int signum) {
	assert(signum == SIGCHLD);
	WRITES("---- entering process_done\n");

	// might have multiple children done.
	for(;;) {
		int status, cpid;

		// we know we received a SIGCHLD so don't wait.
		cpid = waitpid(-1, &status, WNOHANG);

		if(cpid < 0) {
			WRITES("cpid < 0\n");
			assertsyscall(kill(0, SIGTERM), != 0);
		} else if(cpid == 0) {
			// no more children.
			break;
		} else {
			PCB* process = getProcessByPID(cpid);
			if (process == NULL) {
				// If we do not have a PCB entry for the PID
				// Then it is a 'system' process
				// And we are done.
				// Kill any other processes in the group.
				WRITES("DONE\n");
				if (kill(0, SIGTERM) == -1) {
					WRITES("in process_done kill error: ");
					WRITEI(errno);
					WRITES("\n");
					return;
				}
			} else {

				WRITES("process exited: ");
				WRITEI(cpid);

				WRITES("\nProcess Name:             ");
				WRITES(process->name);

				WRITES("\nInteruptions:             ");
				WRITEI(process->interrupts);

				WRITES("\nSwitches:                 ");
				WRITEI(process->switches);

				WRITES("\nTotal time to completion: ");
				WRITEI(sys_time - process->started);

				WRITES("\nTotal cpu time:           ");
				WRITEI(process->cputime);

				WRITES("\n");
				process->state = TERMINATED;
				running = idle;
				
				if (kill(idle->pid, SIGCONT) == -1) {
					WRITES("in process_done kill error: ");
					WRITEI(errno);
					WRITES("\n");
					return;
				}
			}

		}
	}

	WRITES("---- leaving process_done\n");
}

// Executed in signalhandler for SIGTRAP
void checkPipe(int signum) {
	WRITES("---- entering checkPipe\n");
	
	// Grab a list iterator to traverse all processes
	// We need to poll all pipes
	// to see which child process requested a service
	list<PCB*>::iterator it = processes.begin();
	for (int i = 0; i < processes.size(); i++) {
		// Get the process entry
		PCB* item = *it;
		// Aliases for pipes
		int* toParent = item->toParent;
		int* toChild = item->toChild;
		// Buffer to hold info from pipe
		char buf[1];
		// Try to read from pipe
		// If nothing is available, instantly finishes
		// And comes back as zero.
		int numRead = read(toParent[READ], buf, 1);

		// -1 means error
		if (numRead <= -1) {
			WRITES("FAILURE to read from pipe ");
			WRITEI(toParent[READ]);
			WRITES(" errno is ");
			WRITEI(errno);
			WRITES("\n");
		} else if (numRead == 0) {
			// Nothing to read
		} else {
			// Read a byte from pipe
			// That holds the code of service requested
			int code = buf[0];
			WRITES("Read code ");
			WRITEI(code);
			WRITES("\n");
			// stringstream
			// so we can make use of the (ostream << PCB) operator
			// to return information more easily.
			stringstream ss;

			if (code == 1) {
				// Request for sys_time
				WRITES("Sending sys_time\n");
				ss << sys_time;
				// Convert to c-string so we can write() it
				const char* cstr = ss.str().c_str();
				write(toChild[WRITE], cstr, strlen(cstr));

			} else if (code == 2) {
				// Request for PCB info
				WRITES("Sending calling process info\n");
				ss << item;
				const char* cstr = ss.str().c_str();
				// Convert to c-string so we can write() it
				write(toChild[WRITE], cstr, strlen(cstr));
			} else if (code == 3) {
				WRITES("Sending process list\n");
				// Request for entire PCB list
				// Iterate entire list again
				list<PCB*>::iterator it2 = processes.begin();
				for (int k = 0; k < processes.size(); k++) {
					ss << "process " << k << ":\n";
					PCB* item2 = *it2;
					ss << item2;
					it2++;
				}

				const char* cstr = ss.str().c_str();
				// Convert to c-string so we can write() it
				write(toChild[WRITE], cstr, strlen(cstr));
			} else if (code == 4) {
				// Request to print something out to parent's stdout
				// Can change this buffersize down to 4 to demonstrate that
				// this will not have issues with really large requests.
				int BUFSIZE = 32;
				char buf[BUFSIZE+1];
				buf[BUFSIZE] = '\0';
				
				int numRead2;
				// Read rest of input from pipe
				while ((numRead2 = read(toParent[READ], buf, BUFSIZE)) > 0) {
					// And just redirect output to stdout
					WRITES(buf);
					// Reset the buffer between each read
					memset(buf, 0, BUFSIZE);
				}
				// Make sure to flush when done.
				WRITES("\n");
				
			}

		}
		// Move iterator forward to next PCB entry.
		++it;	
	}

	WRITES("---- leaving checkPipe\n");

}

/*
** set up the "hardware"
*/
void boot() {
	sys_time = 0;
	
	ISV[SIGALRM] = scheduler;
	ISV[SIGCHLD] = process_done;
	// Inserts checkPipe into ISV matrix
	ISV[SIGTRAP] = checkPipe;
	tick = create_handler(SIGALRM, ISR);
	child = create_handler(SIGCHLD, ISR);
	// and creates/registers handler for SIGTRAP
	check = create_handler(SIGTRAP, ISR);


	// start up clock interrupt
	int ret;
	assertsyscall((ret = fork()), >= 0);
	if (ret == 0) {
		send_signals(SIGALRM, getppid(), 1, NUM_SECONDS);
		exit(0);
	}
	else {
		send_signals_pid = ret;
	}
}

void create_idle() {
	idle = new(PCB);
	idle->state = READY;
	idle->name = "IDLE";
	idle->ppid = getpid();
	idle->interrupts = 0;
	idle->switches = 0;
	idle->started = sys_time;
	idle->cputime = 0;

	if ((idle->pid = fork()) == 0) {
		pause();
		perror("pause in create_idle");
	}
}

void initProcessList(int argc, char** argv) {
	//printf("size of processes: %d\n", (int)processes.size());
	int ppid = getpid();

	for (int i = 1; i < argc; i++) {

		PCB* pcb = new PCB();
		
		pcb->state = NEW;
		// Add pipes to PCB
		assertsyscall(pipe(pcb->toParent), == 0);
		assertsyscall(pipe(pcb->toChild), == 0);
		pcb->ppid = ppid;
		pcb->pid = -1;
		pcb->started = -1;
		pcb->interrupts = 0;
		pcb->switches = 0;
		pcb->cputime = 0;
		pcb->name = argv[i];

		//printf("Set process #%d as [%s]\n", i, pcb->name);
		processes.push_back(pcb);

	}

}

int main(int argc, char** argv) {
	schedulerPid = getpid();
	printf("PID to target for scheduler interupts: %d\n", schedulerPid);
	for (int i = 0; i < argc; i++) {
		//printf("Arg %d %s\n", i, argv[i]);
	}

	initProcessList(argc, argv);

	//printf("EINVAL is %d\n", EINVAL);
	//printf("EPERM  is %d\n", EPERM);
	//printf("ESRCH  is %d\n", ESRCH);
	
	list<PCB*>::iterator it = processes.begin();
	for (int i = 0; i < processes.size(); i++) {
		
		PCB* item = *it;
		//const char* str = item->name;
		printf("Process to run #%d has name %s\n", i, item->name);

		++it;
	}

	boot();
	create_idle();
	running = idle;
	cout << running;

	printf("\nWaiting for everything to end, on process %d\n", getpid());
	int s;
	assertsyscall(waitpid(send_signals_pid, &s, 0), == send_signals_pid);
	delete(tick);
	// Deletes created handler
	delete(check);
	delete(child);
	delete(idle);
	kill(0, SIGTERM);

}
