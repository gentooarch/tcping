#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>

typedef struct {
    int id;
    int count;
    char host[256];
    int port;
} ThreadArgs;

void* tcp_ping(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    struct addrinfo hints, *res;
    int i;

    for (i = 0; i < args->count; ++i) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", args->port);

        int ret = getaddrinfo(args->host, port_str, &hints, &res);
        if (ret != 0) {
            fprintf(stderr, "Thread %d: getaddrinfo failed: %s\n", args->id, gai_strerror(ret));
            continue;
        }

        int sockfd;
        struct timeval start, end;

        gettimeofday(&start, NULL);
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sockfd < 0) {
            perror("socket");
            freeaddrinfo(res);
            continue;
        }

        ret = connect(sockfd, res->ai_addr, res->ai_addrlen);
        gettimeofday(&end, NULL);

        if (ret == 0) {
            long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                              (end.tv_usec - start.tv_usec) / 1000;

            // 提取实际连接的 IP 地址
            char ip_str[INET6_ADDRSTRLEN];
            void* addr;

            if (res->ai_family == AF_INET) {
                struct sockaddr_in* ipv4 = (struct sockaddr_in*)res->ai_addr;
                addr = &(ipv4->sin_addr);
            } else {
                struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)res->ai_addr;
                addr = &(ipv6->sin6_addr);
            }

            inet_ntop(res->ai_family, addr, ip_str, sizeof(ip_str));

            printf("Thread %d - Attempt %d: Connected to %s (%s):%d, time=%ld ms\n",
                   args->id, i + 1, args->host, ip_str, args->port, elapsed_ms);
        } else {
            printf("Thread %d - Attempt %d: Connection failed: %s\n", 
                   args->id, i + 1, strerror(errno));
        }

        close(sockfd);
        freeaddrinfo(res);
        usleep(500 * 1000); // sleep 500ms
    }

    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
    int threads = 8;
    int count = 1;
    int port = 443;
    char* host = NULL;

    if (argc == 2) {
        host = argv[1];
    } else if (argc == 3) {
        threads = atoi(argv[1]);
        host = argv[2];
    } else if (argc == 4) {
        threads = atoi(argv[1]);
        count = atoi(argv[2]);
        host = argv[3];
    } else if (argc == 5) {
        threads = atoi(argv[1]);
        count = atoi(argv[2]);
        port = atoi(argv[3]);
        host = argv[4];
    } else {
        fprintf(stderr, "Usage: %s [threads] [count] [port] hostname\n", argv[0]);
        return 1;
    }

    pthread_t tid[threads];
    ThreadArgs args[threads];

    for (int i = 0; i < threads; ++i) {
        args[i].id = i;
        args[i].count = count;
        args[i].port = port;
        strncpy(args[i].host, host, sizeof(args[i].host) - 1);
        args[i].host[sizeof(args[i].host) - 1] = '\0';
        pthread_create(&tid[i], NULL, tcp_ping, &args[i]);
    }

    for (int i = 0; i < threads; ++i) {
        pthread_join(tid[i], NULL);
    }

    return 0;
}

