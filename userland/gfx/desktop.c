/* rozOS — desktop: টেক্সট মোড থেকে ডেস্কটপ (rozwm) চালু (ধাপ ১৪)
 *   desktop          rozwm -t (একটা টার্মিনালসহ); ডেস্কটপ বন্ধ হলে এই shell-এ ফেরা
 * বুটে ডেস্কটপ না চাইলে /etc/rozos.conf-এ mode=text (help desktop)। */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    execl("/bin/rozwm", "rozwm", "-t", (char *)NULL);
    fprintf(stderr, "desktop: /bin/rozwm: %s\n", strerror(errno));
    return 1;
}
