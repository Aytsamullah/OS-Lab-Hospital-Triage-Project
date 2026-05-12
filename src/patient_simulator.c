/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : patient_simulator.c
 * Members : Member 1 (24F-0569), Member 2 (24F-0563)
 * Purpose : Child process created by fork() + execv().
 * Compile : gcc -Wall -Wextra -pthread -o bin/patient_simulator src/patient_simulator.c
 * ============================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc != 6) {
        fprintf(stderr,
                "Usage: %s <patient_id> <priority> <bed_id> <bed_type> <seconds>\n",
                argv[0]);
        return 1;
    }

    int patient_id = atoi(argv[1]);
    int priority = atoi(argv[2]);
    int bed_id = atoi(argv[3]);
    const char *bed_type = argv[4];
    int seconds = atoi(argv[5]);

    printf("[patient %d] arrived priority=%d bed=%d type=%s\n",
           patient_id,
           priority,
           bed_id,
           bed_type);

    int shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard), 0666);
    if (shm_id != -1) {
        SharedWard *ward = shmat(shm_id, NULL, 0);
        if (ward != (void *)-1) {
            printf("[patient %d] shared memory has %d bed partitions\n",
                   patient_id,
                   ward->bed_count);
            shmdt(ward);
        }
    }

    printf("[patient %d] treatment started for %d seconds\n", patient_id, seconds);
    sleep((unsigned int)seconds);

    int fd = open(DISCHARGE_FIFO, O_WRONLY);
    if (fd == -1) {
        perror("open discharge fifo");
        return 1;
    }

    char message[32];
    int length = snprintf(message, sizeof(message), "%d\n", patient_id);
    if (write(fd, message, (size_t)length) == -1) {
        perror("write discharge fifo");
        close(fd);
        return 1;
    }
    close(fd);

    printf("[patient %d] discharged\n", patient_id);
    return 0;
}
