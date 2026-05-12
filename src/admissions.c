/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : admissions.c
 * Members : Member 1 (24F-0569), Member 2 (24F-0563)
 * Purpose : Simple central admissions manager.
 * Compile : gcc -Wall -Wextra -pthread -o bin/admissions src/admissions.c
 * ============================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_SCHEDULE_PATIENTS 100

typedef struct {
    int patient_id;
    char name[MAX_NAME];
    int priority;
    int burst_time;
} SchedulePatient;

static volatile sig_atomic_t running = 1;
static SharedWard *ward = NULL;
static int shm_id = -1;

static pthread_mutex_t bed_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bed_available = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t discharge_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t discharge_available = PTHREAD_COND_INITIALIZER;

static sem_t empty_slots;
static sem_t filled_slots;
static sem_t *icu_sem = SEM_FAILED;
static sem_t *iso_sem = SEM_FAILED;

static PatientRecord queue[MAX_QUEUE];
static int queue_count = 0;
static int discharge_queue[MAX_QUEUE];
static int discharge_count = 0;
static pid_t children[MAX_CHILDREN];
static int child_count = 0;
static pthread_mutex_t schedule_lock = PTHREAD_MUTEX_INITIALIZER;
static SchedulePatient schedule_patients[MAX_SCHEDULE_PATIENTS];
static int schedule_count = 0;

static char allocation_strategy[16] = "best";
static FILE *memory_log = NULL;
static FILE *schedule_log = NULL;

static const char *bed_type_for_patient(const PatientRecord *patient)
{
    if (patient->care_units == ICU_UNITS) {
        return "ICU";
    }
    if (patient->care_units == ISO_UNITS || patient->infectious) {
        return "ISOLATION";
    }
    return "GENERAL";
}

static int treatment_time_for_patient(const PatientRecord *patient)
{
    const char *fast = getenv("HOSPITAL_FAST");
    if (fast != NULL && strcmp(fast, "1") == 0) {
        return patient->care_units;
    }
    if (patient->care_units == ICU_UNITS) {
        return 5 + (rand() % 11);
    }
    if (patient->care_units == ISO_UNITS) {
        return 3 + (rand() % 8);
    }
    return 2 + (rand() % 7);
}

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
    sem_post(&filled_slots);
    if (icu_sem != SEM_FAILED) {
        sem_post(icu_sem);
    }
    if (iso_sem != SEM_FAILED) {
        sem_post(iso_sem);
    }
}

static void reap_child(int signal_number)
{
    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        ;
    }
}

static void set_bed(BedPartition *bed, int id, int start, int size, const char *type)
{
    bed->partition_id = id;
    bed->start_unit = start;
    bed->size = size;
    bed->is_free = 1;
    bed->patient_id = -1;
    snprintf(bed->bed_type, sizeof(bed->bed_type), "%s", type);
}

static void renumber_beds(void)
{
    for (int i = 0; i < ward->bed_count; i++) {
        ward->beds[i].partition_id = i;
    }
}

static void init_ward(void)
{
    int index = 0;
    int start = 0;

    memset(ward, 0, sizeof(*ward));
    ward->next_patient_id = 1;

    for (int i = 0; i < ICU_CAPACITY; i++) {
        set_bed(&ward->beds[index], index, start, ICU_UNITS, "FREE");
        start += ICU_UNITS;
        index++;
    }
    for (int i = 0; i < ISO_CAPACITY; i++) {
        set_bed(&ward->beds[index], index, start, ISO_UNITS, "FREE");
        start += ISO_UNITS;
        index++;
    }
    for (int i = 0; i < GENERAL_CAPACITY; i++) {
        set_bed(&ward->beds[index], index, start, GENERAL_UNITS, "FREE");
        start += GENERAL_UNITS;
        index++;
    }

    ward->bed_count = index;
}

static void make_ward_map(char *buffer, size_t size)
{
    char units[TOTAL_UNITS + 1];
    size_t used = 0;

    for (int i = 0; i < TOTAL_UNITS; i++) {
        units[i] = '.';
    }
    units[TOTAL_UNITS] = '\0';

    for (int i = 0; i < ward->bed_count; i++) {
        BedPartition *bed = &ward->beds[i];
        char mark = '.';
        if (!bed->is_free) {
            if (strcmp(bed->bed_type, "ICU") == 0) {
                mark = 'I';
            } else if (strcmp(bed->bed_type, "ISOLATION") == 0) {
                mark = 'X';
            } else {
                mark = 'G';
            }
        }
        for (int u = bed->start_unit; u < bed->start_unit + bed->size && u < TOTAL_UNITS; u++) {
            units[u] = mark;
        }
    }

    buffer[0] = '\0';
    for (int i = 0; i < TOTAL_UNITS && used + 3 < size; i++) {
        used += (size_t)snprintf(buffer + used, size - used, "%c", units[i]);
        if ((i + 1) % PAGE_SIZE == 0 && i + 1 < TOTAL_UNITS) {
            used += (size_t)snprintf(buffer + used, size - used, " ");
        }
    }
}

static void current_time_text(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm local_time;

    localtime_r(&now, &local_time);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &local_time);
}

static void log_memory_event(const char *event, const PatientRecord *patient, int internal_frag)
{
    int total_free = 0;
    int largest_free = 0;
    int page_owner[PAGE_COUNT];
    char map[128];
    char timestamp[32];

    for (int i = 0; i < PAGE_COUNT; i++) {
        page_owner[i] = -1;
    }

    for (int i = 0; i < ward->bed_count; i++) {
        BedPartition *bed = &ward->beds[i];
        if (bed->is_free) {
            total_free += bed->size;
            if (bed->size > largest_free) {
                largest_free = bed->size;
            }
        } else {
            int first_page = bed->start_unit / PAGE_SIZE;
            int last_page = (bed->start_unit + bed->size - 1) / PAGE_SIZE;
            for (int p = first_page; p <= last_page && p < PAGE_COUNT; p++) {
                page_owner[p] = bed->patient_id;
            }
        }
    }

    double external_frag = 0.0;
    if (total_free > 0) {
        external_frag = (1.0 - ((double)largest_free / (double)total_free)) * 100.0;
    }

    make_ward_map(map, sizeof(map));
    current_time_text(timestamp, sizeof(timestamp));
    fprintf(memory_log,
            "[%s] %s | free=%d largest=%d external=%.2f%% map=%s\n",
            timestamp,
            event,
            total_free,
            largest_free,
            external_frag,
            map);

    if (patient != NULL) {
        fprintf(memory_log,
                "  patient=%d name=%s care_units=%d internal_frag=%d\n",
                patient->patient_id,
                patient->name,
                patient->care_units,
                internal_frag);
    }

    fprintf(memory_log, "  pages:");
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (page_owner[i] == -1) {
            fprintf(memory_log, " P%d=FREE", i);
        } else {
            fprintf(memory_log, " P%d=%d", i, page_owner[i]);
        }
    }
    fprintf(memory_log, "\n");
    fflush(memory_log);

    printf("[memory] %s | free=%d largest=%d external=%.2f%% map=%s\n",
           event,
           total_free,
           largest_free,
           external_frag,
           map);
}

static int find_free_partition(int units)
{
    int selected = -1;

    for (int i = 0; i < ward->bed_count; i++) {
        if (!ward->beds[i].is_free || ward->beds[i].size < units) {
            continue;
        }
        if (strcmp(allocation_strategy, "first") == 0) {
            return i;
        }
        if (selected == -1) {
            selected = i;
        } else if (strcmp(allocation_strategy, "best") == 0 &&
                   ward->beds[i].size < ward->beds[selected].size) {
            selected = i;
        } else if (strcmp(allocation_strategy, "worst") == 0 &&
                   ward->beds[i].size > ward->beds[selected].size) {
            selected = i;
        }
    }

    return selected;
}

static int allocate_partition(PatientRecord *patient)
{
    int index = find_free_partition(patient->care_units);
    if (index == -1) {
        return -1;
    }

    BedPartition *bed = &ward->beds[index];
    int old_size = bed->size;
    int remaining = old_size - patient->care_units;
    int internal_frag = ((patient->care_units + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE -
                        patient->care_units;

    bed->size = patient->care_units;
    bed->is_free = 0;
    bed->patient_id = patient->patient_id;
    snprintf(bed->bed_type, sizeof(bed->bed_type), "%s", bed_type_for_patient(patient));

    if (remaining > 0 && ward->bed_count < MAX_BEDS) {
        for (int i = ward->bed_count; i > index + 1; i--) {
            ward->beds[i] = ward->beds[i - 1];
        }
        set_bed(&ward->beds[index + 1],
                index + 1,
                bed->start_unit + patient->care_units,
                remaining,
                "FREE");
        ward->bed_count++;
    }

    renumber_beds();
    ward->served_count++;
    ward->active_count++;
    log_memory_event("allocation", patient, internal_frag);
    return bed->partition_id;
}

static int find_patient_partition(int patient_id)
{
    for (int i = 0; i < ward->bed_count; i++) {
        if (!ward->beds[i].is_free && ward->beds[i].patient_id == patient_id) {
            return i;
        }
    }
    return -1;
}

static int coalesce(int index)
{
    if (index > 0 && ward->beds[index - 1].is_free) {
        ward->beds[index - 1].size += ward->beds[index].size;
        for (int i = index; i < ward->bed_count - 1; i++) {
            ward->beds[i] = ward->beds[i + 1];
        }
        ward->bed_count--;
        index--;
    }

    if (index + 1 < ward->bed_count && ward->beds[index + 1].is_free) {
        ward->beds[index].size += ward->beds[index + 1].size;
        for (int i = index + 1; i < ward->bed_count - 1; i++) {
            ward->beds[i] = ward->beds[i + 1];
        }
        ward->bed_count--;
    }

    renumber_beds();
    return index;
}

static void free_patient_bed(int patient_id, const char *nurse_type)
{
    PatientRecord log_patient;
    char released_type[MAX_BED_TYPE] = "GENERAL";
    char before[128];
    char after[128];

    memset(&log_patient, 0, sizeof(log_patient));
    log_patient.patient_id = patient_id;

    pthread_mutex_lock(&bed_lock);
    int index = find_patient_partition(patient_id);
    if (index == -1) {
        pthread_mutex_unlock(&bed_lock);
        return;
    }

    BedPartition *bed = &ward->beds[index];
    snprintf(released_type, sizeof(released_type), "%s", bed->bed_type);
    log_patient.care_units = bed->size;
    make_ward_map(before, sizeof(before));

    printf("[nurse-%s] freeing patient %d\n", nurse_type, patient_id);
    printf("[coalesce] before: %s\n", before);

    bed->is_free = 1;
    bed->patient_id = -1;
    snprintf(bed->bed_type, sizeof(bed->bed_type), "%s", "FREE");
    coalesce(index);
    ward->freed_count++;
    ward->active_count--;

    make_ward_map(after, sizeof(after));
    printf("[coalesce] after : %s\n", after);
    log_memory_event("deallocation/coalescing", &log_patient, 0);
    pthread_cond_broadcast(&bed_available);
    pthread_mutex_unlock(&bed_lock);

    if (strcmp(released_type, "ICU") == 0) {
        sem_post(icu_sem);
    } else if (strcmp(released_type, "ISOLATION") == 0) {
        sem_post(iso_sem);
    }
}

static int parse_patient_line(char *line, PatientRecord *patient)
{
    int count = sscanf(line,
                       "%63[^|]|%d|%d|%d|%d|%d",
                       patient->name,
                       &patient->age,
                       &patient->severity,
                       &patient->priority,
                       &patient->care_units,
                       &patient->infectious);
    if (count != 6) {
        return -1;
    }
    return 0;
}

static void enqueue_patient(PatientRecord patient)
{
    sem_wait(&empty_slots);
    pthread_mutex_lock(&queue_lock);

    int pos = queue_count;
    while (pos > 0 && queue[pos - 1].priority > patient.priority) {
        queue[pos] = queue[pos - 1];
        pos--;
    }
    queue[pos] = patient;
    queue_count++;

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_lock);
    sem_post(&filled_slots);
}

static int dequeue_patient(PatientRecord *patient)
{
    sem_wait(&filled_slots);
    if (!running) {
        return -1;
    }

    pthread_mutex_lock(&queue_lock);
    while (running && queue_count == 0) {
        pthread_cond_wait(&queue_not_empty, &queue_lock);
    }
    if (queue_count == 0) {
        pthread_mutex_unlock(&queue_lock);
        return -1;
    }

    *patient = queue[0];
    for (int i = 1; i < queue_count; i++) {
        queue[i - 1] = queue[i];
    }
    queue_count--;

    pthread_mutex_unlock(&queue_lock);
    sem_post(&empty_slots);
    return 0;
}

static void add_discharge(int patient_id)
{
    pthread_mutex_lock(&discharge_lock);
    if (discharge_count < MAX_QUEUE) {
        discharge_queue[discharge_count++] = patient_id;
        pthread_cond_broadcast(&discharge_available);
    }
    pthread_mutex_unlock(&discharge_lock);
}

static void remember_patient_for_scheduling(const PatientRecord *patient)
{
    pthread_mutex_lock(&schedule_lock);
    if (schedule_count < MAX_SCHEDULE_PATIENTS) {
        schedule_patients[schedule_count].patient_id = patient->patient_id;
        snprintf(schedule_patients[schedule_count].name,
                 sizeof(schedule_patients[schedule_count].name),
                 "%s",
                 patient->name);
        schedule_patients[schedule_count].priority = patient->priority;
        schedule_patients[schedule_count].burst_time = patient->treatment_time;
        schedule_count++;
    }
    pthread_mutex_unlock(&schedule_lock);
}

static int discharge_matches_type(int patient_id, const char *type)
{
    int matched = 0;

    pthread_mutex_lock(&bed_lock);
    int index = find_patient_partition(patient_id);
    if (index != -1 && strcmp(ward->beds[index].bed_type, type) == 0) {
        matched = 1;
    }
    pthread_mutex_unlock(&bed_lock);

    return matched;
}

static int take_discharge_for_type(const char *type, int *patient_id)
{
    for (int i = 0; i < discharge_count; i++) {
        if (discharge_matches_type(discharge_queue[i], type)) {
            *patient_id = discharge_queue[i];
            for (int j = i + 1; j < discharge_count; j++) {
                discharge_queue[j - 1] = discharge_queue[j];
            }
            discharge_count--;
            return 1;
        }
    }
    return 0;
}

static void wait_for_capacity(sem_t *limit, const char *type, int patient_id)
{
    if (sem_trywait(limit) == 0) {
        printf("[semaphore] %s capacity granted for patient %d\n", type, patient_id);
        return;
    }

    if (errno == EAGAIN) {
        printf("[semaphore] %s full; patient %d blocks until discharge\n",
               type,
               patient_id);
    }

    while (sem_wait(limit) == -1) {
        if (errno == EINTR && running) {
            continue;
        }
        return;
    }

    printf("[semaphore] %s released; patient %d continues\n", type, patient_id);
}

static void launch_patient(PatientRecord *patient, int bed_id)
{
    char id_arg[16];
    char priority_arg[16];
    char bed_arg[16];
    char time_arg[16];
    const char *type = bed_type_for_patient(patient);

    snprintf(id_arg, sizeof(id_arg), "%d", patient->patient_id);
    snprintf(priority_arg, sizeof(priority_arg), "%d", patient->priority);
    snprintf(bed_arg, sizeof(bed_arg), "%d", bed_id);
    snprintf(time_arg, sizeof(time_arg), "%d", patient->treatment_time);

    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {
            "./bin/patient_simulator",
            id_arg,
            priority_arg,
            bed_arg,
            (char *)type,
            time_arg,
            NULL
        };
        execv(args[0], args);
        perror("execv");
        _exit(127);
    }
    if (pid > 0) {
        if (child_count < MAX_CHILDREN) {
            children[child_count++] = pid;
        }
        printf("[scheduler] fork + exec patient %d pid=%ld\n", patient->patient_id, (long)pid);
    }
}

static void *receptionist_thread(void *arg)
{
    (void)arg;

    while (running) {
        int fd = open(TRIAGE_FIFO, O_RDONLY);
        if (fd == -1) {
            continue;
        }

        FILE *input = fdopen(fd, "r");
        if (input == NULL) {
            close(fd);
            continue;
        }

        char line[256];
        while (running && fgets(line, sizeof(line), input) != NULL) {
            PatientRecord patient;
            if (strncmp(line, "SHUTDOWN", 8) == 0) {
                running = 0;
                break;
            }
            if (parse_patient_line(line, &patient) == 0) {
                pthread_mutex_lock(&bed_lock);
                patient.patient_id = ward->next_patient_id++;
                pthread_mutex_unlock(&bed_lock);
                patient.arrival_time = time(NULL);
                patient.treatment_time = treatment_time_for_patient(&patient);
                remember_patient_for_scheduling(&patient);
                enqueue_patient(patient);
                printf("[receptionist] patient %d queued name=%s priority=%d care_units=%d treatment=%ds\n",
                       patient.patient_id,
                       patient.name,
                       patient.priority,
                       patient.care_units,
                       patient.treatment_time);
            }
        }

        fclose(input);
    }

    return NULL;
}

static void *scheduler_thread(void *arg)
{
    (void)arg;

    while (running) {
        PatientRecord patient;
        if (dequeue_patient(&patient) != 0) {
            continue;
        }

        const char *type = bed_type_for_patient(&patient);
        if (strcmp(type, "ICU") == 0) {
            wait_for_capacity(icu_sem, "ICU", patient.patient_id);
        } else if (strcmp(type, "ISOLATION") == 0) {
            wait_for_capacity(iso_sem, "ISOLATION", patient.patient_id);
        }
        if (!running) {
            break;
        }

        int bed_id = -1;
        while (running && bed_id == -1) {
            pthread_mutex_lock(&bed_lock);
            bed_id = allocate_partition(&patient);
            if (bed_id == -1) {
                pthread_cond_wait(&bed_available, &bed_lock);
            }
            pthread_mutex_unlock(&bed_lock);
        }

        if (bed_id != -1) {
            launch_patient(&patient, bed_id);
        }
    }

    return NULL;
}

static void *discharge_reader_thread(void *arg)
{
    (void)arg;

    while (running) {
        int fd = open(DISCHARGE_FIFO, O_RDONLY);
        if (fd == -1) {
            continue;
        }

        FILE *input = fdopen(fd, "r");
        if (input == NULL) {
            close(fd);
            continue;
        }

        char line[64];
        while (running && fgets(line, sizeof(line), input) != NULL) {
            int patient_id = atoi(line);
            if (patient_id > 0) {
                add_discharge(patient_id);
            }
        }

        fclose(input);
    }

    return NULL;
}

static void *nurse_thread(void *arg)
{
    const char *type = (const char *)arg;

    while (running) {
        int patient_id = -1;

        pthread_mutex_lock(&discharge_lock);
        while (running && !take_discharge_for_type(type, &patient_id)) {
            pthread_cond_wait(&discharge_available, &discharge_lock);
        }
        pthread_mutex_unlock(&discharge_lock);

        if (patient_id > 0) {
            free_patient_bed(patient_id, type);
        }
    }

    return NULL;
}

static void sort_order_by_burst(const SchedulePatient patients[], int order[], int n)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int left = order[i];
            int right = order[j];
            if (patients[left].burst_time > patients[right].burst_time) {
                int temp = order[i];
                order[i] = order[j];
                order[j] = temp;
            }
        }
    }
}

static void sort_order_by_priority(const SchedulePatient patients[], int order[], int n)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int left = order[i];
            int right = order[j];
            if (patients[left].priority > patients[right].priority) {
                int temp = order[i];
                order[i] = order[j];
                order[j] = temp;
            }
        }
    }
}

static void write_non_preemptive_schedule(const char *name,
                                          const SchedulePatient patients[],
                                          const int order[],
                                          int n)
{
    int time_now = 0;
    double total_wait = 0.0;
    double total_turnaround = 0.0;

    fprintf(schedule_log, "\n%s\nGantt: ", name);
    for (int i = 0; i < n; i++) {
        int index = order[i];
        int wait = time_now;
        fprintf(schedule_log,
                "| t=%d P%d(%s) ",
                time_now,
                patients[index].patient_id,
                patients[index].name);
        time_now += patients[index].burst_time;
        total_wait += wait;
        total_turnaround += time_now;
    }
    fprintf(schedule_log,
            "| t=%d\nAverage waiting time: %.2f\nAverage turnaround time: %.2f\n",
            time_now,
            total_wait / n,
            total_turnaround / n);
}

static void write_round_robin_schedule(const SchedulePatient patients[], int n, int quantum)
{
    int remaining[MAX_SCHEDULE_PATIENTS];
    int completion[MAX_SCHEDULE_PATIENTS];
    int done = 0;
    int time_now = 0;
    double total_wait = 0.0;
    double total_turnaround = 0.0;

    for (int i = 0; i < n; i++) {
        remaining[i] = patients[i].burst_time;
        completion[i] = 0;
    }

    fprintf(schedule_log, "\nRound Robin q=%d\nGantt: ", quantum);
    while (done < n) {
        for (int i = 0; i < n; i++) {
            if (remaining[i] <= 0) {
                continue;
            }
            int slice = remaining[i] < quantum ? remaining[i] : quantum;
            fprintf(schedule_log,
                    "| t=%d P%d(%s) ",
                    time_now,
                    patients[i].patient_id,
                    patients[i].name);
            time_now += slice;
            remaining[i] -= slice;
            if (remaining[i] == 0) {
                completion[i] = time_now;
                done++;
            }
        }
    }
    fprintf(schedule_log, "| t=%d\n", time_now);

    for (int i = 0; i < n; i++) {
        total_turnaround += completion[i];
        total_wait += completion[i] - patients[i].burst_time;
    }

    fprintf(schedule_log,
            "Average waiting time: %.2f\nAverage turnaround time: %.2f\n",
            total_wait / n,
            total_turnaround / n);
}

static void write_scheduling_report(void)
{
    SchedulePatient patients[MAX_SCHEDULE_PATIENTS];
    int order[MAX_SCHEDULE_PATIENTS];
    int n;

    if (schedule_log == NULL) {
        return;
    }

    pthread_mutex_lock(&schedule_lock);
    n = schedule_count;
    for (int i = 0; i < n; i++) {
        patients[i] = schedule_patients[i];
    }
    pthread_mutex_unlock(&schedule_lock);

    rewind(schedule_log);
    fprintf(schedule_log, "Scheduling log generated from actual submitted patients\n");

    if (n == 0) {
        fprintf(schedule_log, "No patients were submitted, so no scheduling metrics were produced.\n");
        fflush(schedule_log);
        return;
    }

    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    write_non_preemptive_schedule("FCFS", patients, order, n);

    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    sort_order_by_burst(patients, order, n);
    write_non_preemptive_schedule("SJF", patients, order, n);

    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    sort_order_by_priority(patients, order, n);
    write_non_preemptive_schedule("Priority Scheduling", patients, order, n);

    write_round_robin_schedule(patients, n, 3);
    fflush(schedule_log);
}

static int run_pipe_demo(void)
{
    char line[256];
    int count = 0;

    printf("[pipe-demo] Reading patient records from ordinary stdin pipe.\n");
    while (fgets(line, sizeof(line), stdin) != NULL) {
        PatientRecord patient;
        if (parse_patient_line(line, &patient) != 0) {
            fprintf(stderr, "[pipe-demo] invalid record: %s", line);
            continue;
        }
        count++;
        printf("[pipe-demo] patient=%d name=%s age=%d severity=%d priority=%d care_units=%d infectious=%d\n",
               count,
               patient.name,
               patient.age,
               patient.severity,
               patient.priority,
               patient.care_units,
               patient.infectious);
    }

    printf("[pipe-demo] Parsed %d record(s) through an anonymous pipe.\n", count);
    return count > 0 ? 0 : 1;
}

static int setup_ipc(void)
{
    int old_id = shmget((key_t)SHM_KEY, sizeof(SharedWard), 0666);
    if (old_id != -1) {
        shmctl(old_id, IPC_RMID, NULL);
    }

    shm_id = shmget((key_t)SHM_KEY, sizeof(SharedWard), IPC_CREAT | 0666);
    if (shm_id == -1) {
        perror("shmget");
        return -1;
    }

    ward = shmat(shm_id, NULL, 0);
    if (ward == (void *)-1) {
        perror("shmat");
        ward = NULL;
        return -1;
    }
    init_ward();

    unlink(TRIAGE_FIFO);
    unlink(DISCHARGE_FIFO);
    if (mkfifo(TRIAGE_FIFO, 0666) == -1 || mkfifo(DISCHARGE_FIFO, 0666) == -1) {
        perror("mkfifo");
        return -1;
    }

    sem_unlink(SEM_ICU_NAME);
    sem_unlink(SEM_ISO_NAME);
    icu_sem = sem_open(SEM_ICU_NAME, O_CREAT, 0666, ICU_CAPACITY);
    iso_sem = sem_open(SEM_ISO_NAME, O_CREAT, 0666, ISO_CAPACITY);
    if (icu_sem == SEM_FAILED || iso_sem == SEM_FAILED) {
        perror("sem_open");
        return -1;
    }

    sem_init(&empty_slots, 0, MAX_QUEUE);
    sem_init(&filled_slots, 0, 0);
    return 0;
}

static void write_pid_file(void)
{
    FILE *file = fopen(PID_FILE, "w");
    if (file != NULL) {
        fprintf(file, "%ld\n", (long)getpid());
        fclose(file);
    }
}

static void cleanup(void)
{
    FILE *summary = fopen("logs/hospital_summary.txt", "w");

    write_scheduling_report();

    if (summary != NULL && ward != NULL) {
        fprintf(summary, "Patients served: %d\n", ward->served_count);
        fprintf(summary, "Beds freed: %d\n", ward->freed_count);
        fprintf(summary, "Active patients: %d\n", ward->active_count);
        fclose(summary);
    }

    for (int i = 0; i < child_count; i++) {
        kill(children[i], SIGTERM);
    }

    if (icu_sem != SEM_FAILED) {
        sem_close(icu_sem);
        sem_unlink(SEM_ICU_NAME);
    }
    if (iso_sem != SEM_FAILED) {
        sem_close(iso_sem);
        sem_unlink(SEM_ISO_NAME);
    }
    sem_destroy(&empty_slots);
    sem_destroy(&filled_slots);

    unlink(TRIAGE_FIFO);
    unlink(DISCHARGE_FIFO);
    unlink(PID_FILE);

    if (ward != NULL) {
        shmdt(ward);
    }
    if (shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);
    }
    if (memory_log != NULL) {
        fclose(memory_log);
    }
    if (schedule_log != NULL) {
        fclose(schedule_log);
    }
}

int main(int argc, char *argv[])
{
    pthread_t receptionist;
    pthread_t scheduler;
    pthread_t discharge_reader;
    pthread_t nurse_icu;
    pthread_t nurse_iso;
    pthread_t nurse_general;

    if (argc == 2 && strcmp(argv[1], "--pipe-demo") == 0) {
        return run_pipe_demo();
    }

    if (argc == 3 && strcmp(argv[1], "--strategy") == 0) {
        if (strcmp(argv[2], "best") != 0 && strcmp(argv[2], "first") != 0 &&
            strcmp(argv[2], "worst") != 0) {
            fprintf(stderr, "Use --strategy best|first|worst\n");
            return 1;
        }
        snprintf(allocation_strategy, sizeof(allocation_strategy), "%s", argv[2]);
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--strategy best|first|worst] [--pipe-demo]\n", argv[0]);
        return 1;
    }

    srand((unsigned int)(time(NULL) ^ getpid()));
    mkdir("logs", 0775);
    memory_log = fopen("logs/memory_log.txt", "w");
    schedule_log = fopen("logs/schedule_log.txt", "w");
    if (memory_log == NULL || schedule_log == NULL) {
        perror("logs");
        return 1;
    }

    signal(SIGTERM, stop_running);
    signal(SIGINT, stop_running);
    signal(SIGCHLD, reap_child);

    if (setup_ipc() != 0) {
        cleanup();
        return 1;
    }

    write_pid_file();

    pthread_mutex_lock(&bed_lock);
    log_memory_event("startup", NULL, 0);
    pthread_mutex_unlock(&bed_lock);

    printf("Hospital started. Strategy=%s\n", allocation_strategy);
    printf("Submit patient: ./scripts/triage.sh \"Ali\" 30 9 no\n");
    printf("Interactive input: ./scripts/triage.sh\n");
    printf("Stop system   : ./scripts/stop_hospital.sh\n");

    pthread_create(&receptionist, NULL, receptionist_thread, NULL);
    pthread_create(&scheduler, NULL, scheduler_thread, NULL);
    pthread_create(&discharge_reader, NULL, discharge_reader_thread, NULL);
    pthread_create(&nurse_icu, NULL, nurse_thread, "ICU");
    pthread_create(&nurse_iso, NULL, nurse_thread, "ISOLATION");
    pthread_create(&nurse_general, NULL, nurse_thread, "GENERAL");

    while (running) {
        sleep(1);
    }

    pthread_cancel(receptionist);
    pthread_cancel(discharge_reader);
    sem_post(&filled_slots);
    if (icu_sem != SEM_FAILED) {
        sem_post(icu_sem);
    }
    if (iso_sem != SEM_FAILED) {
        sem_post(iso_sem);
    }
    pthread_cond_broadcast(&queue_not_empty);
    pthread_cond_broadcast(&discharge_available);
    pthread_cond_broadcast(&bed_available);

    pthread_join(receptionist, NULL);
    pthread_join(scheduler, NULL);
    pthread_join(discharge_reader, NULL);
    pthread_join(nurse_icu, NULL);
    pthread_join(nurse_iso, NULL);
    pthread_join(nurse_general, NULL);

    cleanup();
    printf("Hospital stopped cleanly.\n");
    return 0;
}
