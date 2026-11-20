/* rozOS: first user-space program (to be compiled inside rozOS in phases 7-8).
 * Note: files shown on the rozOS console should stay ASCII - the VGA font has
 * no Bengali glyphs yet. */
#include <stdio.h>

int main(void)
{
    printf("Hello from rozOS!\n");
    return 0;
}
