#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define CHAT_PORT 8080
#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define NICKNAME_SIZE 30
#define ROOM_NAME_SIZE 50

// 클라이언트 정보를 저장하는 구조체
typedef struct {
    int socket_fd;
    char nickname[NICKNAME_SIZE];
    char room_name[ROOM_NAME_SIZE]; // 클라이언트가 속한 채팅방 이름
} ClientInfo;

ClientInfo clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- 클라이언트 관리 및 브로드캐스트 함수 ---

// 서버 시스템 메시지를 특정 방에 있는 모든 클라이언트에게 전송
// 이 함수는 [SERVER] prefix를 붙여 전송하거나, 
// 클라이언트가 이미 [닉네임]을 붙여 보낸 메시지를 그대로 중계할 때 사용됩니다.
void send_system_message_to_room(const char *room_name, const char *message) {
    char full_msg[BUFFER_SIZE];
    // 시스템 메시지가 아닌 경우 (채팅 메시지 중계)는 그대로 사용
    if (strncmp(message, "[SERVER]", 8) != 0) {
        snprintf(full_msg, sizeof(full_msg), "%s", message);
    } else {
        snprintf(full_msg, sizeof(full_msg), "%s", message);
    }

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].room_name, room_name) == 0) {
            send(clients[i].socket_fd, full_msg, strlen(full_msg), 0);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 특정 닉네임을 가진 클라이언트에게 메시지 전송 (파일 전송 중계용)
int send_to_client(const char *target_nickname, const char *message) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].nickname, target_nickname) == 0) {
            send(clients[i].socket_fd, message, strlen(message), 0);
            pthread_mutex_unlock(&clients_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return 0; // 타겟 클라이언트 없음
}

// 클라이언트 목록에서 제거
void remove_client(int sock_fd) {
    char leaving_room[ROOM_NAME_SIZE] = "";
    char leaving_nickname[NICKNAME_SIZE] = "";
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket_fd == sock_fd) {
            strncpy(leaving_room, clients[i].room_name, ROOM_NAME_SIZE - 1);
            leaving_room[ROOM_NAME_SIZE - 1] = '\0';
            strncpy(leaving_nickname, clients[i].nickname, NICKNAME_SIZE - 1);
            leaving_nickname[NICKNAME_SIZE - 1] = '\0';
            
            for (int j = i; j < client_count - 1; j++) {
                clients[j] = clients[j + 1];
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(sock_fd);

    if (strlen(leaving_room) > 0) {
        printf("Client disconnected: %s from room %s\n", leaving_nickname, leaving_room);
        char leave_msg[120];
        snprintf(leave_msg, sizeof(leave_msg), "[SERVER] %s has left the chat room.", leaving_nickname);
        send_system_message_to_room(leaving_room, leave_msg);
    }
}

// 클라이언트의 룸 이름을 설정
void set_client_room(const char *nickname, const char *new_room) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].nickname, nickname) == 0) {
            strncpy(clients[i].room_name, new_room, ROOM_NAME_SIZE - 1);
            clients[i].room_name[ROOM_NAME_SIZE - 1] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// --- 클라이언트 처리 스레드 함수 ---

void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    char buffer[BUFFER_SIZE];
    char nickname[NICKNAME_SIZE] = "Unknown";
    char current_room[ROOM_NAME_SIZE] = "";
    int bytes_read;
    
    char success_msg[120]; 
    char fail_msg[120];

    // 1. 닉네임 등록
    if ((bytes_read = recv(client_sock, nickname, NICKNAME_SIZE - 1, 0)) > 0) {
        nickname[bytes_read] = '\0';
        
        pthread_mutex_lock(&clients_mutex);
        if (client_count < MAX_CLIENTS) {
            clients[client_count].socket_fd = client_sock;
            strncpy(clients[client_count].nickname, nickname, NICKNAME_SIZE - 1);
            clients[client_count].nickname[NICKNAME_SIZE - 1] = '\0';
            clients[client_count].room_name[0] = '\0';
            client_count++;
            printf("New client connected: %s\n", nickname);
            send(client_sock, "[SERVER] Please create a room (CREATE_ROOM:name) or join one (JOIN_ROOM:name)", 80, 0);
        } else {
            send(client_sock, "[SERVER] Max clients reached.", 28, 0);
            close(client_sock);
            pthread_mutex_unlock(&clients_mutex);
            return NULL;
        }
        pthread_mutex_unlock(&clients_mutex);
    } else {
        close(client_sock);
        return NULL;
    }

    // 2. 메시지 루프
    while ((bytes_read = recv(client_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';

        // --- 명령어 처리 ---
        char *command = buffer;
        char *param = strchr(buffer, ':');
        if (param) *param++ = '\0'; // 명령어와 매개변수 분리 (첫 번째 콜론 기준)

        if (strcmp(command, "CREATE_ROOM") == 0 || strcmp(command, "JOIN_ROOM") == 0) {
            if (param && strlen(param) > 0) {
                
                // 이전 방이 있다면 나가는 메시지 전송
                if (strlen(current_room) > 0) {
                     snprintf(success_msg, sizeof(success_msg), "[SERVER] %s has left room '%s'.", nickname, current_room);
                     send_system_message_to_room(current_room, success_msg);
                }
                
                strncpy(current_room, param, ROOM_NAME_SIZE - 1);
                current_room[ROOM_NAME_SIZE - 1] = '\0';
                set_client_room(nickname, current_room);

                snprintf(success_msg, sizeof(success_msg), "[SERVER] %s has entered room '%s'.", nickname, current_room);
                send_system_message_to_room(current_room, success_msg);
                printf("%s has entered room %s\n", nickname, current_room);

            } else {
                send(client_sock, "[SERVER] Invalid room command format.", 35, 0);
            }
        }
        else if (strcmp(command, "MSG") == 0) {
             // [수정]: 일반 채팅 메시지 처리. param은 "닉네임: 메시지 내용" 형태입니다.
            if (strlen(current_room) > 0 && param) {
                // param의 내용을 그대로 같은 방에 있는 클라이언트에게 중계합니다.
                send_system_message_to_room(current_room, param); 
                printf("Received message in room %s: %s\n", current_room, param);
            } else {
                send(client_sock, "[SERVER] You must join a room first.", 36, 0);
            }
        }
        else if (strncmp(command, "FILE_REQ", 8) == 0) {
             // FILE_REQ:타겟닉네임:파일명:파일크기:송신자IP:송신자Port
            char *target = strtok(param, ":");
            char *filename = strtok(NULL, ":");
            char *filesize = strtok(NULL, ":");
            char *sender_ip = strtok(NULL, ":");
            char *sender_port = strtok(NULL, ":");

            if (target && filename && filesize && sender_ip && sender_port) {
                char alert_msg[BUFFER_SIZE];
                
                snprintf(alert_msg, BUFFER_SIZE, "FILE_ALERT:%s:%s:%s:%s:%s", 
                         nickname, filename, filesize, sender_ip, sender_port);
                
                if (send_to_client(target, alert_msg)) {
                    printf("File transfer alert sent from %s to %s\n", nickname, target);
                    snprintf(success_msg, sizeof(success_msg), "[SERVER] File request sent to %s.", target);
                    send(client_sock, success_msg, strlen(success_msg), 0);
                } else {
                    snprintf(fail_msg, sizeof(fail_msg), "[SERVER] User %s not found.", target);
                    send(client_sock, fail_msg, strlen(fail_msg), 0);
                }
            } else {
                 send(client_sock, "[SERVER] File request format error.", 34, 0);
            }
        }
        else {
            send(client_sock, "[SERVER] Unknown command or protocol error.", 42, 0);
        }
    }

    // 3. 연결 종료 처리
    remove_client(client_sock);
    return NULL;
}

int main(void) {
    int server_sock, new_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    pthread_t tid;

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CHAT_PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, 3) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    printf("Chat Server running on port %d...\n", CHAT_PORT);

    while (1) {
        client_len = sizeof(client_addr);
        if ((new_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len)) < 0) {
            perror("accept failed");
            continue;
        }
        
        if (pthread_create(&tid, NULL, handle_client, &new_sock) != 0) {
            perror("thread creation failed");
            close(new_sock);
        }
        pthread_detach(tid);
    }
    close(server_sock);
    return 0;
}