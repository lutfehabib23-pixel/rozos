/* rozOS — poweroff [-h]: বিদ্যুৎ বন্ধ (ACPI), -h = শুধু থামা (ধাপ ১১খ)
 * QEMU-তে poweroff দিলে QEMU নিজেও বন্ধ হয়। */

#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>

int main(int argc, char **argv)
{
    int halt = argc > 1 && !strcmp(argv[1], "-h");
    printf("%s...\n", halt ? "halting" : "powering off");
    fflush(stdout);
    reboot(halt ? RB_HALT_SYSTEM : RB_POWER_OFF);
    perror("poweroff");
    return 1;
}
