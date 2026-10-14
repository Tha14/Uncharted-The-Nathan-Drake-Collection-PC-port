#undef NDEBUG
#include "../src/runtime_kernel.c"
#include <assert.h>
static int32_t test_errno;
int32_t *runtime_errno(void) { return &test_errno; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
void runtime_report(void) {}
int main(void) {
    const uint64_t frequency=tsc_frequency();
    const uint64_t begin=read_tsc();
    const uint64_t time_begin=process_time_counter();
    GuestTimespec proc_begin, proc_end;
    assert(clock_read(15,&proc_begin)==0);
    sleep_ns(150000000);
    const uint64_t end=read_tsc();
    const uint64_t time_end=process_time_counter();
    assert(clock_read(15,&proc_end)==0);
    const double elapsed=(double)(time_end-time_begin)/process_time_frequency();
    const double tsc_elapsed=(double)(end-begin)/frequency;
    assert(elapsed>0.14 && elapsed<1.0);
    assert(tsc_elapsed/elapsed>0.98 && tsc_elapsed/elapsed<1.02);
    assert(process_time_frequency()==tsc_frequency());
    const double proc_elapsed=(double)(proc_end.sec-proc_begin.sec)+(double)(proc_end.nsec-proc_begin.nsec)/1e9;
    assert(proc_elapsed/elapsed>0.98 && proc_elapsed/elapsed<1.02);
#ifdef _WIN32
    if (native_tsc_hz) {
        const uint64_t raw_begin=__rdtsc();
        sleep_ns(100000000);
        const uint64_t raw_end=__rdtsc();
        const double raw_elapsed=(double)(raw_end-raw_begin)/frequency;
        assert(raw_elapsed>0.09 && raw_elapsed<0.2);
    }
#endif
    puts("Clock API and inline CPU clock agree with elapsed time");
}
