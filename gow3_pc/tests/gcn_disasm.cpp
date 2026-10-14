// gow3: offline GCN disassembly with the recompiler's own decoder (LLVM cannot disassemble
// gfx7). Usage: gcn-disasm shader.bin — prints offset, opcode and operands until s_endpgm.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <magic_enum/magic_enum.hpp>
#include "shader_recompiler/frontend/decode.h"
#include "common/logging/log.h"

extern "C" {
uint32_t runtime_disabled_optimizations = 0;
}
void assert_fail_impl() {
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    std::abort();
}
namespace Common::Log {
bool ShouldLog(Level) {
    return false;
}
void Write(std::string_view, Level, const char*, int, const char*, std::string&&) {}
} // namespace Common::Log

using namespace Shader::Gcn;

static void PrintOperand(const InstOperand& op) {
    const auto field = op.field;
    switch (field) {
    case OperandField::ScalarGPR:
        std::printf(" s%u", op.code);
        break;
    case OperandField::VectorGPR:
        std::printf(" v%u", op.code);
        break;
    case OperandField::LiteralConst:
        std::printf(" lit(0x%08x=%g)", op.code, *reinterpret_cast<const float*>(&op.code));
        break;
    case OperandField::SignedConstIntPos:
    case OperandField::SignedConstIntNeg:
        std::printf(" %d", int(op.code));
        break;
    case OperandField::ConstFloatPos_0_5:
        std::printf(" 0.5");
        break;
    case OperandField::ConstFloatNeg_0_5:
        std::printf(" -0.5");
        break;
    case OperandField::ConstFloatPos_1_0:
        std::printf(" 1.0");
        break;
    case OperandField::ConstFloatNeg_1_0:
        std::printf(" -1.0");
        break;
    case OperandField::ConstFloatPos_2_0:
        std::printf(" 2.0");
        break;
    case OperandField::ConstFloatNeg_2_0:
        std::printf(" -2.0");
        break;
    case OperandField::ConstFloatPos_4_0:
        std::printf(" 4.0");
        break;
    case OperandField::ConstFloatNeg_4_0:
        std::printf(" -4.0");
        break;
    case OperandField::Undefined:
        break;
    default:
        std::printf(" %.*s(%u)", int(magic_enum::enum_name(field).size()),
                    magic_enum::enum_name(field).data(), op.code);
        break;
    }
    if (op.input_modifier.neg) {
        std::printf("[neg]");
    }
    if (op.input_modifier.abs) {
        std::printf("[abs]");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s shader.bin\n", argv[0]);
        return 1;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    std::vector<u32> words;
    u32 w;
    while (std::fread(&w, 4, 1, f) == 1) {
        words.push_back(w);
    }
    std::fclose(f);
    GcnCodeSlice code(words.data(), words.data() + words.size());
    GcnDecodeContext decoder;
    u32 pc = 0;
    while (!code.atEnd()) {
        const GcnInst inst = decoder.decodeInstruction(code);
        const auto name = magic_enum::enum_name(inst.opcode);
        std::printf("%04x: %-24.*s", pc, int(name.size()), name.data());
        for (u32 i = 0; i < inst.dst_count; ++i) {
            PrintOperand(inst.dst[i]);
        }
        std::printf(" <-");
        for (u32 i = 0; i < inst.src_count; ++i) {
            PrintOperand(inst.src[i]);
        }
        std::printf("\n");
        pc += inst.length;
        if (inst.opcode == Opcode::S_ENDPGM) {
            break;
        }
    }
    return 0;
}
