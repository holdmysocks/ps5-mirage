#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("mirage hello: pid %d\n", getpid());
    return 0;
}
