/*
 * SERVIDOR WEB HTTP/1.1 COM QoS (MVP2)
 * -------------------------------------------------------------------
 * Este servidor implementa:
 * 1. Concorrência: Usa uma thread por cliente (Pthreads).
 * 2. Persistência: Mantém conexões HTTP/1.1 abertas para múltiplas requisições.
 * 3. QoS - Controle de Admissão: Recusa novos clientes se a vazão total exceder o limite.
 * 4. QoS - Controle de Taxa (Throttling): Limita a velocidade de download para arquivos não-HTML.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>  // Necessário para criar threads e usar mutexes
#include <fcntl.h>    // Manipulação de arquivos (open, etc)
#include <sys/stat.h> // Para obter informações de arquivos (tamanho)

// Inclui nosso cabeçalho personalizado que define a função 'send_throttled'
#include "controle_taxa.h" 

// --- DEFINIÇÕES GERAIS ---
#define PORTA 5000       // Porta onde o servidor vai escutar
#define BUFFER_SIZE 4096   // Tamanho do buffer para leitura de requisições
#define MAX_CLIENTS 30     // Número máximo de conexões pendentes na fila do SO
#define DEFAULT_RATE_KBPS 1000 // Taxa padrão para IPs não listados no arquivo de configuração
#define CONFIG_FILE "rates.conf" // Nome do arquivo que contém as regras IP -> Taxa

// --- ESTRUTURAS DE DADOS ---
// Estrutura para guardar uma regra de taxa lida do arquivo rates.conf
struct RateConfig {
    char ip[INET_ADDRSTRLEN]; // Armazena o endereço IP (ex: "192.168.0.1")
    int rate_kbps;            // Armazena a taxa associada em Kbps
};

// --- VARIÁVEIS GLOBAIS (ESTADO DO SERVIDOR) ---
// Array para armazenar todas as regras carregadas do arquivo
struct RateConfig rate_configs[MAX_CLIENTS];
int num_rate_configs = 0; // Contador de quantas regras foram carregadas

// Variáveis cruciais para o Controle de Admissão
int max_server_rate_kbps = 0;        // O limite total de velocidade do servidor (passado via linha de comando)
int current_allocated_rate_kbps = 0; // Quanto da velocidade total já está sendo usada no momento

// Mutex (Mutual Exclusion): Essencial em programas multithread.
// Ele garante que apenas UMA thread por vez possa alterar 'current_allocated_rate_kbps'.
// Sem isso, duas threads poderiam ler/escrever a variável ao mesmo tempo, causando erros de cálculo (condição de corrida).
pthread_mutex_t rate_mutex; 

// --- PROTÓTIPOS DE FUNÇÕES ---
void load_rate_configs();
int get_client_rate(const char* client_ip);
void *handle_client(void *client_socket_ptr);
const char *get_content_type(const char *file_name);

// --- FUNÇÃO PRINCIPAL (MAIN) ---
int main(int argc, char const *argv[]) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // 1. VALIDAÇÃO DE ARGUMENTOS
    // O servidor PRECISA saber qual é sua velocidade máxima para fazer o controle de admissão.
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <vazao_maxima_kbps>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    max_server_rate_kbps = atoi(argv[1]); // Converte o argumento de string para inteiro
    if (max_server_rate_kbps <= 0) {
        fprintf(stderr, "Vazão máxima deve ser um número positivo.\n");
        exit(EXIT_FAILURE);
    }
    printf("Servidor iniciado com vazão máxima global de %d kbps.\n", max_server_rate_kbps);

    // 2. INICIALIZAÇÃO
    // Inicializa o mutex antes de qualquer thread ser criada.
    if (pthread_mutex_init(&rate_mutex, NULL) != 0) {
        perror("Falha ao inicializar mutex");
        exit(EXIT_FAILURE);
    }
    // Carrega as regras de velocidade do arquivo para a memória.
    load_rate_configs();

    // 3. CONFIGURAÇÃO DO SOCKET (Boilerplate padrão de servidores C)
    // Cria o socket TCP (IPv4, Stream)
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Falha ao criar o socket");
        exit(EXIT_FAILURE);
    }
    // Permite reutilizar a porta imediatamente após fechar o servidor (evita erro "Address already in use")
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Falha ao configurar setsockopt");
        exit(EXIT_FAILURE);
    }
    // Define o endereço e porta onde vamos escutar (IP 0.0.0.0 = aceita todas as interfaces locais)
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORTA);

    // Vincula o socket à porta 5000
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Falha no bind");
        exit(EXIT_FAILURE);
    }
    // Coloca o socket em modo passivo (escutando)
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Falha no listen");
        exit(EXIT_FAILURE);
    }
    printf("Servidor escutando na porta %d\n", PORTA);

    // 4. LOOP PRINCIPAL (ACEITAÇÃO DE CONEXÕES)
    while (1) {
        // O servidor fica bloqueado aqui até um cliente tentar conectar.
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Falha no accept");
            continue;
        }

        // Aloca memória para o descritor do socket do cliente.
        // Isso é necessário porque se passássemos apenas o endereço da variável 'new_socket',
        // a próxima iteração do loop poderia sobrescrever o valor antes da thread ler.
        int *client_socket = malloc(sizeof(int));
        if (client_socket == NULL) {
            perror("Falha ao alocar memória para o socket do cliente");
            close(new_socket);
            continue;
        }
        *client_socket = new_socket;

        // Cria uma NOVA THREAD para lidar com este cliente.
        // A função 'handle_client' será executada em paralelo.
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client_socket) != 0) {
            perror("Falha ao criar a thread");
            free(client_socket);
            close(new_socket);
        }
        
        // 'detach' permite que o sistema operacional limpe os recursos da thread
        // automaticamente quando ela terminar, sem precisarmos chamar pthread_join().
        pthread_detach(thread_id);
    }

    // Limpeza (código inalcançável no loop infinito atual, mas boa prática)
    pthread_mutex_destroy(&rate_mutex);
    close(server_fd);
    return 0;
}

// --- FUNÇÕES AUXILIARES ---

// Lê o arquivo rates.conf linha por linha e preenche o array global 'rate_configs'
void load_rate_configs() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp == NULL) {
        perror("Aviso: Não foi possível abrir 'rates.conf'. Usando apenas taxas padrão");
        return;
    }

    char line[100];
    // Lê enquanto houver linhas e espaço no array
    while (fgets(line, sizeof(line), fp) && num_rate_configs < MAX_CLIENTS) {
        // sscanf faz o parse da linha: espera uma string (IP) e um inteiro (taxa)
        if (sscanf(line, "%s %d", 
                  rate_configs[num_rate_configs].ip, 
                  &rate_configs[num_rate_configs].rate_kbps) == 2) {
            num_rate_configs++;
        }
    }

    printf("QoS: Carregadas %d regras de taxa do arquivo '%s'.\n", num_rate_configs, CONFIG_FILE);
    fclose(fp);
}

// Procura o IP do cliente no array de configurações carregado.
// Se achar, retorna a taxa configurada. Se não, retorna a taxa padrão (1000kbps).
int get_client_rate(const char* client_ip) {
    for (int i = 0; i < num_rate_configs; i++) {
        if (strcmp(client_ip, rate_configs[i].ip) == 0) {
            return rate_configs[i].rate_kbps;
        }
    }
    return DEFAULT_RATE_KBPS;
}

// Retorna o MIME type (Content-Type) baseado na extensão do arquivo.
// Importante para o navegador saber como renderizar o que recebe.
const char *get_content_type(const char *file_name) {
    const char *ext = strrchr(file_name, '.'); // Acha o último ponto no nome do arquivo
    if (!ext) return "application/octet-stream"; // Tipo genérico binário se não tiver extensão
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0) return "image/jpeg";
    if (strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    return "application/octet-stream";
}

// --- LÓGICA DE ATENDIMENTO AO CLIENTE (EXECUTADA POR CADA THREAD) ---
void *handle_client(void *client_socket_ptr) {
    // Resgata o descritor do socket e libera a memória alocada na main
    int client_socket = *((int *)client_socket_ptr);
    free(client_socket_ptr);

    char buffer[BUFFER_SIZE] = {0};
    char client_ip[INET_ADDRSTRLEN];
    
    // Obtém o endereço IP do cliente para logs e para as regras de QoS
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    // === [QoS] INÍCIO DO CONTROLE DE ADMISSÃO ===
    // 1. Determina qual taxa este cliente deveria ter
    int client_rate = get_client_rate(client_ip);
    
    // 2. Tenta "reservar" essa banda no servidor.
    //    Precisamos do mutex para verificar e atualizar a variável global com segurança.
    pthread_mutex_lock(&rate_mutex);

    // Verifica se adicionar este novo cliente excederia a capacidade total do servidor
    if (current_allocated_rate_kbps + client_rate > max_server_rate_kbps) {
        // CASO DE FALHA: Servidor cheio.
        pthread_mutex_unlock(&rate_mutex); // Libera o mutex imediatamente!
        
        printf("QoS [Admissão]: Conexão recusada de %s. Necessário: %dkbps, Disponível: %dkbps.\n", 
               client_ip, client_rate, (max_server_rate_kbps - current_allocated_rate_kbps));
        
        // Envia erro 503 (Serviço Indisponível) padrão HTTP para indicar sobrecarga
        char *response = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\nServer too busy.";
        write(client_socket, response, strlen(response));
        
        close(client_socket);
        pthread_exit(NULL); // Encerra esta thread
    }
    
    // SUCESSO: Cliente admitido. Atualiza a contabilidade global de banda.
    current_allocated_rate_kbps += client_rate;
    pthread_mutex_unlock(&rate_mutex); // Libera o mutex para outras threads usarem
    
    printf("QoS [Admissão]: Cliente %s admitido com %d kbps. Uso Global: %d/%d kbps\n", 
           client_ip, client_rate, current_allocated_rate_kbps, max_server_rate_kbps);
    // === [QoS] FIM DO CONTROLE DE ADMISSÃO ===


    // LOOP DE CONEXÃO PERSISTENTE (HTTP/1.1 Keep-Alive)
    // A thread fica neste loop atendendo várias requisições do mesmo cliente
    // até que ele feche a conexão ou ocorra um erro.
    while (1) {
        memset(buffer, 0, BUFFER_SIZE); // Limpa o buffer
        // Lê a requisição HTTP enviada pelo cliente
        int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);

        if (bytes_read <= 0) {
            // Se leu 0 bytes, o cliente fechou a conexão graciosamente.
            printf("Cliente %s desconectou.\n", client_ip);
            break; // Sai do loop while(1)
        }

        // (Opcional) Imprime a requisição para debug
        // printf("--- Requisição de %s ---\n%s\n", client_ip, buffer);

        // --- PARSE DA REQUISIÇÃO HTTP ---
        // Tenta extrair Método (GET), Caminho (/index.html) e Versão (HTTP/1.1)
        char method[16], path[256], version[16];
        if (sscanf(buffer, "%s %s %s", method, path, version) < 3) {
            // Se não conseguiu ler os 3, provavelmente não é uma requisição HTTP válida
            continue; 
        }

        // Ajusta o caminho do arquivo:
        // 1. Remove a barra inicial se houver (ex: "/foto.jpg" -> "foto.jpg")
        char *file_path_relative = path;
        if (file_path_relative[0] == '/') {
            file_path_relative++;
        }
        // 2. Se o caminho for vazio (apenas "/"), serve o "index.html" por padrão
        if (strlen(file_path_relative) == 0) {
            file_path_relative = "index.html";
        }
        
        // 3. Define a pasta base como "HTML/" para organização
        char full_file_path[512];
        sprintf(full_file_path, "HTML/%s", file_path_relative);

        // --- LEITURA DO ARQUIVO SOLICITADO ---
        FILE *file = fopen(full_file_path, "rb"); // "rb" = read binary (importante para imagens!)
        if (file == NULL) {
            // Arquivo não existe: Retorna erro 404 Not Found
            printf("404 Not Found: %s\n", full_file_path);
            char *response = "HTTP/1.1 404 Not Found\r\nConnection: keep-alive\r\nContent-Length: 13\r\n\r\n404 Not Found";
            write(client_socket, response, strlen(response));
            continue; // Volta a esperar nova requisição na mesma conexão
        }

        // Descobre o tamanho do arquivo (necessário para o cabeçalho Content-Length)
        fseek(file, 0, SEEK_END); // Vai para o fim do arquivo
        long file_size = ftell(file); // Vê a posição atual (que é o tamanho)
        fseek(file, 0, SEEK_SET); // Volta para o início

        // Aloca um buffer na memória do tamanho exato do arquivo
        char *file_content = malloc(file_size);
        // Lê o arquivo inteiro para a memória RAM
        fread(file_content, 1, file_size, file);
        fclose(file); // Fecha o arquivo no disco

        // --- PREPARAÇÃO DA RESPOSTA HTTP ---
        char http_header[BUFFER_SIZE];
        const char *content_type = get_content_type(file_path_relative);
        
        // Monta os cabeçalhos HTTP padrão
        sprintf(http_header, 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Connection: keep-alive\r\n" // Informa que suportamos manter a conexão aberta
                "\r\n", // Linha em branco obrigatória entre headers e corpo
                content_type, file_size);
        
        // 1. ENVIA CABEÇALHOS (Sempre rápido, sem controle de taxa)
        write(client_socket, http_header, strlen(http_header));

        // 2. ENVIA O CONTEÚDO DO ARQUIVO (Com possível controle de taxa)
        // Aplica a regra de negócio: "enviar objetos (exceto html) com taxa controlada"
        if (strcmp(content_type, "text/html") == 0) {
            // CASO 1: É HTML. Envia tudo de uma vez (velocidade máxima)
            printf("Enviando HTML %s sem throttling.\n", file_path_relative);
            write(client_socket, file_content, file_size);
        } else {
            // CASO 2: NÃO é HTML (ex: imagem). Aplica o Throttling.
            // Chama nossa função customizada que envia devagar usando 'nanosleep'
            printf("QoS [Throttling]: Enviando %s limitado a %d kbps.\n", file_path_relative, client_rate);
            send_throttled(client_socket, file_content, file_size, client_rate);
        }
        
        free(file_content); // Libera a memória RAM que usou para o arquivo

        // Verifica se o cliente pediu explicitamente para fechar a conexão
        if (strstr(buffer, "Connection: close") != NULL) {
            break;
        }
    } // Fim do loop while(1)

    // === [QoS] SAÍDA E LIBERAÇÃO DE RECURSOS ===
    // O cliente desconectou. Precisamos "devolver" a banda que ele estava usando
    // para a piscina global, para que outros possam usar.
    pthread_mutex_lock(&rate_mutex);
    current_allocated_rate_kbps -= client_rate; // Subtrai a taxa deste cliente do total
    pthread_mutex_unlock(&rate_mutex);
    
    close(client_socket); // Fecha o socket TCP
    printf("Conexão fechada. Banda liberada: %d kbps. Uso Global: %d/%d kbps\n", 
           client_rate, current_allocated_rate_kbps, max_server_rate_kbps);
           
    pthread_exit(NULL); // Termina a thread
}
