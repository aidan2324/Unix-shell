/*
 * Aidan Murray and Cole Feely
 * October 2021
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);
char get_redirects(char *cmdline, char **argv);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
        break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
        break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
        break;
    default:
            usage();
    }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

    /* Read command line */
    if (emit_prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
        app_error("fgets error");
    if (feof(stdin)) { /* End of file (ctrl-d) */
        fflush(stdout);
        exit(0);
    }

    /* Evaluate the command line */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
*/
void eval(char *cmdline)
{
    int bg;              //if the job should run, this would be TRUE
    pid_t pid;
    sigset_t mask, prev_mask;
    int clean = 0;
    int nstdin; // standard input
    int nstdout;  // standard output
    int nstderr;  // standard error
    int fd[2];
    int i = 0;
    int Child1;
    int Child2;
    char *argv[MAXARGS]; //arguement list
    char buf[MAXLINE];   // this holds the modified command line
    char *arg_clean[MAXARGS]; // clean arg list for redirects
    // parse the line
    strcpy(buf, cmdline);
    bg = parseline(cmdline, argv);
    //check if valid builtin_cmd
    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL || bg == -1){// Ignore empty lines
       printf("No line contents\n");
       return;
    }
    // blocking first
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (!builtin_cmd(argv)){
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);
        pid = fork();
        if (pid < 0)// error in fork
        {
            unix_error("unable to run command");
        }
        else if (pid == 0){ // if process is child
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
            setpgid(0, 0); // set child process group to its pid
	    while(argv[i+1]!=NULL){
                if(!strcmp(argv[i], "<")){ //series of ifs looking for which redirection operator is entered
		    i++;
		    fclose(fopen(argv[i],"w")); // empty file before appending
                    nstdin = open(argv[i],O_RDONLY,S_IRWXU | S_IRWXG | S_IRWXO ); //open file in read only
                    dup2(nstdin, STDIN_FILENO);//copying input to file
                    close(nstdin); 
                    continue;
                }
                else if(!strcmp(argv[i], ">")){
		    i++;
                    nstdout = open(argv[i], O_WRONLY | O_CREAT,S_IRWXU | S_IRWXG | S_IRWXO );//opening the file in write mode
                    dup2(nstdout,STDOUT_FILENO);//copying entry nstdOUT to stdout_fileno
                    close(nstdout);
                    continue;
                }
                else if(!strcmp(argv[i], ">>")){
                    i++;
                    nstdout = open(argv[++i],O_WRONLY | O_CREAT | O_APPEND,S_IRWXU | S_IRWXG | S_IRWXO);//opening the file in appendmode
                    dup2(nstdout,STDOUT_FILENO);
                    close(nstdout);
                    continue;
                }
                else if(!strcmp(argv[i], "2>")){
                    i++;
                    nstderr = open(argv[i],O_WRONLY | O_CREAT,S_IRWXU | S_IRWXG | S_IRWXO);//opening the file in appendmode
                    dup2(nstderr,STDERR_FILENO); 
                    close(nstderr);
                    continue;
                }
		if (pipe(fd) < 0){
			printf("Pipe init error\n");
			exit(0);
		}

                if(!strcmp(argv[i],"|")){ //get what is on the right side of the pipe
                    char * rP[MAXARGS];
                    int l = 0;
                    while(argv[i] !=NULL){
                        rP[l++] = argv[++i];
                    }
                    if(Child1 = fork() == 0){//init pipe 
			close(fd[0]); //close up input end of the pipe
                        dup2(fd[1],STDOUT_FILENO);
                        arg_clean[clean] = NULL;
                        if (execve(arg_clean[0], arg_clean, environ) < 0)                            {
                                printf("%s: Command not found.\n", argv[0]);
                                exit(0);
                        }
   
                    }
                    if(Child1 < 0){ //if error in fork
                         printf("Error forking.\n");
                         exit(EXIT_FAILURE);                     
		    }
                    if(Child2 = fork () == 0) // second child init
                    {
                        close(fd[1]);//close output end of the pipe
                        dup2(fd[0],STDIN_FILENO);
                        if (execve(rP[0], rP, environ) < 0)                            {
                                printf("%s: Command not found.\n", rP[0]);
                                exit(0);
                        }
		    }
                    if(Child2 < 0){ //if error in fork
                         printf("Error forking.\n");
                         exit(EXIT_FAILURE);     
                    }
                    exit(0);
                }
                arg_clean[clean++] = argv[i++];
            }
            arg_clean[clean] = NULL;
            if (execve(arg_clean[0], arg_clean, environ) < 0)
            {
                printf("%s: Command not found.\n", arg_clean[0]);
                exit(1);
            }
        }
        // parent is going to add job first
	    else {//bg = 1 backround job, bg = 0 foreground job
	      if (!bg) { //parent adds job
	        // bg = 0, foreground job
	        addjob(jobs, pid, FG, cmdline);
	        sigprocmask(SIG_SETMASK, &prev_mask, NULL); //allow parent to recieve sigchild
	        waitfg(pid);
	        return;
	      }
	      addjob(jobs, pid, BG, cmdline);
	      sigprocmask(SIG_SETMASK, &prev_mask, NULL);
	      printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); //allow parent to recieve sigchild
	      return;
        }
    }
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv)
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
    buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
    buf++;
    delim = strchr(buf, '\'');
    }
    else {
    delim = strchr(buf, ' ');
    }

    while (delim) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* ignore spaces */
           buf++;

    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }
    }
    argv[argc] = NULL;

    if (argc == 0)  /* ignore blank line */
    return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
    argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 */
int builtin_cmd(char **argv) 
{
   
    if(strcmp(argv[0], "quit") == 0) {
      // if quit command, terminates the shell and exits
      exit(0);
      return 1; // Done
    }
    else if(strcmp(argv[0], "jobs") == 0) {
	// else if jobs, list all the jobs in the background
      listjobs(jobs);
      return 1; // Done
    }
    else if((strcmp(argv[0], "bg") == 0) || (strcmp(argv[0], "fg") == 0) ) {
	// if bg command, run the do_bgfg()
      do_bgfg(argv);
      return 1; 
    }
    else {
      // else not an built in function
      return 0;     
    }
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
{
      if (argv[1] == NULL){
		// check if command has PID
	      printf("%s command requires PID or %%jobid argument\n", argv[0]);
	      return;
      }

      if(!isdigit(argv[1][0]) && argv[1][0] != '%') {  
		// check if argument has PID          
	      printf("%s: argument must be a PID or %%jobid\n", argv[0]);
	      return;
      }
	// Set a var for fg and bg
      char *fgorbg = argv[0];  
	// Set a car for pid or jid 
      char *pidojid = argv[1];  
	// var for current job  
      struct job_t *cur_job;
      pid_t cur_pid;
      
      // Check for job id
      if (atoi(pidojid) == 0){
		// Set a var for the length of thejobid
	      int len_arg = strlen(pidojid);
		// prep array to modify
	      char mod_arg[len_arg-1];   
	      
	      // Remove %
	      for (int i=0; i<len_arg-1; i++){
		      mod_arg[i] = pidojid[i+1];
	      }

	      // atoi is str->int, set job id to int
	      int possjobid = atoi(mod_arg);

	      // Check if job exists
	      cur_job = getjobjid(jobs, possjobid);
	      if (cur_job == NULL){
		      printf("%s: No such job\n", pidojid);
		      return;
	      }
	      cur_pid = cur_job->pid;
      }

      // check if process id exists
      else{
	      
	      int possible_pid = atoi(pidojid); // atoi is str->int, set possid to int

	      // check for job
	      cur_job = getjobpid(jobs, possible_pid);
	      if (cur_job == NULL){
		      printf("(%s) No such process\n",pidojid);
		      return;
	      }
	      cur_pid = cur_job->pid;
      }
      
      // run in fg
      if(strcmp(fgorbg, "fg") == 0){
	      kill(-cur_pid, SIGCONT);
	      cur_job->state = FG;
	      waitfg(cur_pid);
      }

      // run in bg
      else if (strcmp(fgorbg, "bg") == 0){
	      kill(-cur_pid, SIGCONT);
	      cur_job->state = BG;
	      printf("[%d] (%d) %s\n", cur_job->jid, cur_job->pid, cur_job->cmdline);
      }

      return;
      }
/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    struct job_t* job;
    job = getjobpid(jobs,pid);
    //check if pid is valid
    if(pid == 0){
        return;
    }
    if(job != NULL){
        //sleep
        while(pid==fgpid(jobs)){
        }
    }
    return;
}
/*

void waitfg(pid_t pid)
{
    // Put your code here. 
    // We want to run while loop indefinetly
    pid_t fg_pid;

    //unix_error("before loop");

    // get job of process in foreground
    struct job_t *fg_job = getjobpid(jobs, pid);

    // while state of job is in foreground, sleep to make other processes wait until finished
    while (fg_job->state == FG){
      sleep(1);
    }
    return;
}

*/
/*****************
 * Signal handlers
 *****************/

/*

 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig)
{
	int status;
	pid_t pid;
	// passing -1 and WNOHANG checks for any zombie children
	while ((pid=waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) {
		int jid = pid2jid(pid);
		// if statement is true when the process is stopped
		if (WIFSTOPPED(status)){
			getjobpid(jobs, pid)->state = ST;
		printf("Job [%d] (%d) Stopped by signal %d\n", jid, pid, WSTOPSIG(status));}
		// if statement true when process is terminated
		else if (WIFSIGNALED(status)){
			printf("Job [%d] (%d) terminated by signal %d\n", jid, pid, WTERMSIG(status));
		deletejob(jobs, pid);}
		// if statement true when the process is exited
		else if (WIFEXITED(status)){
		deletejob(jobs, pid);}
	}
	return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{
    pid_t pid = fgpid(jobs);

    //check for valid pid
    if (pid != 0) {
        kill(-pid, sig);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    pid_t pid = fgpid(jobs);
    //check for valid pid
    if (pid != 0) {
        kill(-pid, sig);
    }
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
        max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;
    if (pid < 1)
    return 0;

    for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == 0) {
        jobs[i].pid = pid;
        jobs[i].state = state;
        jobs[i].jid = nextjid++;
        if (nextjid > MAXJOBS)
        nextjid = 1;
        strcpy(jobs[i].cmdline, cmdline);
        if(verbose){
            printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
    }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
    return 0;

    for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == pid) {
        clearjob(&jobs[i]);
        nextjid = maxjid(jobs)+1;
        return 1;
    }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].state == FG)
        return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
    return NULL;
    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
        return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
    return NULL;
    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
        return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
    return 0;
    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs)
{
    int i;
    for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid != 0) {
        printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
        switch (jobs[i].state) {
        case BG:
            printf("Running ");
            break;
        case FG:
            printf("Foreground ");
            break;
        case ST:
            printf("Stopped ");
            break;
        default:
            printf("listjobs: Internal error: job[%d].state=%d ", 
               i, jobs[i].state);
        }
        printf("%s", jobs[i].cmdline);
    }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
//------------------------------- Assignment 5 Code ------------------------------------------
/*int get_length(char **argv){
	
	
}
*/

char get_redirects(char *cmdline, char **argv) {
  // check out parseline
  int newstdin;
  int newstdout;
  printf("getting redirects...\n");
  int i = 0;
  while (argv[i] != NULL){
  if (!strcmp(argv[i], "<")) { // stdin
    printf("<\n");
    newstdin = open("program.stats", O_RDONLY, S_IRWXU | S_IRWXG | S_IRWXO);
    close(0); // 0 is default file descriptor for stdin
    // dup2(newstin, 0);
    dup(newstdin);
    close(newstdin);
  }
  else if (!strcmp(argv[i], ">")) { // stdout
    printf("%s > %s\n", argv[i-1], argv[i+1]);
    newstdout = open("input.txt", O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
    printf("newstdout:%d\n", newstdout);
    
    close(1); // 1 is default file descriptor for stdout
    printf("1");
    dup(newstdout);
    //dup2(newstdout, 1);
    printf("2");
    close(newstdout);
    printf("end\n");
    return "c";
  }
  else if (!strcmp(argv[i], ">>")) { // stdout append
    printf(">>\n");
    newstdout = open("program.stats", O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRWXG | S_IRWXO);
    close(1); // 1 is default file descriptor for stdout
    dup(newstdout);
    // dup2(newstdout, 1);
    close(newstdout);
  } // potential error if two things tryna append to same file, delay or wait?
  else if (!strcmp(argv[i], "2>")){
    printf("2>\n");
    dup2(2,1); // close stdout and redirect stderror to stdout
  }
  i = i+1;
  }
  printf("returning\n");
  return "c";
}
