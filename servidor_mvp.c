#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // Para tcp_info
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "controle_taxa.h"

#define PORTA 5000
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 30
#define DEFAULT_RATE_KBPS 1000
#define CONFIG_FILE "rates.conf"

struct RateConfig {
    char ip[INET_ADDRSTRLEN];
    int rate_kbps;
    int active_conns; // --- [QoS SHARED] Contador de conexões ativas por IP ---
};

struct RateConfig rate_configs[MAX_CLIENTS];
int num_rate_configs = 0;
int max_server_rate_kbps = 0;
int current_allocated_rate_kbps = 0;
pthread_mutex_t rate_mutex;

// Protótipos
void load_rate_configs();
int get_config_index(const char* client_ip); // Mudamos para retornar o índice
void *handle_client(void *client_socket_ptr);
const char *get_content_type(const char *file_name);
void print_connection_stats(int socket_fd, char* client_ip, int allocated_rate);

int main(int argc, char const *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if (argc < 2) {
        fprintf(stderr, "Uso: %s <vazao_maxima_kbps>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    max_server_rate_kbps = atoi(argv[1]);
    if (max_server_rate_kbps <= 0) exit(EXIT_FAILURE);
    
    printf("Servidor iniciado. Vazão Máxima: %d kbps.\n", max_server_rate_kbps);

    if (pthread_mutex_init(&rate_mutex, NULL) != 0) exit(EXIT_FAILURE);
    load_rate_configs();

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) exit(EXIT_FAILURE);
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) exit(EXIT_FAILURE);
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORTA);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) exit(EXIT_FAILURE);
    if (listen(server_fd, MAX_CLIENTS) < 0) exit(EXIT_FAILURE);

    printf("Servidor escutando na porta %d\n", PORTA);

    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Falha no accept");
            continue;
        }

        int *client_socket = malloc(sizeof(int));
        *client_socket = new_socket;

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, (void *)client_socket);
        pthread_detach(thread_id);
    }

    pthread_mutex_destroy(&rate_mutex);
    close(server_fd);
    return 0;
}

void load_rate_configs() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) return;
    char line[100];
    while (fgets(line, sizeof(line), fp) && num_rate_configs < MAX_CLIENTS) {
        sscanf(line, "%s %d", rate_configs[num_rate_configs].ip, &rate_configs[num_rate_configs].rate_kbps);
        rate_configs[num_rate_configs].active_conns = 0; // Inicializa contador
        num_rate_configs++;
    }
    fclose(fp);
}

// --- [QoS SHARED] Retorna o índice no array em vez da taxa direta ---
int get_config_index(const char* client_ip) {
    for (int i = 0; i < num_rate_configs; i++) {
        if (strcmp(client_ip, rate_configs[i].ip) == 0) {
            return i; // Retorna o índice onde estão os dados deste IP
        }
    }
    return -1; // -1 significa "IP não encontrado no arquivo"
}

const char *get_content_type(const char *file_name) {
    const char *ext = strrchr(file_name, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    return "application/octet-stream";
}

void print_connection_stats(int socket_fd, char* client_ip, int allocated_rate) {
    struct tcp_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(socket_fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
        printf("\n\033[1;36m=== STATS [%s] ===\033[0m\n", client_ip);
        printf(" -> Rate Base: %d kbps\n", allocated_rate);
        printf(" -> RTT: %.2f ms\n", (double)info.tcpi_rtt / 1000.0);
        printf("\033[1;36m======================\033[0m\n\n");
    }
}

void *handle_client(void *client_socket_ptr) {
    int client_socket = *((int *)client_socket_ptr);
    free(client_socket_ptr);

    char buffer[BUFFER_SIZE] = {0};
    char client_ip[INET_ADDRSTRLEN];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    // --- [QoS SHARED] Lógica de Admissão e Compartilhamento ---
    int config_idx = get_config_index(client_ip);
    int client_rate = DEFAULT_RATE_KBPS;
    int *conn_counter_ptr = NULL; // Ponteiro para o contador compartilhado

    // Dummy counter para IPs desconhecidos (sempre será 1)
    int dummy_counter = 1; 

    pthread_mutex_lock(&rate_mutex);
    
    if (config_idx >= 0) {
        // IP Conhecido: Usamos a configuração do arquivo
        client_rate = rate_configs[config_idx].rate_kbps;
        rate_configs[config_idx].active_conns++; // Incrementa contador compartilhado!
        conn_counter_ptr = &rate_configs[config_idx].active_conns; // Aponta para o contador global
    } else {
        // IP Desconhecido: Usa padrão e contador local
        client_rate = DEFAULT_RATE_KBPS;
        conn_counter_ptr = &dummy_counter;
    }

    // Verifica Vazão Global (Admissão)
    if (current_allocated_rate_kbps + client_rate > max_server_rate_kbps) {
        // Reverte o incremento se for recusar
        if (config_idx >= 0) rate_configs[config_idx].active_conns--;
        pthread_mutex_unlock(&rate_mutex);
        
        printf("Recusado: %s (Cheio)\n", client_ip);
        char *resp = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
        write(client_socket, resp, strlen(resp));
        close(client_socket);
        pthread_exit(NULL);
    }
    
    current_allocated_rate_kbps += client_rate;
    pthread_mutex_unlock(&rate_mutex);
    
    printf("Conectado: %s (Rate Base: %d). Ativos no IP: %d\n", 
           client_ip, client_rate, *conn_counter_ptr);

    // --- Loop Principal ---
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        if (read(client_socket, buffer, BUFFER_SIZE - 1) <= 0) break;

        char method[16], path[256], version[16];
        if (sscanf(buffer, "%s %s %s", method, path, version) < 3) continue;

        char *file_path_relative = path;
        if (file_path_relative[0] == '/') file_path_relative++;
        if (strlen(file_path_relative) == 0) file_path_relative = "index.html";
        
        char full_file_path[512];
        sprintf(full_file_path, "HTML/%s", file_path_relative);

        FILE *file = fopen(full_file_path, "rb");
        if (!file) {
            char *resp = "HTTP/1.1 404 Not Found\r\n\r\n";
            write(client_socket, resp, strlen(resp));
            continue;
        }

        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        char *file_content = malloc(file_size);
        fread(file_content, 1, file_size, file);
        fclose(file);

        char header[BUFFER_SIZE];
        const char *ctype = get_content_type(file_path_relative);
        sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: keep-alive\r\n\r\n", ctype, file_size);
        write(client_socket, header, strlen(header));

        if (strcmp(ctype, "text/html") == 0) {
            write(client_socket, file_content, file_size);
        } else {
            // --- [QoS SHARED] Passamos o ponteiro do contador aqui! ---
            send_throttled(client_socket, file_content, file_size, client_rate, conn_counter_ptr);
        }
        
        free(file_content);
        print_connection_stats(client_socket, client_ip, client_rate);

        if (strstr(buffer, "Connection: close")) break;
    }

    // --- [QoS SHARED] Limpeza na desconexão ---
    pthread_mutex_lock(&rate_mutex);
    current_allocated_rate_kbps -= client_rate;
    if (config_idx >= 0) {
        rate_configs[config_idx].active_conns--; // Decrementa contador compartilhado
        printf("Desconectado: %s. Restantes no IP: %d\n", client_ip, rate_configs[config_idx].active_conns);
    }
    pthread_mutex_unlock(&rate_mutex);
    
    close(client_socket);
    pthread_exit(NULL);
}
