#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>

#define SOCKET_PATH "/tmp/server.socket"

int main() {
    int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un servaddr = {.sun_family = AF_UNIX};
    strcpy(servaddr.sun_path, SOCKET_PATH);

    char msg[] = "PING";
    sendto(sockfd, msg, sizeof(msg), 0, 
          (struct sockaddr*)&servaddr, sizeof(servaddr));

    char buffer[100];
    recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);
    printf("Resposta: %s\n", buffer);

    close(sockfd);
    return 0;
}