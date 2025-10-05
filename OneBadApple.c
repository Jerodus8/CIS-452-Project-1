#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#define MAXMSG 512

typedef struct {
    int dest;           // destination node, -1 means "empty"
    int len;            // length of valid bytes in text
    char text[MAXMSG];  // message text (not necessarily null-terminated)
} Message;

static pid_t *childPids = NULL;
static int childCount = 0;
static volatile sig_atomic_t sigintReceived = 0;
int dataReadFd;
int dataWriteFd;
int tokenReadFd;
int tokenWriteFd;
int (*dataPipe)[2];
int (*tokenPipe)[2];

void handleSigInt(int sig) {
    printf("\n[parent] SIGINT received: shutting down children and exiting...\n");
    // send SIGTERM to children
    for (int i = 0; i < childCount; ++i) {
        kill(childPids[i], SIGTERM);
    }
    // Close our fds to cause children to eventually exit if they're blocked
    close(dataReadFd);
    close(dataWriteFd);
    close(tokenReadFd);
    close(tokenWriteFd);

    // Wait for children
    for (int i = 0; i < childCount; ++i) {
        waitpid(childPids[i], NULL, 0);
    }
    printf("[parent] all children terminated. exiting.\n");
    fflush(stdout);
    free(dataPipe);
    free(tokenPipe);
    free(childPids);
    exit(0);
}

ssize_t readn(int fd, void *buf, size_t n) {
    size_t left = n;
    char *p = buf;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break; // EOF
        left -= r;
        p += r;
    }
    return (ssize_t)(n - left);
}

ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n;
    const char *p = buf;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= w;
        p += w;
    }
    return (ssize_t)n;
}

void childLoop(int myId, int k,
               int dataReadFd, int dataWriteFd,
               int tokenReadFd, int tokenWriteFd) {
    // Child handles SIGTERM to exit gracefully (default action is terminate).
    sigset_t emptySet;
    sigemptyset(&emptySet);

    printf("[node %d] started (pid %d)\n", myId, getpid());
    fflush(stdout);

    while (1) {
        // Wait for apple (one byte)
        unsigned char token;
        ssize_t rn = readn(tokenReadFd, &token, 1);
        if (rn <= 0) {
            if (rn == 0) {
                // upstream write end closed -> shutdown
                fprintf(stderr, "[node %d] token pipe closed, exiting\n", myId);
                break;
            } else {
                perror("[node] token read");
                break;
            }
        }

        printf("[node %d] got the apple\n", myId);
        fflush(stdout);

        // Read the message struct from dataRead
        Message msg;
        ssize_t r = readn(dataReadFd, &msg, sizeof(Message));
        if (r != sizeof(Message)) {
            if (r == 0) {
                fprintf(stderr, "[node %d] data pipe closed, exiting\n", myId);
            } else {
                perror("[node] data read");
            }
            break;
        }

        // Diagnose
        if (msg.dest == -1) {
            printf("[node %d] message header is EMPTY — nothing to consume. Forwarding.\n", myId);
        } else if (msg.dest == myId) {
            // Copy the message (print it) and empty the header
            printf("[node %d] message is FOR ME! -- received (%d bytes): \"", myId, msg.len);
            fwrite(msg.text, 1, msg.len, stdout);
            printf("\"\n");
            // "Consume" message: set header empty
            msg.dest = -1;
            msg.len = 0;
            memset(msg.text, 0, sizeof(msg.text));
        } else {
            // Not for me — forward it
            printf("[node %d] message for node %d — forwarding.\n", myId, msg.dest);
        }
        fflush(stdout);

        // Write the (possibly modified) message to next node
        if (writen(dataWriteFd, &msg, sizeof(Message)) != sizeof(Message)) {
            perror("[node] data write");
            break;
        } else {
            printf("[node %d] wrote message to next node\n", myId);
        }
        fflush(stdout);

        // Pass the apple on (write one byte)
        unsigned char pass = 1;
        if (writen(tokenWriteFd, &pass, 1) != 1) {
            perror("[node] token write");
            break;
        } else {
            printf("[node %d] passed the apple\n", myId);
        }
        fflush(stdout);

        // loop again waiting for the apple
    }

    // Clean up descriptors and exit
    close(dataReadFd);
    close(dataWriteFd);
    close(tokenReadFd);
    close(tokenWriteFd);

    printf("[node %d] exiting\n", myId);
    fflush(stdout);
    _exit(0);
}

int main(void) {
    // Parent gets k
    int k;
    printf("Enter number of nodes k (>=1): ");
    fflush(stdout);
    if (scanf("%d%*c", &k) != 1 || k < 1) {
        fprintf(stderr, "Invalid k\n");
        return 1;
    }

    // allocate arrays
    int (*dataPipe)[2] = calloc(k, sizeof(int[2]));
    int (*tokenPipe)[2] = calloc(k, sizeof(int[2]));
    if (!dataPipe || !tokenPipe) {
        perror("calloc");
        return 1;
    }

    for (int i = 0; i < k; ++i) {
        if (pipe(dataPipe[i]) < 0) { perror("pipe data"); return 1; }
        if (pipe(tokenPipe[i]) < 0) { perror("pipe token"); return 1; }
    }

    childPids = calloc(k-1 > 0 ? k-1 : 1, sizeof(pid_t));
    childCount = 0;

    // Fork k-1 children (parent will be node 0)
    for (int i = 1; i < k; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            // Child process for node i
            int myId = i;

            // Determine descriptors to keep:
            int dataReadFd  = dataPipe[(myId - 1 + k) % k][0];
            int dataWriteFd = dataPipe[myId][1];
            int tokenReadFd = tokenPipe[(myId - 1 + k) % k][0];
            int tokenWriteFd= tokenPipe[myId][1];

            // Close all other descriptors
            for (int j = 0; j < k; ++j) {
                // data pipe: keep only dataPipe[(i-1)][0] and dataPipe[i][1]
                if (dataPipe[j][0] != dataReadFd) close(dataPipe[j][0]);
                if (dataPipe[j][1] != dataWriteFd) close(dataPipe[j][1]);
                // token pipe: keep only tokenPipe[(i-1)][0] and tokenPipe[i][1]
                if (tokenPipe[j][0] != tokenReadFd) close(tokenPipe[j][0]);
                if (tokenPipe[j][1] != tokenWriteFd) close(tokenPipe[j][1]);
            }

            // Child loop
            childLoop(myId, k, dataReadFd, dataWriteFd, tokenReadFd, tokenWriteFd);
            // never returns
        } else {
            // Parent records child pid
            childPids[childCount++] = pid;
            // continue to create other children
        }
    }

    // Parent continues as node 0

    // Parent's descriptors to keep:
    int myId = 0;
    int dataReadFd  = dataPipe[(myId - 1 + k) % k][0]; // dataPipe[k-1][0]
    int dataWriteFd = dataPipe[myId][1];                // dataPipe[0][1]
    int tokenReadFd = tokenPipe[(myId - 1 + k) % k][0];
    int tokenWriteFd = tokenPipe[myId][1];
    signal(SIGINT, handleSigInt);

    printf("[parent node 0] spawned %d children; parent pid %d\n", childCount, getpid());
    fflush(stdout);

    // Install SIGINT handler for graceful shutdown
    struct sigaction sa;
    sa.sa_handler = handleSigInt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        // continue even if signal install fails
    }

    // Seed the ring with an empty message (so reads won't block FIFO-wisely)
    Message initial;
    initial.dest = -1;
    initial.len = 0;
    memset(initial.text, 0, sizeof(initial.text));

    if (writen(/* write into previous's write end to seed parent read */ dataPipe[k-1][1], &initial, sizeof(initial)) != sizeof(initial)) {
        perror("seed data write");
        // Not fatal — continue
    } else {
        printf("[parent] seeded initial empty message into ring\n");
    }
    fflush(stdout);

    // Give the apple to parent (write one token byte into the token writer for the predecessor so parent can read it)
    // The parent reads from tokenPipe[k-1][0], so write a token into tokenPipe[k-1][1].
    unsigned char seed = 1;
    if (writen(tokenPipe[k-1][1], &seed, 1) != 1) {
        perror("seed token write");
    } else {
        printf("[parent] seeded apple to start at node 0\n");
    }
    fflush(stdout);

    // Parent close unused ends
    for (int j = 0; j < k; ++j) {
        if (dataPipe[j][0] != dataReadFd) close(dataPipe[j][0]);
        if (dataPipe[j][1] != dataWriteFd) close(dataPipe[j][1]);
        if (tokenPipe[j][0] != tokenReadFd) close(tokenPipe[j][0]);
        if (tokenPipe[j][1] != tokenWriteFd) close(tokenPipe[j][1]);
    }

    // Parent main loop: behaves like other nodes except it prompts user when it holds the apple
    while (1) {
        unsigned char token;
        ssize_t rn = readn(tokenReadFd, &token, 1);
        if (rn <= 0) {
            if (rn == 0) {
                fprintf(stderr, "[parent] token pipe closed, shutting down\n");
                break;
            } else {
                perror("[parent] token read");
                break;
            }
        }

        printf("[parent] got the apple\n");
        fflush(stdout);

        Message msg;
        ssize_t r = readn(dataReadFd, &msg, sizeof(Message));
        if (r != sizeof(Message)) {
            if (r == 0) {
                fprintf(stderr, "[parent] data pipe closed, exiting\n");
            } else {
                perror("[parent] data read");
            }
            break;
        }

        // If message addressed to parent, consume and then prompt
        if (msg.dest == -1) {
            printf("[parent] message header EMPTY (nothing to consume)\n");
        } else if (msg.dest == myId) {
            printf("[parent] message FOR ME! received (%d bytes): \"", msg.len);
            fwrite(msg.text, 1, msg.len, stdout);
            printf("\"\n");
            msg.dest = -1;
            msg.len = 0;
            memset(msg.text, 0, sizeof(msg.text));
        } else {
            printf("[parent] message for node %d — will forward after prompt.\n", msg.dest);
        }
        fflush(stdout);

        // Prompt user for next message/destination
        printf("Enter message to send (empty to send nothing): ");
        fflush(stdout);

        char input[MAXMSG];
        if (!fgets(input, sizeof(input), stdin)) {
            // EOF or error on stdin -> treat as empty
            input[0] = '\0';
        }
        // Remove trailing newline if present
        size_t inlen = strlen(input);
        if (inlen > 0 && input[inlen-1] == '\n') {
            input[inlen-1] = '\0';
            inlen--;
        }

        if (inlen > 0) {
            int dest;
            printf("Enter destination node id (0 .. %d): ", k-1);
            fflush(stdout);
            if (scanf("%d%*c", &dest) != 1 || dest < 0 || dest >= k) {
                printf("Invalid destination. Message not sent.\n");
            } else {
                // populate msg with user message
                msg.dest = dest;
                msg.len = (int) (inlen < MAXMSG ? inlen : MAXMSG);
                memcpy(msg.text, input, msg.len);
                printf("[parent] queued message to node %d: \"%.*s\"\n", dest, msg.len, msg.text);
            }
        } else {
            printf("[parent] no message entered; leaving header empty.\n");
        }
        fflush(stdout);

        // Write the (possibly updated) message back into ring
        if (writen(dataWriteFd, &msg, sizeof(Message)) != sizeof(Message)) {
            perror("[parent] data write");
            break;
        } else {
            printf("[parent] wrote message to next node\n");
        }
        fflush(stdout);

        // Pass the apple on
        unsigned char pass = 1;
        if (writen(tokenWriteFd, &pass, 1) != 1) {
            perror("[parent] token write");
            break;
        } else {
            printf("[parent] passed the apple\n");
        }
        fflush(stdout);
    }

    // Cleanup (if we reach here)
    close(dataReadFd);
    close(dataWriteFd);
    close(tokenReadFd);
    close(tokenWriteFd);

    for (int i = 0; i < childCount; ++i) {
        kill(childPids[i], SIGTERM);
    }
    for (int i = 0; i < childCount; ++i) waitpid(childPids[i], NULL, 0);

    free(dataPipe);
    free(tokenPipe);
    free(childPids);

    return 0;
}