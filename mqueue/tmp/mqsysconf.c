#include <stdio.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("MQ_OPEN_MAX =%ld, MQ_PRIO_MAX = %ld\n",
        sysconf(_SC_MQ_OPEN_MAX), sysconf(_SC_MQ_PRIO_MAX));
        exit(0);

}