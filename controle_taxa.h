#ifndef CONTROLE_TAXA_H
#define CONTROLE_TAXA_H

// Atualizado: agora recebe um ponteiro para o número de clientes ativos
void send_throttled(int socket_fd, const char *data, long data_size, int rate_kbps, int *active_clients_ptr);

#endif
