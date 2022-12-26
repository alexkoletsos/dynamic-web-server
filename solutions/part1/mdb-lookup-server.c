#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mdb.h"
#include <mylist.h>

#define KeyMax 5

static void die(const char *s)
{
    perror(s);
    exit(1);
}

static void handle_client(const char *mdb_filename, int clnt_fd)
{
    /*
     * Wrap client file descriptor in FILE pointers.
     */

    FILE *clnt_r = fdopen(clnt_fd, "rb"),
         *clnt_w = fdopen(dup(clnt_fd), "wb");

    if (clnt_r == NULL || clnt_w == NULL) {
        perror("fdopen");
        goto clnt_out;
    }

    FILE *mdb_fp = fopen(mdb_filename, "rb");
    if (mdb_fp == NULL) {
        perror("fopen");
        goto clnt_out;
    }

    /*
     * Read all records into memory.
     */

    struct List list;
    initList(&list);

    struct MdbRec r;
    struct Node *node = NULL;

    while (fread(&r, sizeof(r), 1, mdb_fp) == 1) {
        struct MdbRec *rec = (struct MdbRec *)malloc(sizeof(r));
        if (!rec)
            die("malloc");

        memcpy(rec, &r, sizeof(r));

        node = addAfter(&list, node, rec);
        if (node == NULL)
            die("addAfter");
    }

    fclose(mdb_fp);

    char line[1024];
    char key[KeyMax + 1];

    while (fgets(line, sizeof(line), clnt_r) != NULL) {

        /*
         * Clean up user input.
         */

        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';

        // If newline is within the first KeyMax characters, remove it.
        int last = strlen(key) - 1;
        if (key[last] == '\n')
            key[last] = '\0';

        // Do the same with carriage return.
        last = strlen(key) - 1;
        if (last >= 0 && key[last] == '\r')
            key[last] = '\0';

        // Make sure we've encountered a newline with fgets().
        while (line[strlen(line) - 1] != '\n' && fgets(line, sizeof(line), clnt_r))
            ;

        /*
         * Perform search with key.
         */

        int recNo = 1;

        for (struct Node *node = list.head; node; node = node->next, recNo++) {
            struct MdbRec *rec = (struct MdbRec *)node->data;

            if (strstr(rec->name, key) || strstr(rec->msg, key)) {
                if (fprintf(clnt_w, "%4d: {%s} said {%s}\n", recNo, rec->name, rec->msg) < 0) {
                    perror("send");
                    goto clnt_out;
                }
            }
        }

        fputs("\n", clnt_w);

        if (fflush(clnt_w) < 0) {
            perror("send");
            goto clnt_out;
        }
    }

    traverseList(&list, &free);
    removeAllNodes(&list);

clnt_out:
    if (clnt_w && fclose(clnt_w) < 0)
        perror("send");
    if (clnt_r && fclose(clnt_r) < 0)
        perror("recv");
}

static void sigchld_handler(int sig)
{
    // Keep reaping dead children until there aren't any to reap.
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char **argv)
{
    /*
     * Configure signal-handling.
     */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL))
        die("sigaction(SIGPIPE)");

    // Install a handler for the SIGCHLD signal so that we can reap children
    // who have finished processing their requests.
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sa.sa_handler = &sigchld_handler;
    if (sigaction(SIGCHLD, &sa, NULL))
        die("sigaction(SIGCHLD)");

    /*
     * Parse arguments.
     */

    if (argc != 3) {
        fprintf(stderr, "usage: %s <server-port> <database>\n", argv[0]);
        exit(1);
    }

    const char *port = argv[1];
    const char *filename = argv[2];

    /*
     * Construct server socket to listen on port.
     */

    struct addrinfo hints, *info;
    int addr_err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // Only accept IPv4 addresses
    hints.ai_socktype = SOCK_STREAM; // stream socket for TCP connections
    hints.ai_protocol = IPPROTO_TCP; // TCP protocol
    hints.ai_flags = AI_PASSIVE;     // Construct socket address for bind()ing

    if ((addr_err = getaddrinfo(NULL, port, &hints, &info)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(addr_err));
        exit(1);
    }

    int serv_fd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (serv_fd < 0)
        die("socket");

    if (bind(serv_fd, info->ai_addr, info->ai_addrlen) < 0)
        die("bind");

    if (listen(serv_fd, 8) < 0)
        die("listen");

    freeaddrinfo(info);

    /*
     * Server accept() loop.
     */

    for (;;) {

        // We only need sockaddr_in since we only accept IPv4 peers.
        struct sockaddr_in clnt_addr;
        socklen_t clnt_len = sizeof(clnt_addr);

        int clnt_fd = accept(serv_fd, (struct sockaddr *)&clnt_addr, &clnt_len);
        if (clnt_fd < 0)
            die("accept");

        pid_t pid = fork();
        if (pid < 0)
            die("fork");

        if (pid > 0) {
            /*
             * Parent process:
             *
             * Close client socket and continue accept()ing connections.
             */

            close(clnt_fd);

            continue;
        }

        /*
         * Child process:
         *
         * Close server socket, handle the request, and exit.
         */

        close(serv_fd);

        char clnt_ip[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &clnt_addr.sin_addr, clnt_ip, sizeof(clnt_ip))
            == NULL)
            die("inet_ntop");

        fprintf(stderr, "Connection started: %s\n", clnt_ip);

        handle_client(filename, clnt_fd);

        fprintf(stderr, "Connection terminated: %s\n", clnt_ip);

        exit(0);
    }
}
