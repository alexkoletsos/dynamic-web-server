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
 * FILE pointers for mdb-lookup-server connection.
 *
 * It's just easier to define these as global variables, so we don't have to
 * pass them around everywhere.
 */
static FILE *mdb_w, *mdb_r;

/*
 * Connect with mdb-lookup-server, where output is line-buffered.
 */
void mdb_connect(const char *mdb_host, const char *mdb_port)
{
    struct addrinfo hints, *info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // Only accept IPv4 addresses
    hints.ai_socktype = SOCK_STREAM; // stream socket for TCP connections
    hints.ai_protocol = IPPROTO_TCP; // TCP protocol

    int addr_err;
    if ((addr_err = getaddrinfo(mdb_host, mdb_port, &hints, &info)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(addr_err));
        exit(1);
    }

    int mdb_fd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (mdb_fd < 0)
        die("socket");

    if (connect(mdb_fd, info->ai_addr, info->ai_addrlen) < 0)
        die("connect");

    freeaddrinfo(info);

    mdb_w = fdopen(mdb_fd, "wb");
    mdb_r = fdopen(dup(mdb_fd), "rb");

    setlinebuf(mdb_w);
}

/*
 * Disconnect from mdb-lookup-server.
 */
void mdb_disconnect(void)
{
    fclose(mdb_w);
    fclose(mdb_r);
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

static int handle_mdb_request(const char *request_uri, FILE *clnt_w)
{
    int status_code = 200;

    const char *form = "<html><body>\n"
                       "<h1>mdb-lookup</h1>\n"
                       "<p>\n"
                       "<form method=GET action=/mdb-lookup>\n"
                       "lookup: <input type=text name=key>\n"
                       "<input type=submit>\n"
                       "</form>\n"
                       "<p>\n",
               *table_header = "<p><table border>\n",
               *table_row = "<tr><td>\n",
               *table_row_alt = "<tr><td bgcolor=yellow>\n",
               *table_footer = "</table>\n",
               *form_end = "</body></html>\n",
               *key_uri = "/mdb-lookup?key=";

    /*
     * Just send the HTML form if URI doesn't specify key.
     */

    if (strncmp(request_uri, key_uri, strlen(key_uri)) != 0) {

        if (send_status_line(clnt_w, status_code) < 0 || send_blank_line(clnt_w) < 0) {
            perror("send");
            goto out;
        }

        if (fprintf(clnt_w, "%s%s", form, form_end) < 0)
            perror("send");

        goto out;
    }

    /*
     * Perform the lookup using specified key in "/mdb-lookup?key=".
     */

    const char *key = request_uri + strlen(key_uri);

    if (fprintf(mdb_w, "%s\n", key) < 0) {
        perror("mdb-lookup-server send");

        status_code = 500; // "Internal Server Error"
        if (send_error_status(clnt_w, status_code) < 0)
            perror("send");

        goto out;
    }

    /*
     * At this point, we are ready to read mdb-lookup-server's response.
     *
     * We will first send our HTTP client the response line + mdb-lookup form,
     * before we send it a dynamic webpage constructed using the results from
     * mdb-lookup-server.
     */

    // Send the response status followed by zero headers.
    if (send_status_line(clnt_w, status_code) < 0 || send_blank_line(clnt_w) < 0) {
        perror("send");
        goto out;
    }

    // Begin dynamic HTML page with the static form followed by a table header.
    if (fprintf(clnt_w, "%s%s", form, table_header) < 0) {
        perror("send");
        goto out;
    }

    /*
     * Read lines from mdb-lookup-server and send them to the client,
     * formatted as rows of an HTML table.
     */

    int row = 1;
    for (;;) {
        // 40 chars + formatting should be less than 128
        char line[128];

        // Read a line from mdb-lookup-server.
        if (fgets(line, sizeof(line), mdb_r) == NULL) {
            perror("mdb-lookup-server recv");
            // We already told the client this is a 200 OK, but we don't have
            // the rest of the results.  Nothing we can do about that, but let's
            // at least make sure that we at least send the client a complete
            // HTML page.
            goto close_table;
        }

        // Blank line indicates the end of results; break out of loop.
        if (strcmp("\n", line) == 0)
            goto close_table;

        // format the line as a HTML table row
        if (fprintf(clnt_w, "%s%s", row++ % 2 ? table_row : table_row_alt, line) < 0) {
            perror("send");
            goto out;
        }
    }

close_table:

    // End the table and close the HTML page.
    if (fprintf(clnt_w, "%s%s", table_footer, form_end) < 0)
        perror("send");

out:
    return status_code;
}

void handle_client(const char *web_root, int clnt_fd, const char *clnt_ip)
{
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

    if (strcmp(request_uri, "/mdb-lookup") == 0 || strncmp(request_uri, "/mdb-lookup?", 12) == 0)
        // mdb-lookup request
        status_code = handle_mdb_request(request_uri, clnt_w);
    else
        // Static file request
        status_code = handle_file_request(web_root, request_uri, clnt_w);

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
        fprintf(stderr, "usage: %s "
                        " <server-port>"
                        " <web-root>"
                        " <mdb-host>"
                        " <mdb-port>\n",
            argv[0]);
        exit(1);
    }

    char *serv_port = argv[1];
    char *web_root = argv[2];
    char *mdb_host = argv[3];
    char *mdb_port = argv[4];

    /*
     * Connect with mdb-lookup-server.
     */

    mdb_connect(mdb_host, mdb_port);

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

        char clnt_ip[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &clnt_addr.sin_addr, clnt_ip, sizeof(clnt_ip))
            == NULL)
            die("inet_ntop");

        handle_client(web_root, clnt_fd, clnt_ip);
    }

    /*
     * UNREACHABLE
     */

    close(serv_fd);
    mdb_disconnect();

    return 0;
}
