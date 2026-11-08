#include "Shaders.h"

namespace rc::shaders {

#define HEAD "#version 330 core\nin vec2 vUv;\nout vec4 o;\n"

const char *convert = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uP0, uP1, uP2, uP3;
uniform int uLayout;        // 0 planar, 1 semi planar, 2 rgba, 3 gray
uniform float uScale;       // normalisation for high bit depth stored in 16 bit
uniform mat3 uYuv;
uniform vec3 uOffset;
uniform int uHasAlpha;
uniform int uBgr;
uniform int uRot;           // 0, 90, 180, 270
uniform int uStraight;      // straight alpha input
uniform int uTransfer;      // 0 SDR, 1 PQ, 2 HLG
uniform int uGamut2020;
vec3 pqEotf(vec3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 p = pow(max(e, 0.0), vec3(1.0 / m2));
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * 10000.0;
}
vec3 hlgInv(vec3 e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    vec3 lo = e * e / 3.0;
    vec3 hi = (exp((e - c) / a) + b) / 12.0;
    return mix(lo, hi, step(0.5, e)) * 1000.0;
}
vec3 tonemap(vec3 nits) {
    vec3 x = nits / 203.0;
    const float W = 4.9;
    vec3 m = x * (1.0 + x / (W * W)) / (1.0 + x);
    return m;
}
void main() {
    vec2 uv = vUv;
    if (uRot == 90) uv = vec2(vUv.y, 1.0 - vUv.x);
    else if (uRot == 180) uv = vec2(1.0 - vUv.x, 1.0 - vUv.y);
    else if (uRot == 270) uv = vec2(1.0 - vUv.y, vUv.x);
    vec3 c;
    float a = 1.0;
    if (uLayout == 2) {
        vec4 t = texture(uP0, uv);
        c = uBgr == 1 ? t.bgr : t.rgb;
        a = uHasAlpha == 1 ? t.a : 1.0;
    } else if (uLayout == 3) {
        c = vec3(texture(uP0, uv).r * uScale);
    } else {
        float y = texture(uP0, uv).r * uScale;
        vec2 cbcr = uLayout == 1 ? texture(uP1, uv).rg * uScale : vec2(texture(uP1, uv).r, texture(uP2, uv).r) * uScale;
        c = uYuv * (vec3(y, cbcr) - uOffset);
        if (uHasAlpha == 1) a = texture(uP3, uv).r * uScale;
    }
    if (uTransfer == 1 || uTransfer == 2) {
        vec3 nits = uTransfer == 1 ? pqEotf(max(c, 0.0)) : hlgInv(max(c, 0.0));
        vec3 lin = tonemap(nits);
        if (uGamut2020 == 1) {
            mat3 m = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187);
            lin = max(m * lin, 0.0);
        }
        c = pow(lin, vec3(1.0 / 2.4));
    }
    c = max(c, 0.0);
    if (uStraight == 0 && uHasAlpha == 1 && uLayout == 2) {
        o = vec4(c, a);   // already premultiplied
    } else {
        o = vec4(c * a, a);
    }
}
)";

const char *layer = HEAD R"(
uniform sampler2D uTex;
uniform float uOpacity;
uniform vec2 uTexel;
uniform float uAntiFlicker;
void main() {
    vec4 c = texture(uTex, vUv);
    if (uAntiFlicker > 0.0) {
        vec4 n = (texture(uTex, vUv + vec2(0.0, uTexel.y)) + texture(uTex, vUv - vec2(0.0, uTexel.y))) * 0.5;
        c = mix(c, n, uAntiFlicker * 0.5);
    }
    o = c * uOpacity;
}
)";

const char *blend = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uBase, uLayer;
uniform int uMode;
uniform float uOpacity;
float burn(float b, float s) { return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s)); }
float dodge(float b, float s) { return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s))); }
float softl(float b, float s) {
    if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
}
float hardl(float b, float s) { return s <= 0.5 ? b * 2.0 * s : b + (2.0 * s - 1.0) - b * (2.0 * s - 1.0); }
float vivid(float b, float s) { return s <= 0.5 ? burn(b, 2.0 * s) : dodge(b, 2.0 * (s - 0.5)); }
float sep(int m, float b, float s) {
    if (m == 1) return min(b, s);
    if (m == 2) return b * s;
    if (m == 3) return burn(b, s);
    if (m == 4) return max(b + s - 1.0, 0.0);
    if (m == 5) return max(b, s);
    if (m == 6) return b + s - b * s;
    if (m == 7) return dodge(b, s);
    if (m == 8) return min(b + s, 1.0);
    if (m == 9) return hardl(s, b);
    if (m == 10) return softl(b, s);
    if (m == 11) return hardl(b, s);
    if (m == 12) return vivid(b, s);
    if (m == 13) return clamp(b + 2.0 * s - 1.0, 0.0, 1.0);
    if (m == 14) return s <= 0.5 ? min(b, 2.0 * s) : max(b, 2.0 * s - 1.0);
    if (m == 15) return vivid(b, s) >= 0.5 ? 1.0 : 0.0;
    if (m == 16) return abs(b - s);
    if (m == 17) return b + s - 2.0 * b * s;
    if (m == 18) return max(b - s, 0.0);
    if (m == 19) return s <= 0.0 ? 1.0 : min(b / s, 1.0);
    return s;
}
float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 clipColor(vec3 c) {
    float l = lum(c), n = min(min(c.r, c.g), c.b), x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / (l - n);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
    return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }
vec3 setSat(vec3 c, float s) {
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    if (mx > mn) return (c - mn) * s / (mx - mn);
    return vec3(0.0);
}
void main() {
    vec4 B = texture(uBase, vUv);
    vec4 L = texture(uLayer, vUv) * uOpacity;
    if (uMode == 0) { o = L + B * (1.0 - L.a); return; }
    vec3 cs = unpremul(L), cb = unpremul(B);
    vec3 r;
    if (uMode == 20) r = setLum(setSat(cs, sat(cb)), lum(cb));
    else if (uMode == 21) r = setLum(setSat(cb, sat(cs)), lum(cb));
    else if (uMode == 22) r = setLum(cs, lum(cb));
    else if (uMode == 23) r = setLum(cb, lum(cs));
    else r = vec3(sep(uMode, cb.r, cs.r), sep(uMode, cb.g, cs.g), sep(uMode, cb.b, cs.b));
    vec3 csp = (1.0 - B.a) * cs + B.a * r;
    vec3 co = L.a * csp + (1.0 - L.a) * B.rgb;
    float ao = L.a + B.a * (1.0 - L.a);
    o = vec4(co, ao);
}
)";

const char *solid = HEAD R"(
uniform vec4 uColor;
void main() { o = vec4(uColor.rgb * uColor.a, uColor.a); }
)";

const char *bars = HEAD R"(
void main() {
    vec2 uv = vUv;
    vec3 c;
    vec3 top[7] = vec3[7](vec3(0.75), vec3(0.75, 0.75, 0.0), vec3(0.0, 0.75, 0.75), vec3(0.0, 0.75, 0.0),
                          vec3(0.75, 0.0, 0.75), vec3(0.75, 0.0, 0.0), vec3(0.0, 0.0, 0.75));
    int i = int(clamp(floor(uv.x * 7.0), 0.0, 6.0));
    if (uv.y < 0.67) c = top[i];
    else if (uv.y < 0.75) {
        vec3 mid[7] = vec3[7](vec3(0.0, 0.0, 0.75), vec3(0.0), vec3(0.75, 0.0, 0.75), vec3(0.0), vec3(0.0, 0.75, 0.75), vec3(0.0), vec3(0.75));
        c = mid[i];
    } else {
        float x = uv.x * 6.0;
        if (x < 1.0) c = vec3(0.0, 0.13, 0.3);
        else if (x < 2.0) c = vec3(1.0);
        else if (x < 3.0) c = vec3(0.2, 0.0, 0.42);
        else if (x < 4.4) c = vec3(0.0);
        else if (x < 4.6) c = vec3(0.035);
        else if (x < 4.8) c = vec3(0.075);
        else c = vec3(0.0);
    }
    o = vec4(c, 1.0);
}
)";

const char *checker = HEAD R"(
uniform vec2 uSize;
void main() {
    vec2 p = floor(vUv * uSize / 12.0);
    float k = mod(p.x + p.y, 2.0);
    o = vec4(vec3(mix(0.22, 0.30, k)), 1.0);
}
)";

const char *present = HEAD R"(
uniform sampler2D uTex;
uniform int uChecker;
uniform vec2 uSize;
uniform vec4 uUvRect;   // x, y, w, h in texture uv (zoom / pan)
void main() {
    vec2 uv = uUvRect.xy + vec2(vUv.x, 1.0 - vUv.y) * uUvRect.zw;
    vec4 c = texture(uTex, uv);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) c = vec4(0.0);
    vec3 bg = vec3(0.0);
    if (uChecker == 1) {
        vec2 p = floor(vUv * uSize / 10.0);
        bg = vec3(mix(0.25, 0.35, mod(p.x + p.y, 2.0)));
    }
    o = vec4(clamp(c.rgb + bg * (1.0 - c.a), 0.0, 1.0), 1.0);
}
)";

const char *blur1d = HEAD R"(
uniform sampler2D uTex;
uniform vec2 uStep;
uniform float uSigma;
uniform int uRadius;
uniform int uRepeat;
vec4 s(vec2 uv) {
    if (uRepeat == 0 && (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)) return vec4(0.0);
    return texture(uTex, uv);
}
void main() {
    vec4 acc = s(vUv);
    float wsum = 1.0;
    float inv = 1.0 / max(uSigma * uSigma, 0.0001);
    for (int i = 1; i <= uRadius; ++i) {
        float w = exp(-0.5 * float(i * i) * inv);
        acc += (s(vUv + uStep * float(i)) + s(vUv - uStep * float(i))) * w;
        wsum += 2.0 * w;
    }
    o = acc / wsum;
}
)";

const char *mask = HEAD R"(
uniform sampler2D uOrig, uFx;
uniform int uCount;
uniform int uShape[4];
uniform vec2 uCenter[4];
uniform vec2 uHalf[4];
uniform float uFeather[4];
uniform float uOpacity[4];
uniform float uRot[4];
uniform int uInvert[4];
uniform float uAspect;
float shapeMask(int i) {
    vec2 d = vUv - uCenter[i];
    d.x *= uAspect;
    float c = cos(-uRot[i]), s = sin(-uRot[i]);
    d = vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    vec2 h = uHalf[i] * vec2(uAspect, 1.0);
    float f = max(uFeather[i], 0.0005);
    float m;
    if (uShape[i] == 0) {
        float r = length(d / max(h, vec2(0.0001)));
        float fr = f / max(min(h.x, h.y), 0.0001);
        m = 1.0 - smoothstep(1.0 - fr, 1.0 + fr, r);
    } else {
        vec2 q = abs(d) - h;
        float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
        m = 1.0 - smoothstep(-f, f, dist);
    }
    if (uInvert[i] == 1) m = 1.0 - m;
    return m * uOpacity[i];
}
void main() {
    float m = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (i >= uCount) break;
        m = max(m, shapeMask(i));
    }
    o = mix(texture(uOrig, vUv), texture(uFx, vUv), m);
}
)";

const char *lumetri = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uTex;
uniform sampler3D uInLut; uniform float uInLutSize; uniform int uHasInLut;
uniform sampler3D uLook; uniform float uLookSize; uniform int uHasLook; uniform float uLookIntensity;
uniform sampler2D uCurve; uniform int uHasCurve;
uniform sampler2D uHueCurves; uniform int uHasHueCurves;
uniform float uTemp, uTint, uExposure, uContrast, uHighlights, uShadows, uWhites, uBlacks, uSaturation, uVibrance;
uniform float uFaded, uSharpen, uTintBalance;
uniform vec3 uShadowTint, uHighlightTint;
uniform vec3 uWheelS, uWheelM, uWheelH;
uniform float uLumaS, uLumaM, uLumaH;
uniform int uHsl, uHslShowMask;
uniform float uHslHue, uHslRange, uHslSatMin, uHslLumaMin, uHslLumaMax, uHslSoft, uHslHueShift, uHslSat, uHslExposure;
uniform float uVigAmount, uVigMid, uVigRound, uVigFeather;
uniform vec2 uTexel;
uniform float uAspect;
vec3 lut3(sampler3D s, float n, vec3 c) {
    vec3 coord = clamp(c, 0.0, 1.0) * (n - 1.0) / n + 0.5 / n;
    return texture(s, coord).rgb;
}
void main() {
    vec4 src = texture(uTex, vUv);
    float a = src.a;
    if (a <= 0.0) { o = vec4(0.0); return; }
    vec3 c = src.rgb / a;
    if (uSharpen != 0.0) {
        vec4 n = (texture(uTex, vUv + vec2(uTexel.x, 0.0)) + texture(uTex, vUv - vec2(uTexel.x, 0.0)) +
                  texture(uTex, vUv + vec2(0.0, uTexel.y)) + texture(uTex, vUv - vec2(0.0, uTexel.y))) * 0.25;
        c += (c - unpremul(n)) * uSharpen * 2.0;
    }
    if (uHasInLut == 1) c = lut3(uInLut, uInLutSize, c);
    vec3 wb = vec3(1.0 + 0.22 * uTemp, 1.0 - 0.22 * uTint, 1.0 - 0.22 * uTemp);
    c *= wb / luma(wb);
    vec3 lin = pow(max(c, 0.0), vec3(2.2)) * exp2(uExposure);
    c = pow(lin, vec3(1.0 / 2.2));
    c = (c - 0.5) * (1.0 + uContrast) + 0.5;
    float l = luma(c);
    c += uHighlights * 0.25 * smoothstep(0.35, 1.0, l);
    c += uShadows * 0.25 * (1.0 - smoothstep(0.0, 0.65, l));
    c += uWhites * 0.15 * smoothstep(0.6, 1.0, l);
    c += uBlacks * 0.12 * (1.0 - smoothstep(0.0, 0.4, l));
    l = luma(c);
    float s = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
    float vib = 1.0 + uVibrance * (1.0 - clamp(s, 0.0, 1.0));
    c = mix(vec3(l), c, uSaturation * vib);
    if (uHasCurve == 1) {
        vec3 cc = clamp(c, 0.0, 1.0);
        cc = vec3(texture(uCurve, vec2(cc.r, 0.5)).r, texture(uCurve, vec2(cc.g, 0.5)).r, texture(uCurve, vec2(cc.b, 0.5)).r);
        cc = vec3(texture(uCurve, vec2(cc.r, 0.5)).g, texture(uCurve, vec2(cc.g, 0.5)).b, texture(uCurve, vec2(cc.b, 0.5)).a);
        c = cc;
    }
    if (uHasHueCurves == 1) {
        vec3 hsv = rgb2hsv(clamp(c, 0.0, 1.0));
        vec4 hc = texture(uHueCurves, vec2(hsv.x, 0.5));
        float ls = texture(uHueCurves, vec2(luma(clamp(c, 0.0, 1.0)), 0.5)).a;
        hsv.y = clamp(hsv.y * hc.x * 2.0 * ls * 2.0, 0.0, 1.0);
        hsv.x = fract(hsv.x + (hc.y - 0.5));
        c = hsv2rgb(hsv) * (1.0 + (hc.z - 0.5) * 2.0);
    }
    l = clamp(luma(c), 0.0, 1.0);
    float ws = (1.0 - l) * (1.0 - l), wm = 4.0 * l * (1.0 - l), wh = l * l;
    c += uWheelS * ws + uWheelM * wm + uWheelH * wh;
    c += vec3(uLumaS * ws + uLumaM * wm + uLumaH * wh) * 0.5;
    if (uHasLook == 1) c = mix(c, lut3(uLook, uLookSize, c), uLookIntensity);
    c = mix(c, c * 0.85 + 0.1, uFaded);
    l = clamp(luma(c), 0.0, 1.0);
    float hm = smoothstep(0.2 + uTintBalance * 0.4, 0.8 + uTintBalance * 0.4, l);
    c += uShadowTint * (1.0 - hm) * 0.3 + uHighlightTint * hm * 0.3;
    if (uHsl == 1) {
        vec3 hsv = rgb2hsv(clamp(c, 0.0, 1.0));
        float dh = abs(hsv.x * 360.0 - uHslHue);
        dh = min(dh, 360.0 - dh);
        float soft = max(uHslSoft, 0.001);
        float m = 1.0 - smoothstep(uHslRange * 0.5, uHslRange * 0.5 + soft * 90.0, dh);
        m *= smoothstep(uHslSatMin - soft * 0.5, uHslSatMin + soft * 0.5, hsv.y);
        float L = luma(c);
        m *= smoothstep(uHslLumaMin - soft * 0.3, uHslLumaMin, L) * (1.0 - smoothstep(uHslLumaMax, uHslLumaMax + soft * 0.3, L));
        if (uHslShowMask == 1) {
            c = vec3(m);
        } else {
            vec3 h2 = hsv;
            h2.x = fract(h2.x + uHslHueShift / 360.0);
            h2.y = clamp(h2.y * uHslSat, 0.0, 1.0);
            c = mix(c, hsv2rgb(h2) * exp2(uHslExposure), m);
        }
    }
    if (uVigAmount != 0.0) {
        vec2 d = vUv - 0.5;
        d.x *= mix(1.0, uAspect, clamp((uVigRound + 1.0) * 0.5, 0.0, 1.0));
        float r = length(d) * 2.0;
        float v = smoothstep(uVigMid, uVigMid + uVigFeather + 0.001, r);
        c *= max(0.0, 1.0 + uVigAmount * 0.25 * v);
        if (uVigAmount > 0.0) c += uVigAmount * 0.05 * v;
    }
    o = vec4(max(c, 0.0) * a, a);
}
)";

const char *lut = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uTex;
uniform sampler3D uLut;
uniform float uSize;
uniform float uIntensity;
void main() {
    vec4 s = texture(uTex, vUv);
    vec3 c = unpremul(s);
    vec3 coord = clamp(c, 0.0, 1.0) * (uSize - 1.0) / uSize + 0.5 / uSize;
    vec3 l = texture(uLut, coord).rgb;
    o = vec4(mix(c, l, uIntensity) * s.a, s.a);
}
)";

const char *simpleColor = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uTex;
uniform int uOp;
uniform float uP[12];
uniform vec4 uC[3];
void main() {
    vec4 s = texture(uTex, vUv);
    vec3 c = unpremul(s);
    if (uOp == 0) {            // brightness & contrast
        c = (c - 0.5) * (1.0 + uP[1]) + 0.5 + uP[0];
    } else if (uOp == 1) {     // hue / saturation
        vec3 hsv = rgb2hsv(clamp(c, 0.0, 1.0));
        if (uP[3] > 0.5) { hsv.x = fract(uP[0] / 360.0); hsv.y = clamp(0.25 + uP[1] * 0.75, 0.0, 1.0); }
        else { hsv.x = fract(hsv.x + uP[0] / 360.0); hsv.y = clamp(hsv.y * (1.0 + uP[1]), 0.0, 1.0); }
        c = hsv2rgb(hsv);
        c = uP[2] >= 0.0 ? mix(c, vec3(1.0), uP[2]) : c * (1.0 + uP[2]);
    } else if (uOp == 2) {     // levels
        c = clamp((c - uP[0]) / max(uP[1] - uP[0], 0.0001), 0.0, 1.0);
        c = pow(c, vec3(1.0 / max(uP[2], 0.01)));
        c = mix(vec3(uP[3]), vec3(uP[4]), c);
    } else if (uOp == 3) {     // lift gamma gain (uC0 lift, uC1 gamma, uC2 gain as offsets around 0)
        vec3 lift = uC[0].rgb + uP[0];
        vec3 gam = uC[1].rgb + uP[1];
        vec3 gain = uC[2].rgb + uP[2];
        c = c * (1.0 + gain) + lift * (1.0 - c);
        c = pow(max(c, 0.0), vec3(1.0) / max(vec3(1.0) + gam, vec3(0.05)));
    } else if (uOp == 4) {     // channel mixer
        vec3 r = vec3(uP[0], uP[1], uP[2]), g = vec3(uP[3], uP[4], uP[5]), b = vec3(uP[6], uP[7], uP[8]);
        c = vec3(dot(c, r), dot(c, g), dot(c, b));
        if (uP[9] > 0.5) c = vec3(c.r);
    } else if (uOp == 5) {     // tint
        c = mix(c, mix(uC[0].rgb, uC[1].rgb, luma(c)), uP[0]);
    } else if (uOp == 6) {     // black & white
        c = vec3(luma(c));
    } else if (uOp == 7) {     // invert
        c = mix(1.0 - c, c, uP[0]);
    } else if (uOp == 8) {     // posterize
        float n = max(uP[0], 2.0) - 1.0;
        c = floor(c * n + 0.5) / n;
    } else if (uOp == 9) {     // threshold
        c = vec3(step(uP[0], luma(c)));
    }
    o = vec4(max(c, 0.0) * s.a, s.a);
}
)";

const char *crop = HEAD R"(
uniform sampler2D uTex;
uniform vec4 uCrop;      // left, top, right, bottom (fractions)
uniform vec2 uFeather;   // in uv
uniform int uZoom;
void main() {
    vec2 uv = vUv;
    if (uZoom == 1) uv = mix(uCrop.xy, vec2(1.0) - uCrop.zw, vUv);
    vec2 lo = uCrop.xy, hi = vec2(1.0) - uCrop.zw;
    vec2 f = max(uFeather, vec2(0.00001));
    float m = smoothstep(lo.x - f.x * 0.5, lo.x + f.x * 0.5, uv.x) * (1.0 - smoothstep(hi.x - f.x * 0.5, hi.x + f.x * 0.5, uv.x)) *
              smoothstep(lo.y - f.y * 0.5, lo.y + f.y * 0.5, uv.y) * (1.0 - smoothstep(hi.y - f.y * 0.5, hi.y + f.y * 0.5, uv.y));
    if (uZoom == 1) m = 1.0;
    o = texture(uTex, uv) * m;
}
)";

const char *distort = HEAD R"(
uniform sampler2D uTex;
uniform int uOp;
uniform float uP[8];
uniform mat3 uMat;
uniform float uAspect;
uniform float uOpacity;
vec4 tap(vec2 uv) {
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0);
    return texture(uTex, uv);
}
void main() {
    vec2 uv = vUv;
    if (uOp == 0) {             // flip
        if (uP[0] > 0.5) uv.x = 1.0 - uv.x;
        if (uP[1] > 0.5) uv.y = 1.0 - uv.y;
        o = texture(uTex, uv);
    } else if (uOp == 1) {      // mirror
        vec2 c = vec2(uP[0], uP[1]);
        vec2 n = vec2(cos(uP[2]), sin(uP[2]));
        vec2 d = (uv - c) * vec2(uAspect, 1.0);
        float k = dot(d, n);
        if (k > 0.0) d -= 2.0 * k * n;
        o = tap(c + d / vec2(uAspect, 1.0));
    } else if (uOp == 2) {      // lens distortion
        vec2 d = (uv - 0.5) * vec2(uAspect, 1.0);
        float r2 = dot(d, d);
        d *= (1.0 + uP[0] * r2) / max(uP[1], 0.01);
        o = tap(d / vec2(uAspect, 1.0) + 0.5);
    } else if (uOp == 3) {      // wave
        vec2 dir = vec2(cos(uP[2]), sin(uP[2]));
        vec2 perp = vec2(-dir.y, dir.x);
        float phase = dot(uv * vec2(uAspect, 1.0), perp) / max(uP[1], 0.0001) * 6.2831853 + uP[3];
        o = tap(uv + dir * sin(phase) * uP[0]);
    } else {                    // matrix based (corner pin homography, transform, stabiliser)
        vec3 q = uMat * vec3(uv, 1.0);
        o = tap(q.xy / q.z) * uOpacity;
    }
}
)";

const char *keying = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uTex;
uniform int uOp;
uniform vec3 uKey;
uniform float uP[8];
void main() {
    vec4 s = texture(uTex, vUv);
    vec3 c = unpremul(s);
    float a = s.a;
    if (uOp == 0) {
        // Chroma distance in YCbCr (BT.709).
        vec2 cc = vec2(dot(c, vec3(-0.1146, -0.3854, 0.5)), dot(c, vec3(0.5, -0.4542, -0.0458)));
        vec2 kc = vec2(dot(uKey, vec3(-0.1146, -0.3854, 0.5)), dot(uKey, vec3(0.5, -0.4542, -0.0458)));
        float d = length(cc - kc) / max(length(kc), 0.05);
        float tol = uP[0], soft = max(uP[1], 0.001);
        float k = smoothstep(tol, tol + soft, d);
        k = clamp((k - uP[2] * 0.5) / max(1.0 - abs(uP[2]) * 0.5, 0.01), 0.0, 1.0);
        // Spill suppression on the dominant key channel.
        int ch = (uKey.g >= uKey.r && uKey.g >= uKey.b) ? 1 : ((uKey.b >= uKey.r) ? 2 : 0);
        float other = ch == 1 ? max(c.r, c.b) : (ch == 2 ? max(c.r, c.g) : max(c.g, c.b));
        float excess = max(0.0, c[ch] - other);
        c[ch] -= excess * uP[3];
        c = mix(c, vec3(luma(c)), uP[4] * clamp(excess * 4.0, 0.0, 1.0));
        a *= k;
        if (uP[5] > 0.5) { o = vec4(vec3(a), 1.0); return; }
    } else {
        float l = luma(c);
        float k = smoothstep(uP[0], uP[0] + max(uP[1], 0.001), l);
        if (uP[2] > 0.5) k = 1.0 - k;
        a *= k;
    }
    o = vec4(c * a, a);
}
)";

const char *trackMatte = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uLayer, uMatte;
uniform int uMode;
uniform int uReverse;
void main() {
    vec4 l = texture(uLayer, vUv);
    vec4 m = texture(uMatte, vUv);
    float k = uMode == 0 ? m.a : luma(unpremul(m)) * m.a;
    if (uReverse == 1) k = 1.0 - k;
    o = l * k;
}
)";

const char *stylize = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uTex;
uniform int uOp;
uniform float uP[8];
uniform vec4 uC0, uC1;
uniform vec2 uTexel;
uniform float uAspect;
uniform float uTime;
void main() {
    vec4 s = texture(uTex, vUv);
    if (uOp == 0) {                       // mosaic
        vec2 n = vec2(max(uP[0], 1.0), max(floor(uP[0] / uAspect), 1.0));
        vec2 cell = (floor(vUv * n) + 0.5) / n;
        o = texture(uTex, cell);
    } else if (uOp == 1) {                // film grain
        vec2 p = floor(vUv / (uTexel * max(uP[1], 0.5)));
        float n = hash12(p + fract(uTime * 13.17) * 517.0) - 0.5;
        vec3 g = uP[2] > 0.5 ? vec3(n) : vec3(n, hash12(p * 1.3 + uTime) - 0.5, hash12(p * 0.7 - uTime) - 0.5);
        vec3 c = unpremul(s) + g * uP[0] * 0.5;
        o = vec4(max(c, 0.0) * s.a, s.a);
    } else if (uOp == 2) {                // vignette
        vec2 d = (vUv - 0.5) * vec2(uAspect, 1.0) / uAspect * 2.0;
        float r = length(d);
        float v = smoothstep(uP[1], uP[1] + max(uP[2], 0.001), r) * uP[0];
        vec3 c = mix(unpremul(s), uC0.rgb, v);
        o = vec4(c * s.a, s.a);
    } else if (uOp == 3) {                // emboss
        vec2 d = vec2(cos(uP[0]), sin(uP[0])) * uTexel * max(uP[1], 0.1);
        float a = luma(unpremul(texture(uTex, vUv - d))) - luma(unpremul(texture(uTex, vUv + d)));
        o = vec4(vec3(0.5 + a * 2.0) * s.a, s.a);
    } else if (uOp == 4) {                // find edges (Sobel)
        float tl = luma(unpremul(texture(uTex, vUv + uTexel * vec2(-1, -1))));
        float t = luma(unpremul(texture(uTex, vUv + uTexel * vec2(0, -1))));
        float tr = luma(unpremul(texture(uTex, vUv + uTexel * vec2(1, -1))));
        float l = luma(unpremul(texture(uTex, vUv + uTexel * vec2(-1, 0))));
        float r = luma(unpremul(texture(uTex, vUv + uTexel * vec2(1, 0))));
        float bl = luma(unpremul(texture(uTex, vUv + uTexel * vec2(-1, 1))));
        float b = luma(unpremul(texture(uTex, vUv + uTexel * vec2(0, 1))));
        float br = luma(unpremul(texture(uTex, vUv + uTexel * vec2(1, 1))));
        float gx = -tl - 2.0 * l - bl + tr + 2.0 * r + br;
        float gy = -tl - 2.0 * t - tr + bl + 2.0 * b + br;
        float e = clamp(length(vec2(gx, gy)), 0.0, 1.0);
        if (uP[0] > 0.5) e = 1.0 - e;
        vec3 c = mix(vec3(e), unpremul(s), uP[1]);
        o = vec4(c * s.a, s.a);
    } else if (uOp == 5) {                // chromatic aberration
        vec2 d = (vUv - 0.5) * uTexel * uP[0] * 2.0;
        vec4 r = texture(uTex, vUv + d), b = texture(uTex, vUv - d);
        o = vec4(r.r, s.g, b.b, max(s.a, max(r.a, b.a)));
    } else if (uOp == 6) {                // letterbox
        float frameRatio = uAspect;
        float h = clamp(frameRatio / max(uP[0], 0.1), 0.0, 1.0);
        float bar = (1.0 - h) * 0.5;
        if (vUv.y < bar || vUv.y > 1.0 - bar) o = vec4(uC0.rgb, 1.0);
        else o = s;
    } else if (uOp == 7) {                // ramp
        vec2 a = vec2(uP[0], uP[1]), b = vec2(uP[2], uP[3]);
        float t;
        if (uP[4] > 0.5) t = length((vUv - a) * vec2(uAspect, 1.0)) / max(length((b - a) * vec2(uAspect, 1.0)), 0.0001);
        else { vec2 ab = b - a; t = dot(vUv - a, ab) / max(dot(ab, ab), 0.000001); }
        vec4 g = mix(uC0, uC1, clamp(t, 0.0, 1.0));
        g.rgb *= g.a;
        o = mix(g, s, uP[5]);
    } else {
        o = s;
    }
}
)";

const char *dirBlur = HEAD R"(
uniform sampler2D uTex;
uniform int uOp;
uniform vec2 uVec;      // direction * length (uv) for directional
uniform vec2 uCenter;
uniform float uAmount;
void main() {
    vec4 acc = vec4(0.0);
    const int N = 32;
    for (int i = 0; i < N; ++i) {
        float t = float(i) / float(N - 1) - 0.5;
        vec2 uv = uOp == 0 ? vUv + uVec * t : uCenter + (vUv - uCenter) * (1.0 - uAmount * (t + 0.5));
        acc += texture(uTex, uv);
    }
    o = acc / float(N);
}
)";

const char *sharpen = HEAD R"(
uniform sampler2D uTex, uBlur;
uniform float uAmount, uThreshold;
void main() {
    vec4 s = texture(uTex, vUv), b = texture(uBlur, vUv);
    vec4 d = s - b;
    float m = step(uThreshold, abs(dot(d.rgb, vec3(0.333))));
    vec4 r = s + d * uAmount * m;
    r.rgb = clamp(r.rgb, 0.0, r.a);
    o = vec4(max(r.rgb, 0.0), s.a);
}
)";

const char *threshold = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uTex;
uniform float uThreshold;
void main() {
    vec4 s = texture(uTex, vUv);
    float l = luma(unpremul(s));
    o = s * smoothstep(uThreshold, uThreshold + 0.1, l);
}
)";

const char *glowCombine = HEAD R"(
uniform sampler2D uTex, uGlow;
uniform float uIntensity;
uniform vec3 uColor;
void main() {
    vec4 s = texture(uTex, vUv), g = texture(uGlow, vUv);
    vec3 add = g.rgb * uColor * uIntensity;
    o = vec4(s.rgb + add * (1.0 - s.rgb), max(s.a, clamp(g.a * uIntensity, 0.0, 1.0)));
}
)";

const char *transition = HEAD R"(
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 unpremul(vec4 c) { return c.a > 0.00001 ? c.rgb / c.a : vec3(0.0); }
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

uniform sampler2D uA, uB;
uniform float uProgress;
uniform int uOp;
uniform float uP[8];
uniform float uAspect;
vec4 tapA(vec2 uv) { if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0); return texture(uA, uv); }
vec4 tapB(vec2 uv) { if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return vec4(0.0); return texture(uB, uv); }
vec2 dirOf(float d) {
    if (d < 0.5) return vec2(-1.0, 0.0);
    if (d < 1.5) return vec2(1.0, 0.0);
    if (d < 2.5) return vec2(0.0, -1.0);
    return vec2(0.0, 1.0);
}
void main() {
    float p = clamp(uProgress, 0.0, 1.0);
    vec4 A = texture(uA, vUv), B = texture(uB, vUv);
    if (uOp == 0) { o = mix(A, B, p); }
    else if (uOp == 1) { o = vec4(pow(mix(pow(A.rgb, vec3(2.2)), pow(B.rgb, vec3(2.2)), p), vec3(1.0 / 2.2)), mix(A.a, B.a, p)); }
    else if (uOp == 2) { o = p < 0.5 ? clamp(A + B * 2.0 * p, 0.0, 1.0) : clamp(A * (2.0 - 2.0 * p) + B, 0.0, 1.0); }
    else if (uOp == 3) { vec4 k = vec4(0.0, 0.0, 0.0, 1.0); o = p < 0.5 ? mix(A, k, 2.0 * p) : mix(k, B, 2.0 * p - 1.0); }
    else if (uOp == 4) { vec4 w = vec4(1.0); o = p < 0.5 ? mix(A, w, 2.0 * p) : mix(w, B, 2.0 * p - 1.0); }
    else if (uOp == 6) {
        float ang = uP[0];
        vec2 d = vec2(cos(ang), sin(ang));
        float ext = (abs(d.x) + abs(d.y)) * 0.5;
        float s = dot(vUv - 0.5, d);
        float f = max(uP[1], 0.0005);
        float edge = mix(-ext - f, ext + f, p);
        float m = 1.0 - smoothstep(edge - f, edge + f, s);
        o = mix(A, B, m);
    } else if (uOp == 7) {
        vec2 d = (vUv - 0.5) * vec2(uAspect, 1.0);
        float a = atan(d.x, -d.y) / 6.2831853 + 0.5;
        a = fract(a + 0.5);
        float f = max(uP[0], 0.001);
        float m = 1.0 - smoothstep(p - f, p + f, a);
        o = mix(A, B, p >= 1.0 ? 1.0 : m);
    } else if (uOp == 8) {
        vec2 d = (vUv - vec2(uP[0], uP[1])) * vec2(uAspect, 1.0);
        float r = length(d);
        float maxR = length(vec2(uAspect, 1.0));
        float f = max(uP[2], 0.001);
        float m = 1.0 - smoothstep(p * (maxR + f) - f, p * (maxR + f) + f, r);
        o = mix(A, B, m);
    } else if (uOp == 9) {
        vec2 d = abs(vUv - vec2(uP[0], uP[1]));
        float r = max(d.x, d.y);
        float f = max(uP[2], 0.001);
        float m = 1.0 - smoothstep(p * (1.0 + f) - f, p * (1.0 + f) + f, r);
        o = mix(A, B, m);
    } else if (uOp == 10) {
        float d = uP[0] > 0.5 ? abs(vUv.y - 0.5) : abs(vUv.x - 0.5);
        o = d < p * 0.5 ? B : A;
    } else if (uOp == 11) {
        vec2 dir = dirOf(uP[0]);
        vec4 a = tapA(vUv - dir * p), b = tapB(vUv - dir * (p - 1.0));
        o = a + b;
    } else if (uOp == 12) {
        vec2 dir = dirOf(uP[0]);
        vec4 b = tapB(vUv - dir * (p - 1.0));
        o = b + A * (1.0 - b.a);
    } else if (uOp == 13) {
        float s = uP[0];
        vec4 acc = vec4(0.0);
        float k = sin(p * 3.14159265);
        for (int i = 0; i < 16; ++i) {
            float t = float(i) / 15.0;
            vec2 uvA = 0.5 + (vUv - 0.5) / (1.0 + p * s * (1.0 + t * k * 0.2));
            vec2 uvB = 0.5 + (vUv - 0.5) * (1.0 + (1.0 - p) * s * 0.5 * (1.0 + t * k * 0.2));
            acc += mix(tapA(uvA), tapB(uvB), smoothstep(0.35, 0.65, p));
        }
        o = acc / 16.0;
    } else if (uOp == 14) {
        float ang = p * 3.14159265;
        vec2 d = (vUv - 0.5) * vec2(uAspect, 1.0);
        bool second = p > 0.5;
        float sc = second ? (p - 0.5) * 2.0 : 1.0 - p * 2.0;
        float c = cos(ang), s = sin(ang);
        vec2 r = vec2(c * d.x - s * d.y, s * d.x + c * d.y) / max(sc, 0.001);
        vec2 uv = r / vec2(uAspect, 1.0) + 0.5;
        o = second ? tapB(uv) : tapA(uv);
    } else if (uOp == 15) {
        vec2 dir = dirOf(uP[0]);
        float k = sin(p * 3.14159265) * 0.25;
        vec4 acc = vec4(0.0);
        for (int i = 0; i < 24; ++i) {
            float t = (float(i) / 23.0 - 0.5) * k;
            vec2 uv = vUv + dir * t;
            acc += tapA(uv - dir * p) + tapB(uv - dir * (p - 1.0));
        }
        o = acc / 24.0;
    } else if (uOp == 16) {
        float k = uP[0] * sin(p * 3.14159265);
        float row = floor(vUv.y * 24.0);
        float n = hash12(vec2(row, floor(p * 30.0)));
        vec2 off = vec2((n - 0.5) * 0.2 * k, 0.0);
        vec4 src = p < 0.5 ? tapA(vUv + off) : tapB(vUv + off);
        vec4 r = p < 0.5 ? tapA(vUv + off + vec2(0.01 * k, 0.0)) : tapB(vUv + off + vec2(0.01 * k, 0.0));
        o = vec4(r.r, src.g, src.b, src.a);
    } else if (uOp == 17) {
        float l = luma(unpremul(B));
        if (uP[1] > 0.5) l = 1.0 - l;
        float f = max(uP[0], 0.001);
        float m = smoothstep(l - f, l + f, p * (1.0 + 2.0 * f) - f);
        o = mix(A, B, m);
    } else {
        o = mix(A, B, p);
    }
}
)";

const char *downscale = HEAD R"(
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    vec4 c = texture(uTex, vUv + uTexel * vec2(-0.25, -0.25)) + texture(uTex, vUv + uTexel * vec2(0.25, -0.25)) +
             texture(uTex, vUv + uTexel * vec2(-0.25, 0.25)) + texture(uTex, vUv + uTexel * vec2(0.25, 0.25));
    o = c * 0.25;
}
)";

} // namespace rc::shaders
