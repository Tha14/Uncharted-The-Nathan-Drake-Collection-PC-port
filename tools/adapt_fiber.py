"""Adapt the saved upstream shadPS4 fiber implementation to this native runtime.

Inputs were downloaded from shadps4-emu/shadPS4 main into analysis/upstream-fiber*.
Copyright/license headers are retained. This script is deterministic and offline.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
source = (root / "analysis/upstream-fiber.cpp").read_text()
header = (root / "analysis/upstream-fiber.h").read_text()
context = (root / "analysis/upstream-fiber-context.cpp").read_text()
errors = (root / "analysis/upstream-fiber-error.h").read_text()
header = header.replace('#include "common/assert.h"\n#include "common/types.h"',
    '#include <cstdint>\n#include <cstddef>\n#include <cstring>\n#include <cstdio>\n#include <cstdlib>\n'
    'using u64=uint64_t; using u32=uint32_t; using u16=uint16_t; using u8=uint8_t;\n'
    'using s32=int32_t; using s8=int8_t;\n#define PS4_SYSV_ABI __attribute__((sysv_abi))\n'
    'extern "C" void **runtime_thread_fiber_slot(void);')
header = header.replace('    u32 magic_end;\n};', '    u32 magic_end;\n};\n'
    'static_assert(offsetof(OrbisFiberContext, fpucw)==0x70);\n'
    'static_assert(offsetof(OrbisFiberContext, mxcsr)==0x74);\n'
    'static_assert(offsetof(OrbisFiberData, mxcsr)==0x2c);')
errors = errors.replace('#include "core/libraries/error_codes.h"', 'constexpr int ORBIS_OK=0;')
source = source.replace('#include "fiber.h"', '#include "gow3_fiber.h"\n#include "gow3_fiber_error.h"')
source = re.sub(r'#include "(?:common/elf_info.h|common/logging/log.h|core/libraries/fiber/fiber_error.h|core/libraries/libs.h|core/tls.h)"\n', '', source)
source = source.replace('static std::atomic<u32> context_size_check = false;',
    'static std::atomic<u32> context_size_check = 0;\n'
    'struct FiberThreadState { OrbisFiberContext*& tcb_fiber; };\n'
    'static FiberThreadState GetThreadState() {\n'
    '    return {*reinterpret_cast<OrbisFiberContext**>(runtime_thread_fiber_slot())};\n}\n')
source = source.replace('return Core::GetTcbBase()->tcb_fiber;', 'return GetThreadState().tcb_fiber;')
source = source.replace('Core::Tcb* tcb = Core::GetTcbBase();', 'FiberThreadState tcb = GetThreadState();')
source = source.replace('tcb->tcb_fiber', 'tcb.tcb_fiber')
source = source.replace('Common::ElfInfo::FW_350', '0x03500000u')
source = source.replace('_sceFiberTerminate(OrbisFiber* fiber,', '_sceFiberTerminate(OrbisFiber*,')
source = source.replace('extern "C" s32 PS4_SYSV_ABI _sceFiberSetJmp',
    'extern "C" __attribute__((returns_twice)) s32 PS4_SYSV_ABI _sceFiberSetJmp')
source = source.replace('extern "C" s32 PS4_SYSV_ABI _sceFiberLongJmp',
    'extern "C" __attribute__((noreturn)) s32 PS4_SYSV_ABI _sceFiberLongJmp')
source = source.replace('UNREACHABLE_MSG("Stack overflow detected in fiber with size = 0x{:x}", stack_size);',
    'std::fprintf(stderr,"STOP: fiber stack overflow, size=%llu\\n",(unsigned long long)stack_size); std::abort();')
source = source.replace('UNREACHABLE_MSG("Fiber entry function returned.");',
    'std::fputs("STOP: fiber entry returned without yielding\\n",stderr); std::abort();')
source = source.replace('LOG_WARNING(Lib_Fiber, "Fiber initialized with small stack area.");',
    'std::fputs("Runtime: fiber initialized with small stack\\n",stderr);')
source = source.replace('strncpy(fiber->name, name, ORBIS_FIBER_MAX_NAME_LENGTH);',
    'strncpy(fiber->name, name, ORBIS_FIBER_MAX_NAME_LENGTH);\n    fiber->name[ORBIS_FIBER_MAX_NAME_LENGTH]=0;')
source = source.replace('strncpy(fiber_info->name, fiber->name, ORBIS_FIBER_MAX_NAME_LENGTH);',
    'strncpy(fiber_info->name, fiber->name, ORBIS_FIBER_MAX_NAME_LENGTH);\n    fiber_info->name[ORBIS_FIBER_MAX_NAME_LENGTH]=0;')
source = source.replace('if (flags & FiberFlags::ContextSizeCheck)', 'if (user_flags & FiberFlags::ContextSizeCheck)')
source = source.replace('                    stack_ptr++;\n                }', '                    stack_ptr++;\n                } else { break; }')
registration = source[source.index('void RegisterLib('):]
exports = re.findall(r'LIB_FUNCTION\("([^"]+)", "libSceFiber", 1, "libSceFiber",\s*(\w+)\)', registration)
assert len(exports) == 15, exports
source = source[:source.index('void RegisterLib(')] + '} // namespace Libraries::Fiber\n'
source += '\nextern "C" uintptr_t runtime_fiber_resolve(const char *name) {\n'
source += '    using namespace Libraries::Fiber;\n'
source += '\n'.join(f'    if (!std::strcmp(name,"{nid}#libSceFiber")) return (uintptr_t){symbol};' for nid, symbol in exports)
source += '\n    return 0;\n}\n'
context = context.replace('#include "common/types.h"', '// Assembly entry points use the guest System V ABI.')
# The C++ context has natural padding after fpucw; use its actual MXCSR offset.
context = context.replace('0x72(%rdi)', '0x74(%rdi)')
dest = root / "gow3_pc/gpu/shim"
for name, data in (("gow3_fiber.cpp", source), ("gow3_fiber.h", header),
                   ("gow3_fiber_context.cpp", context), ("gow3_fiber_error.h", errors)):
    (dest / name).write_text(data, encoding="utf-8")
print(f"Adapted {len(exports)} fiber exports")
