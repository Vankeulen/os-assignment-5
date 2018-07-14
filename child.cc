
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>

#define assertsyscall(x, y) if(!((x) y)){int err = errno; \
    fprintf(stderr, "In file %s at line %d: ", __FILE__, __LINE__); \
        perror(#x); exit(err);}

#define READ 0
#define WRITE 1

// Child program
int main(int argc, char** argv) {
    int toParent[2];
    int toChild[2];
    
    // we pass in information to this program from the one invoking it
    int schedulerPid = strtol(argv[1], NULL, 10); 
    printf("child process targeting pid %d\n", schedulerPid);
    toParent[READ] = strtol(argv[2], NULL, 10);
    toParent[WRITE] = strtol(argv[3], NULL, 10);
    toChild[READ] = strtol(argv[4], NULL, 10);
    toChild[WRITE] = strtol(argv[5], NULL, 10);

    // close unused pipe FDs. Crashes.
    //assertsyscall(close(toParent[READ]), == 0);
    //assertsyscall(close(toChild[WRITE]), == 0);

    char message[2];
    message[0] = 0x05;
    message[1] = '\0';
    // Write message to parent. crashes.
    assertsyscall(write(toParent[WRITE], message, strlen(message)), != -1);

    // send sigtrap to scheduler.
    kill(schedulerPid, SIGTRAP);



    return 0;
}