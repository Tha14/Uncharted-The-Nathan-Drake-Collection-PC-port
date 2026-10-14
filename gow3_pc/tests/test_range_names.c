/* Test diagnostic naming without reserving/mapping host address space. */
#include "../src/runtime_memory.c"
#include <assert.h>
static int32_t test_errno;
static size_t notification_count;
static void notification(uintptr_t address,uint64_t size) {
    assert(address==USER_MIN+notification_count*PAGE && size==PAGE);
    ++notification_count;
}
int32_t *runtime_errno(void) { return &test_errno; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
int main(void) {
    uintptr_t start=USER_MIN;
    Vma region={start,start+3*PAGE,KIND_DIRECT,0x33,0,0,"direct"};
    assert(!vma_insert(0,region));
    assert(!set_range_name((void *)(start+PAGE),PAGE,"Uncharted heap"));
    VirtualQueryInfo info;
    assert(!virtual_query((void *)(start+PAGE),0,&info,sizeof(info)));
    assert(!strcmp(info.name,"Uncharted heap"));
    assert(info.start==start+PAGE && info.end==start+2*PAGE && info.offset==PAGE);
    assert(!virtual_query((void *)start,0,&info,sizeof(info)));
    assert(!strcmp(info.name,"direct"));
    assert(!virtual_query((void *)(start+2*PAGE),0,&info,sizeof(info)));
    assert(!strcmp(info.name,"direct"));
    assert(set_range_name((void *)start,PAGE,NULL)==(int32_t)0x8002000e);
    assert(set_range_name((void *)start,PAGE,"01234567890123456789012345678901")== (int32_t)0x8002003f);
    assert(set_range_name((void *)start,0,"empty")==INVALID);
    assert(set_range_name((void *)(start+1),PAGE,"unaligned")==INVALID);
    assert(set_range_name((void *)(start+3*PAGE),PAGE,"unmapped")==ACCESS);
    assert(set_range_name((void *)(UINTPTR_MAX-PAGE+1),2*PAGE,"overflow")==INVALID);
    assert(!clear_range_name((void *)(start+PAGE),PAGE));
    assert(!virtual_query((void *)(start+PAGE),0,&info,sizeof(info)) && !info.name[0]);
    assert(runtime_memory_resolve("DGMG3JshrZU#libkernel")== (uintptr_t)set_range_name);
    hook_map=notification;
    write_lock();
    for (unsigned i=0;i<1024;++i) queue_hook(HOOK_MAP,USER_MIN+i*PAGE,PAGE);
    write_unlock();
    assert(pending_count==1024);
    flush_hooks();
    assert(notification_count==1024 && !pending_count);
    puts("Range naming tests passed");
    return 0;
}
