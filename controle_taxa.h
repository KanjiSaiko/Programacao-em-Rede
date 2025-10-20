#ifndef CONTROLE_TAXA_H
#define CONTROLE_TAXA_H

// Este é o "anúncio" (protótipo) da sua função.
// Ele diz ao compilador: "Existe uma função chamada send_throttled
// que recebe estes parâmetros. Confie em mim."
void send_throttled(int socket_fd, const char *data, long data_size, int rate_kbps);

#endif // CONTROLE_TAXA_H
