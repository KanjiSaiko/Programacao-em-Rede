#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>

#define CHUNK_SIZE 1024 // 1 KB

void send_throttled(int socket_fd, const char *data, long data_size, int rate_kbps, int *active_clients_ptr) {
    if (rate_kbps <= 0) {
        write(socket_fd, data, data_size);
        return;
    }

    long chunks_to_send = data_size / CHUNK_SIZE;
    long last_chunk_size = data_size % CHUNK_SIZE;
    long bytes_sent = 0;

    // Loop de envio
    for (int i = 0; i < chunks_to_send; i++) {
        // 1. Envia o pedaço
        if (write(socket_fd, data + bytes_sent, CHUNK_SIZE) < 0) {
            perror("Erro ao enviar pedaço");
            return;
        }
        bytes_sent += CHUNK_SIZE;
        
        // --- LÓGICA DE TAXA COMPARTILHADA ---
        // Verifica quantos clientes estão compartilhando essa taxa agora
        int divisor = 1;
        if (active_clients_ptr != NULL) {
            divisor = *active_clients_ptr;
            if (divisor < 1) divisor = 1; // Segurança
        }

        // Divide a taxa total pelo número de clientes
        double effective_rate_kbps = (double)rate_kbps / (double)divisor;

        // Recalcula o tempo de espera para este pedaço específico
        double rate_bytes_per_sec = (effective_rate_kbps * 1000.0) / 8.0;
        double sleep_time_sec = (double)CHUNK_SIZE / rate_bytes_per_sec;
        long sleep_time_usec = (long)(sleep_time_sec * 1000000.0);

        struct timespec sleep_spec;
        sleep_spec.tv_sec = sleep_time_usec / 1000000;
        sleep_spec.tv_nsec = (sleep_time_usec % 1000000) * 1000;

        // Pausa dinâmica
        nanosleep(&sleep_spec, NULL);
    }

    if (last_chunk_size > 0) {
        write(socket_fd, data + bytes_sent, last_chunk_size);
    }
}
