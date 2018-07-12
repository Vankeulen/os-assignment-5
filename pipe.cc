#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

extern int errno;

#define assertsyscall(x, y) if(!((x) y)){int err = errno; \
    fprintf(stderr, "In file %s at line %d: ", __FILE__, __LINE__); \
        perror(#x); exit(err);}

#define READ 0
#define WRITE 1

int main (int argc, char** argv) {
    int child2parent[2];
    int parent2child[2];

    assertsyscall(pipe(child2parent), == 0);
    assertsyscall(pipe(parent2child), == 0);

    /* for non-blocking calls
    int fl;
    assertsyscall((fl = fcntl(child2parent[READ], F_GETFL)), != -1);
    assertsyscall(fcntl(child2parent[READ], F_SETFL, fl | O_NONBLOCK), == 0);
    */

    if (fork() == 0) {
        // close the ends we should't use
        assertsyscall(close(child2parent[READ]), == 0);
        assertsyscall(close(parent2child[WRITE]), == 0);

        char *message = "Hello parent\n";
        assertsyscall(write(child2parent[WRITE], message, strlen(message)),
            != -1);

        char buffer[1024];
        int len;
        assertsyscall((len = read(parent2child[READ], buffer,
            sizeof (buffer))), != -1);
        buffer[len] = 0;
        assertsyscall(printf("%s", buffer), > 0);
        exit (0);
    } else {
        // close the ends we should't use
        assertsyscall(close(child2parent[WRITE]), == 0);
        assertsyscall(close(parent2child[READ]), == 0);

        char *message = "Hello child\n";
        assertsyscall(write(parent2child[WRITE], message, strlen(message)),
            != -1);

        char buffer[1024];
        int len;
        assertsyscall((len = read(child2parent[READ], buffer,
            sizeof (buffer))), != -1);
        buffer[len] = 0;
        assertsyscall(printf("%s", buffer), > 0);
        exit (0);
    }
}
