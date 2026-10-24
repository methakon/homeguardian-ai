"""Standalone Android decoder contract tests (no CMake, device or network).

Run: python3 tests/portal/test_android_camera_decoder.py
The host shim tests control flow, NOT Android's JPEG implementation. The optional
NDK test compiles/links the extracted decoder against the real API30 headers/libs.
Extraction keeps this low-resource test independent of the full bridge's deps.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def decoder_source():
    header = (ROOT / 'src/bridge/CameraAnalysis.h').read_text()
    live = (ROOT / 'src/bridge/LiveCameraFeed.h').read_text()
    return ('#include <vector>\n#include <memory>\n#include <cstdint>\n'
            '#include <cstring>\n#include <cstddef>\n'
            'namespace detail {\n' +
            function(live, 'inline bool looks_like_jpeg(') + '\n' +
            function(live, 'inline bool jpeg_dimensions(') + '\n}\n' +
            function(header, 'inline std::vector<uint8_t> decode_jpeg_luma('))


SHIM = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
struct AImageDecoder {};
struct AImageDecoderHeaderInfo {};
constexpr int ANDROID_IMAGE_DECODER_SUCCESS=0;
constexpr int ANDROID_BITMAP_FORMAT_RGBA_8888=1;
constexpr int ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE=1;
int stage=0, calls=0, deleted=0, width=160, height=120;
size_t min_stride=640;
const char* mime="image/jpeg";
int alpha=ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
AImageDecoder decoder;
AImageDecoderHeaderInfo header;
int AImageDecoder_createFromBuffer(const void*, size_t, AImageDecoder** p) {
 ++calls; if(stage==1)return -1; *p=&decoder; return 0;
}
void AImageDecoder_delete(AImageDecoder* p){assert(p==&decoder);++deleted;}
const AImageDecoderHeaderInfo* AImageDecoder_getHeaderInfo(const AImageDecoder*) {
 return stage==2?nullptr:&header;
}
int32_t AImageDecoderHeaderInfo_getWidth(const AImageDecoderHeaderInfo*){return width;}
int32_t AImageDecoderHeaderInfo_getHeight(const AImageDecoderHeaderInfo*){return height;}
const char* AImageDecoderHeaderInfo_getMimeType(const AImageDecoderHeaderInfo*){return mime;}
int AImageDecoderHeaderInfo_getAlphaFlags(const AImageDecoderHeaderInfo*){return alpha;}
int AImageDecoder_setAndroidBitmapFormat(AImageDecoder*,int32_t f){assert(f==1);return stage==3?-1:0;}
int AImageDecoder_setTargetSize(AImageDecoder*,int32_t w,int32_t h){assert(w==160&&h==120);return stage==4?-1:0;}
size_t AImageDecoder_getMinimumStride(AImageDecoder*){return min_stride;}
int AImageDecoder_decodeImage(AImageDecoder*,void* p,size_t stride,size_t size){
 assert(stride==640&&size==76800);
 if(stage==5)return -1; // includes partial/incomplete output: never expose it
 auto* b=static_cast<uint8_t*>(p);
 for(size_t i=0;i<size;i+=4){b[i]=255;b[i+1]=0;b[i+2]=0;b[i+3]=255;}
 return 0;
}
'''

MAIN = r'''
int main(){
 // Marker-valid bounded input is sufficient for a control-flow shim, not a JPEG fixture.
 std::vector<uint8_t> jpeg={255,216,255,192,0,8,8,0,120,0,160,0,255,217};
 assert(decode_jpeg_luma({}).empty());
 assert(decode_jpeg_luma(std::vector<uint8_t>(512*1024+1,255)).empty());
 auto invalid=jpeg; invalid[0]=0;assert(decode_jpeg_luma(invalid).empty());
 invalid=jpeg;invalid[9]=16;invalid[10]=1;assert(decode_jpeg_luma(invalid).empty());
 invalid=jpeg;invalid[7]=16;invalid[8]=1;assert(decode_jpeg_luma(invalid).empty());
 assert(calls==0);
 for(stage=1;stage<=5;++stage){
  int before=deleted;assert(decode_jpeg_luma(jpeg).empty());
  assert(deleted-before==(stage==1?0:1));
 }
 stage=0;
 for(int bad:{0,-1,4097}){
  width=bad;assert(decode_jpeg_luma(jpeg).empty());width=160;
  height=bad;assert(decode_jpeg_luma(jpeg).empty());height=120;
 }
 mime="image/png";assert(decode_jpeg_luma(jpeg).empty());mime="image/jpeg";
 alpha=0;assert(decode_jpeg_luma(jpeg).empty());alpha=ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
 for(size_t bad:{size_t(0),size_t(636),size_t(644),size_t(-1)}){
  min_stride=bad;assert(decode_jpeg_luma(jpeg).empty());
 }
 min_stride=640;
 auto luma=decode_jpeg_luma(jpeg);assert(luma.size()==19200);
 for(auto y:luma)assert(y==76); // rounded BT.601 red luminance
 assert(deleted==calls-1);
}
'''


class AndroidDecoderTests(unittest.TestCase):
    def setUp(self):
        # Honor Hermes scratch; never create artifacts in the repository.
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def test_android_control_flow_and_cleanup(self):
        source = self.path / 'contract.cpp'
        source.write_text('#define __ANDROID__ 1\n#define __ANDROID_API__ 30\n' +
                          SHIM + decoder_source() + MAIN)
        exe = self.path / 'contract'
        subprocess.run(['c++', '-std=c++17', '-O0', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

    def test_linux_decoder_with_real_synthetic_jpeg(self):
        header = (ROOT / 'src/bridge/CameraAnalysis.h').read_text()
        start = header.index('#if defined(__ANDROID__)')
        end = header.index('#include <mutex>', start)
        source = self.path / 'linux.cpp'
        source.write_text('#include <chrono>\n#include <iterator>\n#include <iostream>\n' +
                          header[start:end] + decoder_source() + r'''
int main(){
 std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(std::cin)),{});
 auto luma=decode_jpeg_luma(jpeg);
 if(luma.size()!=19200)return 1;
 for(auto y:luma)if(y<30||y>50)return 2;
 return 0;
}
''')
        exe = self.path / 'linux'
        subprocess.run(['c++', '-std=c++17', '-O0', str(source), '-o', str(exe)], check=True)
        jpeg = subprocess.run(['/usr/bin/ffmpeg', '-nostdin', '-v', 'error',
                               '-threads', '1', '-f', 'rawvideo', '-pix_fmt', 'gray',
                               '-s', '160x120', '-i', 'pipe:0', '-frames:v', '1',
                               '-threads', '1', '-f', 'mjpeg', 'pipe:1'],
                              input=bytes([40]) * 19200, capture_output=True, check=True).stdout
        subprocess.run([str(exe)], input=jpeg, check=True)

    def test_real_ndk_api30_and_unsupported_api_guard(self):
        ndk = Path(os.environ.get('ANDROID_NDK_HOME',
                       str(Path.home() / 'Android/Sdk/ndk/android-ndk-r26d')))
        compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang++'
        if not compiler.exists():
            self.skipTest('NDK r26d not available; set ANDROID_NDK_HOME')
        header = (ROOT / 'src/bridge/CameraAnalysis.h').read_text()
        # Compile the production platform include/guard block too.
        start = header.index('#if defined(__ANDROID__)')
        end = header.index('#include <mutex>', start)
        includes = header[start:end]
        source = self.path / 'api30.cpp'
        source.write_text(includes + decoder_source() +
                          '\nint main(){return decode_jpeg_luma({}).size();}\n')
        subprocess.run([str(compiler), '-std=c++17', '-O0', str(source),
                        '-ljnigraphics', '-o', str(self.path / 'api30')], check=True)
        old = compiler.with_name('aarch64-linux-android29-clang++')
        result = subprocess.run([str(old), '-std=c++17', '-fsyntax-only', str(source)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires Android API level 30', result.stderr)


if __name__ == '__main__':
    unittest.main()
