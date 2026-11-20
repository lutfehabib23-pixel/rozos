/* rozOS — textmode: ডেস্কটপ বন্ধ করে টেক্সট কনসোলে যাওয়া (ধাপ ১৪)
 *   textmode            ডেস্কটপের সব উইন্ডো বন্ধ, টেক্সট কনসোলে shell (init চালায়)
 *   textmode -r / -p    restart / বন্ধ (poweroff) — Start মেনুর মতো
 * rozwm-কে socket-এ WM_QUIT পাঠায়। ডেস্কটপ না চললে কিছু করার নেই। */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <rozwm_proto.h>

int main(int argc, char **argv)
{
    struct sockaddr_un a;
    struct wm_msg m;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, ROZWM_SOCKET);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "textmode: the desktop is not running (already text mode)\n");
        return 1;
    }
    memset(&m, 0, sizeof(m));
    m.type = WM_QUIT;
    m.a = (argc > 1 && !strcmp(argv[1], "-r")) ? 'r' : (argc > 1 && !strcmp(argv[1], "-p")) ? 'p' : 't';
    write(fd, &m, sizeof(m));
    close(fd);
    return 0;
}
