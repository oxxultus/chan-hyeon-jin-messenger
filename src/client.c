#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h> // get_external_ip 함수를 위해 추가
#include <errno.h>    // 에러 디버깅을 위해 추가

#define SERVER_IP "221.161.182.186"
#define CHAT_PORT 8080
#define BUFFER_SIZE 1024
#define NICKNAME_SIZE 30
#define ROOM_NAME_SIZE 50
#define FILE_TRANSFER_PORT 8081

// --- 전역 변수 및 GTK 위젯 ---
GtkTextView *chat_output;
GtkEntry *message_entry;
GtkWidget *main_window;
char my_nickname[NICKNAME_SIZE] = "";
int chat_sock_fd = -1;
char my_external_ip[16] = ""; // ✅ [수정] 공인 IP 주소를 저장할 전역 변수

// --- 네트워크 및 파일 전송 관련 함수 선언 ---
void on_send_button_clicked(GtkWidget *widget, gpointer data);
void on_file_button_clicked(GtkWidget *widget, gpointer data);
void connect_and_start_chat(const char *nickname, GtkWidget *parent_window);
void* receive_thread(void* arg);
void* file_receive_client_thread(void *arg);
void* file_send_server_thread(void *arg);
int get_external_ip(char *ip_buffer, size_t buffer_size); // ✅ [추가] 외부 IP 획득 함수 선언

// --- GTK GUI 업데이트 (메인 스레드 안전) ---

gboolean add_message_to_textview(gpointer data) {
    const char *message = (const char *)data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(chat_output);
    GtkTextIter iter;

    gtk_text_buffer_get_end_iter(buffer, &iter);
    gtk_text_buffer_insert(buffer, &iter, message, -1);
    gtk_text_buffer_insert(buffer, &iter, "\n", -1);

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(gtk_widget_get_parent(GTK_WIDGET(chat_output))));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));

    g_free(data); 
    return G_SOURCE_REMOVE;
}

// --- 외부 IP 획득 함수 구현 ---

int get_external_ip(char *ip_buffer, size_t buffer_size) {
    // curl을 실행하여 외부 IP를 얻고, 결과를 임시 파일에 저장합니다.
    const char *command = "curl -s icanhazip.com > external_ip.txt";
    int status = system(command);

    if (status == -1 || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[ERROR] Failed to execute curl command to get external IP. (Status: %d)\n", status);
        return -1;
    }

    FILE *fp = fopen("external_ip.txt", "r");
    if (fp == NULL) {
        fprintf(stderr, "[ERROR] Failed to open external_ip.txt.\n");
        remove("external_ip.txt"); 
        return -1;
    }

    if (fgets(ip_buffer, buffer_size, fp) == NULL) {
        fclose(fp);
        fprintf(stderr, "[ERROR] Failed to read IP from file.\n");
        remove("external_ip.txt");
        return -1;
    }
    
    fclose(fp);

    // IP 주소 끝의 개행 문자(\n) 제거 (필수)
    ip_buffer[strcspn(ip_buffer, "\n")] = 0; 

    // 임시 파일 삭제
    remove("external_ip.txt");
    return 0;
}

// --- 파일 전송 로직 (일부 생략) ---

typedef struct {
    char target_ip[16];
    int port;
    char filepath[BUFFER_SIZE];
} FileSendArgs;

void* file_send_server_thread(void *arg) {
    FileSendArgs *args = (FileSendArgs*)arg;
    int listen_sock, data_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd = -1;
    ssize_t read_bytes, sent_bytes;
    char file_buffer[BUFFER_SIZE];
    
    if ((fd = open(args->filepath, O_RDONLY)) < 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to open file for sending."));
        goto cleanup;
    }

    if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to create file transfer socket."));
        goto cleanup;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(args->port);
    
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 이 bind는 클라이언트 A의 임시 파일 서버 소켓이므로, 
    // 모든 인터페이스(INADDR_ANY)에 바인딩되어야 외부에서 접속 가능합니다.
    server_addr.sin_addr.s_addr = INADDR_ANY; 

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0 || listen(listen_sock, 1) < 0) {
        char err_msg[100];
        snprintf(err_msg, sizeof(err_msg), "[SERVER] Failed to bind/listen for file transfer: %s", strerror(errno));
        g_idle_add(add_message_to_textview, g_strdup(err_msg));
        close(listen_sock);
        goto cleanup;
    }
    
    g_idle_add(add_message_to_textview, g_strdup("[SERVER] Waiting for receiver to connect..."));

    data_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &client_len);
    if (data_sock < 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] File transfer accept failed."));
        close(listen_sock);
        goto cleanup;
    }
    
    g_idle_add(add_message_to_textview, g_strdup("[SERVER] Receiver connected. Starting file transfer..."));

    while ((read_bytes = read(fd, file_buffer, BUFFER_SIZE)) > 0) {
        if ((sent_bytes = send(data_sock, file_buffer, read_bytes, 0)) != read_bytes) {
             g_idle_add(add_message_to_textview, g_strdup("[SERVER] File transfer error during send."));
             break;
        }
    }

    g_idle_add(add_message_to_textview, g_strdup("[SERVER] File sent successfully."));

    close(data_sock);
    close(listen_sock);
cleanup:
    if (fd >= 0) close(fd);
    if (args) free(args);
    return NULL;
}


typedef struct {
    char sender_ip[16];
    int port;
    char filename[BUFFER_SIZE];
    long filesize;
    char sender_nickname[NICKNAME_SIZE];
} FileRecvArgs;

void* file_receive_client_thread(void *arg) {
    FileRecvArgs *args = (FileRecvArgs*)arg;
    int data_sock = -1;
    struct sockaddr_in server_addr;
    int fd = -1;
    ssize_t recv_bytes;
    char file_buffer[BUFFER_SIZE];
    long received_size = 0;
    
    char recv_filename[BUFFER_SIZE + 5];
    snprintf(recv_filename, sizeof(recv_filename), "recv_%s", args->filename);

    if ((fd = open(recv_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to create file for receiving."));
        goto cleanup;
    }

    if ((data_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to create file receive socket."));
        goto cleanup;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(args->port);
    if (inet_pton(AF_INET, args->sender_ip, &server_addr.sin_addr) <= 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Invalid sender IP address."));
        close(data_sock);
        goto cleanup;
    }

    if (connect(data_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        char err_msg[100];
        snprintf(err_msg, sizeof(err_msg), "[SERVER] Failed to connect to file sender: %s", strerror(errno));
        g_idle_add(add_message_to_textview, g_strdup(err_msg));
        close(data_sock);
        goto cleanup;
    }

    g_idle_add(add_message_to_textview, g_strdup("[SERVER] Connected to sender. Receiving file..."));

    while (received_size < args->filesize && (recv_bytes = recv(data_sock, file_buffer, BUFFER_SIZE, 0)) > 0) {
        write(fd, file_buffer, recv_bytes);
        received_size += recv_bytes;
    }
    
    char success_msg[sizeof(recv_filename) + 50]; 
    
    if (received_size == args->filesize) {
        snprintf(success_msg, sizeof(success_msg), "[SERVER] File received successfully as %s.", recv_filename);
        g_idle_add(add_message_to_textview, g_strdup(success_msg));
    } else {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] File receive completed but size mismatch or error."));
    }

    close(data_sock);
cleanup:
    if (fd >= 0) close(fd);
    if (args) free(args);
    return NULL;
}


// --- GTK 콜백 및 UI 생성 ---

void on_file_button_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog;
    gint res;

    dialog = gtk_file_chooser_dialog_new("Select File to Send", 
                                        GTK_WINDOW(main_window),
                                        GTK_FILE_CHOOSER_ACTION_OPEN,
                                        "Cancel", GTK_RESPONSE_CANCEL,
                                        "Select", GTK_RESPONSE_ACCEPT,
                                        NULL);
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filepath;
        filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        
        GtkWidget *target_dialog = gtk_dialog_new_with_buttons("Enter Target Nickname", 
                                                               GTK_WINDOW(main_window), 
                                                               GTK_DIALOG_MODAL,
                                                               "Cancel", GTK_RESPONSE_CANCEL,
                                                               "Send", GTK_RESPONSE_ACCEPT,
                                                               NULL);
        GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(target_dialog));
        GtkWidget *entry = gtk_entry_new();
        gtk_container_add(GTK_CONTAINER(content_area), entry);
        gtk_widget_show_all(target_dialog);
        
        gint target_res = gtk_dialog_run(GTK_DIALOG(target_dialog));
        
        if (target_res == GTK_RESPONSE_ACCEPT) {
            const gchar *target_nickname = gtk_entry_get_text(GTK_ENTRY(entry));

            struct stat st;
            if (stat(filepath, &st) == 0) {
                char *filename = strrchr(filepath, '/') ? strrchr(filepath, '/') + 1 : filepath;
                char request_msg[BUFFER_SIZE];
                
                // ✅ [수정] 전역 변수에 저장된 IP를 사용
                const char *local_ip = my_external_ip; 
                int temp_port = FILE_TRANSFER_PORT; 
                
                // IP 주소를 제대로 획득했는지 확인
                if (strlen(local_ip) == 0 || strcmp(local_ip, "127.0.0.1") == 0) {
                     g_idle_add(add_message_to_textview, g_strdup("[SERVER] ERROR: External IP not ready. File transfer failed."));
                     gtk_widget_destroy(target_dialog);
                     gtk_widget_destroy(dialog);
                     g_free(filepath);
                     return;
                }

                // FILE_REQ:타겟닉네임:파일명:파일크기:송신자IP:송신자Port
                snprintf(request_msg, BUFFER_SIZE, "FILE_REQ:%s:%s:%ld:%s:%d", 
                         target_nickname, filename, (long)st.st_size, local_ip, temp_port);
                
                send(chat_sock_fd, request_msg, strlen(request_msg), 0);
                
                FileSendArgs *args = malloc(sizeof(FileSendArgs));
                if (!args) {
                    g_idle_add(add_message_to_textview, g_strdup("[SERVER] Memory allocation failed."));
                } else {
                    strncpy(args->filepath, filepath, BUFFER_SIZE - 1);
                    args->filepath[BUFFER_SIZE - 1] = '\0';
                    args->port = temp_port;
                    
                    pthread_t tid;
                    if (pthread_create(&tid, NULL, file_send_server_thread, args) != 0) {
                         g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to start file send thread."));
                         free(args);
                    } else {
                        pthread_detach(tid);
                    }
                }
                
                char status_msg[BUFFER_SIZE];
                snprintf(status_msg, BUFFER_SIZE, "[SERVER] Request sent to send '%s' (%ld bytes) to %s. Sender IP: %s", filename, (long)st.st_size, target_nickname, local_ip);
                g_idle_add(add_message_to_textview, g_strdup(status_msg));

            } else {
                g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to get file information."));
            }
            g_free(filepath);
        }
        gtk_widget_destroy(target_dialog);
    }
    gtk_widget_destroy(dialog);
}

void on_send_button_clicked(GtkWidget *widget, gpointer data) {
    const gchar *text = gtk_entry_get_text(message_entry);
    
    if (strlen(text) > 0 && chat_sock_fd != -1) {
        
        char full_message[BUFFER_SIZE + NICKNAME_SIZE + 10]; 
        
        snprintf(full_message, sizeof(full_message), "MSG:%s: %s", my_nickname, text);

        send(chat_sock_fd, full_message, strlen(full_message), 0);
        
        gtk_entry_set_text(message_entry, ""); 
    }
}

// --- 네트워크 스레드 (수신 로직) ---

void* receive_thread(void* arg) {
    char buffer[BUFFER_SIZE];
    int bytes_read;

    while (chat_sock_fd != -1 && (bytes_read = recv(chat_sock_fd, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        
        if (strncmp(buffer, "FILE_ALERT:", 11) == 0) {
            
            char *alert_token = buffer + 11;
            char *sender_nickname = strtok(alert_token, ":");
            char *filename = strtok(NULL, ":");
            char *filesize_str = strtok(NULL, ":");
            char *sender_ip = strtok(NULL, ":");
            char *port_str = strtok(NULL, ":");
            
            if (sender_nickname && filename && filesize_str && sender_ip && port_str) {
                
                FileRecvArgs *args = malloc(sizeof(FileRecvArgs));
                if (!args) {
                    g_idle_add(add_message_to_textview, g_strdup("[SERVER] Memory allocation failed for file receive."));
                    continue;
                }
                
                strncpy(args->sender_nickname, sender_nickname, NICKNAME_SIZE - 1);
                args->sender_nickname[NICKNAME_SIZE - 1] = '\0';
                strncpy(args->filename, filename, BUFFER_SIZE - 1);
                args->filename[BUFFER_SIZE - 1] = '\0';
                strncpy(args->sender_ip, sender_ip, 15);
                args->sender_ip[15] = '\0';
                args->filesize = atol(filesize_str);
                args->port = atoi(port_str);

                pthread_t tid;
                if (pthread_create(&tid, NULL, file_receive_client_thread, args) != 0) {
                    g_idle_add(add_message_to_textview, g_strdup("[SERVER] Failed to start file receive thread."));
                    free(args);
                } else {
                    pthread_detach(tid);
                    char alert_msg[BUFFER_SIZE];
                    snprintf(alert_msg, BUFFER_SIZE, "[SERVER] Receiving file '%s' from %s...", filename, sender_nickname);
                    g_idle_add(add_message_to_textview, g_strdup(alert_msg));
                }
            } else {
                 g_idle_add(add_message_to_textview, g_strdup("[SERVER] Invalid file alert format received."));
            }
        } else {
            g_idle_add(add_message_to_textview, g_strdup(buffer));
        }
    }

    if (chat_sock_fd != -1) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Connection lost."));
        close(chat_sock_fd);
        chat_sock_fd = -1;
    }
    return NULL;
}


// --- 초기화 및 메인 함수 ---

void connect_and_start_chat(const char *nickname, GtkWidget *parent_window) {
    struct sockaddr_in server_addr;

    if ((chat_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Socket creation error\n");
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CHAT_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address/ Address not supported\n");
        close(chat_sock_fd);
        chat_sock_fd = -1;
        return;
    }

    if (connect(chat_sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed"); 
        close(chat_sock_fd);
        chat_sock_fd = -1;
        return;
    }

    // 1. 닉네임 전송
    send(chat_sock_fd, nickname, strlen(nickname), 0);
    
    // 2. 공인 IP 획득 및 저장 (연결 성공 후)
    if (get_external_ip(my_external_ip, sizeof(my_external_ip)) != 0) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] WARNING: Failed to get external IP. File transfers may fail."));
        // 실패 시 파일 전송을 막기 위해 127.0.0.1이 아닌 빈 문자열 유지
        my_external_ip[0] = '\0'; 
    } else {
        char status_msg[60];
        snprintf(status_msg, sizeof(status_msg), "[SERVER] External IP registered: %s", my_external_ip);
        g_idle_add(add_message_to_textview, g_strdup(status_msg));
    }
    
    // 3. 수신 스레드 시작
    pthread_t tid;
    if (pthread_create(&tid, NULL, receive_thread, NULL) != 0) {
        fprintf(stderr, "Receive thread creation failed\n");
        close(chat_sock_fd);
        chat_sock_fd = -1;
        return;
    }
    pthread_detach(tid);

    // 4. 방 선택 대화 상자
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Room Selection", GTK_WINDOW(parent_window), GTK_DIALOG_MODAL,
                                                    "Create", 1,
                                                    "Join", 2,
                                                    NULL);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new("Enter Room Name:");
    GtkWidget *room_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(room_entry), ROOM_NAME_SIZE - 1);
    gtk_container_add(GTK_CONTAINER(content_area), label);
    gtk_container_add(GTK_CONTAINER(content_area), room_entry);
    gtk_widget_show_all(dialog);

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    const char *room_name = gtk_entry_get_text(GTK_ENTRY(room_entry));
    
    if (strlen(room_name) > 0 && chat_sock_fd != -1) {
        char cmd[ROOM_NAME_SIZE + 15];
        if (res == 1) {
            snprintf(cmd, sizeof(cmd), "CREATE_ROOM:%s", room_name);
            send(chat_sock_fd, cmd, strlen(cmd), 0);
        } else if (res == 2) {
            snprintf(cmd, sizeof(cmd), "JOIN_ROOM:%s", room_name);
            send(chat_sock_fd, cmd, strlen(cmd), 0);
        }
    } else if (res == GTK_RESPONSE_DELETE_EVENT || res == GTK_RESPONSE_NONE) {
        g_idle_add(add_message_to_textview, g_strdup("[SERVER] Room selection skipped or cancelled. Disconnecting..."));
        close(chat_sock_fd);
        chat_sock_fd = -1;
    }

    gtk_widget_destroy(dialog);
}

static void activate(GtkApplication *app, gpointer user_data) {
    main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(main_window), "C/GTK Messenger");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 600, 400);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    chat_output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(chat_output, FALSE);
    gtk_text_view_set_cursor_visible(chat_output, FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled_window), GTK_WIDGET(chat_output));

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

    message_entry = GTK_ENTRY(gtk_entry_new());
    gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(message_entry), TRUE, TRUE, 0);
    g_signal_connect(message_entry, "activate", G_CALLBACK(on_send_button_clicked), NULL);

    GtkWidget *send_button = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(hbox), send_button, FALSE, FALSE, 0);
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_send_button_clicked), NULL);

    GtkWidget *file_button = gtk_button_new_with_label("File");
    gtk_box_pack_start(GTK_BOX(hbox), file_button, FALSE, FALSE, 0);
    g_signal_connect(file_button, "clicked", G_CALLBACK(on_file_button_clicked), NULL);
    
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Enter Your Nickname", GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
                                                    "Cancel", GTK_RESPONSE_CANCEL,
                                                    "Connect", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *nick_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(nick_entry), NICKNAME_SIZE - 1);
    gtk_container_add(GTK_CONTAINER(content_area), nick_entry);
    gtk_widget_show_all(dialog);

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        const char *nick = gtk_entry_get_text(GTK_ENTRY(nick_entry));
        if (strlen(nick) > 0) {
            strncpy(my_nickname, nick, NICKNAME_SIZE - 1);
            my_nickname[NICKNAME_SIZE - 1] = '\0';
            
            connect_and_start_chat(my_nickname, main_window); 
            gtk_widget_show_all(main_window);
        }
    }
    gtk_widget_destroy(dialog);
}

int main(int argc, char *argv[]) {
    GtkApplication *app;
    int status;

    app = gtk_application_new("org.gtk.messenger", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    if (chat_sock_fd != -1) {
        close(chat_sock_fd);
    }

    return status;
}