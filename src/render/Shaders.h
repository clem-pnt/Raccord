#pragma once
// GLSL fragment shaders used by the renderer. All work on premultiplied RGBA in
// display referred (Rec.709 gamma) space, uv (0,0) = image top-left.
namespace rc::shaders {

extern const char *convert;      // YUV / RGB planes -> RGBA
extern const char *layer;        // transformed clip onto a layer
extern const char *blend;        // blend modes
extern const char *solid;
extern const char *bars;
extern const char *checker;
extern const char *present;
extern const char *blur1d;
extern const char *mask;
extern const char *lumetri;
extern const char *lut;
extern const char *simpleColor;  // brightness/contrast, hue/sat, levels, balance, mixer, tint, b&w, invert, posterize, threshold
extern const char *crop;
extern const char *distort;      // mirror, lens, wave, corner pin, flip, transform-like
extern const char *keying;       // chroma / luma key
extern const char *trackMatte;
extern const char *stylize;      // mosaic, grain, vignette, emboss, edges, aberration, letterbox, gradient
extern const char *dirBlur;      // directional + zoom blur
extern const char *sharpen;
extern const char *glowCombine;
extern const char *threshold;
extern const char *transition;
extern const char *downscale;

} // namespace rc::shaders
