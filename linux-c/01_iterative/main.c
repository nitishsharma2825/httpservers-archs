#define _GNU_SOURCE /* For asprintf() */
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

#define SERVER_STRING                   "Server: nitishhttpd/0.1\r\n"
#define DEFAULT_SERVER_PORT             8000
#define REDIS_SERVER_HOST               "127.0.0.1"
#define REDIS_SERVER_PORT               6379

#define METHOD_HANDLED                  0
#define METHOD_NOT_HANDLED              1

#define GUESTBOOK_ROUTE                 "/guestbook"
#define GUESTBOOK_TEMPLATE              "templates/guestbook/index.html"
#define GUESTBOOK_REDIS_VISITOR_KEY     "visitor_count"
#define GUESTBOOK_REDIS_REMARKS_KEY     "guestbook_remarks"
#define GUESTBOOK_TMPL_VISITOR          "$VISITOR_COUNT$"
#define GUESTBOOK_TMPL_REMARKS          "$GUEST_REMARKS$"

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

/* Converts a hex character to its integer value */
char from_hex(char ch)
{
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*
    Sends "HTTP Not Found" code and message to the client
*/
void handle_http_404(int client_socket)
{
    send(client_socket, http_404_content, strlen(http_404_content), 0);
}

/*
    HTML URls and other data like data sent over POST method are encoded using a simple schema
    This functions take encoded data and returns a regular, decoded string
    eg:
    Encoded: Nothing+is+better+than+bread+%26+butter%21
    Decoded: Nothing is better than bread & butter!
*/
char* urlencoding_decode(char* str)
{
    char* pstr = str;
    char* buf = malloc(strlen(str) + 1);
    char* pbuf = buf;

    while (*pstr)
    {
        if(*pstr == '%')
        {
            if(pstr[1] && pstr[2])
            {
                *pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
                pstr += 2;
            }
        }
        else if (*pstr == '+')
        {
            *pbuf++ = ' ';
        }
        else
        {
            *pbuf++ = *pstr;
        }
        pstr++;
    }
    *pbuf = '\0';

    return buf;
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

/*
    Internal function. It sends the command to the server,
    but reads back the raw server response. Not very useful to be used directly without
    first processing it to extract the required data
*/
int _redis_get_key(const char* key, char* value_buffer, int value_buffer_sz)
{
    char *req_buffer;
    /*
        asprintf() is a useful GNU extension that allocates the string as required
        No more guessing the right size for the buffer that holds the string
        Don't forget to call free() once done
    */
   asprintf(&req_buffer, "*2\r\n$3\r\nGET\r\n$%ld\r\n%s\r\n", strlen(key), key);
   write(redis_socket_fd, req_buffer, strlen(req_buffer));
   free(req_buffer);
   read(redis_socket_fd, value_buffer, value_buffer_sz);
   return 0;
}

/*
    Given the key, fetch the number value associated with it
*/
int redis_get_int_key(const char* key, int *value)
{
    /*
     * Example response from server:
     * $3\r\n385\r\n
     * This means that the server is telling us to expect a string
     * of 3 characters.
    */
    
    char redis_response[64] = "";
    _redis_get_key(key, redis_response, sizeof(redis_response));
    char* p = redis_response;
    if (*p != '$') return -1;

    while(*p++ != '\n');

    /* Convert string representation of a number to a number */
    int intval = 0;
    while (*p != '\r')
    {
        intval = (intval * 10) + (*p - '0');
        p++;
    }
    *value = intval;

    return 0;
}

/* Increment given key by incr_by. Key is created by Redis if it doesn't exist */
int redis_incr_by(char* key, int incr_by)
{
    char cmd_buf[1024] = "";
    char incr_by_str[16] = "";
    sprintf(incr_by_str, "%d", incr_by);
    sprintf(cmd_buf, "*3\r\n$6\r\nINCRBY\r\n$%ld\r\n%s\r\n$%ld\r\n%d\r\n", strlen(key), key, strlen(incr_by_str), incr_by);
    write(redis_socket_fd, cmd_buf, strlen(cmd_buf));
    bzero(cmd_buf, sizeof(cmd_buf));
    read(redis_socket_fd, cmd_buf, sizeof(cmd_buf));
    return 0;
}

/* Increment value of key in redis by 1 */
int redis_incr(char* key)
{
    return redis_incr_by(key, 1);
}

/*
    Appends an item pointed to by 'value' to the list in redis referred by 'key'
    Uses the redis RPUSH command
*/
int redis_list_append(char* key, char* value)
{
    char cmd_buf[1024] = "";
    sprintf(cmd_buf, "*3\r\n$5\r\nRPUSH\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", strlen(key), key, strlen(value), value);
    write(redis_socket_fd, cmd_buf, strlen(cmd_buf));
    bzero(cmd_buf, sizeof(cmd_buf));
    read(redis_socket_fd, cmd_buf, sizeof(cmd_buf));
    return 0;
}

/*
    Get range of items in a list from 'start' to 'end'
    This function allocates memory. An array of pointers and all strings pointed to by it are dynamically allocated.
*/
int redis_list_get_range(char* key, int start, int end, char*** items, int* items_count)
{
    /*
     *  The Redis protocol is elegantly simple. The following is a response for an array
     *  that has 3 elements (strings):
     *  Example response:
     *  *3\r\n$5\r\nHello\r\n$6\r\nLovely\r\n\$5\r\nWorld\r\n
     *
     *  What it means:
     *  *3      -> Array with 3 items
     *  $5      -> string with 5 characters
     *  Hello   -> actual string
     *  $6      -> string with 6 characters
     *  Lovely  -> actual string
     *  $5      -> string with 5 characters
     *  World   -> actual string
     *
     *  A '\r\n' (carriage return + line feed) sequence is used as the delimiter.
     *  Now, you should be able to understand why we're doing what we're doing in this function
     * */

    char cmd_buf[1024]="", start_str[16], end_str[16];
    sprintf(start_str, "%d", start);
    sprintf(end_str, "%d", end);
    sprintf(cmd_buf, "*4\r\n$6\r\nLRANGE\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", strlen(key), key, strlen(start_str), start_str, strlen(end_str), end_str);
    write(redis_socket_fd, cmd_buf, strlen(cmd_buf));

    /* Find the length of the returned array */
    char ch;
    read(redis_socket_fd, &ch, 1);
    if (ch != '*') return -1;

    int returned_items = 0;
    while(1)
    {
        read(redis_socket_fd, &ch, 1);
        if (ch == '\r')
        {
            // read the next \n char
            read(redis_socket_fd, &ch, 1);
            break;
        }
        returned_items = (returned_items * 10) + (ch - '0');
    }

    *items_count = returned_items;
    /* Allocate array that will hold pointer each for every element in the returned list */
    char** items_holder = malloc(sizeof(char*) * returned_items);
    *items = items_holder;

    /*
        We know length of array. Loop that many iterations and grap those strings
        allocating a new chunk of memory for each
    */
    for (int i = 0; i < returned_items; i++)
    {
        // read the fist $
        read(redis_socket_fd, &ch, 1);
        int str_size = 0;
        while(1)
        {
            read(redis_socket_fd, &ch, 1);
            if (ch == '\r')
            {
                read(redis_socket_fd, &ch, 1);
                break;
            }
            str_size = (str_size * 10) + (ch - '0');
        }

        // allocate and read the string
        char *str = malloc(sizeof(char) * str_size + 1);
        items_holder[i] = str;
        read(redis_socket_fd, str, str_size);
        str[str_size] = '\0';

        // Read the '\r\n' chars
        read(redis_socket_fd, &ch, 1);
        read(redis_socket_fd, &ch, 1);
    }
}

/* Free all dynamically allocated string */
int redis_free_array_result(char** items, int length)
{
    for (int i = 0; i < length; i++)
    {
        free(items[i]);
    }
    free(items);
}

/*
    Utility function to get the whole list
*/
int redis_get_list(char* key, char*** items, int* items_count)
{
    return redis_list_get_range(key, 0, -1, items, items_count);
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
    /* safe programming, all offsets are set to \0, else they are filled with garbage. */
    char templ[16384] = "";
    char rendering[16384] = "";

    /* Read the template file*/
    int fd = open(GUESTBOOK_TEMPLATE, O_RDONLY);
    if (fd == -1) fatal_error("Template read()");
    read(fd, templ, sizeof(templ));
    close(fd);

    /* Get guestbook entries and render them as HTML */
    int entries_count;
    char** guest_entries;
    char guest_entries_html[16384] = "";

    redis_get_list(GUESTBOOK_REDIS_REMARKS_KEY, &guest_entries, &entries_count);
    for (int i = 0; i < entries_count; i++)
    {
        char guest_entry[1024];
        sprintf(guest_entry, "<p class=\"guest-entry\">%s</p>", guest_entries[i]);
        strcat(guest_entries_html, guest_entry);
    }
    redis_free_array_result(guest_entries, entries_count);

    /* In Redis, increment visitor count and fetch latest value */
    int visitor_count;
    char visitor_count_str[16] = "";
    redis_incr(GUESTBOOK_REDIS_VISITOR_KEY);
    redis_get_int_key(GUESTBOOK_REDIS_VISITOR_KEY, &visitor_count);
    sprintf(visitor_count_str, "%'d", visitor_count);

    /* Replace guestbook entries in HTML*/
    char *entries = strstr(templ, GUESTBOOK_TMPL_REMARKS);
    if (entries)
    {
        memcpy(rendering, templ, entries - templ);
        strcat(rendering, guest_entries_html);
        char* copy_offset = templ + (entries - templ) + strlen(GUESTBOOK_TMPL_REMARKS);
        strcat(rendering, copy_offset);
        strcpy(templ, rendering);
        bzero(rendering, sizeof(rendering));
    }

    /* Replace visitor count in HTML*/
    char* vcount = strstr(templ, GUESTBOOK_TMPL_VISITOR);
    if (vcount)
    {
        memcpy(rendering, templ, vcount - templ);
        strcat(rendering, visitor_count_str);
        char* copy_offset = templ + (vcount - templ) + strlen(GUESTBOOK_TMPL_VISITOR);
        strcat(rendering, copy_offset);
        strcpy(templ, rendering);
        bzero(rendering, sizeof(rendering));
    }

    /*
        Template is rendered, Send headers and template over to the client
    */
    char send_buffer[1024];
    strcpy(send_buffer, "HTTP/1.0 200 OK\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, SERVER_STRING);
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, "Content-Type: text/html\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    sprintf(send_buffer, "content-length: %ld\r\n", strlen(templ));
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, "\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);

    // send template
    send(client_socket, templ, strlen(templ), 0);
    printf("200 GET /guestbook %ld bytes\n", strlen(templ));
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

/*
    Guest submits name and remarks via the form on the page.
    That data is available to us as post x-www-form-urlencoded data.
    Need to decode and get what we need in plain text.

    At this point, we have already read the headers and whats left is post data, which forms the body of request
*/
void handle_new_guest_remarks(int client_socket)
{
    char remarks[1024] = "";
    char name[512] = "";
    char buffer[4026] = "";
    char* c1;
    char* c2;
    read(client_socket, buffer, sizeof(buffer));
    /*
     * Sample data format:
     * guest-remarks=Relatively+great+service&guest-name=Albert+Einstein
     * */

    char* assignment = strtok_r(buffer, "&", &c1);
    do
    {
        char* subassignment = strtok_r(assignment, "=", &c2);
        if (!subassignment) break;
        
        do
        {
            if (strcmp(subassignment, "guest-name") == 0)
            {
                subassignment = strtok_r(NULL, "=", &c2);
                if (!subassignment)
                {
                    name[0] = '\0';
                    break;
                }
                else
                {
                    strcpy(name, subassignment);
                }
            }

            if (strcmp(subassignment, "guest-remarks") == 0)
            {
                subassignment = strtok_r(NULL, "=", &c2);
                if(!subassignment)
                {
                    remarks[0] = '\0';
                    break;
                }
                else
                {
                    strcpy(remarks, subassignment);
                }
            }
            subassignment = strtok_r(NULL, "=", &c2);
            if (!subassignment) break;
        } while (1);

        assignment = strtok_r(NULL, "&", &c1);
        if (!assignment) break;
    } while (1);

    /* Validate name and remark lenghts and show an error page if required */
    if(strlen(name) == 0 || strlen(remarks) == 0)
    {
        char* html = "HTTP/1.0 400 Bad Request\r\ncontent-type: text/html\r\n\r\n<html><title>Error</title><body><p>Error: Do not leave name or remarks empty.</p><p><a href=\"/guestbook\">Go back to Guestbook</a></p></body></html>";
        write(client_socket, html, strlen(html));
        printf("400 POST /guestbook\n");
        return;
    }

    /*
        POST uses form URL encoding. Decode the strings and append them to the Redis
        list that holds all remarks.
    */
   char* decoded_name = urlencoding_decode(name);
   char* decoded_remarks = urlencoding_decode(remarks);
   bzero(buffer, sizeof(buffer));
   sprintf(buffer, "%s - %s", decoded_remarks, decoded_name);
   redis_list_append(GUESTBOOK_REDIS_REMARKS_KEY, buffer);
   free(decoded_name);
   free(decoded_remarks);

   /* All good! Show a 'thank you' page. */
   char *html = "HTTP/1.0 200 OK\r\ncontent-type: text/html\r\n\r\n<html><title>Thank you!</title><body><p>Thank you for leaving feedback! We really appreciate that!</p><p><a href=\"/guestbook\">Go back to Guestbook</a></p></body></html>";
   write(client_socket, html, strlen(html));
   printf("200 POST /guestbook\n");
}

/*
    This is the routing function for POST calls,
    Can be extended by adding newer POST methods and its handlers
*/
int handle_app_post_routes(char* path, int client_socket)
{
    if (strcmp(path, GUESTBOOK_ROUTE) == 0)
    {
        handle_new_guest_remarks(client_socket);
        return METHOD_HANDLED;
    }

    // add new app routes here
    return METHOD_NOT_HANDLED;
}

void handle_post_method(char* path, int client_socket)
{
    // it can only be for app methods
    handle_app_post_routes(path, client_socket);
}

void handle_unimplemented_method(int client_socket)
{
    send(client_socket, unimplemented_content, strlen(unimplemented_content), 0);
}

void handle_http_method(char* method_buffer, int client_socket)
{
    char* method;
    char* path;
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
        // we read rest of the header lines and throw them away
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

        // handles only 1 request per client connection right now
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