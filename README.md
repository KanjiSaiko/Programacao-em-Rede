# Servidor Web HTTP/1.1 com QoS (MVP 2)

**Universidade:** Universidade Federal do Pampa (UNIPAMPA)  
**Disciplina:** Fundamentos e Avaliação de Redes de Computadores  
**Semestre:** 2025/2  

---

## 👥 Autores
* **Henrique de Lima Bortolomiol**
* **Heduardo Witkoski**

---

## 📝 Descrição do Projeto
Este projeto consiste no desenvolvimento de um servidor web compatível com o protocolo **HTTP/1.1**, implementado em Linguagem C utilizando a biblioteca `Pthreads` para suporte a conexões concorrentes.

Esta versão (**MVP 2**) foca na implementação de mecanismos de **Qualidade de Serviço (QoS)**, atendendo aos seguintes requisitos:

1.  **Controle de Admissão:** O servidor limita a aceitação de novas conexões baseando-se na vazão total disponível (informada via linha de comando).
2.  **Traffic Shaping (Throttling):** Limita a taxa de transmissão de objetos (exceto arquivos HTML) com base no IP de origem, utilizando um arquivo de configuração (`rates.conf`).
3.  **Taxa Compartilhada:** Múltiplas conexões simultâneas de um mesmo endereço IP dividem dinamicamente a taxa pré-definida para aquele endereço.
4.  **Monitoramento em Tempo Real:** Exibe visualmente no terminal o status da conexão, a taxa alocada e uma estimativa de RTT (Round-Trip Time) obtida via Kernel Linux (`tcp_info`).

---

## 📂 Estrutura de Arquivos

Para que o servidor funcione corretamente, a estrutura de diretórios deve ser organizada da seguinte forma:

```text
.
├── servidor_mvp.c      # Código principal: Gerenciamento de socket, threads e lógica HTTP
├── controle_taxa.c     # Módulo QoS: Algoritmo de traffic shaping (envio fracionado)
├── controle_taxa.h     # Header: Definições e protótipos do módulo QoS
├── rates.conf          # Configuração: Lista de IPs e suas taxas (kbps)
├── README.md           # Este arquivo de documentação
└── HTML/               # [IMPORTANTE] Diretório contendo os arquivos do site
    ├── index.html      # Página principal
    └── imagem.jpg      # Imagem grande para teste de throttling

## ⚙️ Compilação

gcc -o servidor_mvp2 servidor_mvp.c controle_taxa.c -pthread -lrt

## 🚀 Como Executar e Testar
Para verificar se a banda está sendo dividida corretamente entre conexões do mesmo IP, abra dois terminais e execute o wget simultaneamente: wget http://localhost:5000/imagem.jpg -O /dev/null
Resultado Esperado: Se o limite do IP for 500 kbps, cada download deve ocorrer a aproximadamente 250 kbps

Acesse o endereço abaixo no seu navegador (se estiver na mesma máquina): http://localhost:5000/index.html

## 🧠 Detalhes de Implementação
**Arquitetura**
O servidor utiliza uma thread dedicada para cada cliente conectado (handle_client), permitindo concorrência real.

**Lógica de Traffic Shaping**
  O controle de taxa é implementado na camada de aplicação (arquivo controle_taxa.c):
    O arquivo é enviado em pedaços (CHUNK_SIZE = 1 KB).
    O servidor calcula o tempo necessário para transmitir esse pedaço na velocidade alvo: Tempo = Tamanho / Taxa.
    A thread é pausada (nanosleep) por esse tempo exato após cada envio.
    Lógica Compartilhada: Uma variável na struct de configuração conta o número de conexões ativas por IP. A cada iteração de envio, a taxa alvo é recalculada: Taxa_Efetiva = Taxa_Configurada / Conexoes_Ativas.
O cálculo do RTT e banda não é feito por "ping", mas sim consultando diretamente as estatísticas do Kernel Linux para o socket TCP aberto, utilizando a syscall getsockopt com a flag TCP_INFO

