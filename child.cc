
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

int request(int toParent[], char code, char* msg) {
    char message[1];
    message[0] = code;

    write(toParent[WRITE], message, 1);
    if (msg != NULL) {
        write(toParent[WRITE], msg, strlen(msg));
    }

    kill(getppid(), SIGTRAP);
}
#define BUFFSIZE 1024
// Child program
int main(int argc, char** argv) {
    int toParent[2];
    int toChild[2];
    printf("child process.----------=-=-=-=------\n");

    // Just some housekeeping, get/print pids
    pid_t parent = getppid();
    pid_t self = getpid();

    printf("child pid %d\n", self);
    printf("parent pid %d\n", parent);
    
    // we pass in information to this program from the one invoking it
    //* // print args
    printf("argc: %d\n", argc);
    printf("args: \n");
    for (int i = 0; i < argc; i++) {
        printf("arg %d: %s\n", i, argv[i]);
    }
    //*/

    // Grab filedescriptors from args
    toParent[READ] = strtol(argv[1], NULL, 10);
    toParent[WRITE] = strtol(argv[2], NULL, 10);
    toChild[READ] = strtol(argv[3], NULL, 10);
    toChild[WRITE] = strtol(argv[4], NULL, 10);

    // Buffer to read input into
    char buffer[BUFFSIZE];

    // close unused pipe FDs
    assertsyscall(close(toParent[READ]), == 0);
    assertsyscall(close(toChild[WRITE]), == 0);

    // Ask parent for current systime
    request(toParent, 0x01, NULL);
    // Wait for parent to fill request
    while (read(toChild[READ], buffer, BUFFSIZE) == 0) {}
    printf("got sys_time [%s]\n", buffer);
    // Reset buffer
    memset(buffer, 0, BUFFSIZE);

    // Ask parent for current process info
    request(toParent, 0x02, NULL);
    // Wait for parent to fill request
    while (read(toChild[READ], buffer, BUFFSIZE) == 0) {}
    printf("got process info:---------\n%s---------------------------\n", buffer);
    // Reset buffer
    memset(buffer, 0, BUFFSIZE);

    // Ask parent for process list
    request(toParent, 0x03, NULL);
    // Wait for parent to fill request
    while (read(toChild[READ], buffer, BUFFSIZE) == 0) {}
    printf("got process List:=========\n%s===========================\n", buffer);
    // Reset buffer
    memset(buffer, 0, BUFFSIZE);

    // Ask parent for current systime
    request(toParent, 0x01, NULL);
    // Wait for parent to fill request
    while (read(toChild[READ], buffer, BUFFSIZE) == 0) {}
    printf("got sys_time [%s]\n", buffer);
    // Reset buffer
    memset(buffer, 0, BUFFSIZE);

    // Tell parent to print something to stdout
    request(toParent, 0x04, "hello world");
    
    
    return 0;
}