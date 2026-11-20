/* rozOS — nslookup <name>: DNS দিয়ে নামের ঠিকানা (ধাপ ৯) */

#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>

int main(int argc, char **argv)
{
    struct addrinfo hints = { 0 }, *ai;
    int r;
    if (argc < 2) {
        fprintf(stderr, "usage: nslookup <name>\n");
        return 2;
    }
    hints.ai_family = AF_INET;
    r = getaddrinfo(argv[1], NULL, &hints, &ai);
    if (r) {
        fprintf(stderr, "nslookup: %s: %s\n", argv[1], gai_strerror(r));
        return 1;
    }
    printf("%s has address %s\n", argv[1], inet_ntoa(((struct sockaddr_in *)ai->ai_addr)->sin_addr));
    freeaddrinfo(ai);
    return 0;
}
