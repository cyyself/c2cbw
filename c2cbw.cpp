#include <iostream>
#include <atomic>
#include <vector>
#include <chrono>
#include <sched.h>
#include <random>
#include <thread>
#include <cassert>
#include <algorithm>
#include <cstring>

int warm_up_sec = 5;
int nr_sample = 1000;
size_t bulk_size = 8192;

struct c2cbw_task {
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> magic;
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> sender_ready;
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> receiver_ready;
    alignas(std::hardware_destructive_interference_size) std::vector<std::chrono::nanoseconds> time;
    alignas(std::hardware_destructive_interference_size) uint32_t *buffer;
};

void pin_one_cpu(int cpu) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu, &cpu_set);
    assert(sched_setaffinity(0, sizeof(cpu_set), &cpu_set) == 0);
}

void warm_up(int sec, c2cbw_task *task) {
    std::mt19937 rng;
    auto ts_start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - ts_start).count() < sec) {
        task->magic.store(rng());
    }
}

void sender_thread(int src_cpu, c2cbw_task *task) {
    pin_one_cpu(src_cpu);
    warm_up(warm_up_sec, task);
    std::mt19937 rng;
    volatile uint32_t *buffer = task->buffer;
    for (int i=0;i<nr_sample;i++) {
        for (int j=0;j<bulk_size/sizeof(uint32_t);j++) {
            buffer[j] = rng();
        }
        task->sender_ready.store(true, std::memory_order_seq_cst);
#ifdef DEBUG_RACE
        printf("sender ready %d!\n", i);
        fflush(stdout);
#endif
        while (!task->receiver_ready.load(std::memory_order_seq_cst));
        task->receiver_ready.store(false);
#ifdef DEBUG_RACE
        printf("sender ack %d!\n", i);
        fflush(stdout);
#endif
    }
}

void receiver_thread(int dst_cpu, c2cbw_task *task) {
    pin_one_cpu(dst_cpu);
    warm_up(warm_up_sec, task);
    volatile uint32_t *buffer = task->buffer;
    for (int i=0;i<nr_sample;i++) {
        while (!task->sender_ready.load(std::memory_order_seq_cst));
        task->sender_ready.store(false, std::memory_order_seq_cst);
#ifdef DEBUG_RACE
        printf("receiver ack %d!\n", i);
        fflush(stdout);
#endif
        auto ts_start = std::chrono::steady_clock::now();
        for (int j=0;j<bulk_size/sizeof(uint32_t);j++) buffer[j];
        auto ts_end = std::chrono::steady_clock::now();
        task->time.push_back(ts_end - ts_start);
        task->receiver_ready.store(true, std::memory_order_seq_cst);
#ifdef DEBUG_RACE
        printf("receiver ready %d!\n", i);
        fflush(stdout);
#endif
    }
}

void measure_bw(int src_cpu, int dst_cpu) {
    alignas(std::hardware_destructive_interference_size) struct c2cbw_task task;
    task.buffer = new uint32_t[bulk_size/sizeof(uint32_t)];
    std::thread t[] = {
        std::thread(sender_thread, src_cpu, &task),
        std::thread(receiver_thread, dst_cpu, &task)
    };
    for (auto &each_thr : t) each_thr.join();
    std::chrono::nanoseconds avg_time = std::chrono::nanoseconds::zero();
    std::chrono::nanoseconds min_time = std::chrono::nanoseconds::max();
    for (auto each_time : task.time) {
        avg_time += each_time;
        min_time = min(min_time, each_time);
    }
    avg_time /= task.time.size();
    std::cout << "avg: " << avg_time.count() << "ns (" << (double)bulk_size / avg_time.count() << " GB/s)" << std::endl;
    std::cout << "min: " << min_time.count() <<  "ns (" << (double)bulk_size / min_time.count() << " GB/s)" << std::endl;
    delete []task.buffer;
}

int main(int argc, char *argv[]) {
    int src = -1;
    int dst = -1;
    for (int i=1;i+1<argc;i++) {
        if (strcmp(argv[i], "-s") == 0) {
            src = atoi(argv[i+1]);
        }
        else if (strcmp(argv[i], "-d") == 0) {
            dst = atoi(argv[i+1]);
        }
        else if (strcmp(argv[i], "-sz") == 0) {
            bulk_size = atoi(argv[i+1]);
        }
        else if (strcmp(argv[i], "-w") == 0) {
            warm_up_sec = atoi(argv[i+1]);
        }
        else if (strcmp(argv[i], "-ns") == 0) {
            nr_sample = atoi(argv[i+1]);
        }
    }
    if (src == -1 || dst == -1) {
        printf("c2cbw -s [SRC_CORE] -d [DST_CORE] -sz [BULK_SIZE default: 8192] -w [WARMUP_SEC default: 5] -ns [NR_SAMPLE default: 100]\n");
        exit(EXIT_FAILURE);
    }
    measure_bw(src, dst);
    return 0;
}