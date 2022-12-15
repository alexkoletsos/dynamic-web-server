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
#include <time.h>
#include <unistd.h>

#define MAXPENDING 5          // Maximum outstanding connection requests
#define MAX_LINE_LENGTH 1024  // Maximum line length for request and headers
#define DISK_IO_BUF_SIZE 4096 // Size of buffer for reading and sending files

static void die(const char *message)
{
    perror(message);
    exit(1);
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */
static struct {
    int status;
    char *reason;
} http_status_codes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *get_reason_phrase(int status_code)
{
    int i = -1;
    while (http_status_codes[++i].status > 0)
        if (http_status_codes[i].status == status_code)
            return http_status_codes[i].reason;
    return "Unknown Status Code";
}

/*
 * Send HTTP status line.
 *
 * Returns negative if send() failed.
 */
static int send_status_line(FILE *fp, int status_code)
{
    const char *reason_phrase = get_reason_phrase(status_code);
    return fprintf(fp, "HTTP/1.0 %d %s\r\n", status_code, reason_phrase);
}

/*
 * Send blank line.
 *
 * Returns number of bytes sent; returns negative if failed.
 */
static int send_blank_line(FILE *fp)
{
    return fprintf(fp, "\r\n");
}

/*
 * Send a generic HTTP response for error statuses (400+).
 *
 * Returns negative if failed.
 */
static int send_error_status(FILE *fp, int status_code)
{
    if (send_status_line(fp, status_code) < 0)
        return -1;
    // no headers needed
    if (send_blank_line(fp) < 0)
        return -1;

    return fprintf(fp,
        "<html><body>\n"
        "<h1>%d %s</h1>\n"
        "</body></html>\n",
        status_code, get_reason_phrase(status_code));
}

/*
 * Send 301 status: redirect the browser to request_uri with '/' appended to it.
 *
 * Returns negative if failed.
 */
static int send301(const char *request_uri, FILE *fp)
{
    if (send_status_line(fp, 301) < 0)
        return -1;

    // Send Location header and format redirection link in HTML in case browser
    // doesn't automatically redirect.
    return fprintf(fp,
        "Location: %s/\r\n"
        "\r\n"
        "<html><body>\n"
        "<h1>301 Moved Permanently</h1>\n"
        "<p>The document has moved "
        "<a href=\"%s/\">here</a>.</p>\n"
        "</body></html>\n",
        request_uri, request_uri);
}

/*
 * Handle static file requests.
 * Returns the HTTP status code that was sent to the browser.
 *
 * If send() ever fails (i.e., could not write to clnt_w), report the error and
 * move on.
 */
static int handle_file_request(const char *web_root, const char *request_uri, FILE *clnt_w)
{
    /*
     * Define variables that we will need to use before we return.
     */
 int status_code; // We'll return this value.
    FILE *fp = NULL; // We'll fclose() this at the end.

    // Set clnt_w to line-buffering so that lines are flushed immediately.
    setlinebuf(clnt_w);

    /*
     * Construct the path of the requested file from web_root and request_uri.
     */

    char file_path[PATH_MAX];

    if (strlen(web_root) + strlen(request_uri) + 12 > sizeof(file_path)) {
        // File paths can't exceed sizeof(file_path) on Linux, so just 404.
        status_code = 404; // "Not Found"
        if (send_error_status(clnt_w, status_code) < 0)
            perror("send");
        goto cleanup;
    }

    strcpy(file_path, web_root);

    // Note: since the URI definitely begins with '/', we don't need to worry
    // about appending '/' to web_root.

    strcat(file_path, request_uri);

    // If request_uri ends with '/', append "index.html".
    if (file_path[strlen(file_path) - 1] == '/')
        strcat(file_path, "index.html");

    /*
     * Open the requested file.
     */

    // See if the requested file is a directory.
    struct stat st;
    if (stat(file_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        status_code = 301; // "Moved Permanently"
        if (send301(request_uri, clnt_w) < 0)
            perror("send");
        goto cleanup;
    }

    // If unable to open the file, send "404 Not Found".
    fp = fopen(file_path, "rb");
    if (fp == NULL) {
        status_code = 404; // "Not Found"
        if (send_error_status(clnt_w, status_code) < 0)
            perror("send");
      goto cleanup;
    }

    // Otherwise, send "200 OK".
    status_code = 200; // "OK"
    if (send_status_line(clnt_w, status_code) < 0 || send_blank_line(clnt_w) < 0) {
        perror("send");
        goto cleanup;
    }

    /*
     * Send the file.
     */

    // Buffer for file contents.
    char file_buf[DISK_IO_BUF_SIZE];

    // Turn off buffering for clnt_w because we already have our own file_buf.
    if (fflush(clnt_w) < 0) {
        perror("send");
        goto cleanup;
    }
    setbuf(clnt_w, NULL);

    // Read and send file in a block at a time.
    size_t n;
    while ((n = fread(file_buf, 1, sizeof(file_buf), fp)) > 0) {
        if (fwrite(file_buf, 1, n, clnt_w) != n) {
            perror("send");
            goto cleanup;
        }
    }

    // fread() returns 0 both on EOF and on error; check if there was an error.
    if (ferror(fp))
        // Note that if we had an error, we sent the client a truncated (i.e.,
        // corrupted) file; not much we can do about that at this point since
        // we already sent the status...
        perror("fread");

cleanup:

    /*
     * close() the FILE pointer and return.
     */

    if (fp)
        fclose(fp);

    return status_code;
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

    /*
     * Parse arguments.
     */

    if (argc != 5) {
        fprintf(stderr, "usage: %s <http-port> <web-root> <mdb-host> <mdb-port>\n", argv[0]);
        exit(1);
    }
    char *http_port = argv[1];
    char *web_root = argv[2];
    //char *mdb_host = argv[3];
    char *mdb_port = argv[4];

    // Construct mdb-lookup socket here

    struct addrinfo mdb_hints, *mdb_info;

    memset(&mdb_hints, 0, sizeof(mdb_hints));
    mdb_hints.ai_family = AF_INET;       // Only accept IPv4 addresses
    mdb_hints.ai_socktype = SOCK_STREAM; // stream socket for TCP connections
    mdb_hints.ai_protocol = IPPROTO_TCP; // TCP protocol
    mdb_hints.ai_flags = AI_PASSIVE;     // Construct socket address for bind()ing

    int mdb_addr_err;
    if ((mdb_addr_err = getaddrinfo(NULL, mdb_port, &mdb_hints, &mdb_info)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(mdb_addr_err));
        exit(1);
    }   

    int mdb_fd = socket(mdb_info->ai_family, mdb_info->ai_socktype, mdb_info->ai_protocol);
    if (mdb_fd < 0)
        die("socket");

    if (connect(mdb_fd, mdb_info->ai_addr, mdb_info->ai_addrlen) < 0)
        die("connect");

    freeaddrinfo(mdb_info);
    
    // Wrap persistent socket connection in FILE pointers for reading and
    // writing
    FILE *mdb_fpr = fdopen(mdb_fd, "r");
    if(mdb_fpr == NULL)
        die("fdopen");

    FILE *mdb_fpw = fdopen(mdb_fd, "w");
    if(mdb_fpw == NULL)
        die("fdopen");

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
    if ((addr_err = getaddrinfo(NULL, http_port, &hints, &info)) != 0) {
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
        char clnt_ip[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &clnt_addr.sin_addr, clnt_ip, sizeof(clnt_ip))
            == NULL)
            die("inet_ntop");

        /*
     * Open client file descriptor as FILE pointers.
     */
    FILE *clnt_r = fdopen(clnt_fd, "rb");
    if (clnt_r == NULL)
        die("fdopen");

    FILE *clnt_w = fdopen(dup(clnt_fd), "wb");
    if (clnt_w == NULL)
        die("fdopen");

    /*
     * Let's parse the request line.
     */

    // Note: we'll use these fields at the end when we log the connection.
    int status_code;
    char *method = NULL, *request_uri = NULL, *http_version = NULL, *extra;

    char request_buf[MAX_LINE_LENGTH];

    if (fgets(request_buf, sizeof(request_buf), clnt_r) == NULL) {
        // Socket closed prematurely; there isn't much we can do
        status_code = 400; // "Bad Request"
        goto terminate_connection;
    }

    char *token_separators = "\t \r\n"; // tab, space, new line

    method = strtok(request_buf, token_separators);
    request_uri = strtok(NULL, token_separators);
    http_version = strtok(NULL, token_separators);
    extra = strtok(NULL, token_separators);

    // Note: We must not modify request_buf past this point, because method,
    // request_uri, http_version, and extra point to within request_buf.

    // Check that we have exactly three tokens in the request line.
    if (!method || !request_uri || !http_version || extra) {
        status_code = 501; // "Not Implemented"
        send_error_status(clnt_w, status_code);
        goto terminate_connection;
    }

    // We only support GET requests.
    if (strcmp(method, "GET")) {
 status_code = 501; // "Not Implemented"
        send_error_status(clnt_w, status_code);
        goto terminate_connection;
    }
// We only support GET requests.
    if (strcmp(method, "GET")) {
 status_code = 501; // "Not Implemented"
        send_error_status(clnt_w, status_code);
        goto terminate_connection;
    }

    // We only support HTTP/1.0 and HTTP/1.1.
    if (strcmp(http_version, "HTTP/1.0") && strcmp(http_version, "HTTP/1.1")) {
        status_code = 501; // "Not Implemented"
        send_error_status(clnt_w, status_code);
        goto terminate_connection;
    }

    // request_uri must begin with "/".
    if (!request_uri || *request_uri != '/') {
        status_code = 400; // "Bad Request"
        send_error_status(clnt_w, status_code);
        goto terminate_connection;
    }

    // Ensure request_uri does not contain "/../" and does not end with "/..".
    int uri_len = strlen(request_uri);
    if (uri_len >= 3) {
        char *tail = request_uri + (uri_len - 3);
        if (strcmp(tail, "/..") == 0 || strstr(request_uri, "/../") != NULL) {
            status_code = 400; // "Bad Request"
            send_error_status(clnt_w, status_code);
            goto terminate_connection;
        }
    }

    /*
     * Skip HTTP headers.
     */

    // We need another buffer for trashing the headers, because request_buf
    // still currently holds the method, request_uri, and http_version strings.
    char line_buf[MAX_LINE_LENGTH];

    while (1) {
        if (fgets(line_buf, sizeof(line_buf), clnt_r) == NULL) {
            // Socket closed prematurely; there isn't much we can do
            status_code = 400; // "Bad Request"
            goto terminate_connection;
        }

        // Check if we have reached the end of the headers, i.e., an empty line.
        if (strcmp("\r\n", line_buf) == 0 || strcmp("\n", line_buf) == 0)
            break;
    }
/*
     * We have a well-formed HTTP GET request; time to handle it.
     */
   
    //if there is a key in the request uri, mdb-lookup the key in the database
         if(strncmp(request_uri, "/mdb-lookup?key=", strlen("/mdb-lookup?key=")) == 0) {
             const char *form =
           "<html><body>\n"
           "<h1>mdb-lookup</h1>\n"
           "<p>\n"
           "<form method=GET action=/mdb-lookup>\n"
           "lookup: <input type=text name=key>\n"
           "<input type=submit>\n"
           "</form>\n"
           "<p>\n";

            char *key = request_uri + strlen("/mdb-lookup?key=");
            //write keyword to mdb_fpw
            fprintf(mdb_fpw, "%s\n", key);
            fflush(mdb_fpw);
           
            char mdb_line[1000];
            int count = 0;
            
            while(1) {
                if(fgets(mdb_line, sizeof(mdb_line), mdb_fpr) == NULL) {
                    status_code = 500;
                    send_error_status(clnt_w, status_code);
                    goto terminate_connection;
                }
                else if(count == 0) {
                    status_code = 200;
                    send_status_line(clnt_w, status_code);
                    send_blank_line(clnt_w);
                    fprintf(clnt_w, "%s", form);
                    fprintf(clnt_w, "<p><table border>\n");
                    fflush(clnt_w);
                }
                if(strcmp(mdb_line, "\n") == 0) {
                    break;
                }
                //check if row number is even or odd to determine formatting
                if(count%2 == 0) {
                    fprintf(clnt_w, "<tr><td>\n");
                    fprintf(clnt_w, "%s", mdb_line);
                    //fprintf(clnt_w, "mom");
                    fflush(clnt_w);
                    count++;
                }
                else {
                    fprintf(clnt_w, "<tr><td bgcolor=yellow>\n");
                    fprintf(clnt_w, "%s", mdb_line);
                    fflush(clnt_w);
                    count++;
                }
            }
            if(ferror(mdb_fpr)) {
                die("mdb lookup");
            }
            fprintf(clnt_w, "</table>\n</body></html>\n");
            fflush(clnt_w);

    }
    else if(strcmp(request_uri, "/mdb-lookup") == 0 || strncmp(request_uri, "/mdb-lookup?", strlen("/mdb-lookup?")) == 0) {
             const char *form =
           "<html><body>\n"
           "<h1>mdb-lookup</h1>\n"
           "<p>\n"
           "<form method=GET action=/mdb-lookup>\n"
           "lookup: <input type=text name=key>\n"
           "<input type=submit>\n"
           "</form>\n"
           "<p>\n"
           "</body></html>\n";    

            status_code = 200;
            send_status_line(clnt_w, status_code);
            send_blank_line(clnt_w);
            fprintf(clnt_w, "%s", form); 
    }
    else {
        status_code = handle_file_request(web_root, request_uri, clnt_w);
    }

terminate_connection:

    /*
     * Done with client request; close the connection and log the transaction.
     */

    // Closing can FILE pointers can also produce errors, which we log.
    if (fclose(clnt_w) < 0)
        perror("send");

    if (fclose(clnt_r) < 0)
        perror("recv");

    fprintf(stderr, "%s \"%s %s %s\" %d %s\n",
        clnt_ip,
        method,
        request_uri,
        http_version,
        status_code,
        get_reason_phrase(status_code));
    }

    /*
     * UNREACHABLE
     */

    close(serv_fd);

    return 0;
}

