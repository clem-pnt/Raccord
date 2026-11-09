// Minimal OpenFX plugin used by the tests: "Raccord Test Invert".
// out.rgb = mix(src.rgb, alpha - src.rgb, amount) * tint ; mode 1 swaps red and blue.
// Exercises the property, parameter, image effect and multithread suites.
#include "ofx/ofxApi.h"
#include <cstring>

namespace {

OfxHost *gHost = nullptr;
const OfxPropertySuiteV1 *gProp = nullptr;
const OfxImageEffectSuiteV1 *gFx = nullptr;
const OfxParameterSuiteV1 *gParam = nullptr;
const OfxMultiThreadSuiteV1 *gThread = nullptr;

struct Job {
    const char *src; char *dst;
    int srcRow, dstRow, x1, y1, x2, y2, sx1, sy1, dx1, dy1;
    double amount, tint[3]; int mode;
};

void work(unsigned int idx, unsigned int n, void *arg)
{
    const Job &j = *static_cast<Job *>(arg);
    const int rows = j.y2 - j.y1;
    const int a = j.y1 + int(long(rows) * idx / n), b = j.y1 + int(long(rows) * (idx + 1) / n);
    for (int y = a; y < b; ++y) {
        const float *s = reinterpret_cast<const float *>(j.src + long(y - j.sy1) * j.srcRow) + (j.x1 - j.sx1) * 4;
        float *d = reinterpret_cast<float *>(j.dst + long(y - j.dy1) * j.dstRow) + (j.x1 - j.dx1) * 4;
        for (int x = j.x1; x < j.x2; ++x, s += 4, d += 4) {
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = float((s[k] + (s[3] - 2 * s[k]) * j.amount) * j.tint[k]);
            if (j.mode == 1) { float t = c[0]; c[0] = c[2]; c[2] = t; }
            d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = s[3];
        }
    }
}

OfxStatus describe(OfxImageEffectHandle e)
{
    OfxPropertySetHandle p;
    gFx->getPropertySet(e, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Raccord Test Invert");
    gProp->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Raccord Tests");
    gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthByte);
    // The host must answer host property queries.
    char *hostName = nullptr;
    if (gProp->propGetString(gHost->host, kOfxPropName, 0, &hostName) != kOfxStatOK || !hostName) return kOfxStatFailed;
    return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle e)
{
    OfxPropertySetHandle cp;
    gFx->clipDefine(e, kOfxImageEffectOutputClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gFx->clipDefine(e, kOfxImageEffectSimpleSourceClipName, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    OfxParamSetHandle ps;
    gFx->getParamSet(e, &ps);
    OfxPropertySetHandle pp;
    gParam->paramDefine(ps, kOfxParamTypeDouble, "amount", &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, "Amount");
    gProp->propSetDouble(pp, kOfxParamPropDefault, 0, 1.0);
    gProp->propSetDouble(pp, kOfxParamPropMin, 0, 0.0);
    gProp->propSetDouble(pp, kOfxParamPropMax, 0, 1.0);
    gParam->paramDefine(ps, kOfxParamTypeRGB, "tint", &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, "Tint");
    const double white[3] = {1, 1, 1};
    gProp->propSetDoubleN(pp, kOfxParamPropDefault, 3, white);
    gParam->paramDefine(ps, kOfxParamTypeChoice, "mode", &pp);
    gProp->propSetString(pp, kOfxPropLabel, 0, "Mode");
    gProp->propSetString(pp, kOfxParamPropChoiceOption, 0, "Normal");
    gProp->propSetString(pp, kOfxParamPropChoiceOption, 1, "Swap Red/Blue");
    gProp->propSetInt(pp, kOfxParamPropDefault, 0, 0);
    gParam->paramDefine(ps, kOfxParamTypeBoolean, "hidden", &pp);
    gProp->propSetInt(pp, kOfxParamPropSecret, 0, 1);
    return kOfxStatOK;
}

OfxStatus render(OfxImageEffectHandle e, OfxPropertySetHandle in)
{
    OfxTime t = 0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    int win[4];
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, win);
    OfxImageClipHandle sc, oc;
    gFx->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &sc, nullptr);
    gFx->clipGetHandle(e, kOfxImageEffectOutputClipName, &oc, nullptr);
    OfxPropertySetHandle si = nullptr, oi = nullptr;
    if (gFx->clipGetImage(sc, t, nullptr, &si) != kOfxStatOK) return kOfxStatFailed;
    if (gFx->clipGetImage(oc, t, nullptr, &oi) != kOfxStatOK) { gFx->clipReleaseImage(si); return kOfxStatFailed; }
    char *depth = nullptr;
    gProp->propGetString(si, kOfxImageEffectPropPixelDepth, 0, &depth);
    OfxStatus st = kOfxStatOK;
    if (!depth || std::strcmp(depth, kOfxBitDepthFloat) != 0) st = kOfxStatErrImageFormat;
    Job j{};
    void *sp = nullptr, *dp = nullptr;
    int sb[4], db[4];
    gProp->propGetPointer(si, kOfxImagePropData, 0, &sp);
    gProp->propGetPointer(oi, kOfxImagePropData, 0, &dp);
    gProp->propGetIntN(si, kOfxImagePropBounds, 4, sb);
    gProp->propGetIntN(oi, kOfxImagePropBounds, 4, db);
    gProp->propGetInt(si, kOfxImagePropRowBytes, 0, &j.srcRow);
    gProp->propGetInt(oi, kOfxImagePropRowBytes, 0, &j.dstRow);
    j.src = static_cast<const char *>(sp);
    j.dst = static_cast<char *>(dp);
    j.x1 = win[0]; j.y1 = win[1]; j.x2 = win[2]; j.y2 = win[3];
    j.sx1 = sb[0]; j.sy1 = sb[1]; j.dx1 = db[0]; j.dy1 = db[1];
    OfxParamSetHandle ps;
    gFx->getParamSet(e, &ps);
    OfxParamHandle h;
    gParam->paramGetHandle(ps, "amount", &h, nullptr);
    gParam->paramGetValueAtTime(h, t, &j.amount);
    gParam->paramGetHandle(ps, "tint", &h, nullptr);
    gParam->paramGetValueAtTime(h, t, &j.tint[0], &j.tint[1], &j.tint[2]);
    gParam->paramGetHandle(ps, "mode", &h, nullptr);
    gParam->paramGetValue(h, &j.mode);
    if (st == kOfxStatOK && j.src && j.dst) {
        unsigned int n = 1;
        gThread->multiThreadNumCPUs(&n);
        gThread->multiThread(work, n, &j);
    }
    gFx->clipReleaseImage(si);
    gFx->clipReleaseImage(oi);
    return st;
}

OfxStatus mainEntry(const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    auto e = (OfxImageEffectHandle)const_cast<void *>(handle);
    if (!std::strcmp(action, kOfxActionLoad)) {
        gProp = static_cast<const OfxPropertySuiteV1 *>(gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1));
        gFx = static_cast<const OfxImageEffectSuiteV1 *>(gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1));
        gParam = static_cast<const OfxParameterSuiteV1 *>(gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1));
        gThread = static_cast<const OfxMultiThreadSuiteV1 *>(gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1));
        return gProp && gFx && gParam && gThread ? kOfxStatOK : kOfxStatErrMissingHostFeature;
    }
    if (!std::strcmp(action, kOfxActionDescribe)) return describe(e);
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(e);
    if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(e, in);
    return kOfxStatReplyDefault;
}

void setHost(OfxHost *h) { gHost = h; }

OfxPlugin gPlugin = {kOfxImageEffectPluginApi, 1, "fr.raccord.test.Invert", 1, 0, setHost, mainEntry};

} // namespace

#ifdef _WIN32
#define OFX_EXPORT extern "C" __declspec(dllexport)
#else
#define OFX_EXPORT extern "C" __attribute__((visibility("default")))
#endif
OFX_EXPORT int OfxGetNumberOfPlugins(void) { return 1; }
OFX_EXPORT OfxPlugin *OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : nullptr; }
