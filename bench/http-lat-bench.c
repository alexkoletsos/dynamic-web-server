/**
 *  http-lat-bench: a workout tool for your HTTP server!
 *
 *  The HTTP latency benchmarking tool measures a server's latency for serving
 *  a client.
 *
 *  This source file makes heavy use of C preprocessor macros, primarily used to
 *  configure its behavior at compile-time (these macros begin with CONFIG_).
 *  Feel free to adjust these macros to suit your benchmarking needs.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/** Value used to seed pseudo-random number generator (i.e., random()).
 *
 *  If you give the same seed to srandom(), random() will return the same
 *  pseudo-random sequence of integers.
 *
 *  We want this because we'd like the experimental setup to be unpredictable
 *  (for the server) but repeatable (for us).
 */
#define CONFIG_RANDOM_SEED 3157

/** Number of rounds to run latency benchmark. */
#define CONFIG_NUM_ROUNDS 256

/** Minimum amount of time to sleep between rounds, in microseconds. */
#define CONFIG_MIN_SLEEP_US 1000

/** Maximum amount of time to sleep between rounds, in microseconds. */
#define CONFIG_MAX_SLEEP_US 10000

/** Uncomment the following to perform HTTP requests in a predictable order. */
// #define CONFIG_URI_ROUND_ROBIN

/** Uncomment the following to include connect() time when measuring latency. */
// #define CONFIG_INCLUDE_CONNECT_TIME

// Compile-time warnings to ensure we have sane time bounds.
#if CONFIG_MIN_SLEEP_US > CONFIG_MAX_SLEEP_US
#error "CONFIG_MIN_SLEEP_US must be less than CONFIG_MAX_SLEEP_US"
#endif

// Everyone loves a 4K buffer.
#define BUF_SIZE 4096

// Convert timespec to double-precision floating point number, in nanoseconds.
#define ts2double(ts) ((double)(ts).tv_sec * 1000000000. + (double)(ts).tv_nsec)

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

int main(int argc, char **argv)
{
    /*
     * Parse arguments and determine output file name.
     */

    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> [<URIs>..]\n", *argv);
        exit(1);
    }

    char *server_name = argv[1];
    char *server_port = argv[2];

    // The rest of argv contains the URIs we want.
    char **uriv = argv + 3;
    int uric = argc - 3;

    /*
     * Seed random number generator.
     */

    srandom(CONFIG_RANDOM_SEED);

    /*
     * Obtain socket address structure from server name and port number.
     */

    struct addrinfo hints, *info;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_INET;       // Only accept IPv4 addresses
    hints.ai_socktype = SOCK_STREAM; // Stream socket for TCP connections
    hints.ai_protocol = IPPROTO_TCP; // TCP protocol

    int aerr;
    if ((aerr = getaddrinfo(server_name, server_port, &hints, &info)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(aerr));
        exit(1);
    }

    /*
     * Attempt to make CONFIG_NUM_ROUNDS connections, and log the latency each time.
     */

    // We will log the latencies in this array.
    struct timespec times[CONFIG_NUM_ROUNDS];

    // Choose the URI we will query in the first round.
#ifdef CONFIG_URI_ROUND_ROBIN
    char *uri = uriv[0];
#else
    char *uri = uriv[random() % uric];
#endif

    for (size_t round = 0; round < CONFIG_NUM_ROUNDS; round++) {

        // We'll measure the time before and after using clock_gettime(),
        // then subtract the two values to obtain the latency.
        struct timespec before, after;

        /*
         * We want to factor out client-side latency as much as we can, so we do
         * a few things first to prepare for the running the benchmark.
         */

        // Prepare the HTTP request before we start timing ourselves.
        char buf[BUF_SIZE];
        int len = snprintf(buf, sizeof(buf),
            "GET %s HTTP/1.0\r\nHost: %s:%s\r\n\r\n", uri, server_name, server_port);

        // Construct our socket before we start timing ourselves.
        int serv_fd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (serv_fd < 0)
            die("socket");

        /*
         * When we start the clock depends on whether we want to include the
         * time it takes to establish the TCP connection.
         */

        // If we're interested in measuring the time it takes to establish
        // a connection, we start measuring the time now.
#ifdef CONFIG_INCLUDE_CONNECT_TIME
        if (clock_gettime(CLOCK_MONOTONIC, &before) < 0)
            die("clock_gettime");
#endif

        // Establish the TCP connection.
        if (connect(serv_fd, info->ai_addr, info->ai_addrlen) < 0)
            // A server might reject us because we are connecting too often,
            // or just because we have flaky internet.
            //
            // To keep things simple, we will simply die() here, but a better
            // benchmarking tool might wait and retry later.
            die("connect");

        // If we're not interested in measuring the connection time, and only
        // how long it takes for the server to respond to our HTTP request,
        // then we start measuring the time now.
#ifndef CONFIG_INCLUDE_CONNECT_TIME
        if (clock_gettime(CLOCK_MONOTONIC, &before) < 0)
            die("clock_gettime");
#endif

        /*
         * At this point, TCP connection is established, and the clock is
         * ticking, so do the client side of the connection as fast as we can.
         * Don't bother with the FILE pointer stuff because that may add
         * potential latency (due to heap allocation).
         */

        // Send the request.
        send(serv_fd, buf, len, 0);

        // Don't bother parsing the response (assume it's correct);
        // just read through it as fast as possible.
        while (recv(serv_fd, buf, sizeof(buf), 0) > 0)
            ;

        // Stop the clock.
        if (clock_gettime(CLOCK_MONOTONIC, &after) < 0)
            die("clock_gettime");

        // Close the connection.
        close(serv_fd);

        /*
         * Compute the time difference between when we started and ended the
         * clock, and store that duration in the times array.
         *
         * We'll compute stats later.
         */

        times[round].tv_sec = after.tv_sec - before.tv_sec;
        times[round].tv_nsec = after.tv_nsec - before.tv_nsec;

        /*
         * Time to prepare for the next round.
         */

        // Nap a little between requests to wait for the server to "cool down"
        // (we're not testing throughput here).
        int sleep_time_us = CONFIG_MIN_SLEEP_US + random() % (CONFIG_MAX_SLEEP_US - CONFIG_MIN_SLEEP_US);
        if (usleep(sleep_time_us) < 0)
            die("usleep");

            // Choose the URI we will query in the next round.
#ifdef CONFIG_URI_ROUND_ROBIN
        uri = uriv[(round + 1) % uric];
#else
        uri = uriv[random() % uric];
#endif
    }

    // We're finally done using this thing.
    freeaddrinfo(info);

    /*
     * Time to compute some stats!
     *
     * We'll use double-precision floating point for all numbers.
     */

    double min_ns = 10000000000., // Assume no connection took longer than 10s
        max_ns = 0.,              // Assume no connection broke laws of physics
        sum_ns = 0.,              // Used to compute mean
        sum_diff2_ns = 0.;        // Used to compute variance/stdev

    // Find the min, max, and sum of measured latencies.
    for (int round = 0; round < CONFIG_NUM_ROUNDS; round++) {
        double ns = ts2double(times[round]);
        min_ns = min_ns > ns ? ns : min_ns;
        max_ns = max_ns < ns ? ns : max_ns;
        sum_ns += ns;
    }

    // Compute the mean.
    double mean_ns = sum_ns / CONFIG_NUM_ROUNDS;

    // Find the total dispersion.
    for (int round = 0; round < CONFIG_NUM_ROUNDS; round++) {
        double diff_ns = mean_ns - ts2double(times[round]);
        sum_diff2_ns = diff_ns * diff_ns;
    }

    // Compute the variance and standard deviation.
    double variance_ns = sum_diff2_ns / CONFIG_NUM_ROUNDS;
    double stdev_ns = sqrtl(variance_ns);

    // Print results.
    printf("mean:  %.3lfus\n", mean_ns / 1000.);
    printf("stdev: %.3lfus\n", stdev_ns / 1000.);
    printf("min:   %.3lfus\n", min_ns / 1000.);
    printf("max:   %.3lfus\n", max_ns / 1000.);

    return 0;
}
