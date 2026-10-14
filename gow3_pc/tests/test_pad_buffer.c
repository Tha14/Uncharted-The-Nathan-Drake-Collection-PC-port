#include "../src/runtime_pad.c"
#include <assert.h>
int gow3gpu_overlay_captures_input(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
int main(void) {
    PadData current={0},out;
    current.left_x=current.left_y=current.right_x=current.right_y=128;
    current.connected=1; current.timestamp=10000;
    assert(buffered_report(&out,&current)==1);
    memset(&out,0xa5,sizeof(out));
    current.timestamp=10001;
    assert(buffered_report(&out,&current)==0 && out.buttons==0xa5a5a5a5);
    current.buttons=BTN_CROSS; current.timestamp=10002;
    assert(buffered_report(&out,&current)==1 && out.buttons==BTN_CROSS);
    for (unsigned i=0;i<100;++i) {
        current.timestamp=10003+i;
        assert(buffered_report(&out,&current)==0);
    }
    current.buttons=0; current.timestamp=10110;
    assert(buffered_report(&out,&current)==1 && !out.buttons);
    current.timestamp=14110;
    assert(buffered_report(&out,&current)==1);
    puts("Buffered pad press/release and duplicate-report tests passed");
}
