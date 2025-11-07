/*
 * MÓDULO DE CONTROLE DE TAXA (QoS - Throttling)
 * -------------------------------------------------------------------
 * Este arquivo implementa o mecanismo de "freio" do servidor.
 * Ele não sabe o que é HTTP, nem quem é o cliente. Sua única função
 * é pegar um bloco de dados e enviá-lo lentamente, respeitando
 * um limite de velocidade definido.
 */

#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>    // Necessário para struct timespec e nanosleep()

// DEFINIÇÃO DA GRANULARIDADE DE ENVIO
// 1024 bytes (1KB) é um bom equilíbrio.
// - Se for muito pequeno (ex: 1 byte), o overhead de chamar write() e nanosleep()
//   milhares de vezes por segundo consumiria muita CPU.
// - Se for muito grande (ex: 1MB), o envio fica "rajado" (bursty), enviando
//   muito rápido e depois dormindo muito tempo, o que não é uma taxa suave.
#define CHUNK_SIZE 1024 

/**
 * Função principal de envio com throttling (Traffic Shaping de Aplicação).
 * * A lógica é baseada na física simples: Velocidade = Distância / Tempo.
 * Aqui, "Distância" é a quantidade de dados (CHUNK_SIZE) e "Velocidade" é a taxa alvo.
 * Nós calculamos o "Tempo" necessário para enviar cada pedaço e pausamos a thread
 * para forçar que esse tempo seja respeitado.
 */
void send_throttled(int socket_fd, const char *data, long data_size, int rate_kbps) {
    
    //VALIDAÇÃO DE SEGURANÇA
    // Se a taxa for inválida (0 ou negativa), não aplicamos o "freio".
    // Enviamos tudo de uma vez usando a chamada de sistema padrão.
    if (rate_kbps <= 0) {
        write(socket_fd, data, data_size);
        return;
    }

    //CONVERSÃO DE UNIDADES
    // O parâmetro vem em Kilobits por segundo (kbps), padrão de redes.
    // Mas as funções de envio (write) trabalham com Bytes.
    // - Multiplicamos por 1000 para ter bits por segundo.
    // - Dividimos por 8 para ter Bytes por segundo.
    double rate_bytes_per_sec = (rate_kbps * 1000.0) / 8.0;

    //PLANEJAMENTO DO ENVIO
    // Calculamos quantos pacotes completos de 1KB (chunks) precisaremos enviar.
    long chunks_to_send = data_size / CHUNK_SIZE;
    // Calculamos se sobra algum restinho de dados menor que 1KB no final.
    long last_chunk_size = data_size % CHUNK_SIZE;

    //CÁLCULO DA PAUSA
    // Quanto tempo devemos levar para enviar 1KB na velocidade desejada?
    // Tempo (s) = Quantidade (Bytes) / Velocidade (Bytes/s)
    double sleep_time_sec = (double)CHUNK_SIZE / rate_bytes_per_sec;

    // Convertemos esse tempo para microssegundos para facilitar a configuração da struct
    long sleep_time_usec = (long)(sleep_time_sec * 1000000.0);

    // Configuramos a estrutura 'timespec' exigida pela função nanosleep().
    // Ela precisa do tempo dividido em segundos inteiros e nanossegundos (parte fracionária).
    // Usamos nanosleep em vez de usleep ou sleep porque ela é mais precisa e padrão POSIX.
    struct timespec sleep_spec;
    sleep_spec.tv_sec = sleep_time_usec / 1000000;           // Parte inteira (segundos)
    sleep_spec.tv_nsec = (sleep_time_usec % 1000000) * 1000; // Resto convertido para nanosegundos

    long bytes_sent = 0; // Rastreador de posição no buffer de dados

    //LOOP DE ENVIO (Throttling Loop)
    for (int i = 0; i < chunks_to_send; i++) {
        // Envia 1KB de dados a partir da posição atual (data + bytes_sent)
        ssize_t written = write(socket_fd, data + bytes_sent, CHUNK_SIZE);
        if (written < 0) {
            perror("Erro fatal ao enviar pedaço (chunk) durante throttling");
            return; // Se a conexão caiu no meio do envio, abortamos.
        }
        bytes_sent += written;
        
        // Pausa a thread exatamente pelo tempo calculado para simular a velocidade desejada.
        // O sistema operacional coloca esta thread para dormir e vai fazer outras coisas.
        nanosleep(&sleep_spec, NULL);
    }

    // Se o arquivo não era múltiplo exato de 1024 bytes, enviamos o que sobrou.
    // Não precisa de pausa depois deste último pedaço, pois já terminamos.
    if (last_chunk_size > 0) {
        if (write(socket_fd, data + bytes_sent, last_chunk_size) < 0) {
            perror("Erro ao enviar último pedaço");
        }
        // bytes_sent += last_chunk_size;
    }
}
