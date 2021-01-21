#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h> // Used for inet_aton
#include <errno.h>

#include "errno_name.h" // Used for resolving errno variable names

#define BACKLOG 128 // Maximum queue length of pending requests
#define PORT 9601 // The server port
#define MAX_MSG_LEN 64 // The maximum length of a message
#define ADDR "127.0.0.1" // The address to listen on
#define MSG_QUEUE_LEN 8

int server_socket; // The file descriptor of the socket of this server

char msg_queue[MSG_QUEUE_LEN][MAX_MSG_LEN];
int msg_queue_newest = 0; // Inclusive
int msg_queue_oldest = MSG_QUEUE_LEN - 1; // Exclusive

void add_msg_to_queue(char* msg) {
    // Ensure no buffer overflow happens. Check for >= because strlen excludes the \0 termination byte,
    // but strcpy copies that byte.
    if (strlen(msg) >= MAX_MSG_LEN) {
        fprintf(stderr, "ERROR: add_msg_to_queue: Upcoming buffer overflow for msg (size %lu) into "
                        "msg_queue[msg_queue_newest](size %lu) avoided.", strlen(msg), sizeof(msg_queue[msg_queue_newest]));
        // Shorten msg to max size of current msg_queue buffer
        msg[MAX_MSG_LEN - 1] = '\0';
    }
    strcpy(msg_queue[msg_queue_newest], msg);

    // Ring buffer is full. Move old pointer one forward in a ring buffer style
    if (msg_queue_newest == msg_queue_oldest) {
        msg_queue_oldest++;
        msg_queue_oldest %= MSG_QUEUE_LEN;
    }

    // Move pointer to next field in a ring buffer style
    msg_queue_newest++;
    msg_queue_newest %= MSG_QUEUE_LEN;
}

void print_msg_queue() {
    // msg_queue_oldest is exclusive
    int pos = msg_queue_oldest;
    printf("oldest: %d   newest: %d", msg_queue_oldest, msg_queue_newest);
    while ((pos = (pos+1) % MSG_QUEUE_LEN) != msg_queue_newest) {
        printf("%s\n", msg_queue[pos]);
    }
}

void signal_handler(int sig) {
    if (close(server_socket) < 0) {
        perror("close socket");
    }
    printf("Exit by signal %d\n", sig);
    exit(sig);
}

void init_signal_handler(void) {
    signal(SIGINT, signal_handler);  // No  2 interrupt
    signal(SIGQUIT, signal_handler); // No  3 quit
    signal(SIGILL, signal_handler);  // No  4 illegal instruction
    signal(SIGABRT, signal_handler); // No  6 abnormal termination
    signal(SIGFPE, signal_handler);  // No  8 floating point exception
    signal(SIGSEGV, signal_handler); // No 11 segfault
    signal(SIGTERM, signal_handler); // No 15 termination
}

void print_buffer(char* buf, size_t size) {
    printf("BUF_START\n");
    for (unsigned long i = 0; i < size; i++) {
        printf("%d ", buf[i]);
    }
    printf("\nBUF_END\n");
}

void cut_newline(char* text) {
    // Position of line feed \n
    size_t lf_pos = strlen(text) - 1;
    if (lf_pos >= 0 && text[lf_pos] == '\n') {
        // Remove \n
        text[lf_pos] = '\0';
        // Position of optional carriage return \r
        size_t cr_pos = lf_pos - 1;
        if (cr_pos >= 0 && text[cr_pos] == '\r') {
            // Remove \r
            text[cr_pos] = '\0';
        }
    }
}


void* client_handler(void *arg) {
    int client_socket = (int) arg;
    printf("client_handler with socket %d\n", client_socket);

    char* username_prompt =  "Hello user. Choose a username: ";
    send(client_socket, username_prompt, strlen(username_prompt), 0);

    char username[MAX_MSG_LEN];
    int err = recv(client_socket, &username, sizeof(username), 0);
    if (err < 0) {
        perror("client_handler recv");
        close(client_socket);
        return NULL;
    }
    cut_newline(username);
    printf("New user: %s\n", username);

    char server_msg[MAX_MSG_LEN];
    sprintf(server_msg, "Hello %s!\n", username);
    send(client_socket, server_msg, strlen(server_msg), 0);

    char client_msg[MAX_MSG_LEN];
    while (1) {
        // Send "send" prompt
        sprintf(server_msg, "Send: ");
        err = send(client_socket, server_msg, strlen(server_msg), 0);
        if (err < 0) {
            perror("client socket send");
            return NULL;
        }

        // Receive message
        err = recv(client_socket, &client_msg, sizeof(client_msg), 0);
        if (err < 0) {
            perror("client socket recv");
            return NULL;
        }
        cut_newline(client_msg);
        add_msg_to_queue(client_msg);

        print_msg_queue();

        // Broadcast message
        sprintf(server_msg, "\033[4mLinus\033[0m: %s\n", client_msg);
        send(client_socket, server_msg, strlen(server_msg), 0);
    }

    close(client_socket);
    return NULL;
}

void create_socket() {
    // Socket config data
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET; // Internet protocol
    server_addr.sin_port = htons(PORT); // Convert server port from host byte order into network byte order
    inet_aton(ADDR, &server_addr.sin_addr); // Convert server address into binary form

    // Create socket
    server_socket = socket(server_addr.sin_family, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("create socket");
        exit(EXIT_FAILURE);
    }
    printf("Created socket with fd %d\n", server_socket);

    // Bind socket to address
    /*int err = bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (err) {
        perror("bind socket");
        fprintf(stderr, "ERROR: Can not bind socket %d to address %s:%d\n", server_socket, ADDR, PORT);
        signal_handler(EXIT_FAILURE);
    }*/
    while (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        server_addr.sin_port = htons(ntohs(server_addr.sin_port)+1);
    }
    printf("Bind socket to %s:%d\n", ADDR, ntohs(server_addr.sin_port));

    // Listen to socket
    int err = listen(server_socket, BACKLOG);
    if (err) {
        perror("listen to socket");
        signal_handler(EXIT_FAILURE);
    }
    printf("Listen to socket with backlog %d\n", BACKLOG);
}

void accept_incoming_request() {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Accept one request
    int client_socket = accept(server_socket, (struct sockaddr*) &client_addr, &client_addr_len);
    if (client_socket < 0) {
        perror("accept connection on socket");
        printf("ERROR %s %d\n", errno_name(errno), errno);
        return;
    }
    printf("Incoming request from %s:%d with fd %d\n", inet_ntoa(client_addr.sin_addr), client_addr.sin_port, client_socket);
    

    // Set thread detach state to detached to free its resources immediately after termination
    pthread_t thread_id;
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);

    // Start thread
    int err = pthread_create(&thread_id, &thread_attr, client_handler, (void *) client_socket);
    if (err) {
        perror("pthread_create");
        close(client_socket);
        return;
    }
    printf("Started thread\n");
    
}

int main() {
    init_signal_handler();
    create_socket();
    while (1) {
        accept_incoming_request();
    }
    return 0;
}
