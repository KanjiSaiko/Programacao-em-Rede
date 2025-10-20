#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h> // Para ler arquivos
#include <sys/stat.h> // Para pegar tamanho do arquivo

#include "controle_taxa.h" // Nosso header para send_throttled()

#define PORTA 5000
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 30
#define DEFAULT_RATE_KBPS 1000 // --- NOVO: Taxa padrão [cite: 36]
#define CONFIG_FILE "rates.conf"

// --- NOVO: Estrutura para armazenar as configurações de taxa  ---
struct RateConfig {
    char ip[INET_ADDRSTRLEN];
    int rate_kbps;
};

// --- NOVO: Variáveis globais para QoS ---
struct RateConfig rate_configs[MAX_CLIENTS];
int num_rate_configs = 0;
int max_server_rate_kbps = 0; // Vazão máxima do servidor [cite: 39]
int current_allocated_rate_kbps = 0; // Vazão atualmente alocada
pthread_mutex_t rate_mutex; // Mutex para proteger a variável acima

// --- NOVO: Protótipos de novas funções ---
void load_rate_configs();
int get_client_rate(const char* client_ip);
void *handle_client(void *client_socket_ptr);
const char *get_content_type(const char *file_name);

int main(int argc, char const *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // --- NOVO: Validar parâmetro de linha de comando [cite: 39] ---
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <vazao_maxima_kbps>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    max_server_rate_kbps = atoi(argv[1]);
    if (max_server_rate_kbps <= 0) {
        fprintf(stderr, "Vazão máxima deve ser um número positivo.\n");
        exit(EXIT_FAILURE);
    }
    printf("Servidor iniciado com vazão máxima de %d kbps.\n", max_server_rate_kbps);
    // --- FIM NOVO ---

    // --- NOVO: Inicializar mutex ---
    if (pthread_mutex_init(&rate_mutex, NULL) != 0) {
        perror("Falha ao inicializar mutex");
        exit(EXIT_FAILURE);
    }
    // --- FIM NOVO ---

    // --- NOVO: Carregar configurações de taxa ---
    load_rate_configs();
    // --- FIM NOVO ---


    // 1. Criando o descritor de arquivo do socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Falha ao criar o socket");
        exit(EXIT_FAILURE);
    }

    // 2. Configurando o socket para reutilizar o endereço e a porta
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Falha ao configurar setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORTA);

    // 3. Vinculando o socket à porta 5000
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Falha no bind");
        exit(EXIT_FAILURE);
    }

    // 4. Colocando o socket em modo de escuta
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Falha no listen");
        exit(EXIT_FAILURE);
    }

    printf("Servidor escutando na porta %d\n", PORTA);

    // 5. Loop principal para aceitar novas conexões
    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Falha no accept");
            continue;
        }

        int *client_socket = malloc(sizeof(int));
        if (client_socket == NULL) {
            perror("Falha ao alocar memória para o socket do cliente");
            close(new_socket);
            continue;
        }
        *client_socket = new_socket;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client_socket) != 0) {
            perror("Falha ao criar a thread");
            free(client_socket);
            close(new_socket);
        }
        
        pthread_detach(thread_id);
    }

    pthread_mutex_destroy(&rate_mutex); // --- NOVO ---
    close(server_fd);
    return 0;
}

// --- NOVO: Função para carregar o arquivo de configuração de taxas ---
void load_rate_configs() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) {
        perror("Não foi possível abrir o arquivo 'rates.conf'. Usando taxas padrão");
        return;
    }

    char line[100];
    while (fgets(line, sizeof(line), fp) && num_rate_configs < MAX_CLIENTS) {
        sscanf(line, "%s %d", 
               rate_configs[num_rate_configs].ip, 
               &rate_configs[num_rate_configs].rate_kbps);
        num_rate_configs++;
    }

    printf("Carregadas %d configurações de taxa do arquivo '%s'.\n", num_rate_configs, CONFIG_FILE);
    fclose(fp);
}

// --- NOVO: Função para encontrar a taxa de um cliente específico ---
int get_client_rate(const char* client_ip) {
    for (int i = 0; i < num_rate_configs; i++) {
        if (strcmp(client_ip, rate_configs[i].ip) == 0) {
            return rate_configs[i].rate_kbps;
        }
    }
    return DEFAULT_RATE_KBPS; // Taxa padrão [cite: 36]
}

// --- NOVO: Função para determinar o Content-Type com base na extensão ---
const char *get_content_type(const char *file_name) {
    const char *ext = strrchr(file_name, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0) return "image/jpeg";
    if (strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    return "application/octet-stream";
}


// --- FUNÇÃO handle_client ATUALIZADA ---
void *handle_client(void *client_socket_ptr) {
    int client_socket = *((int *)client_socket_ptr);
    free(client_socket_ptr);

    char buffer[BUFFER_SIZE] = {0};
    char client_ip[INET_ADDRSTRLEN];
    
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    // --- NOVO: Controle de Admissão [cite: 39] ---
    int client_rate = get_client_rate(client_ip);
    
    pthread_mutex_lock(&rate_mutex);
    if (current_allocated_rate_kbps + client_rate > max_server_rate_kbps) {
        // Servidor está cheio
        pthread_mutex_unlock(&rate_mutex);
        
        printf("Conexão recusada de %s (taxa: %d kbps). Vazão excedida.\n", client_ip, client_rate);
        
        // Envia HTTP 503 Service Unavailable
        char *response = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
        write(client_socket, response, strlen(response));
        
        close(client_socket);
        pthread_exit(NULL);
    }
    
    // Aloca a taxa para este cliente
    current_allocated_rate_kbps += client_rate;
    pthread_mutex_unlock(&rate_mutex);
    
    printf("Nova conexão de %s (taxa: %d kbps). Vazão atual: %d/%d kbps\n", 
           client_ip, client_rate, current_allocated_rate_kbps, max_server_rate_kbps);
    // --- FIM NOVO ---


    // Loop para manter a conexão persistente (HTTP 1.1)
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);

        if (bytes_read <= 0) {
            printf("Cliente %s desconectado ou erro de leitura.\n", client_ip);
            break;
        }

        printf("--- Requisição de %s ---\n%s\n-------------------------\n", client_ip, buffer);

        // --- NOVO: Parse da Requisição HTTP ---
        char method[16], path[256], version[16];
        if (sscanf(buffer, "%s %s %s", method, path, version) < 3) {
            printf("Requisição mal formatada.\n");
            continue; // Espera pela próxima requisição
        }

        // Remove a barra inicial do caminho (ex: "/index.html" vira "index.html")
        char *file_path_relative = path;
        if (file_path_relative[0] == '/') {
            file_path_relative++;
        }
        // Se o caminho for vazio, serve o "index.html"
        if (strlen(file_path_relative) == 0) {
            file_path_relative = "index.html";
        }
        
        // --- MODIFICAÇÃO AQUI ---
        // Cria o caminho completo para o arquivo dentro da pasta HTML
        char full_file_path[512];
        sprintf(full_file_path, "HTML/%s", file_path_relative);
        // --- FIM DA MODIFICAÇÃO ---


        // --- NOVO: Ler o arquivo do disco ---
        // Modificado para usar o full_file_path
        FILE *file = fopen(full_file_path, "rb"); // "rb" = read binary
        if (file == NULL) {
            // Arquivo não encontrado
            // Modificado para mostrar o caminho completo no log de erro
            printf("Arquivo não encontrado: %s\n", full_file_path);
            char *response = "HTTP/1.1 404 Not Found\r\nConnection: keep-alive\r\n\r\n<html><body>404 Not Found</body></html>";
            write(client_socket, response, strlen(response));
            continue;
        }


        // Obter o tamanho do arquivo
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        // Alocar memória para o conteúdo do arquivo
        char *file_content = malloc(file_size);
        if (file_content == NULL) {
            perror("Falha ao alocar memória para o arquivo");
            fclose(file);
            continue;
        }
        
        // Ler o arquivo para a memória
        if (fread(file_content, 1, file_size, file) != file_size) {
            perror("Falha ao ler o arquivo");
            free(file_content);
            fclose(file);
            continue;
        }
        fclose(file);
        // --- FIM NOVO ---


        // --- NOVO: Montar Resposta HTTP Dinâmica ---
        char http_header[BUFFER_SIZE];
        const char *content_type = get_content_type(file_path_relative);
        
        sprintf(http_header, 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Connection: keep-alive\r\n"
                "\r\n", 
                content_type, file_size);
        
        // 1. Envia o cabeçalho (sempre em velocidade máxima)
        if (write(client_socket, http_header, strlen(http_header)) < 0) {
            perror("Falha ao enviar header");
            free(file_content);
            break;
        }

        // 2. Envia o conteúdo (com ou sem throttling)
        //  "enviar objetos (exceto arquivo html) com taxa controlada"
        if (strcmp(content_type, "text/html") == 0) {
            // É HTML: envia sem controle
            printf("Enviando %s (%ld bytes) sem controle de taxa.\n", file_path_relative, file_size);
            if (write(client_socket, file_content, file_size) < 0) {
                 perror("Falha ao enviar conteúdo HTML");
            }
        } else {
            // NÃO é HTML: envia com controle de taxa
            printf("Enviando %s (%ld bytes) com taxa de %d kbps.\n", file_path_relative, file_size, client_rate);
            send_throttled(client_socket, file_content, file_size, client_rate);
        }
        
        free(file_content); // Libera a memória do arquivo
        // --- FIM NOVO ---

        // Verifica se o cliente pediu para fechar a conexão
        if (strstr(buffer, "Connection: close") != NULL) {
            printf("Cliente %s pediu para fechar a conexao.\n", client_ip);
            break;
        }
    }

    // --- NOVO: Liberar a taxa alocada ---
    pthread_mutex_lock(&rate_mutex);
    current_allocated_rate_kbps -= client_rate;
    pthread_mutex_unlock(&rate_mutex);
    // --- FIM NOVO ---
    
    close(client_socket);
    printf("Conexão com %s fechada. Vazão atual: %d/%d kbps\n", 
           client_ip, current_allocated_rate_kbps, max_server_rate_kbps);
    pthread_exit(NULL);
}
