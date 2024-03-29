#define _GNU_SOURCE
#include <arpa/inet.h>
#include <linux/limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mylist.h>
#include "mdb.h"

#define MAXPENDING 5          // Maximum outstanding connection requests
#define MAX_LINE_LENGTH 1024  // Maximum line length for request and headers
#define DISK_IO_BUF_SIZE 4096 // Size of buffer for reading and sending files

static void die(const char *message)
{
    perror(message);
    exit(1);
}

int loadmdb(FILE *fp, struct List *dest)
{
     /*
      * read all records into memory
      */
  
     struct MdbRec r;
     struct Node *node = NULL;
     int count = 0;
 
     while (fread(&r, sizeof(r), 1, fp) == 1) {
 
         // allocate memory for a new record and copy into it the one
         // that was just read from the database.
         struct MdbRec *rec = (struct MdbRec *)malloc(sizeof(r));
         if (!rec)
             return -1;
 
         memcpy(rec, &r, sizeof(r));
 
         // add the record to the linked list.
         node = addAfter(dest, node, rec);
         if (node == NULL)
             return -1;
 
         count++;
     }
 
     // see if fread() produced error
     if (ferror(fp))
         return -1;
 
     return count;
 }
 
 void freemdb(struct List *list)
 {
     // free all the records
     traverseList(list, &free);
     removeAllNodes(list);
 }

static void sigchld_handler(int sig)
{
    // Keep reaping dead children until there aren't any to reap.
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int main(int argc, char *argv[])
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

    char *serv_port = argv[1];
    char *database = argv[2];

    /*
     * Construct server socket to listen on serv_port.
     */

    struct addrinfo hints, *info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // Only accept IPv4 addresses
    hints.ai_socktype = SOCK_STREAM; // stream socket for TCP connections
    hints.ai_protocol = IPPROTO_TCP; // TCP protocol
    hints.ai_flags = AI_PASSIVE;     // Construct socket address for bind()ing

    int addr_err;
    if ((addr_err = getaddrinfo(NULL, serv_port, &hints, &info)) != 0) {
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
        
        //Print connection started message
        fprintf(stderr, "Connection started: %s\n", clnt_ip);
        
        //FILE* for reading
        FILE *fpr = fdopen(clnt_fd, "r");
        if(fpr == NULL)
            die("fdopen");

        //FILE* for writing
        FILE *fpw = fdopen(clnt_fd, "w");
        if(fpw == NULL)
            die("fdopen");

        //FILE* to store database
        FILE *fp = fopen(database,"rb");
        if(fp == NULL)
            die("fopen");

        //Set fpw to line-buffering so that lines are flushed immediately
        setlinebuf(fpw);

        //mdb-lookup code
        struct List list;
        initList(&list);

        int loaded = loadmdb(fp, &list);
        if (loaded < 0)
            die("loadmdb");

        fclose(fp);

        /*
         * lookup loop
         */

        char line[1024];
        char key[6];


        while (fgets(line, sizeof(line), fpr) != NULL) {

            /*
             * clean up user input
             */

            // must null-terminate the string manually after strncpy().
            strncpy(key, line, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';

            // if newline or carriage return is within the first KeyMax characters, remove it.
            int last = strlen(key) - 1;
            if (key[last] == '\n'|| key[last] == '\r'){
                if(key[last-1] == '\r'){
                    key[last-1] = '\0';
                }
                else{
                    key[last] = '\0';
                }
            }
            
            // user might have typed more than sizeof(line) - 1 characters in line;
            // continue fgets()ing until we encounter a newline.
            while (line[strlen(line) - 1] != '\n' && fgets(line, sizeof(line), fpr))
                ;

            /*
             * search with key
             */

            // traverse the list, printing out the matching records
            struct Node *node = list.head;
            int recNo = 1;
            while (node) {
                struct MdbRec *rec = (struct MdbRec *)node->data;

                if (strstr(rec->name, key) || strstr(rec->msg, key))
                    fprintf(fpw, "%4d: {%s} said {%s}\n", recNo, rec->name, rec->msg);

                node = node->next;
                recNo++;
            }
            //print new line to separate requests
            fprintf(fpw, "\n");
        }
        freemdb(&list);

        //Send message that connection terminated
        fprintf(stderr, "Connection terminated: %s\n", clnt_ip);
        
        fclose(fpr);
        fclose(fpw);
        exit(0);
    }

    /*
     * UNREACHABLE
     */

    close(serv_fd);

    return 0;
}
