CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -pthread

all: bin/admissions bin/patient_simulator

bin:
	mkdir -p bin logs

bin/admissions: src/admissions.c src/common.h | bin
	$(CC) $(CFLAGS) -o bin/admissions src/admissions.c

bin/patient_simulator: src/patient_simulator.c src/common.h | bin
	$(CC) $(CFLAGS) -o bin/patient_simulator src/patient_simulator.c

run: all
	./scripts/start_hospital.sh best

test: all
	./scripts/stop_hospital.sh >/dev/null 2>&1 || true
	./scripts/triage.sh "Pipe Demo" 30 8 no --stdout | ./bin/admissions --pipe-demo
	HOSPITAL_FAST=1 ./scripts/start_hospital.sh best
	./scripts/triage.sh "Manual ICU" 45 10 no
	./scripts/triage.sh "Manual Isolation" 32 5 yes
	./scripts/stress_test.sh
	sleep 12
	./scripts/stop_hospital.sh
	test -s logs/schedule_log.txt
	test -s logs/memory_log.txt
	grep -q "Round Robin" logs/schedule_log.txt
	grep -q "external=" logs/memory_log.txt

clean:
	rm -rf bin
	rm -f logs/*.txt
	rm -f /tmp/triage_fifo /tmp/discharge_fifo /tmp/hospital_admissions.pid
	ipcrm -M 0xBEDF00D 2>/dev/null || true
	rm -f /dev/shm/sem.sem_icu_limit /dev/shm/sem.sem_iso_limit 2>/dev/null || true

valgrind: all
	@if command -v valgrind >/dev/null 2>&1; then \
		HOSPITAL_FAST=1 timeout 12s valgrind --leak-check=full ./bin/admissions --strategy best; \
	else \
		echo "Valgrind is not installed. Install it with: sudo apt update && sudo apt install valgrind"; \
		exit 1; \
	fi
