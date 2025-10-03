#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORTA 5000
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 30

// Função que será executada por cada thread para lidar com um cliente
void *handle_client(void *client_socket_ptr);

int main(int argc, char const *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // 1. Criando o descritor de arquivo do socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Falha ao criar o socket");
        exit(EXIT_FAILURE);
    }

    // 2. Configurando o socket para reutilizar o endereço e a porta (evita erro "Address already in use")
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Falha ao configurar setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Aceita conexões de qualquer IP
    address.sin_port = htons(PORTA);

    // 3. Vinculando o socket à porta 5000
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Falha no bind");
        exit(EXIT_FAILURE);
    }

    // 4. Colocando o socket em modo de escuta para novas conexões
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Falha no listen");
        exit(EXIT_FAILURE);
    }

    printf("Servidor escutando na porta %d\n", PORTA);

    // 5. Loop principal para aceitar novas conexões
    while (1) {
        // Aceita uma nova conexão de cliente
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Falha no accept");
            continue; // Continua para a próxima iteração
        }

        // Para passar o descritor do socket para a thread de forma segura,
        // alocamos memória para ele.
        int *client_socket = malloc(sizeof(int));
        if (client_socket == NULL) {
            perror("Falha ao alocar memória para o socket do cliente");
            close(new_socket);
            continue;
        }
        *client_socket = new_socket;

        // Cria uma nova thread para lidar com o cliente
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client_socket) != 0) {
            perror("Falha ao criar a thread");
            free(client_socket);
            close(new_socket);
        }
        
        // Desanexa a thread para que seus recursos sejam liberados automaticamente ao terminar
        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}

void *handle_client(void *client_socket_ptr) {
    int client_socket = *((int *)client_socket_ptr);
    free(client_socket_ptr); // Libera a memória alocada na main

    char buffer[BUFFER_SIZE] = {0};
    char client_ip[INET_ADDRSTRLEN];
    
    // Obtém o endereço IP do cliente para log
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    printf("Nova conexão de %s:%d na thread %lu\n", client_ip, ntohs(client_addr.sin_port), pthread_self());

    // Loop para manter a conexão persistente (HTTP 1.1)
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);

        if (bytes_read <= 0) {
            // Se read retorna 0, o cliente fechou a conexão.
            // Se retorna < 0, ocorreu um erro.
            printf("Cliente %s desconectado ou erro de leitura.\n", client_ip);
            break;
        }

        // Imprime a requisição recebida (para depuração)
        printf("--- Requisição de %s ---\n%s\n-------------------------\n", client_ip, buffer);

        // --- Monta a Resposta HTTP ---
        // Este é um exemplo simples. O ideal é ler um arquivo (ex: index.html)
        char *html_content = "<html><head><title>Servidor MVP1</title></head>"
                             "<body><h1>Ola!</h1><p>Este eh o MVP1 do servidor HTTP concorrente.</p></body></html>";
        
        char http_response[BUFFER_SIZE];
        sprintf(http_response, 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %ld\r\n"
                "Connection: keep-alive\r\n" // Informa ao cliente para manter a conexão aberta
                "\r\n" // Linha em branco, fim dos headers
                "%s", 
                strlen(html_content), html_content);
        
        // Envia a resposta
        int bytes_sent = write(client_socket, http_response, strlen(http_response));
        if (bytes_sent < 0) {
            perror("Falha ao escrever no socket");
            break;
        }

        // Verifica se o cliente pediu para fechar a conexão
        if (strstr(buffer, "Connection: close") != NULL) {
            printf("Cliente %s pediu para fechar a conexao.\n", client_ip);
            break;
        }
    }

    // Fecha o socket e finaliza a thread
    close(client_socket);
    printf("Conexão com %s fechada. Finalizando thread %lu.\n", client_ip, pthread_self());
    pthread_exit(NULL);
}
