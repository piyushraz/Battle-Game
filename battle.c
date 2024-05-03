#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#ifndef PORT
    #define PORT 51621
#endif

#define MAX_NAME_LEN 20
#define MAX_BUF 512
#define MAX_MESSAGE_LEN 20

struct client {
    int fd; 
    struct in_addr ipaddr; 
    struct client *next; 
    char name[MAX_NAME_LEN]; 
    int in_game; 
    struct client *last_opponent; 
    int hitpoints; 
    int powermoves; 
    int is_turn; 
    int name_entered;
    int is_messaging; 
    char message[MAX_MESSAGE_LEN + 1]; 
    int message_length;
    time_t start_time; 
    int time_left; 
};

int bindandlisten(void);
static struct client *addclient(struct client *top, int fd, struct in_addr addr);
struct client *removeclient(struct client *top, int fd);
void broadcast(struct client *top, char *s, int size);
int handleclient(struct client *p, struct client *top);
struct client *matchmaker(struct client *top, struct client *new_client);
void start_match(struct client *p, struct client *opponent, struct client **top);
void switch_turn(struct client *p, struct client *opponent);
void init_battle(struct client *p);
void disconnect_client(struct client *p, struct client **top, fd_set *allset, int *maxfd);

/***
Helps setup the TCP server on the PORT 51621 (My Port).
Creates a socket and binds it to a PORT, and listens for any connections.
If success, it listens on the port, and returns a file descriptor.
On failure it prints error to std-error and exits.
***/
int bindandlisten(void) {
    int listenfd;
    struct sockaddr_in serveraddr;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
        perror("setsockopt");
        exit(1);
    }
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    serveraddr.sin_port = htons(PORT); 
    if (bind(listenfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(listenfd, 5) < 0) { 
        perror("listen");
        exit(1);
    }
    printf("Server listening on port %d\n", PORT);
    return listenfd;
}

/***
Initalizes a server state and handles multiple TCP connections
Uses PORT and stores client IP address to server, and handles 
disconnection as well. Select is used for handling multiple 
clients, and new client's file descriptor is added. 
Takes addclient, handleclient, and disconnect_client to process
clients.
***/
int main(void) {
    int listenfd, newfd, maxfd;
    fd_set allset, rset;
    struct client *head = NULL, *p, *next_p;
    struct sockaddr_in clientaddr;
    socklen_t addrlen;
    listenfd = bindandlisten(); 
    maxfd = listenfd;
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    while (1) {
        rset = allset; 
        if (select(maxfd + 1, &rset, NULL, NULL, NULL) < 0) {
            perror("select");
            continue; 
        }
        if (FD_ISSET(listenfd, &rset)) { 
            addrlen = sizeof(clientaddr);
            newfd = accept(listenfd, (struct sockaddr *)&clientaddr, &addrlen);
            if (newfd < 0) {
                perror("accept");
                continue;
            }
            printf("New connection from %s\n", inet_ntoa(clientaddr.sin_addr));
            head = addclient(head, newfd, clientaddr.sin_addr);
            FD_SET(newfd, &allset);
            if (newfd > maxfd) maxfd = newfd;
        }
        for (p = head; p != NULL; p = next_p) { 
            next_p = p->next; 
            if (FD_ISSET(p->fd, &rset)) {
                if (handleclient(p, head) == -1) {
                    disconnect_client(p, &head, &allset, &maxfd); 
                }
            }
        }
    }
    close(listenfd);
    return 0;
}

/***
Adds a new client to the linked list of clients with a 
welcome message. Dynamically allocates memory for a new client.
Handles error if memory allocations fails.
struct client *top -> First client in linked list 
int fd -> File descriptor of client connection
struct in_addr addr -> IP address of new client
Return -> A pointer to top which is first client in linked list
***/
static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
    struct client *newclient = (struct client *)malloc(sizeof(struct client));
    if (newclient == NULL) {
        perror("malloc");
        exit(1);
    }
    newclient->is_messaging = 0;
    newclient->message[0] = '\0'; 
    newclient->message_length = 0;
    newclient->name_entered = 0;
    newclient->fd = fd;
    newclient->ipaddr = addr;
    newclient->in_game = 0;
    newclient->last_opponent = NULL;
    memset(newclient->name, 0, sizeof(newclient->name));
    newclient->next = NULL;  
    const char *welcome = "Welcome! Please enter your name: ";
    if (write(fd, welcome, strlen(welcome)) < 0) {
        perror("write");
        free(newclient);
        return top;
    }
    if (top == NULL) {
        return newclient;
    } else {
        struct client *cur = top;
        while (cur->next != NULL) {
            cur = cur->next;
        }
        cur->next = newclient;
    }
    return top;
}

/***
Majority of the work and game logic happens here. This function handles 
client features, inputs, game state, and messages. It manages player and
the opponent, switching turns, and reading inputs at 1 character at a time.
1. Ensures unique names and prevents duplicates
2. Handles attack, powermove, speak, timeleft
3. Switching turns, and 30 second timer
4. Sending client(s) information through snprintf and broadcast
struct client *p -> Pointer to a clients struct information
struct client *top -> Front of a client list
Return -> Return 0 on success, otherwise -1 for client disconnection
***/
int handleclient(struct client *p, struct client *top) {
    char buf[256], outbuf[512];
    int len;
    len = read(p->fd, buf, 1);
    if (len > 0) {
        buf[len] = '\0'; 
        if (!p->in_game && p->name_entered && p->last_opponent == NULL) {
            if (!p->is_turn) {
                return 0; 
            }
        }
        if (p->in_game) {
            if (!p->is_turn) {
                return 0; 
            }
            struct client *opponent = p->last_opponent;
            time_t now = time(NULL);
            if (p->is_turn && p->in_game) {
                double elapsed = difftime(now, p->start_time);
                if (elapsed >= p->time_left) {
                    if (opponent) {
                        snprintf(outbuf, sizeof(outbuf), "\nTime's up! %s didn't make a move in time. 0 damage dealt. It's now your turn.\n", p->name);
                        write(opponent->fd, outbuf, strlen(outbuf));
                        snprintf(outbuf, sizeof(outbuf), "\nTime's up! You didnt attack. Wait till your turn.\n");
                        write(p->fd, outbuf, strlen(outbuf));
                        p->is_turn = 0;
                        opponent->is_turn = 1;
                        opponent->time_left = 30;
                        opponent->start_time = now;
                        p->time_left = 30; 
                        p->start_time = now;
                        switch_turn(opponent, p);
                    }
                    return 0;
                }
            } else if (!p->is_turn) {
                double elapsed = difftime(now, opponent->start_time);
                if (elapsed >= opponent->time_left) {
                    p->is_turn = 1;
                    opponent->is_turn = 0;
                    opponent->time_left = 30;
                    opponent->start_time = now;
                    p->time_left = 30; 
                    p->start_time = now;
                    switch_turn(p, opponent);
                    return 0;
                }
            }
            if (p->is_messaging) {
                if (buf[0] == '\n') {
                    if (p->message_length > MAX_MESSAGE_LEN) {
                        write(p->fd, "Message too long! Not sent.\n", 28);
                    } else if (p->message_length > 0) {
                        snprintf(outbuf, sizeof(outbuf), "%s says: %s\n", p->name, p->message);
                        write(p->last_opponent->fd, outbuf, strlen(outbuf));
                    } else {
                        write(p->fd, "\nYou didn't say anything.\n", 25);
                    }
                    p->is_messaging = 0;
                    memset(p->message, 0, sizeof(p->message));
                    p->message_length = 0;
                    switch_turn(p, p->last_opponent);
                } else if (buf[0] != '\r') {
                    if (p->message_length < MAX_MESSAGE_LEN) {
                        p->message[p->message_length++] = buf[0];
                    } else {
                        if (p->message_length == MAX_MESSAGE_LEN) {
                            p->message_length++;  
                            write(p->fd, "\nMessage too long! Finish and hit enter.\n", 41);
                        }
                    }
                }
                return 0;
            }
            if (buf[0] == 't') {
                int remaining_time;
                if (p->is_turn) {
                    remaining_time = p->time_left - (int)difftime(time(NULL), p->start_time);
                } else if (opponent->is_turn) {
                    remaining_time = opponent->time_left - (int)difftime(time(NULL), opponent->start_time);
                } else {
                    remaining_time = 30;
                }
                if (remaining_time < 0) {
                    remaining_time = 0; 
                }
                snprintf(outbuf, sizeof(outbuf), "\nRemaining time: %d seconds.\n", remaining_time);
                write(p->fd, outbuf, strlen(outbuf));
                return 0;
            }
            if (buf[0] == 's' && !p->is_messaging) {
                p->is_messaging = 1;
                p->message_length = 0;
                memset(p->message, 0, sizeof(p->message));
                write(p->fd, "\nSpeak (max 20 chars): ", 22);
                return 0;
            } else if ((buf[0] == 'a' || (buf[0] == 'p' && p->powermoves > 0)) && !p->is_messaging) {
                int damage;
                if (buf[0] == 'a') {
                    damage = (rand() % 5) + 2; 
                } else {
                    if (buf[0] == 'p' && p->powermoves == 0) {
                        snprintf(outbuf, sizeof(outbuf), "No more power moves left!\n");
                        write(p->fd, outbuf, strlen(outbuf));
                        return 0;
                    } else {
                        if (rand() % 2 == 0) {
                            damage = ((rand() % 5) + 2) * 3; 
                        } else {
                            damage = 0; 
                        }
                        p->powermoves--;
                    }
                }
                opponent->hitpoints -= damage;
                snprintf(outbuf, sizeof(outbuf), "\nYou attacked %s for %d damage.\n", opponent->name, damage);
                write(p->fd, outbuf, strlen(outbuf));
                snprintf(outbuf, sizeof(outbuf), "%s attacked you for %d damage.\n", p->name, damage);
                write(opponent->fd, outbuf, strlen(outbuf));
                if (buf[0] == 'p' && damage == 0) {
                    snprintf(outbuf, sizeof(outbuf), "Your power move missed!\n");
                    write(p->fd, outbuf, strlen(outbuf));
                    snprintf(outbuf, sizeof(outbuf), "%s's power move missed!\n", p->name);
                    write(opponent->fd, outbuf, strlen(outbuf));
                }
                if ((opponent->hitpoints <= 0) && opponent->in_game) {
                    snprintf(outbuf, sizeof(outbuf), "You defeated %s! Congratulations!\n", opponent->name);
                    write(p->fd, outbuf, strlen(outbuf));
                    snprintf(outbuf, sizeof(outbuf), "%s defeated you. Better luck next time!\n", p->name);
                    write(opponent->fd, outbuf, strlen(outbuf));
                    p->in_game = 0;
                    opponent->in_game = 0;
                    p->last_opponent = opponent; 
                    opponent->last_opponent = p; 
                    snprintf(outbuf, sizeof(outbuf), "%s has entered the arena.\n", opponent->name);
                    broadcast(top, outbuf, strlen(outbuf));
                    snprintf(outbuf, sizeof(outbuf), "%s has entered the arena.\n", p->name);
                    broadcast(top, outbuf, strlen(outbuf));
                    struct client *new_opponent_for_p = matchmaker(top, p);
                    if (new_opponent_for_p != NULL) {
                        start_match(p, new_opponent_for_p, &top);
                    } else {
                        char *wait_msg = "You are awaiting an opponent...\n";
                        write(p->fd, wait_msg, strlen(wait_msg));
                    }
                    struct client *new_opponent_for_opponent = matchmaker(top, opponent);
                    if (new_opponent_for_opponent != NULL) {
                        start_match(opponent, new_opponent_for_opponent, &top);
                    } else {
                        char *wait_msg = "You are awaiting an opponent...\n";
                        write(opponent->fd, wait_msg, strlen(wait_msg));
                    }
                    p->name_entered = 1;
                    opponent->name_entered = 1;
                } else if ((p->hitpoints <= 0) && p->in_game) {
                    snprintf(outbuf, sizeof(outbuf), "%s defeated you. Better luck next time!\n", opponent->name);
                    write(p->fd, outbuf, strlen(outbuf));
                    snprintf(outbuf, sizeof(outbuf), "You defeated %s! Congratulations!\n", p->name);
                    write(opponent->fd, outbuf, strlen(outbuf));
                    p->in_game = 0;
                    opponent->in_game = 0;
                    p->last_opponent = opponent;
                    opponent->last_opponent = p;
                    snprintf(outbuf, sizeof(outbuf), "%s has entered the arena.\n", p->name);
                    broadcast(top, outbuf, strlen(outbuf));
                    snprintf(outbuf, sizeof(outbuf), "%s has entered the arena.\n", opponent->name);
                    broadcast(top, outbuf, strlen(outbuf));
                    struct client *new_opponent_for_p = matchmaker(top, p);
                    if (new_opponent_for_p != NULL) {
                        start_match(p, new_opponent_for_p, &top);
                    } else {
                        char *wait_msg = "You are awaiting an opponent...\n";
                        write(p->fd, wait_msg, strlen(wait_msg));
                    }
                    struct client *new_opponent_for_opponent = matchmaker(top, opponent);
                    if (new_opponent_for_opponent != NULL) {
                        start_match(opponent, new_opponent_for_opponent, &top);
                    } else {
                        char *wait_msg = "You are awaiting an opponent...\n";
                        write(opponent->fd, wait_msg, strlen(wait_msg));
                    }
                    p->name_entered = 1;
                    opponent->name_entered = 1;
                } else {
                    p->is_turn = 0;
                    opponent->is_turn = 1;
                    snprintf(outbuf, sizeof(outbuf), "\nIt's your turn\n\nYour hitpoints: %d\nYour powermoves: %d\n\n%s's hitpoints: %d\n\n(a)ttack\n(p)owermove\n(s)peak\n(t)ime left\n\n", opponent->hitpoints, opponent->powermoves, p->name, p->hitpoints);
                    write(opponent->fd, outbuf, strlen(outbuf));
                    snprintf(outbuf, sizeof(outbuf), "Waiting for %s to make a move...\n", opponent->name);
                    write(p->fd, outbuf, strlen(outbuf));
                    opponent->start_time = time(NULL);
                    opponent->time_left = 30;
                }
            } else {
                return 0;
            }
        } else {
            if (!p->name_entered) {
                if (buf[0] == '\n' || buf[0] == '\r') {
                    if (strlen(p->name) == 0) {
                        char *prompt = "Name cannot be empty, please enter your name: ";
                        write(p->fd, prompt, strlen(prompt));
                        return 0;
                    }
                    struct client *existing_client = top;
                    while (existing_client != NULL) {
                        if (existing_client != p && strcmp(existing_client->name, p->name) == 0) {
                            char *prompt = "Name already taken, please enter a different name: ";
                            write(p->fd, prompt, strlen(prompt));
                            memset(p->name, 0, sizeof(p->name));
                            return 0;
                        }
                        existing_client = existing_client->next;
                    }
                    if (p->name_entered == 0) {
                        snprintf(outbuf, sizeof(outbuf), "%s has entered the arena.\n", p->name);
                        printf("%s has joined the server.\n", p->name);
                        broadcast(top, outbuf, strlen(outbuf));
                    }
                    p->name_entered = 1;
                    char *wait_msg = "You are awaiting an opponent...\n";
                    write(p->fd, wait_msg, strlen(wait_msg));
                    struct client *opponent = matchmaker(top, p);
                    if (opponent != NULL && opponent->name_entered) {
                        start_match(p, opponent, &top);
                    }
                } else {
                    if (!p->in_game) {
                        strncat(p->name, buf, 1);
                    }
                }
            }
        }
    } else if (len == 0) {
        return -1;
    } else {
        perror("read");
    }
    return 0;
}

/***
Finding a opponent for a player in the list of clients connected
Goes through list of clients looking for possible opponents avoiding
rematches and ensuring constantly and fair matchmaking. Clients
are not matched with someone who they just played.
If no opponent can be found, client waits till suitable opponent 
exists.
struct client *top -> Top of the client list (linked list)
struct client *matchmake -> Potential opponent to matchmake with
Return -> Returns struct client pointer for opponent found, otherwise NULL.
***/
struct client *matchmaker(struct client *top, struct client *matchmake) {
    struct client *cur = top;
    struct client *b_opponent = NULL;
    while (cur != NULL) {
        if (cur != matchmake && !cur->in_game && cur->name_entered) {
            if (matchmake->last_opponent != cur && cur->last_opponent != matchmake) {
                b_opponent = cur;
                break; 
            }
        }
        cur = cur->next;
    }
    return b_opponent; 
}

/***
Creates a match and randomly assignes who goes first with inital game settings.
Both clients get set for battle with in game status, and are both notified.
Each client has 30 seconds to attack during their turn.
struct client *p -> Client for the match
struct client *opponent -> Another client for the match
struct client **top -> First client in linked list
***/
void start_match(struct client *p, struct client *opponent, struct client **top) {
    init_battle(p);
    init_battle(opponent);
    p->in_game = 1;
    opponent->in_game = 1;
    p->last_opponent = opponent;
    opponent->last_opponent = p;
    char msg[512];
    snprintf(msg, sizeof(msg), "Match started! Remember during your turn you have 30 seconds to attack.\n");
    write(p->fd, msg, strlen(msg));
    write(opponent->fd, msg, strlen(msg));
    char your_turn_msg[512];
    char opponent_turn_msg[512];
    if (rand() % 2 == 0) {
        p->start_time = time(NULL);
        p->time_left = 30; 
        p->is_turn = 1;
        opponent->is_turn = 0;
        snprintf(your_turn_msg, sizeof(your_turn_msg),
                 "You are matched with %s! Let the battle begin!\nYou go first.\n",
                 opponent->name);
        snprintf(opponent_turn_msg, sizeof(opponent_turn_msg),
                 "You are matched with %s! Let the battle begin!\nYou go second.\n",
                 p->name);
    } else {
        opponent->start_time = time(NULL);
        opponent->time_left = 30; 
        p->is_turn = 0;
        opponent->is_turn = 1;
        snprintf(your_turn_msg, sizeof(your_turn_msg),
                 "You are matched with %s! Let the battle begin!\nYou go second.\n",
                 opponent->name);
        snprintf(opponent_turn_msg, sizeof(opponent_turn_msg),
                 "You are matched with %s! Let the battle begin!\nYou go first.\n",
                 p->name);
    }
    write(p->fd, your_turn_msg, strlen(your_turn_msg));
    write(opponent->fd, opponent_turn_msg, strlen(opponent_turn_msg));
    switch_turn(p, opponent);
    switch_turn(opponent, p);
}

/***
Notifies turn switch to clients.
Sends a message to client whose turn it si and displays hitpoints, powermoves.
Also, displays player actions (a)ttack, (p)owermove, (s)peak, (t)ime left
struct client *p -> Used for client to display their info.
struct client *opponent -> Used to display opponent hitpoints
***/
void switch_turn(struct client *p, struct client *opponent) {
    char prompt[512];
    snprintf(prompt, sizeof(prompt), "\n\nYour hitpoints: %d\nYour powermoves: %d\nOpponent's hitpoints: %d\n\n(a)ttack\n(p)owermove\n(s)peak\n(t)ime left\n\n", p->hitpoints, p->powermoves, opponent->hitpoints);
    write(p->fd, prompt, strlen(prompt));
}

/***
Sets up game with hitpoints for both users opponent and player
Randomly assignes hitpoints to both users (20-30) and powermoves (1-3)
struct client *p -> CLient pointer to whose hitpoints and power moves
are setup.
***/
void init_battle(struct client *p) {
    p->hitpoints = (rand() % 11) + 20;
    p->powermoves = (rand() % 3) + 1;
}

/***
Handles the disconnecting of a client when user leaves the server mid-game
or while waiting. If client that disconnect is in game, the opponent is granted
a win and notified that their opponent dropped or left. Remove client is called to
remove the dropped client from linked list.
Also a when client drops, it is broadcasted to the entire connected clients to notify
them as well. Memory is freed also that was allocated dynamically to that client
struct client *p -> Pointer of the disconnecting client
struct cliet **top -> Pointer to first client that needs to be updated
fd_set *allset -> Pointer to file descriptor that needs to be removed of dropped client
int *maxfd -> File descriptor number that may need to the updated
***/
void disconnect_client(struct client *p, struct client **top, fd_set *allset, int *maxfd) {
    p->time_left = 30;
    if (p == NULL || p->fd < 0) return; 
    char addrbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(p->ipaddr), addrbuf, INET_ADDRSTRLEN);
    printf("Connection from %s disconnected.\n", addrbuf);
    struct client *opponent = p->last_opponent;
    if (p->in_game && p->last_opponent != NULL) {
        struct client *opponent = p->last_opponent;
        char msg[MAX_BUF];
        snprintf(msg, sizeof(msg), "%s has dropped. You Won! You are back in the arena waiting for a new opponent.\n", p->name);
        write(opponent->fd, msg, strlen(msg));
        opponent->in_game = 0;
        opponent->last_opponent = NULL;
        char *wait_msg = "You are awaiting an opponent...\n";
        write(opponent->fd, wait_msg, strlen(wait_msg));
    }
    if (strlen(p->name) > 0) {
        char outbuf[MAX_BUF];
        snprintf(outbuf, sizeof(outbuf), "%s has left the arena.\n", p->name);
        broadcast(*top, outbuf, strlen(outbuf));
    }
    *top = removeclient(*top, p->fd);
    p->name_entered = 0;
    close(p->fd);
    FD_CLR(p->fd, allset);
    p->fd = -1;
    if (*maxfd == p->fd) {
        int new_maxfd = 0;
        for (struct client *temp = *top; temp; temp = temp->next) {
            if (temp->fd > new_maxfd) new_maxfd = temp->fd;
        }
        *maxfd = new_maxfd;
    }
    free(p);
    if (opponent != NULL) {
        struct client *new_opponent = matchmaker(*top, opponent);
        if (new_opponent != NULL) {
            start_match(opponent, new_opponent, top);
        }
    }
}

/***
Removes a client from linked list from file descriptor. Goes through linked list 
and finds client with correct file descriptor and removes it from list.
Linked list is then adjust to prevent issues and gaps. Usually called when client
leaves or disconnects from server.
struct client *top -> Pointer to first client in list
int fd -> File descriptor of client being removed
Return -> Update list and pointer to first client in list
***/
struct client *removeclient(struct client *top, int fd) {
    struct client *p, *prev = NULL;
    for (p = top; p != NULL; prev = p, p = p->next) {
        if (p->fd == fd) {
            if (prev == NULL) {
                top = p->next;
            } else {
                prev->next = p->next;
            }
            break;
        }
    }
    return top;
}

/***
Sends message to all clients connected
Iterates through linked list and sends message to everyone
struct client *top -> Pointer to first client in linked list
char *s -> Message being broadcasted
int size -> Length of message being sent
***/
void broadcast(struct client *top, char *s, int size) {
    struct client *p;
    for (p = top; p != NULL; p = p->next) {
        if (p->fd >= 0 && p->name_entered) { 
            if (write(p->fd, s, size) == -1) {
                perror("write");
            }
        }
    }
}
