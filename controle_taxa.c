#include <unistd.h> // Para usleep()
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>    // Para nanosleep (alternativa mais precisa)

// Tamanho do "pedaço" que enviaremos de cada vez
#define CHUNK_SIZE 1024 // 1 KB

/**
 * Envia dados por um socket com taxa controlada.
 * * @param socket_fd O descritor do socket do cliente.
 * @param data O ponteiro para os dados a serem enviados.
 * @param data_size O tamanho total dos dados a serem enviados.
 * @param rate_kbps A taxa máxima de envio desejada, em kilobits por segundo.
 */
void send_throttled(int socket_fd, const char *data, long data_size, int rate_kbps) {
    if (rate_kbps <= 0) {
        // Se a taxa for 0 ou negativa, apenas envia tudo de uma vez
        write(socket_fd, data, data_size);
        return;
    }

    // 1. Converter a taxa de Kilobits/s para Bytes/s
    double rate_bytes_per_sec = (rate_kbps * 1000.0) / 8.0;

    // 2. Calcular quantos "pedaços" (chunks) temos que enviar
    long chunks_to_send = data_size / CHUNK_SIZE;
    long last_chunk_size = data_size % CHUNK_SIZE;

    // 3. Calcular o tempo de espera (sleep) por pedaço, em microssegundos
    // Tempo = Tamanho / Taxa
    // Tempo (s) = CHUNK_SIZE (bytes) / rate_bytes_per_sec (bytes/s)
    double sleep_time_sec = (double)CHUNK_SIZE / rate_bytes_per_sec;
    long sleep_time_usec = (long)(sleep_time_sec * 1000000.0);

    // Estrutura de tempo para nanosleep (mais preciso que usleep)
    struct timespec sleep_spec;
    sleep_spec.tv_sec = sleep_time_usec / 1000000;
    sleep_spec.tv_nsec = (sleep_time_usec % 1000000) * 1000;


    long bytes_sent = 0;

    // 4. Loop de envio
    for (int i = 0; i < chunks_to_send; i++) {
        if (write(socket_fd, data + bytes_sent, CHUNK_SIZE) < 0) {
            perror("Erro ao enviar pedaço (chunk)");
            return; // Encerra a função se houver erro
        }
        bytes_sent += CHUNK_SIZE;
        
        // Pausa para controlar a taxa
        nanosleep(&sleep_spec, NULL);
    }

    // 5. Envia o último pedaço (se houver)
    if (last_chunk_size > 0) {
        if (write(socket_fd, data + bytes_sent, last_chunk_size) < 0) {
            perror("Erro ao enviar último pedaço");
        }
        bytes_sent += last_chunk_size;
    }
    
    // printf("Enviados %ld bytes com taxa controlada de %d kbps\n", bytes_sent, rate_kbps);
}
