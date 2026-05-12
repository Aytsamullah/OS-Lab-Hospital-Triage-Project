# Hospital Patient Triage & Bed Allocator

Simple Ubuntu/Linux C implementation for the Operating Systems Lab project.

## Folder Structure

```text
src/
  common.h
  admissions.c
  patient_simulator.c
scripts/
  triage.sh
  start_hospital.sh
  stop_hospital.sh
  stress_test.sh
logs/
Makefile
README.md
```

## Build

```bash
make all
```

## Run

```bash
./scripts/start_hospital.sh best
./scripts/triage.sh "Ali" 30 9 no
./scripts/stress_test.sh
./scripts/stop_hospital.sh
```

Interactive patient input:

```bash
./scripts/triage.sh
```

Anonymous pipe demo required by the manual:

```bash
./scripts/triage.sh "Pipe Demo" 30 8 no --stdout | ./bin/admissions --pipe-demo
```

Strategies:

```bash
./scripts/start_hospital.sh best
./scripts/start_hospital.sh first
./scripts/start_hospital.sh worst
```

## Logs

- `logs/hospital_output.txt`
- `logs/schedule_log.txt`
- `logs/memory_log.txt`
- `logs/hospital_summary.txt`

## Test

```bash
make test
```

This runs the anonymous pipe demo, starts admissions, submits two manual patients, runs the 20-patient stress-test bonus, stops the hospital, and checks the schedule/memory logs.

`logs/schedule_log.txt` is generated from the actual patients submitted during the run. It is not based on fixed sample arrays.

## Concepts Used

- `fork()` + `execv()`
- `waitpid(WNOHANG)` and `SIGCHLD`
- named FIFOs
- SysV shared memory
- POSIX threads
- mutexes and condition variables
- semaphores
- priority queue
- FCFS, SJF, Priority, Round Robin scheduling logs
- first-fit, best-fit, worst-fit allocation
- coalescing
- external and internal fragmentation logs

## Included Bonus

- Automated stress-test script that sends 20 patients quickly.
- You can also choose a count for extra testing, for example `./scripts/stress_test.sh 30`.

No other bonus features are included.
