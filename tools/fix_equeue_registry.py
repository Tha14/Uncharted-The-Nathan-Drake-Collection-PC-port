"""Replace the vendored equeue's racy raw-pointer registry with shared ownership."""
from pathlib import Path
root=Path(__file__).resolve().parent.parent / "gow3_pc/gpu/shadps4/core/libraries/kernel"
path=root / "equeue.cpp"
source=path.read_text()
if 'SharedHandleTable<EqueueInternal' in source:
    print("Registry already adapted")
    raise SystemExit(0)
source=source.replace('#include <atomic>', '#include <atomic>\n#include "common/shared_handle_table.h"')
source=source.replace('static std::unordered_map<s32, EqueueInternal*> kqueues;',
    'static Common::SharedHandleTable<EqueueInternal,OrbisKernelEqueue> kqueues;')
start=source.index('EqueueInternal* GetEqueue(')
end=source.index('// Events are uniquely identified',start)
source=source[:start]+'''std::shared_ptr<EqueueInternal> GetEqueue(OrbisKernelEqueue eq) {
    return kqueues.Find(eq);
}
static void HrTimerCallback(OrbisKernelEqueue eq,const OrbisKernelEvent& kevent) {
    if (auto equeue=GetEqueue(eq)) {
        equeue->TriggerEvent(kevent.ident,OrbisKernelEvent::Filter::HrTimer,kevent.udata);
    }
}
static void TimerCallback(OrbisKernelEqueue eq,const OrbisKernelEvent& kevent) {
    if (auto equeue=GetEqueue(eq); equeue && equeue->EventExists(kevent.ident,kevent.filter)) {
        equeue->TriggerEvent(kevent.ident,OrbisKernelEvent::Filter::Timer,kevent.udata);
        if (!(kevent.flags & OrbisKernelEvent::Flags::OneShot)) {
            equeue->ScheduleEvent(kevent.ident,kevent.filter,TimerCallback);
        }
    }
}

'''+source[end:]
source=source.replace('kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);',
    'kqueues.Insert(kqueue_handle,std::make_shared<EqueueInternal>(kqueue_handle,name));')
start=source.index('int PS4_SYSV_ABI sceKernelDeleteEqueue(')
end=source.index('int PS4_SYSV_ABI sceKernelWaitEqueue(',start)
source=source[:start]+'''int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq) {
    return kqueues.Erase(eq) ? ORBIS_OK : ORBIS_KERNEL_ERROR_EBADF;
}

'''+source[end:]
for key in ('eq','handle'):
    source=source.replace(f'if (!kqueues.contains({key})) {{',
        f'auto equeue=GetEqueue({key});\n    if (!equeue) {{')
    source=source.replace(f'    auto& equeue = kqueues[{key}];\n','')
    source=source.replace(f'    auto equeue = kqueues[{key}];\n','')
    source=source.replace(f'kqueues[{key}]->','equeue->')
assert 'kqueues[' not in source and 'kqueues.contains' not in source
path.write_text(source,encoding='utf-8')
header=root / 'equeue.h'
source=header.read_text().replace('#include <mutex>','#include <mutex>\n#include <memory>')
source=source.replace('EqueueInternal* GetEqueue(OrbisKernelEqueue eq);',
    'std::shared_ptr<EqueueInternal> GetEqueue(OrbisKernelEqueue eq);')
header.write_text(source,encoding='utf-8')
print('Equeue registry adapted')
