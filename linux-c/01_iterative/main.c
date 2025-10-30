#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define DEFAULT_SERVER_PORT 8000
#define REDIS_SERVER_HOST   "127.0.0.1"
#define REDIS_SERVER_PORT   6379

char    redis_host_ip[32];
int     redis_socket_fd;

void fatal_error(const char *syscall)
{
    perror(syscall);
    exit(1);
}

// create a client socket for redis
void connect_to_redis_server()
{
    struct sockaddr_in redis_srvaddr;
    redis_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    bzero(&redis_srvaddr, sizeof(redis_srvaddr));
    redis_srvaddr.sin_family = AF_INET;
    redis_srvaddr.sin_port = htons(REDIS_SERVER_PORT); // convert from host order to network byte order

    int pton_ret = inet_pton(AF_INET, redis_host_ip, &redis_srvaddr.sin_addr.s_addr);
    if (pton_ret < 0) fatal_error("inet_pton()");
    else if (pton_ret == 0)
    {
        fprintf(stderr, "Error: Please provide a valid Redis server IP address.\n");
        exit(1);
    }

    int cret = connect(redis_socket_fd, (struct sockaddr *)&redis_srvaddr, sizeof(redis_srvaddr));
    if (cret == -1) fatal_error("redis connect()");
    else printf("Connected to Redis server@ %s:%d\n", redis_host_ip, REDIS_SERVER_PORT);
}

// creates a server socket, defines a socket address
// bind them together and converts the socket to listening socket
int setup_listening_socket(int server_port)
{
    int sock;

    // describes a socket address
    struct sockaddr_in srv_addr;
    bzero(&srv_addr, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(server_port);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // open an IPv4, TCP connection socket
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) fatal_error("socket()");

    int enable = 1;
    // set options on the socket fd
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) fatal_error("setsockopt(SO_REUSEADDR)");

    // we bind this socket to this socket address
    if (bind(sock, (const struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) fatal_error("bind()");

    // turn this socket into a listening socket with request queue length
    if (listen(sock, 10) < 0) fatal_error("listen()");

    return sock;
}

void handle_client(int client_socket)
{
    printf("handling client\n");
}

// accept client connections and calls handle_client() to serve the request
// Once the request is served, it closes the client connection
// and waits for a new client connection calling accept() again which is blocking
void enter_server_loop(int server_socket)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    while(1)
    {
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1) fatal_error("accept()");

        handle_client(client_socket);
        close(client_socket);
    }
}

// When Ctrl+C is pressed, the shell sends our process SIGINT
void print_stats(int signo)
{
    struct rusage rusagebuf;
    getrusage(RUSAGE_SELF, &rusagebuf);
    printf("\nUser time: %lds %ldms, System time: %lds %ldms\n",
            rusagebuf.ru_utime.tv_sec, rusagebuf.ru_utime.tv_usec/1000,
            rusagebuf.ru_stime.tv_sec, rusagebuf.ru_stime.tv_usec/1000);
    exit(0);
}

int main(int argc, char* argv[])
{
    int server_port;
    if (argc > 1)
    {
        server_port = atoi(argv[1]);
    }
    else
    {
        server_port = DEFAULT_SERVER_PORT;
    }

    if (argc > 2)
    {
        strcpy(redis_host_ip, argv[2]);
    }
    else
    {
        strcpy(redis_host_ip, REDIS_SERVER_HOST);
    }

    // set up the listening socket
    int server_socket = setup_listening_socket(server_port);
    
    // establish connection to redis
    connect_to_redis_server();

    setlocale(LC_NUMERIC, "");
    
    printf("ZeroHTTPd server listening on port %d\n", server_port);
    
    // set up signal handler for SIGINT
    signal(SIGINT, print_stats);
    
    // enter loop which accepts and serve client requests
    enter_server_loop(server_socket);

    return 0;
}