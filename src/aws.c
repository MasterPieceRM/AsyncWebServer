// SPDX-License-Identifier: BSD-3-Clause

#include <aio.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aws.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/util.h"
#include "utils/w_epoll.h"
#define MAXEVENTS 100

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

void set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    flags |= O_NONBLOCK;

    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }
}

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len) {
    struct connection *conn = (struct connection *)p->data;

    memcpy(conn->request_path, buf, len);
    conn->request_path[len] = '\0';
    conn->have_path = 1;

    return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn) {
    const char *header_format = "HTTP/1.0 200 OK\r\n"
                                "Content-Length: %ld\r\n"
                                "Content-Type: application/octet-stream\r\n"
                                "Connection: close\r\n"
                                "\r\n";
    int header_len = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
                              header_format, conn->file_size);
    conn->send_len = header_len;
}

static void connection_prepare_send_404(struct connection *conn) {
    /* Prepare the connection buffer to send the 404 header. */

    // Prepare the 404 response
    const char *response = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/html\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "<html><body><h1>404 Not Found</h1></body></html>\n";

    // send the 404 response
    ssize_t bytes_sent;
    do {
        bytes_sent = send(conn->sockfd, response, strlen(response), 0);
    } while (bytes_sent > 0);

    if (bytes_sent <= 0) {
        // Handle error: failed to send the 404 response
        perror("send");
        connection_remove(conn);
    }
}

static enum resource_type
connection_get_resource_type(struct connection *conn) {
    /* Get resource type depending on request path/filename. Filename
     * should point to the static or dynamic folder.
     */

    // Check if the request path points to the static folder
    if (strncmp(conn->request_path, "/static/", 8) == 0) {
        conn->res_type = RESOURCE_TYPE_STATIC;
        return RESOURCE_TYPE_STATIC;
    }

    // Check if the request path points to the dynamic folder
    if (strncmp(conn->request_path, "/dynamic/", 9) == 0) {
        conn->res_type = RESOURCE_TYPE_DYNAMIC;
        return RESOURCE_TYPE_DYNAMIC;
    }

    // If the request path doesn't point to either folder, return
    // RESOURCE_TYPE_NONE
    return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd) {
    /* Initialize connection structure on given socket. */

    // Allocate memory for the connection structure
    struct connection *conn = malloc(sizeof(struct connection));
    if (conn == NULL) {
        perror("malloc");
        return NULL;
    }

    // Initialize the connection fields
    conn->sockfd = sockfd;
    conn->fd = -1;
    conn->file_size = 0;
    conn->file_pos = 0;
    conn->send_len = 0;
    conn->send_pos = 0;
    conn->recv_len = 0;
    conn->state = STATE_INITIAL;
    conn->recv_buffer[0] = '\0';
    conn->send_buffer[0] = '\0';
    conn->eventfd = eventfd(0, EFD_NONBLOCK);

    return conn;
}

void connection_start_async_io(struct connection *conn) {
    /* Start asynchronous operation (read from file).
     * Use io_submit(2) & friends for reading data asynchronously.
     */

    io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer,
                  sizeof(conn->send_buffer), conn->file_pos);

    // Submit the IO request
    conn->piocb[0] = &conn->iocb;
    io_set_eventfd(&conn->iocb, conn->eventfd);
    w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);
    w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
    if (io_submit(ctx, 1, conn->piocb) != 1) {
        perror("io_submit");
        io_destroy(ctx);
        return;
    }

    conn->ctx = ctx;
}

void connection_remove(struct connection *conn) {
    /* Remove connection handler. */

    // Close the socket
    if (conn->sockfd != -1) {
        close(conn->sockfd);
        conn->sockfd = -1;
    }

    // Close the file descriptor if it's open
    if (conn->fd != -1) {
        close(conn->fd);
        conn->fd = -1;
    }

    if (conn != NULL) {
        // Free the connection handler
        free(conn);
    }
    dlog(LOG_INFO, "Connection removed\n");
}
void handle_new_connection(void) {
    int client_socket = accept(listenfd, NULL, NULL);
    if (client_socket == -1) {
        perror("accept");
        return;
    }
    set_nonblocking(client_socket);
    struct connection *conn = connection_create(client_socket);
    if (conn == NULL) {
        close(client_socket);
        return;
    }

    w_epoll_add_ptr_in(epollfd, conn->sockfd, conn);
}
void receive_data(struct connection *conn) {
    /* Receive message on socket.
     * Store message in recv_buffer in struct connection.
     */
    char buffer[BUFSIZ];
    ssize_t bytes;
    do {
        bytes = recv(conn->sockfd, buffer, BUFSIZ, 0);
        strcat(conn->recv_buffer, buffer);
        conn->recv_len += bytes;
    } while (bytes > 0);

    // Process the received data...
    if (parse_header(conn) == -1) {
        // The header is invalid, so send a 404 response
        conn->state = STATE_SENDING_404;
        return;
    } else {
        // The header is valid, so open the file
        if (connection_open_file(conn) == -1) {
            // An error occurred while opening the file, so send a 404
            // response
            conn->state = STATE_SENDING_404;
            return;
        }
    }
    conn->state = STATE_REQUEST_RECEIVED;
}
int connection_open_file(struct connection *conn) {
    /* Open file and update connection fields. */

    // Open the file
    int fd = open(conn->filename, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Get the file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return -1;
    }

    conn->fd = fd;
    conn->file_size = st.st_size;
    conn->file_pos = 0;

    return 0;
}
void connection_complete_async_io(struct connection *conn) {
    /* Complete asynchronous operation; operation returns successfully.
     * Prepare socket for sending.
     */
    // Create an array to hold the completed events
    struct io_event events[20];

    // Get the completed events
    int num_events = io_getevents(conn->ctx, 1, 1, events, NULL);
    dlog(LOG_INFO, "Num events: %d\n", num_events);
    if (num_events == -1) {
        perror("io_getevents");
        return;
    }
    ssize_t bytes = events[0].res;

    if (bytes < 0) {
        perror("io_getevents");
        return;
    }

    conn->send_len = bytes;
    conn->send_pos = 0;
    conn->file_pos += bytes;
    w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
    w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
    conn->state = STATE_SENDING_DYNAMIC;
}

int parse_header(struct connection *conn) {
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);

    parser.data = conn; // Set connection as parser data

    http_parser_settings settings_on_path = {.on_message_begin = 0,
                                             .on_header_field = 0,
                                             .on_header_value = 0,
                                             .on_path = aws_on_path_cb,
                                             .on_url = 0,
                                             .on_fragment = 0,
                                             .on_query_string = 0,
                                             .on_body = 0,
                                             .on_headers_complete = 0,
                                             .on_message_complete = 0};

    http_parser_execute(&parser, &settings_on_path, conn->recv_buffer,
                        conn->recv_len);

    if (conn->have_path) {
        // Check conn->request_path for the parsed path
        // Update conn->res_type based on the path (static or dynamic)
        // Set other necessary fields based on the path
        // Example:
        if (connection_get_resource_type(conn) == RESOURCE_TYPE_STATIC) {
            snprintf(conn->filename, BUFSIZ, "%s%s", AWS_DOCUMENT_ROOT,
                     conn->request_path + 1);
        } else if (connection_get_resource_type(conn) ==
                   RESOURCE_TYPE_DYNAMIC) {
            snprintf(conn->filename, BUFSIZ, "%s%s", AWS_DOCUMENT_ROOT,
                     conn->request_path + 1);
        } else {
            // Invalid path, send 404
            connection_prepare_send_404(conn);
            return -1;
        }

        return 0; // Successfully parsed the header
    }
    // Parser needs more data to extract the path
    return 1;
}

enum connection_state connection_send_static(struct connection *conn) {
    /* Send static data using sendfile(2). */
    // Send the file content
    off_t offset = conn->file_pos;
    int bytes_sent = sendfile(conn->sockfd, conn->fd, &offset,
                              conn->file_size - conn->file_pos);
    if (bytes_sent <= 0) {
        if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
            perror("sendfile");
            conn->state = STATE_SENDING_404;
            return conn->state;
        }
    } else {
        conn->file_pos += bytes_sent;
        if (conn->file_pos == conn->file_size) {
            // All data has been sent, so return the next state
            conn->state = STATE_DATA_SENT;
            return STATE_DATA_SENT;
        } else {
            conn->state = STATE_SENDING_DATA;
            return STATE_SENDING_DATA;
        }
    }
    return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn) {
    /* Send as much data as possible from the connection send buffer.
     * Returns the number of bytes sent or -1 if an error occurred.
     */
    int bytes_sent, total_bytes_sent = 0;
    do {
        bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos,
                          conn->send_len - conn->send_pos, 0);
        conn->send_pos += bytes_sent;
        total_bytes_sent += bytes_sent;
    } while (conn->send_pos < conn->send_len);
    return total_bytes_sent;
}

// int connection_send_dynamic(struct connection *conn) {
//     /* Read data asynchronously.
//      * Returns 0 on success and -1 on error.
//      */

//     // Initialize the control block
//     memset(&conn->iocb, 0, sizeof(conn->iocb));
//     conn->iocb.aio_fildes = conn->fd;
//     conn->iocb.u.c.buf = (void *)(uintptr_t)conn->send_buffer;
//     conn->iocb.u.c.nbytes = conn->send_len;
//     conn->iocb.u.c.offset = conn->file_pos;
//     conn->iocb.aio_lio_opcode = IO_CMD_PREAD;

//     // Prepare the iocb pointer array
//     conn->piocb[0] = &conn->iocb;

//     // Submit the asynchronous read request
//     if (io_submit(conn->ctx, 1, conn->piocb) == -1) {
//         perror("io_submit");
//         return -1;
//     }

//     // Update the file position
//     conn->file_pos += BUFSIZ;

//     return 0;
// }

void handle_input(struct connection *conn) {
    switch (conn->state) {
    case STATE_INITIAL:
        // TODO: Handle initial state logic, e.g., set up for receiving data
        conn->state = STATE_RECEIVING_DATA;
        receive_data(conn);
        w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
        break;

    case STATE_ASYNC_ONGOING:
        connection_complete_async_io(conn);
        break;
    default:
        // Handle other states if necessary
        printf("Unexpected state %d\n", conn->state);
        break;
    }
}

void handle_output(struct connection *conn) {
    dlog(LOG_INFO, "Handle output\n");
    switch (conn->state) {
    case STATE_SENDING_404:
        connection_prepare_send_404(conn);
        conn->state = STATE_404_SENT;
        break;

    case STATE_SENDING_DYNAMIC:
        connection_send_data(conn);
        if (conn->file_pos == conn->file_size) {
            conn->state = STATE_DATA_SENT;
        } else {
            conn->state = STATE_SENDING_DATA;
        }
        break;

    case STATE_ASYNC_ONGOING:
        connection_start_async_io(conn);
        break;

    case STATE_REQUEST_RECEIVED:
        connection_prepare_send_reply_header(conn);
        conn->state = STATE_SENDING_HEADER;
        break;

    case STATE_SENDING_HEADER:
        connection_send_data(conn);
        conn->state = STATE_HEADER_SENT;
        break;

    case STATE_404_SENT:
        dlog(LOG_INFO, "Data sent 404\n");
        connection_remove(conn);
        break;

    case STATE_HEADER_SENT:
        dlog(LOG_INFO, "Data sent header\n");
        if (conn->res_type == RESOURCE_TYPE_STATIC) {
            conn->state = STATE_SENDING_DATA;
        } else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
            conn->state = STATE_SENDING_DATA;
        } else {
            conn->state = STATE_SENDING_404;
        }
        break;

    case STATE_DATA_SENT:
        dlog(LOG_INFO, "Data sent\n");
        connection_remove(conn);
        break;

    case STATE_SENDING_DATA:
        dlog(LOG_INFO, "Sending data\n");
        if (conn->res_type == RESOURCE_TYPE_STATIC) {
            conn->state = connection_send_static(conn);
        } else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
            conn->state = STATE_ASYNC_ONGOING;
            connection_start_async_io(conn);
        } else {
            conn->state = STATE_SENDING_404;
        }
        break;

    default:
        // Handle other states if necessary
        printf("Unexpected state %d\n", conn->state);
        break;
    }
}

void handle_client(uint32_t events, struct connection *conn) {
    if (events & EPOLLIN) {
        handle_input(conn);
        // dlog(LOG_INFO, "INPUT\n");
        // dlog(LOG_INFO, "Request path: %s\n", conn->request_path);
        // dlog(LOG_INFO, "Resource type: %d\n", conn->res_type);
        // dlog(LOG_INFO, "Have path: %d\n", conn->have_path);
        // dlog(LOG_INFO, "State: %d\n", conn->state);
        // dlog(LOG_INFO, "Filename: %s\n", conn->filename);
        // dlog(LOG_INFO, "File size: %ld\n", conn->file_size);
        // dlog(LOG_INFO, "File pos: %ld\n", conn->file_pos);
        // dlog(LOG_INFO, "Send len: %ld\n", conn->send_len);
        // dlog(LOG_INFO, "Send pos: %ld\n", conn->send_pos);
        // dlog(LOG_INFO, "Recv len: %ld\n", conn->recv_len);
        // dlog(LOG_INFO, "Recv buffer: %s\n", conn->recv_buffer);
        // dlog(LOG_INFO, "Send buffer: %s\n", conn->send_buffer);
        // dlog(LOG_INFO, "Request parser type: %d\n",
        // conn->request_parser.type); dlog(LOG_INFO, "Request parser flags:
        // %d\n ",
        //      conn->request_parser.flags);
        // dlog(LOG_INFO, "Request parser state: %d\n",
        //      conn->request_parser.state);
        // dlog(LOG_INFO, "Final HANDLE_INPUT\n");
    }

    if (events & EPOLLOUT) {
        // dlog(LOG_INFO, "OUTPUT\n");
        // dlog(LOG_INFO, "Request path: %s\n", conn->request_path);
        // dlog(LOG_INFO, "Resource type: %d\n", conn->res_type);
        // dlog(LOG_INFO, "Have path: %d\n", conn->have_path);
        // dlog(LOG_INFO, "State: %d\n", conn->state);
        // dlog(LOG_INFO, "Filename: %s\n", conn->filename);
        // dlog(LOG_INFO, "File size: %ld\n", conn->file_size);
        // dlog(LOG_INFO, "File pos: %ld\n", conn->file_pos);
        // dlog(LOG_INFO, "Send len: %ld\n", conn->send_len);
        // dlog(LOG_INFO, "Send pos: %ld\n", conn->send_pos);
        // dlog(LOG_INFO, "Recv len: %ld\n", conn->recv_len);
        // dlog(LOG_INFO, "Recv buffer: %s\n", conn->recv_buffer);
        // dlog(LOG_INFO, "Send buffer: %s\n", conn->send_buffer);
        // dlog(LOG_INFO, "Request parser type: %d\n",
        // conn->request_parser.type); dlog(LOG_INFO, "Request parser flags:
        // %d\n ",
        //      conn->request_parser.flags);
        // dlog(LOG_INFO, "Request parser state: %d\n",
        //      conn->request_parser.state);
        handle_output(conn);
    }

    if (events & (EPOLLHUP | EPOLLERR)) {
        dlog(LOG_INFO, "HUP/ERR\n");
        connection_remove(conn);
    }
}

int main(void) {
    int rc;

    /* TODO: Initialize asynchronous operations. */
    ctx = 0;
    if (io_setup(1, &ctx) != 0) {
        perror("io_setup");
        return -1;
    }

    /* TODO: Initialize multiplexing. */
    epollfd = w_epoll_create();
    if (epollfd < 0) {
        perror("w_epoll_create");
        return -1;
    }

    /* TODO: Create server socket. */
    listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
    if (listenfd < 0) {
        perror("tcp_create_listener");
        return -1;
    }

    /* TODO: Add server socket to epoll object*/
    rc = w_epoll_add_fd_in(epollfd, listenfd);
    if (rc < 0) {
        perror("w_epoll_add_fd_in");
        return -1;
    }

    /* Uncomment the following line for debugging. */
    // dlog(LOG_INFO, "Server waiting for connections on port %d\n",
    // AWS_LISTEN_PORT);

    /* server main loop */
    while (1) {
        struct epoll_event rev;

        // /* TODO: Wait for events. */
        int nfds = epoll_wait(epollfd, &rev, 1, -1);
        if (nfds < 0) {
            perror("w_epoll_wait");
            return -1;
        }

        // /* TODO: Switch event types; consider
        //  *   - new connection requests (on server socket)
        //  *   - socket communication (on connection sockets)
        //  */
        if (rev.data.fd == listenfd) {
            handle_new_connection();
            dlog(LOG_INFO, "New connection\n");
        } else {
            dlog(LOG_INFO, "Received event on fd %d\n", rev.data.fd);
            struct connection *conn = (struct connection *)rev.data.ptr;
            handle_client(rev.events, conn);
            dlog(LOG_INFO, "Final HANDLE_CLIENT\n");
        }
    }

    return 0;
}
