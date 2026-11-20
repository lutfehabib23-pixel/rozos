/* rozOS — http-get: HTTP/1.0 দিয়ে একটা পাতা আনা (ধাপ ৯; HTTPS ধাপ ১০-এ)
 *
 *   http-get [-h] [-o file] http://host[:port]/path
 *   -h        শুধু হেডার দেখানো        -o file   শরীরটা ফাইলে লেখা */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <rozos.h>

int main(int argc, char **argv)
{
    char host[256], path[1024], req[1600], buf[4096];
    const char *url, *p, *outfile = NULL;
    struct addrinfo hints, *ai;
    struct timeval tv;
    int fd, out = 1, port = 80, r, opt, headers_only = 0, in_body = 0, nl = 0;
    long total = 0, t0;
    ssize_t n;

    while ((opt = getopt(argc, argv, "ho:")) != -1) {
        if (opt == 'h') headers_only = 1;
        else if (opt == 'o') outfile = optarg;
        else return 2;
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: http-get [-h] [-o file] http://host[:port]/path\n");
        return 2;
    }
    url = argv[optind];
    if (!strncmp(url, "https://", 8)) {
        fprintf(stderr, "http-get: https is not supported yet (TLS comes in phase 10)\n");
        return 2;
    }
    if (!strncmp(url, "http://", 7)) url += 7;
    p = url + strcspn(url, ":/");
    snprintf(host, sizeof(host), "%.*s", (int)(p - url), url);
    if (*p == ':') {
        port = atoi(p + 1);
        p += strcspn(p, "/");
    }
    snprintf(path, sizeof(path), "%s", *p ? p : "/");

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(buf, sizeof(buf), "%d", port);
    t0 = rozos_now_ms();
    if ((r = getaddrinfo(host, buf, &hints, &ai)) != 0) {
        fprintf(stderr, "http-get: %s: %s\n", host, gai_strerror(r));
        return 1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("http-get: socket"); return 1; }
    fprintf(stderr, "connecting to %s (%s) port %d ...\n", host,
            inet_ntoa(((struct sockaddr_in *)ai->ai_addr)->sin_addr), port);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        fprintf(stderr, "http-get: connect: %s\n", strerror(errno));
        return 1;
    }
    freeaddrinfo(ai);
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    n = snprintf(req, sizeof(req),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: rozOS-http-get/0.9\r\nAccept: */*\r\n"
                 "Connection: close\r\n\r\n", path, host);
    if (send(fd, req, (size_t)n, 0) != n) { perror("http-get: send"); return 1; }

    if (outfile) {
        out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { fprintf(stderr, "http-get: %s: %s\n", outfile, strerror(errno)); return 1; }
    }
    /* হেডার stderr-এ (বা -h হলে stdout-এ), শরীর stdout/ফাইলে */
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        ssize_t i = 0;
        while (!in_body && i < n) {
            char c = buf[i++];
            if (headers_only) write(1, &c, 1);
            else write(2, &c, 1);
            if (c == '\n') {
                if (++nl == 2) in_body = 1;
            } else if (c != '\r') {
                nl = 0;
            }
        }
        if (in_body && !headers_only && i < n) {
            write(out, buf + i, (size_t)(n - i));
            total += n - i;
        }
    }
    if (n < 0) fprintf(stderr, "http-get: %s\n", strerror(errno));
    close(fd);
    if (outfile) close(out);
    fprintf(stderr, "\n%ld bytes in %ld ms\n", total, rozos_now_ms() - t0);
    return n < 0;
}
