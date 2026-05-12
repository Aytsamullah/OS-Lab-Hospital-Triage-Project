/*
 * ============================================================
 * Project : Hospital Patient Triage & Bed Allocator
 * File    : common.h
 * Group   : Group XX
 * Members : Member 1 (24F-XXXX), Member 2 (24F-YYYY)
 * Date    : 2026-05-08
 * Purpose : Shared constants and required project structures.
 * ============================================================
 */

#ifndef COMMON_H
#define COMMON_H

#include <time.h>

#define SHM_KEY 0xBEDF00D
#define TRIAGE_FIFO "/tmp/triage_fifo"
#define DISCHARGE_FIFO "/tmp/discharge_fifo"
#define PID_FILE "/tmp/hospital_admissions.pid"

#define SEM_ICU_NAME "/sem_icu_limit"
#define SEM_ISO_NAME "/sem_iso_limit"

#define MAX_NAME 64
#define MAX_BED_TYPE 16
#define MAX_BEDS 40
#define MAX_QUEUE 50
#define MAX_CHILDREN 50

#define ICU_CAPACITY 4
#define ISO_CAPACITY 4
#define GENERAL_CAPACITY 12

#define ICU_UNITS 3
#define ISO_UNITS 2
#define GENERAL_UNITS 1
#define TOTAL_UNITS 32
#define PAGE_SIZE 2
#define PAGE_COUNT 16

/* Patient record passed via IPC */
typedef struct {
    int patient_id;
    char name[MAX_NAME];
    int age;
    int severity;       /* 1-10 raw severity from triage */
    int priority;       /* 1-5 computed triage priority */
    int care_units;     /* memory units required */
    int infectious;     /* 1 = isolation case */
    int treatment_time; /* seconds */
    time_t arrival_time;
} PatientRecord;

/* Single bed partition in the ward memory model */
typedef struct {
    int partition_id;
    int start_unit;
    int size;
    int is_free;
    int patient_id;
    char bed_type[MAX_BED_TYPE];
} BedPartition;

typedef struct {
    BedPartition beds[MAX_BEDS];
    int bed_count;
    int next_patient_id;
    int served_count;
    int freed_count;
    int active_count;
} SharedWard;

#endif
