"""Adapt saved upstream Videodec2 ABI/FFmpeg code into the native runtime."""
from pathlib import Path
import re
root=Path(__file__).resolve().parent.parent
dest=root / "gow3_pc/gpu/shim/videodec2"
dest.mkdir(parents=True,exist_ok=True)
for filename in ("videodec2.h","videodec2_impl.h","videodec_error.h"):
    input_name="upstream-videodec-error.h" if filename=="videodec_error.h" else "upstream-"+filename
    data=(root / "analysis" / input_name).read_text()
    (dest/filename).write_text(data,encoding="utf-8")
for filename in ("videodec2.cpp","videodec2_impl.cpp"):
    data=(root / "analysis" / ("upstream-"+filename)).read_text()
    data=data.replace('#include "core/libraries/videodec/', '#include "')
    data=data.replace('#include "core/libraries/libs.h"','')
    data=data.replace('#include "common/logging/log.h"','')
    data=data.replace('#include "common/assert.h"', '#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n'
        '#define ASSERT(x) do { if (!(x)) { std::fputs("STOP: video decoder invariant failed\\n",stderr); std::abort(); } } while (0)\n'
        '#define UNREACHABLE() std::abort()')
    data=data.replace('#include "video_utils.h"','#include "core/libraries/videodec/video_utils.h"')
    data=re.sub(r'LOG_(?:TRACE|DEBUG|INFO|WARNING|ERROR)\(.*?\);', '((void)0);', data, flags=re.S)
    if filename=='videodec2.cpp':
        registration=data[data.index('void RegisterLib('):]
        exports=re.findall(r'LIB_FUNCTION\("([^"]+)", "libSceVideodec2", 1, "libSceVideodec2",\s*(\w+)\)',registration)
        assert len(exports)==12, exports
        data=data[:data.index('void RegisterLib(')]+'} // namespace Libraries::Videodec2\n'
        data+='\nextern "C" uintptr_t runtime_videodec2_resolve(const char* name) {\n    using namespace Libraries::Videodec2;\n'
        data+='\n'.join(f'    if (!std::strcmp(name,"{nid}#libSceVideodec2")) return (uintptr_t){symbol};' for nid,symbol in exports)
        data+='\n    return 0;\n}\n'
        # Validate both supported output ABI versions on decode, as flush already does.
        data=data.replace('frame_buffer->this_size != sizeof(OrbisVideodec2FrameBuffer)) {',
            'frame_buffer->this_size != sizeof(OrbisVideodec2FrameBuffer) ||\n'
            '        (output_info->this_size | 8) != sizeof(OrbisVideodec2OutputInfo)) {')
        data=data.replace('auto size = *reinterpret_cast<u64*>(p_1st_picture_info);',
            'auto size = *reinterpret_cast<u64*>(p_1st_picture_info);\n'
            '        const u64 maximum=output_info->codec_type==OrbisVideodec2CodecType::Avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);\n'
            '        if (size<8 || size>maximum) return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;\n'
            '        if (!output_info->frame_buffer) return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;')
        data=data.replace('    *decoder = new VdecDecoder',
            '    if (decoder_cfg_info->codec_type!=OrbisVideodec2CodecType::Avc && decoder_cfg_info->codec_type!=OrbisVideodec2CodecType::Hevc) return ORBIS_VIDEODEC2_ERROR_CODEC_TYPE;\n'
            '    std::puts("Runtime: Videodec2 FFmpeg decoder created");\n'
            '    *decoder = new VdecDecoder')
    else:
        # Use row-wise copies; FFmpeg line stride may exceed the visible width.
        data=data.replace('#include "core/libraries/videodec/video_utils.h"','')
        copier='''
static void CopyNV12Data(u8* dst, u64, const AVFrame& src) {
    const u32 pitch=Common::AlignUp<u32>(src.width,64), rows=Common::AlignUp<u32>(src.height,16);
    u8* chroma=dst+(u64)pitch*rows;
    for (u32 y=0;y<rows;++y) {
        const u32 source=y<(u32)src.height ? y : src.height-1;
        std::memcpy(dst+(u64)y*pitch,src.data[0]+(u64)source*src.linesize[0],src.width);
        std::memset(dst+(u64)y*pitch+src.width,0,pitch-src.width);
        if (y<rows/2) {
            const u32 row=y<(u32)src.height/2 ? y : src.height/2-1;
            std::memcpy(chroma+(u64)y*pitch,src.data[1]+(u64)row*src.linesize[1],src.width);
            std::memset(chroma+(u64)y*pitch+src.width,128,pitch-src.width);
        }
    }
}
'''
        data=data.replace('namespace Libraries::Videodec2 {','namespace Libraries::Videodec2 {\n'+copier,1)
        data=data.replace('Videodec::CopyNV12Data','CopyNV12Data')
        # Bounds checks must precede copying pixels and appended picture metadata.
        before='    CopyNV12Data((u8*)frame_buffer.frame_buffer,'
        guard='    const u64 required=((u64)Common::AlignUp<u32>(frame->width,64)*Common::AlignUp<u32>(frame->height,16)*3)/2+info_size;\n'
        guard+='    if (!frame_buffer.frame_buffer || frame_buffer.frame_buffer_size<required) {\n'
        guard+='        av_frame_free(&frame);\n'
        # Packet exists only in Decode, not Flush.
        first=data.index(before)
        data=data[:first]+guard+'        av_packet_free(&packet);\n        return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;\n    }\n'+data[first:]
        second=data.index(before,data.index('s32 VdecDecoder::Flush'))
        data=data[:second]+guard+'        return ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;\n    }\n'+data[second:]
    (dest/filename).write_text(data,encoding="utf-8")
print("Adapted 12 Videodec2 exports")
