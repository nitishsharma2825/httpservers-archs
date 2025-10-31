#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h> // for tolower

#define SERVER_STRING           "Server: nitishhttpd/0.1\r\n"
#define DEFAULT_SERVER_PORT     8000
#define REDIS_SERVER_HOST       "127.0.0.1"
#define REDIS_SERVER_PORT       6379

#define METHOD_HANDLED          0
#define METHOD_NOT_HANDLED      1

#define GUESTBOOK_ROUTE         "/guestbook"
#define GUESTBOOK_TEMPLATE      "templates/guestbook/index.html"

const char *unimplemented_content = \
        "HTTP/1.0 400 Bad Request\r\n"
        "Content-type: text/html\r\n"
        "\r\n"
        "<html>"
        "<head>"
        "<title>ZeroHTTPd: Unimplemented</title>"
        "</head>"
        "<body>"
        "<h1>Bad Request (Unimplemented)</h1>"
        "<p>Your client sent a request ZeroHTTPd did not understand and it is probably not your fault.</p>"
        "</body>"
        "</html>";

const char *http_404_content = \
        "HTTP/1.0 404 Not Found\r\n"
        "Content-type: text/html\r\n"
        "\r\n"
        "<html>"
        "<head>"
        "<title>ZeroHTTPd: Not Found</title>"
        "</head>"
        "<body>"
        "<h1>Not Found (404)</h1>"
        "<p>Your client is asking for an object that was not found on this server.</p>"
        "</body>"
        "</html>";

char    redis_host_ip[32];
int     redis_socket_fd;

void fatal_error(const char *syscall)
{
    perror(syscall);
    exit(1);
}

/*
    Utility function to convert string to lowercase in place
*/
void strtolower(char* str)
{
    for(; *str; ++str) *str = (char)tolower(*str); 
}

const char* get_filename_ext(const char* filename)
{
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";
    return dot + 1;
}

/*
    Sends "HTTP Not Found" code and message to the client
*/
void handle_http_404(int client_socket)
{
    send(client_socket, http_404_content, strlen(http_404_content), 0);
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

int get_line(int sock, char* buf, int size)
{
    int i = 0;
    char c = '\0';
    ssize_t n;

    while ((i < size - 1) && (c != '\n'))
    {
        // read 1 char byte, should be read in chunks for effeciency
        n = recv(sock, &c, 1, 0);
        if (n > 0)
        {
            // check for \r\n line boundary as per HTTP protocol
            if (c == '\r')
            {
                n = recv(sock, &c, 1, MSG_PEEK); // only peeks, don't read
                if ((n > 0) && (c == '\n'))
                {
                    recv(sock, &c, 1, 0);
                }
            }
            else
            {
                buf[i] = c;
                i++;
            }
        }
        else
        {
            return 0;
        }
    }
    buf[i] = '\0';
    return i;
}

/*
    Read the static file and write to client socket using sendfile() system call [zero copy]
*/
void transfer_file_contents(char* file_path, int client_socket, off_t file_size)
{
    int fd;
    fd = open(file_path, O_RDONLY);
    sendfile(client_socket, fd, NULL, file_size);
    close(fd);
}

/*
    Sends HTTP 200 OK header
*/
void send_headers(const char* path, off_t len, int client_socket)
{
    char small_case_path[1024];
    char send_buffer[1024];
    strcpy(small_case_path, path);
    strtolower(small_case_path);

    strcpy(send_buffer, "HTTP/1.0 200 OK\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, SERVER_STRING);
    send(client_socket, send_buffer, strlen(send_buffer), 0);

    /*
        Check file extensions for certain common types of files on web pages
        and send the appropriate content type header 
    */
   const char* file_ext = get_filename_ext(small_case_path);
   if (strcmp("jpg", file_ext) == 0) strcpy(send_buffer, "Content-Type: image/jpeg\r\n");
   if (strcmp("jpeg", file_ext) == 0) strcpy(send_buffer, "Content-Type: image/jpeg\r\n");
   if (strcmp("png", file_ext) == 0) strcpy(send_buffer, "Content-Type: image/png\r\n");
   if (strcmp("gif", file_ext) == 0) strcpy(send_buffer, "Content-Type: image/gif\r\n");
   if (strcmp("htm", file_ext) == 0) strcpy(send_buffer, "Content-Type: text/html\r\n");
   if (strcmp("html", file_ext) == 0) strcpy(send_buffer, "Content-Type: text/html\r\n");
   if (strcmp("js", file_ext) == 0) strcpy(send_buffer, "Content-Type: application/javascript\r\n");
   if (strcmp("css", file_ext) == 0) strcpy(send_buffer, "Content-Type: text/css\r\n");
   if (strcmp("txt", file_ext) == 0) strcpy(send_buffer, "Content-Type: text/plain\r\n");
   send(client_socket, send_buffer, strlen(send_buffer), 0);

   /* Send the content length header*/
   sprintf(send_buffer, "content-length: %ld\r\n", len);
   send(client_socket, send_buffer, strlen(send_buffer), 0);

   /* This signals browser there are no more headers. Content May follow */
   strcpy(send_buffer, "\r\n");
   send(client_socket, send_buffer, strlen(send_buffer), 0);
}

/*
    The guest book template file is a normal HTML file except 2 special strings:
    $GUEST_REMARKS$ and $VISITOR_COUNT$
    In this method, these template variables are replaced by content generated by us.
    That content is based on stuff we retrieve from Redis
*/
int render_guestbook_template(int client_socket)
{
    char templ[16384];
    char rendering[16384];

    /* Read the template file*/
    int fd = open(GUESTBOOK_TEMPLATE, O_RDONLY);
    if (fd == -1) fatal_error("Template read()");
    read(fd, templ, sizeof(templ));
    close(fd);

    /* Get guestbook entries and render them as HTML */
}

/*
    If we are not serving static files and we want to write web apps, this is the place to add more routes
    If this function returns METHOD_NOT_HANDLED, the request is considered a regular static file request
    This function gets precedence over static file serving
*/
int handle_app_get_routes(char* path, int client_socket)
{
    if (strcmp(path, GUESTBOOK_ROUTE) == 0)
    {
        render_guestbook_template(client_socket);
        return METHOD_HANDLED;
    }

    return METHOD_NOT_HANDLED;
}

/*
    Main GET method handler. Checks for any app methods,
    else proceeds to look for static files or index files of directories
*/
void handle_get_method(char* path, int client_socket)
{
    char final_path[1024];

    /* check if this request is for any app method */
    if (handle_app_get_routes(path, client_socket) == METHOD_HANDLED) return;

    /* request is for static file serving */
    
    /*
        If path ends in a /, client wants the index file inside that directory
        eg: GET /               => this means client want index file in root directory which is public
        eg: GET /work.html      => this means client want work.html file inside public directory
        eg: GET /work/          => this means client wnat index.html file inside work directory inside public dir
        eg: GET /work/me.html   => me.html file inside work directory in public directory 
    */
    if (path[strlen(path) - 1] == '/')
    {
        strcpy(final_path, "public");
        strcat(final_path, path);
        strcat(final_path, "index.html");
    }
    else
    {
        strcpy(final_path, "public");
        strcat(final_path, path);
    }

    struct stat path_stat;
    if (stat(final_path, &path_stat) == -1)
    {
        printf("404 Not Found: %s\n", final_path);
        handle_http_404(client_socket);
    }
    else
    {
        /* Check if this is a regular file and not a directory or something else */
        if (S_ISREG(path_stat.st_mode))
        {
            send_headers(final_path, path_stat.st_size, client_socket);
            transfer_file_contents(final_path, client_socket, path_stat.st_size);
            printf("200 %s %ld bytes\n", final_path, path_stat.st_size);
        }
        else
        {
            handle_http_404(client_socket);
            printf("404 Not Found: %s\n", final_path);
        }
    }
}

void handle_post_method(char* path, int client_socket)
{
    printf("post method handler");
}

void handle_unimplemented_method(int client_socket)
{
    printf("unimplemented method handler");
}

void handle_http_method(char* method_buffer, int client_socket)
{
    char* method, *path;
    method = strtok(method_buffer, " ");
    strtolower(method);
    path = strtok(NULL, " ");

    if (strcmp(method, "get") == 0)
    {
        handle_get_method(path, client_socket);
    }
    else if (strcmp(method, "post") == 0)
    {
        handle_post_method(path, client_socket);
    }
    else
    {
        handle_unimplemented_method(client_socket);
    }
}

void handle_client(int client_socket)
{
    char line_buffer[1024];
    char method_buffer[1024];
    int method_line = 0;

    while (1)
    {
        get_line(client_socket, line_buffer, sizeof(line_buffer));
        method_line++;

        unsigned long len = strlen(line_buffer);

        // 1st line has HTTP method, we only care about that
        // we read rest of the lines and throw them away
        if (method_line == 1)
        {
            if (len == 0) return;
            strcpy(method_buffer, line_buffer);
        }
        else
        {
            // end of request
            if (len == 0) break;
        }
    }

    handle_http_method(method_buffer, client_socket);
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