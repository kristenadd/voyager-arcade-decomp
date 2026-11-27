#include "gpu/renderer.hpp"

#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "host/fail.hpp"

namespace voy::gpu {

namespace {

const char* kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

// Packed: 40 bytes, as gl::Vertex (float4 would pad the struct to 48).
struct Vertex { packed_float4 pos; packed_float4 color; packed_float2 uv; };
struct VOut { float4 pos [[position]]; float4 color; float2 uv; float psize [[point_size]]; };

vertex VOut game_vs(uint vid [[vertex_id]], const device Vertex* v [[buffer(0)]], constant float& psize [[buffer(1)]]) {
    VOut o;
    o.pos = float4(v[vid].pos);
    o.color = float4(v[vid].color);
    o.uv = float2(v[vid].uv);
    o.psize = psize;
    return o;
}

struct Frag {
    uint textured;   // 0 / 1
    uint env;        // GL texture environment mode
    uint has_rgb;    // the texture's base format has colour
    uint has_alpha;  // ... and alpha
    uint intensity;  // GL_INTENSITY
    uint alpha_test, alpha_func;
    float alpha_ref;
};

fragment float4 game_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],
                        constant Frag& f [[buffer(0)]]) {
    float4 c = in.color;
    if (f.textured != 0) {
        float4 t = tex.sample(smp, in.uv);
        if (f.intensity != 0) t = float4(t.r);
        if (f.env == 0x1E01) {            // GL_REPLACE
            c = float4(f.has_rgb != 0 ? t.rgb : c.rgb, f.has_alpha != 0 ? t.a : c.a);
        } else if (f.env == 0x2101) {     // GL_DECAL
            c.rgb = f.has_alpha != 0 ? mix(c.rgb, t.rgb, t.a) : t.rgb;
        } else if (f.env == 0x0BE2) {     // GL_BLEND (environment colour 0)
            c = float4(c.rgb * (1.0 - t.rgb), c.a * t.a);
        } else if (f.env == 0x0104) {     // GL_ADD
            c = float4(min(c.rgb + t.rgb, 1.0), c.a * t.a);
        } else {                           // GL_MODULATE
            c = c * t;
        }
    }
    if (f.alpha_test != 0) {
        bool pass = true;
        switch (f.alpha_func) {
        case 0x0200: pass = false; break;
        case 0x0201: pass = c.a < f.alpha_ref; break;
        case 0x0202: pass = c.a == f.alpha_ref; break;
        case 0x0203: pass = c.a <= f.alpha_ref; break;
        case 0x0204: pass = c.a > f.alpha_ref; break;
        case 0x0205: pass = c.a != f.alpha_ref; break;
        case 0x0206: pass = c.a >= f.alpha_ref; break;
        default: break;
        }
        if (!pass) discard_fragment();
    }
    return c;
}

struct PVOut { float4 pos [[position]]; float2 uv; };
vertex PVOut present_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    PVOut o;
    o.pos = float4(p * 2.0 - 1.0, 0, 1);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
struct Cursors { float4 pos[2]; float4 color[2]; };  // pos: x, y (game pixels), visible
fragment float4 present_fs(PVOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],
                           constant Cursors& cur [[buffer(0)]]) {
    float3 c = tex.sample(smp, in.uv).rgb;
    float2 p = in.uv * float2(512.0, 384.0);
    for (int i = 0; i < 2; ++i) {
        if (cur.pos[i].z == 0) continue;
        float2 d = p - cur.pos[i].xy;
        float r = length(d);
        bool ring = abs(r - 7.0) < 0.9;
        bool cross = (abs(d.x) < 0.6 && abs(d.y) > 3.0 && abs(d.y) < 11.0) || (abs(d.y) < 0.6 && abs(d.x) > 3.0 && abs(d.x) < 11.0);
        if (ring || cross) c = mix(c, cur.color[i].rgb, 0.9);
    }
    return float4(c, 1.0);
}
)MSL";

struct FragU {
    uint32_t textured, env, has_rgb, has_alpha, intensity, alpha_test, alpha_func;
    float alpha_ref;
};

MTLBlendFactor blend_factor(uint32_t gl) {
    switch (gl) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 0x300: return MTLBlendFactorSourceColor;
    case 0x301: return MTLBlendFactorOneMinusSourceColor;
    case 0x302: return MTLBlendFactorSourceAlpha;
    case 0x303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x304: return MTLBlendFactorDestinationAlpha;
    case 0x305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x306: return MTLBlendFactorDestinationColor;
    case 0x307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x308: return MTLBlendFactorSourceAlphaSaturated;
    default: return MTLBlendFactorOne;
    }
}

MTLCompareFunction compare(uint32_t gl) {
    switch (gl) {
    case 0x200: return MTLCompareFunctionNever;
    case 0x201: return MTLCompareFunctionLess;
    case 0x202: return MTLCompareFunctionEqual;
    case 0x203: return MTLCompareFunctionLessEqual;
    case 0x204: return MTLCompareFunctionGreater;
    case 0x205: return MTLCompareFunctionNotEqual;
    case 0x206: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLSamplerAddressMode address(uint32_t gl) { return gl == 0x2901 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge; }

class MetalRenderer : public Renderer {
public:
    MetalRenderer(id<MTLDevice> device, RendererOptions o) : dev_(device), opt_(o) {
        queue_ = [dev_ newCommandQueue];
        NSError* err = nil;
        id<MTLLibrary> lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:kShaders] options:nil error:&err];
        if (!lib) {
            host::logf("[metal] shader compile failed: %s\n", err.localizedDescription.UTF8String);
            return;
        }
        vs_ = [lib newFunctionWithName:@"game_vs"];
        fs_ = [lib newFunctionWithName:@"game_fs"];
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"present_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"present_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        present_ = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        width_ = gl::kWidth * opt_.scale;
        height_ = gl::kHeight * opt_.scale;
        if (opt_.samples > 1 && ![dev_ supportsTextureSampleCount:NSUInteger(opt_.samples)]) opt_.samples = 1;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:NSUInteger(width_)
                                                                                     height:NSUInteger(height_)
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        resolved_ = [dev_ newTextureWithDescriptor:td];
        if (opt_.samples > 1) {
            td.textureType = MTLTextureType2DMultisample;
            td.sampleCount = NSUInteger(opt_.samples);
            td.usage = MTLTextureUsageRenderTarget;
            color_ = [dev_ newTextureWithDescriptor:td];
        }
        MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                      width:NSUInteger(width_)
                                                                                     height:NSUInteger(height_)
                                                                                  mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        if (opt_.samples > 1) {
            dd.textureType = MTLTextureType2DMultisample;
            dd.sampleCount = NSUInteger(opt_.samples);
        }
        depth_ = [dev_ newTextureWithDescriptor:dd];
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = opt_.smooth_present ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        present_sampler_ = [dev_ newSamplerStateWithDescriptor:sd];
        // A 1x1 white texture for untextured draws (the shader ignores it).
        MTLTextureDescriptor* wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:1
                                                                                     height:1
                                                                                  mipmapped:NO];
        white_ = [dev_ newTextureWithDescriptor:wd];
        const uint32_t px = 0xFFFFFFFF;
        [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
    }

    void draw(const gl::Frame& frame) override {
        if (!vs_) return;
        ++frame_no_;
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            // Textures first (uploads and mipmaps), in their own blit pass.
            std::vector<id<MTLTexture>> need_mips;
            for (const auto& c : frame.commands)
                if (c.kind == gl::Command::Draw && c.state.tex) texture_for(*c.state.tex, need_mips);
            if (!need_mips.empty()) {
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                for (id<MTLTexture> t : need_mips) [b generateMipmapsForTexture:t];
                [b endEncoding];
            }
            // Vertices.
            const std::size_t vbytes = std::max<std::size_t>(frame.vertices.size() * sizeof(gl::Vertex), 64);
            id<MTLBuffer> vb = [dev_ newBufferWithLength:vbytes options:MTLResourceStorageModeShared];
            if (!frame.vertices.empty()) std::memcpy(vb.contents, frame.vertices.data(), frame.vertices.size() * sizeof(gl::Vertex));
            // The pass: the first full clear becomes the load action.
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = color_ ? color_ : resolved_;
            if (color_) {
                rp.colorAttachments[0].resolveTexture = resolved_;
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            } else {
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            }
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.depthAttachment.texture = depth_;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 1.0;
            rp.depthAttachment.storeAction = MTLStoreActionDontCare;
            std::size_t first = 0;
            if (!frame.commands.empty()) {
                const auto& c0 = frame.commands[0];
                if (c0.kind == gl::Command::Clear && !c0.state.scissor && c0.state.color_mask == 0xF &&
                    (c0.clear_mask & 0x4000) && (c0.clear_mask & 0x0100) && c0.state.depth_write) {
                    rp.colorAttachments[0].clearColor =
                        MTLClearColorMake(c0.clear_color[0], c0.clear_color[1], c0.clear_color[2], c0.clear_color[3]);
                    rp.depthAttachment.clearDepth = c0.clear_depth;
                    first = 1;
                }
            }
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setVertexBuffer:vb offset:0 atIndex:0];
            // Clear quads (scissored or masked clears) use their own small vertex buffer.
            for (std::size_t i = first; i < frame.commands.size(); ++i) {
                const auto& c = frame.commands[i];
                if (c.kind == gl::Command::Clear) clear_quad(e, c, vb);
                else draw_cmd(e, c, vb);
            }
            [e endEncoding];
            [cb commit];
            last_cb_ = cb;
        }
        evict();
    }

    void present(void* drawable_ptr, int w, int h, float* rect) override {
        if (!present_) return;
        @autoreleasepool {
            id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawable_ptr;
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            // Letterbox to 4:3.
            double vw = w, vh = h;
            if (vw * 3 > vh * 4) vw = vh * 4 / 3;
            else vh = vw * 3 / 4;
            const double x0 = (w - vw) / 2, y0 = (h - vh) / 2;
            if (rect) {
                rect[0] = float(x0); rect[1] = float(y0); rect[2] = float(x0 + vw); rect[3] = float(y0 + vh);
            }
            [e setViewport:(MTLViewport){x0, y0, vw, vh, 0, 1}];
            [e setRenderPipelineState:present_];
            [e setFragmentTexture:resolved_ atIndex:0];
            [e setFragmentSamplerState:present_sampler_ atIndex:0];
            float cur[16] = {};
            for (int i = 0; i < 2; ++i) {
                cur[4 * i] = cursors_[i].x; cur[4 * i + 1] = cursors_[i].y; cur[4 * i + 2] = cursors_[i].visible ? 1 : 0;
                cur[8 + 4 * i] = cursors_[i].r; cur[9 + 4 * i] = cursors_[i].g; cur[10 + 4 * i] = cursors_[i].b;
            }
            [e setFragmentBytes:cur length:sizeof cur atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            [cb presentDrawable:drawable];
            [cb commit];
        }
    }

    void set_cursors(const Cursor* cursors, int n) override {
        for (int i = 0; i < 2; ++i) cursors_[i] = i < n ? cursors[i] : Cursor{};
    }

    bool read_rgba(std::vector<std::uint8_t>& out, int* w, int* h) override {
        @autoreleasepool {
            id<MTLBuffer> buf = [dev_ newBufferWithLength:NSUInteger(width_ * height_ * 4) options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [queue_ commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:resolved_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(NSUInteger(width_), NSUInteger(height_), 1) toBuffer:buf destinationOffset:0
               destinationBytesPerRow:NSUInteger(width_ * 4) destinationBytesPerImage:NSUInteger(width_ * height_ * 4)];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            out.resize(std::size_t(width_) * height_ * 4);
            const auto* src = static_cast<const std::uint8_t*>(buf.contents);
            for (std::size_t i = 0; i < out.size(); i += 4) {  // BGRA → RGBA
                out[i] = src[i + 2]; out[i + 1] = src[i + 1]; out[i + 2] = src[i]; out[i + 3] = 255;
            }
            *w = width_;
            *h = height_;
            return true;
        }
    }

private:
    struct CachedTex {
        id<MTLTexture> tex;
        id<MTLSamplerState> sampler;
        std::uint64_t used = 0;
    };

    id<MTLTexture> texture_for(const gl::TexImage& t, std::vector<id<MTLTexture>>& need_mips) {
        auto it = textures_.find(t.serial);
        if (it != textures_.end()) {
            it->second.used = frame_no_;
            return it->second.tex;
        }
        const bool mip = t.mipmaps || (t.min_filter >= 0x2700 && t.min_filter <= 0x2703);
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:NSUInteger(std::max(t.width, 1))
                                                                                    height:NSUInteger(std::max(t.height, 1))
                                                                                 mipmapped:mip];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [dev_ newTextureWithDescriptor:d];
        if (!t.rgba.empty())
            [tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(t.width), NSUInteger(t.height)) mipmapLevel:0
                     withBytes:t.rgba.data() bytesPerRow:NSUInteger(t.width * 4)];
        if (mip && tex.mipmapLevelCount > 1) need_mips.push_back(tex);
        CachedTex ct;
        ct.tex = tex;
        ct.sampler = sampler_for(t);
        ct.used = frame_no_;
        textures_[t.serial] = ct;
        return tex;
    }

    id<MTLSamplerState> sampler_for(const gl::TexImage& t) {
        const std::uint64_t key = std::uint64_t(t.min_filter) << 48 | std::uint64_t(t.mag_filter) << 32 |
                                  std::uint64_t(t.wrap_s) << 16 | t.wrap_t;
        auto it = samplers_.find(key);
        if (it != samplers_.end()) return it->second;
        MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
        const bool min_linear = t.min_filter == 0x2601 || t.min_filter == 0x2701 || t.min_filter == 0x2703;
        s.minFilter = min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        s.magFilter = t.mag_filter == 0x2600 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        if (t.min_filter == 0x2700 || t.min_filter == 0x2701) s.mipFilter = MTLSamplerMipFilterNearest;
        else if (t.min_filter == 0x2702 || t.min_filter == 0x2703) s.mipFilter = MTLSamplerMipFilterLinear;
        else s.mipFilter = MTLSamplerMipFilterNotMipmapped;
        s.sAddressMode = address(t.wrap_s);
        s.tAddressMode = address(t.wrap_t);
        id<MTLSamplerState> st = [dev_ newSamplerStateWithDescriptor:s];
        samplers_[key] = st;
        return st;
    }

    id<MTLRenderPipelineState> pipeline(const gl::DrawState& s) {
        const std::uint32_t key = (s.blend ? 1u : 0u) | (s.blend ? (s.blend_src & 0x30F) << 1 | (s.blend_dst & 0x30F) << 11 : 0) |
                                  std::uint32_t(s.color_mask) << 22;
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vs_;
        pd.fragmentFunction = fs_;
        pd.rasterSampleCount = NSUInteger(opt_.samples);
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        MTLColorWriteMask m = MTLColorWriteMaskNone;
        if (s.color_mask & 1) m |= MTLColorWriteMaskRed;
        if (s.color_mask & 2) m |= MTLColorWriteMaskGreen;
        if (s.color_mask & 4) m |= MTLColorWriteMaskBlue;
        if (s.color_mask & 8) m |= MTLColorWriteMaskAlpha;
        pd.colorAttachments[0].writeMask = m;
        if (s.blend) {
            auto* ca = pd.colorAttachments[0];
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = blend_factor(s.blend_src);
            ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = blend_factor(s.blend_dst);
        }
        NSError* err = nil;
        id<MTLRenderPipelineState> ps = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) host::logf("[metal] pipeline: %s\n", err.localizedDescription.UTF8String);
        pipelines_[key] = ps;
        return ps;
    }

    id<MTLDepthStencilState> depth_state(bool test, std::uint32_t func, bool write) {
        const std::uint32_t key = (test ? 1u : 0u) | (func & 0xF) << 1 | (write ? 0x100u : 0u);
        auto it = depths_.find(key);
        if (it != depths_.end()) return it->second;
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = test ? compare(func) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = test && write;
        id<MTLDepthStencilState> st = [dev_ newDepthStencilStateWithDescriptor:d];
        depths_[key] = st;
        return st;
    }

    void set_rects(id<MTLRenderCommandEncoder> e, const gl::DrawState& s) {
        const int S = opt_.scale, H = gl::kHeight;
        [e setViewport:(MTLViewport){double(s.viewport[0] * S), double((H - s.viewport[1] - s.viewport[3]) * S),
                                     double(s.viewport[2] * S), double(s.viewport[3] * S), 0, 1}];
        int x = 0, y = 0, w = width_, h = height_;
        if (s.scissor) {
            x = s.scissor_box[0] * S;
            y = (H - s.scissor_box[1] - s.scissor_box[3]) * S;
            w = s.scissor_box[2] * S;
            h = s.scissor_box[3] * S;
            if (x < 0) { w += x; x = 0; }
            if (y < 0) { h += y; y = 0; }
            w = std::min(w, width_ - x);
            h = std::min(h, height_ - y);
            if (w <= 0 || h <= 0) { x = y = 0; w = h = 0; }
        }
        [e setScissorRect:(MTLScissorRect){NSUInteger(x), NSUInteger(y), NSUInteger(std::max(w, 0)), NSUInteger(std::max(h, 0))}];
    }

    void draw_cmd(id<MTLRenderCommandEncoder> e, const gl::Command& c, id<MTLBuffer> vb) {
        if (!c.count) return;
        if (c.state.scissor && (c.state.scissor_box[2] <= 0 || c.state.scissor_box[3] <= 0)) return;
        id<MTLRenderPipelineState> ps = pipeline(c.state);
        if (!ps) return;
        [e setRenderPipelineState:ps];
        [e setDepthStencilState:depth_state(c.state.depth_test, c.state.depth_func, c.state.depth_write)];
        set_rects(e, c.state);
        if (c.state.polygon_offset) [e setDepthBias:c.state.offset_units * 1.0f / 16777216.0f slopeScale:c.state.offset_factor clamp:0];
        else [e setDepthBias:0 slopeScale:0 clamp:0];
        FragU u{};
        if (c.state.tex) {
            std::vector<id<MTLTexture>> unused;
            const gl::TexImage& t = *c.state.tex;
            id<MTLTexture> tex = texture_for(t, unused);
            u.textured = 1;
            u.has_rgb = t.format != 2;
            u.has_alpha = t.format == 0 || t.format == 2 || t.format == 4 || t.format == 5;
            u.intensity = t.format == 5;
            [e setFragmentTexture:tex atIndex:0];
            [e setFragmentSamplerState:textures_[t.serial].sampler atIndex:0];
        } else {
            [e setFragmentTexture:white_ atIndex:0];
            [e setFragmentSamplerState:present_sampler_ atIndex:0];
        }
        u.env = c.state.env_mode;
        u.alpha_test = c.state.alpha_test;
        u.alpha_func = c.state.alpha_func;
        u.alpha_ref = c.state.alpha_ref;
        [e setFragmentBytes:&u length:sizeof u atIndex:0];
        const float psize = float(opt_.scale);
        [e setVertexBytes:&psize length:sizeof psize atIndex:1];
        [e setVertexBufferOffset:0 atIndex:0];
        MTLPrimitiveType pt = c.prim == gl::Prim::Triangles ? MTLPrimitiveTypeTriangle
                              : c.prim == gl::Prim::Lines   ? MTLPrimitiveTypeLine
                                                            : MTLPrimitiveTypePoint;
        [e drawPrimitives:pt vertexStart:c.first vertexCount:c.count];
    }

    // glClear honours the scissor box and the colour / depth write masks: a quad.
    void clear_quad(id<MTLRenderCommandEncoder> e, const gl::Command& c, id<MTLBuffer> vb) {
        gl::DrawState s = c.state;
        s.blend = false;
        if (!(c.clear_mask & 0x4000)) s.color_mask = 0;
        const bool depth = (c.clear_mask & 0x0100) && c.state.depth_write;
        if (!s.color_mask && !depth) return;
        s.viewport[0] = 0; s.viewport[1] = 0; s.viewport[2] = gl::kWidth; s.viewport[3] = gl::kHeight;
        id<MTLRenderPipelineState> ps = pipeline(s);
        if (!ps) return;
        [e setRenderPipelineState:ps];
        [e setDepthStencilState:depth_state(depth, 0x207, depth)];
        set_rects(e, s);
        [e setDepthBias:0 slopeScale:0 clamp:0];
        const float z = c.clear_depth;
        gl::Vertex q[6];
        const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 6; ++i)
            q[i] = {xy[i][0], xy[i][1], z, 1, c.clear_color[0], c.clear_color[1], c.clear_color[2], c.clear_color[3], 0, 0};
        [e setVertexBytes:q length:sizeof q atIndex:0];
        FragU u{};
        [e setFragmentBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:white_ atIndex:0];
        [e setFragmentSamplerState:present_sampler_ atIndex:0];
        const float psize = 1;
        [e setVertexBytes:&psize length:sizeof psize atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [e setVertexBuffer:vb offset:0 atIndex:0];
    }

    void evict() {
        if (frame_no_ % 60) return;
        for (auto it = textures_.begin(); it != textures_.end();) {
            if (frame_no_ - it->second.used > 120) it = textures_.erase(it);
            else ++it;
        }
    }

    id<MTLDevice> dev_;
    RendererOptions opt_;
    id<MTLCommandQueue> queue_;
    id<MTLFunction> vs_, fs_;
    id<MTLRenderPipelineState> present_;
    id<MTLSamplerState> present_sampler_;
    id<MTLTexture> color_, resolved_, depth_, white_;
    id<MTLCommandBuffer> last_cb_;
    int width_ = 0, height_ = 0;
    std::uint64_t frame_no_ = 0;
    Cursor cursors_[2];
    std::unordered_map<std::uint64_t, CachedTex> textures_;
    std::unordered_map<std::uint64_t, id<MTLSamplerState>> samplers_;
    std::unordered_map<std::uint32_t, id<MTLRenderPipelineState>> pipelines_;
    std::unordered_map<std::uint32_t, id<MTLDepthStencilState>> depths_;
};

}  // namespace

std::unique_ptr<Renderer> Renderer::create(void* device, RendererOptions options) {
    id<MTLDevice> dev = device ? (__bridge id<MTLDevice>)device : MTLCreateSystemDefaultDevice();
    if (!dev) return nullptr;
    return std::make_unique<MetalRenderer>(dev, options);
}

bool write_png(const std::string& path, const std::vector<std::uint8_t>& rgba, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w * 4), cs,
                                   kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast, dp, nullptr, false,
                                   kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           CFIndex(path.size()), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return ok;
}

}
