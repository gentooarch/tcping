#define _GNU_SOURCE // For clock_gettime with CLOCK_MONOTONIC if not default
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // getopt, close
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>      // fcntl
#include <errno.h>
#include <time.h>
#include <sys/select.h> // select
#include <getopt.h>     // getopt_long

#define MAX_IP_LEN 40       // Max length for "xxx.xxx.xxx.xxx:ppppp"
#define MAX_RESULT_LEN 200  // Max length for result string
#define RESULTS_BUFFER_SIZE 1024 // Size of the shared results buffer

// --- Shared Data Structure ---
typedef struct {
    char ip[MAX_IP_LEN];
    char port[6]; // Max port length is 5 + null terminator
    long timeout_ms;
} ping_args_t;

// --- Results Buffer ---
// Simple ring buffer for results (can be improved with dynamic resizing or linked list)
char results_buffer[RESULTS_BUFFER_SIZE][MAX_RESULT_LEN];
int result_write_idx = 0;
int result_read_idx = 0;
int result_count = 0;
pthread_mutex_t results_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t results_cond_not_empty = PTHREAD_COND_INITIALIZER; // Signal when data is added
pthread_cond_t results_cond_not_full = PTHREAD_COND_INITIALIZER;  // Signal when space is available (optional for strict buffering)
int writer_finished = 0; // Flag to signal writer thread to exit

// --- Concurrency Control ---
sem_t semaphore;

// --- Output File ---
FILE *output_fp = NULL;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for writing to file directly from ping threads (alternative)

// --- Function Prototypes ---
void *tcp_ping_thread(void *arg);
void *writer_thread(void *arg);
void add_result(const char *result);
int get_result(char *buffer, size_t buffer_size);


// --- tcp_ping implementation ---
// Attempts a TCP connection with timeout.
// Returns 0 on success, -1 on timeout, -2 on other errors.
// Fills duration_ms with connection time on success.
int tcp_ping(const char *ip, const char *port_str, long timeout_ms, double *duration_ms) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct timespec start, end;
    long connect_timeout_sec = timeout_ms / 1000;
    long connect_timeout_usec = (timeout_ms % 1000) * 1000;
    int ret = -1; // Default to timeout

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        // Don't add this error to results, it's likely a local issue
        // perror("socket creation failed");
        return -2; // Other error
    }

    // Set socket to non-blocking
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        //perror("fcntl F_GETFL failed");
        close(sockfd);
        return -2;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
       // perror("fcntl F_SETFL O_NONBLOCK failed");
        close(sockfd);
        return -2;
    }

    // Prepare server address
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    int port_num = atoi(port_str);
    if (port_num <= 0 || port_num > 65535) {
        //fprintf(stderr, "Invalid port number: %s\n", port_str);
        close(sockfd);
        return -2;
    }
    serv_addr.sin_port = htons(port_num);
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        //fprintf(stderr, "Invalid IP address format: %s\n", ip);
        close(sockfd);
        return -2;
    }

    // Start timer
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Initiate non-blocking connect
    int connect_ret = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    if (connect_ret == 0) {
        // Connection established immediately (unlikely for non-local)
        ret = 0; // Success
    } else if (errno == EINPROGRESS) {
        // Connection attempt is in progress
        fd_set write_fds;
        struct timeval tv;

        FD_ZERO(&write_fds);
        FD_SET(sockfd, &write_fds);

        tv.tv_sec = connect_timeout_sec;
        tv.tv_usec = connect_timeout_usec;

        // Wait for socket to become writable (connection complete) or timeout
        int select_ret = select(sockfd + 1, NULL, &write_fds, NULL, &tv);

        if (select_ret > 0) {
            // Socket is writable, check for errors
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0) {
                if (so_error == 0) {
                    ret = 0; // Success
                } else {
                    // Connection failed (e.g., ECONNREFUSED, ETIMEDOUT (kernel timeout), ENETUNREACH)
                    errno = so_error; // Set errno for consistent reporting
                    ret = -2; // Other error
                }
            } else {
                // getsockopt failed
                //perror("getsockopt failed");
                ret = -2; // Other error
            }
        } else if (select_ret == 0) {
            // select timed out
            ret = -1; // Timeout
        } else {
            // select failed
            //perror("select failed");
            ret = -2; // Other error
        }
    } else {
        // connect failed immediately (e.g., network unreachable before attempt)
        ret = -2; // Other error
    }

    // Stop timer
    clock_gettime(CLOCK_MONOTONIC, &end);
    *duration_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

    // Restore blocking mode (optional, good practice)
    // fcntl(sockfd, F_SETFL, flags); // Commented out as we close immediately

    close(sockfd);
    return ret;
}


// --- Thread function for performing TCP ping ---
void *tcp_ping_thread(void *arg) {
    ping_args_t *ping_data = (ping_args_t *)arg;
    char result_str[MAX_RESULT_LEN];
    double duration_ms = 0;
    int ping_status;

    // Store errno from connect/getsockopt if it's a non-timeout error
    int original_errno = 0;

    ping_status = tcp_ping(ping_data->ip, ping_data->port, ping_data->timeout_ms, &duration_ms);
    if (ping_status == -2) {
        original_errno = errno; // Capture errno right after the failure
    }


    // --- Format and Add Result (only if not timeout) ---
    if (ping_status == 0) {
        // Success
        snprintf(result_str, sizeof(result_str), "IP: %s:%s 连接成功 延迟: %.2f ms\n",
                 ping_data->ip, ping_data->port, duration_ms);
        add_result(result_str);
    } else if (ping_status == -2) {
         // Failure (non-timeout)
        snprintf(result_str, sizeof(result_str), "IP: %s:%s 连接失败: %s\n",
                 ping_data->ip, ping_data->port, strerror(original_errno)); // Use captured errno
        add_result(result_str);
    }
    // else ping_status == -1 (Timeout) -> Do nothing


    // --- Cleanup and Semaphore Release ---
    free(ping_data);         // Free the allocated argument struct
    sem_post(&semaphore);    // Release the semaphore slot
    pthread_exit(NULL);      // Exit the thread
}

// --- Function to add result to the shared buffer ---
void add_result(const char *result) {
    pthread_mutex_lock(&results_mutex);

    // Simple wait if buffer is full (could use condition variable for better efficiency)
    while (result_count >= RESULTS_BUFFER_SIZE) {
        // Optional: Use results_cond_not_full here for better waiting
        // pthread_cond_wait(&results_cond_not_full, &results_mutex);
        pthread_mutex_unlock(&results_mutex); // Release lock temporarily
        usleep(10000); // Simple sleep to avoid busy-waiting if full
        pthread_mutex_lock(&results_mutex);
    }

    strncpy(results_buffer[result_write_idx], result, MAX_RESULT_LEN - 1);
    results_buffer[result_write_idx][MAX_RESULT_LEN - 1] = '\0'; // Ensure null termination
    result_write_idx = (result_write_idx + 1) % RESULTS_BUFFER_SIZE;
    result_count++;

    // Signal the writer thread that data is available
    pthread_cond_signal(&results_cond_not_empty);

    pthread_mutex_unlock(&results_mutex);
}

// --- Function for writer thread to get result from buffer ---
// Returns 1 if a result was retrieved, 0 if buffer is empty
int get_result(char *buffer, size_t buffer_size) {
    int got_result = 0;
    // Assumes results_mutex is already locked by the caller (writer_thread)

    if (result_count > 0) {
        strncpy(buffer, results_buffer[result_read_idx], buffer_size - 1);
        buffer[buffer_size - 1] = '\0'; // Ensure null termination
        result_read_idx = (result_read_idx + 1) % RESULTS_BUFFER_SIZE;
        result_count--;
        got_result = 1;
        // Optional: Signal if space becomes available
        // pthread_cond_signal(&results_cond_not_full);
    }
    return got_result;
}


// --- Writer thread function ---
void *writer_thread(void *arg) {
    char current_result[MAX_RESULT_LEN];
    FILE *fp = (FILE *)arg;

    while (1) {
        pthread_mutex_lock(&results_mutex);

        // Wait until buffer is not empty OR the writer is signaled to finish
        while (result_count == 0 && !writer_finished) {
            // Wait for a signal that data has been added
            pthread_cond_wait(&results_cond_not_empty, &results_mutex);
        }

        // If woken up and buffer is still empty, it means we should finish
        if (result_count == 0 && writer_finished) {
            pthread_mutex_unlock(&results_mutex);
            break; // Exit loop
        }

        // Get and write all available results in the buffer
        while(get_result(current_result, sizeof(current_result))) {
             // Unlock temporarily while writing (or use separate file mutex)
            pthread_mutex_unlock(&results_mutex);
            if (fprintf(fp, "%s", current_result) < 0) {
                 perror("Warning: Failed writing to output file");
                 // Decide how to handle write errors (e.g., print to stderr, retry?)
            }
            fflush(fp); // Ensure data is written promptly
            pthread_mutex_lock(&results_mutex); // Re-lock before checking condition again
        }

        pthread_mutex_unlock(&results_mutex);

        // Small sleep if the buffer was emptied to prevent busy-waiting
        // if the ping threads are much faster than writing.
        // Alternatively, rely purely on the condition variable wait.
        // usleep(1000);
    }

    printf("Writer thread finished.\n");
    pthread_exit(NULL);
}

// --- Print usage information ---
void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -i <base_ip> [-s <start>] [-e <end>] [-p <port>] [-o <outfile>] [-c <concurrency>]\n", prog_name);
    fprintf(stderr, "  -i, --ip=<base_ip>      Base IP prefix (e.g., 192.168, 10.0.5)\n");
    fprintf(stderr, "  -s, --start=<start>     Starting third octet (0-255, default: 0)\n");
    fprintf(stderr, "  -e, --end=<end>         Ending third octet (0-255, default: 255)\n");
    fprintf(stderr, "  -p, --port=<port>       TCP port to ping (default: 443)\n");
    fprintf(stderr, "  -o, --output=<outfile>  Output file name (default: tcp_ping_results.txt)\n");
    fprintf(stderr, "  -c, --concurrency=<num> Number of concurrent pings (default: 100)\n");
    fprintf(stderr, "  -t, --timeout=<ms>      Connection timeout in milliseconds (default: 100)\n");
    fprintf(stderr, "  -h, --help              Show this help message\n");
}

// --- Main Function ---
int main(int argc, char *argv[]) {
    char *base_ip = NULL;
    char *port_str = "443";
    int start_subnet = 0;
    int end_subnet = 255;
    char *output_filename = "tcp_ping_results.txt";
    int concurrency = 100;
    long timeout_ms = 100; // Default timeout

    // --- Argument Parsing (using getopt_long) ---
    int opt;
    struct option long_options[] = {
        {"ip",          required_argument, 0, 'i'},
        {"start",       required_argument, 0, 's'},
        {"end",         required_argument, 0, 'e'},
        {"port",        required_argument, 0, 'p'},
        {"output",      required_argument, 0, 'o'},
        {"concurrency", required_argument, 0, 'c'},
        {"timeout",     required_argument, 0, 't'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "i:s:e:p:o:c:t:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': base_ip = optarg; break;
            case 's': start_subnet = atoi(optarg); break;
            case 'e': end_subnet = atoi(optarg); break;
            case 'p': port_str = optarg; break;
            case 'o': output_filename = optarg; break;
            case 'c': concurrency = atoi(optarg); break;
            case 't': timeout_ms = atol(optarg); break; // Use atol for long
            case 'h': print_usage(argv[0]); exit(EXIT_SUCCESS);
            default: print_usage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    // --- Argument Validation ---
    if (base_ip == NULL) {
        fprintf(stderr, "Error: Base IP (-i or --ip) is required.\n");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
    // Basic validation for base_ip format (improve if needed)
    int dots = 0;
    for(char *p = base_ip; *p; p++) {
        if (*p == '.') dots++;
    }
    if (dots != 1 && dots != 2) {
         fprintf(stderr, "Error: Invalid base IP format '%s'. Use format like '192.168' or '10.0.5'.\n", base_ip);
         exit(EXIT_FAILURE);
    }

    if (start_subnet < 0 || start_subnet > 255 || end_subnet < 0 || end_subnet > 255 || start_subnet > end_subnet) {
        fprintf(stderr, "Error: Start/End subnet must be between 0-255, and start <= end.\n");
        exit(EXIT_FAILURE);
    }
    if (concurrency <= 0) {
        fprintf(stderr, "Error: Concurrency must be greater than 0.\n");
        exit(EXIT_FAILURE);
    }
     if (timeout_ms <= 0) {
        fprintf(stderr, "Error: Timeout must be greater than 0 ms.\n");
        exit(EXIT_FAILURE);
    }
    int port_num_check = atoi(port_str);
     if (port_num_check <= 0 || port_num_check > 65535) {
        fprintf(stderr, "Error: Invalid port number %s.\n", port_str);
        exit(EXIT_FAILURE);
    }


    // --- File Operation ---
    output_fp = fopen(output_filename, "w");
    if (output_fp == NULL) {
        perror("Error opening output file");
        exit(EXIT_FAILURE);
    }
    printf("Results will be written to %s (Timeouts ignored)\n", output_filename);

    // --- Initialize Synchronization Primitives ---
    if (sem_init(&semaphore, 0, concurrency) != 0) {
        perror("Semaphore initialization failed");
        fclose(output_fp);
        exit(EXIT_FAILURE);
    }
    // Mutexes are statically initialized

    // --- Start Writer Thread ---
    pthread_t writer_tid;
    if (pthread_create(&writer_tid, NULL, writer_thread, (void *)output_fp) != 0) {
        perror("Failed to create writer thread");
        fclose(output_fp);
        sem_destroy(&semaphore);
        exit(EXIT_FAILURE);
    }


    // --- Start Ping Threads ---
    int num_ips = end_subnet - start_subnet + 1;
    pthread_t *threads = malloc(num_ips * sizeof(pthread_t));
    if (threads == NULL) {
        perror("Failed to allocate memory for thread IDs");
        // Need to signal writer thread to exit and cleanup
        pthread_mutex_lock(&results_mutex);
        writer_finished = 1;
        pthread_cond_signal(&results_cond_not_empty); // Wake up writer
        pthread_mutex_unlock(&results_mutex);
        pthread_join(writer_tid, NULL); // Wait for writer
        fclose(output_fp);
        sem_destroy(&semaphore);
        exit(EXIT_FAILURE);
    }

    printf("Starting scan from %s.%d.1 to %s.%d.1 on port %s (Concurrency: %d, Timeout: %ld ms)...\n",
           base_ip, start_subnet, base_ip, end_subnet, port_str, concurrency, timeout_ms);

    int thread_count = 0;
    for (int i = start_subnet; i <= end_subnet; i++) {
        char current_ip[MAX_IP_LEN];
        snprintf(current_ip, sizeof(current_ip), "%s.%d.1", base_ip, i);

        // Allocate arguments for the thread
        ping_args_t *args = malloc(sizeof(ping_args_t));
        if (args == NULL) {
            fprintf(stderr, "Warning: Failed to allocate memory for thread args for %s. Skipping.\n", current_ip);
            continue; // Skip this IP
        }
        strncpy(args->ip, current_ip, MAX_IP_LEN -1);
        args->ip[MAX_IP_LEN -1] = '\0';
        strncpy(args->port, port_str, sizeof(args->port) - 1);
        args->port[sizeof(args->port) - 1] = '\0';
        args->timeout_ms = timeout_ms;


        // Wait for semaphore (acquire slot)
        if (sem_wait(&semaphore) != 0) {
             perror("sem_wait failed");
             free(args); // Clean up allocated args
             // Consider more robust error handling here - maybe break the loop?
             break;
        }


        // Create ping thread
        if (pthread_create(&threads[thread_count], NULL, tcp_ping_thread, (void *)args) != 0) {
            perror("Failed to create ping thread");
            sem_post(&semaphore); // Release semaphore if thread creation failed
            free(args); // Clean up allocated args
            // Consider more robust error handling
        } else {
            thread_count++; // Only increment if thread was successfully created
        }
    }

    // --- Wait for Ping Threads to Complete ---
    printf("Waiting for %d ping threads to complete...\n", thread_count);
    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("All ping threads finished.\n");

    // --- Signal Writer Thread to Finish ---
    pthread_mutex_lock(&results_mutex);
    writer_finished = 1;
    pthread_cond_signal(&results_cond_not_empty); // Wake up writer thread one last time
    pthread_mutex_unlock(&results_mutex);

    // --- Wait for Writer Thread ---
    pthread_join(writer_tid, NULL);

    // --- Cleanup ---
    free(threads);
    fclose(output_fp);
    sem_destroy(&semaphore);
    pthread_mutex_destroy(&results_mutex); // Optional for statically initialized mutexes
    pthread_cond_destroy(&results_cond_not_empty);
    pthread_cond_destroy(&results_cond_not_full);
    // pthread_mutex_destroy(&file_mutex);

    printf("Scan complete.\n");

    return EXIT_SUCCESS;
}
