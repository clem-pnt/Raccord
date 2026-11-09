#include "OfxHost.h"
#include "ofxApi.h"
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QStandardPaths>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Handle types (opaque to plugins)

struct Prop {
    char type = 0;   // 's' string, 'd' double, 'i' int, 'p' pointer
    std::vector<std::string> s;
    std::vector<double> d;
    std::vector<int> i;
    std::vector<void *> p;
    int dim() const { return type == 's' ? (int)s.size() : type == 'd' ? (int)d.size() : type == 'i' ? (int)i.size() : (int)p.size(); }
};

struct OfxPropertySetStruct {
    std::map<std::string, Prop> m;
    virtual ~OfxPropertySetStruct() = default;

    void setS(const std::string &k, std::vector<std::string> v) { auto &p = m[k]; p = {}; p.type = 's'; p.s = std::move(v); }
    void setS(const std::string &k, const std::string &v) { setS(k, std::vector<std::string>{v}); }
    void setD(const std::string &k, std::vector<double> v) { auto &p = m[k]; p = {}; p.type = 'd'; p.d = std::move(v); }
    void setI(const std::string &k, std::vector<int> v) { auto &p = m[k]; p = {}; p.type = 'i'; p.i = std::move(v); }
    void setP(const std::string &k, void *v) { auto &p = m[k]; p = {}; p.type = 'p'; p.p = {v}; }
    std::string s(const std::string &k, int idx = 0, const std::string &def = {}) const
    {
        auto it = m.find(k);
        if (it == m.end() || it->second.type != 's' || idx >= (int)it->second.s.size()) return def;
        return it->second.s[idx];
    }
    std::vector<std::string> sv(const std::string &k) const
    {
        auto it = m.find(k);
        return it == m.end() || it->second.type != 's' ? std::vector<std::string>{} : it->second.s;
    }
    double d(const std::string &k, int idx = 0, double def = 0) const
    {
        auto it = m.find(k);
        if (it == m.end()) return def;
        const Prop &p = it->second;
        if (p.type == 'd' && idx < (int)p.d.size()) return p.d[idx];
        if (p.type == 'i' && idx < (int)p.i.size()) return p.i[idx];
        return def;
    }
    int i(const std::string &k, int idx = 0, int def = 0) const { return (int)std::lround(d(k, idx, def)); }
    bool has(const std::string &k) const { return m.count(k) > 0; }
};

struct OfxParamStruct {
    std::string type, name;
    OfxPropertySetStruct props;
    double v[4] = {0, 0, 0, 0};
    std::string str;
    int dims() const
    {
        if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D) return 2;
        if (type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D || type == kOfxParamTypeRGB) return 3;
        if (type == kOfxParamTypeRGBA) return 4;
        return 1;
    }
    bool isInt() const
    {
        return type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice ||
               type == kOfxParamTypeInteger2D || type == kOfxParamTypeInteger3D;
    }
    bool isString() const { return type == kOfxParamTypeString || type == kOfxParamTypeCustom || type == kOfxParamTypeStrChoice; }
};

struct OfxParamSetStruct {
    OfxPropertySetStruct props;
    std::vector<std::unique_ptr<OfxParamStruct>> list;
    OfxParamStruct *find(const std::string &n) const
    {
        for (const auto &p : list)
            if (p->name == n) return p.get();
        return nullptr;
    }
};

struct OfxImageEffectStruct;

struct OfxImageClipStruct {
    std::string name;
    OfxPropertySetStruct props;
    OfxImageEffectStruct *effect = nullptr;
};

namespace rc::ofx {
struct Plugin;
}

struct RenderState {
    std::vector<float> src, dst;   // bottom-up
    int w = 0, h = 0;
    double scale = 1.0;
    double time = 0;
    double projW = 1920, projH = 1080;
};

struct OfxImageEffectStruct {
    OfxPropertySetStruct props;
    OfxParamSetStruct params;
    std::vector<std::unique_ptr<OfxImageClipStruct>> clips;
    rc::ofx::Plugin *plugin = nullptr;
    RenderState *rs = nullptr;
    OfxImageClipStruct *clip(const std::string &n) const
    {
        for (const auto &c : clips)
            if (c->name == n) return c.get();
        return nullptr;
    }
};

struct ImageProps : OfxPropertySetStruct {};

struct OfxImageMemoryStruct {
    std::vector<char> buf;
};

struct OfxMutex {
    std::recursive_mutex m;
};

namespace rc::ofx {

struct Plugin {
    QLibrary *lib = nullptr;
    OfxPlugin *api = nullptr;
    QString id;
    std::string context;
    std::unique_ptr<OfxImageEffectStruct> descriptor;   // described in context
    std::map<QString, std::unique_ptr<OfxImageEffectStruct>> instances;
};

namespace {

std::mutex g_renderMutex;
std::set<ImageProps *> g_images;
thread_local unsigned int t_threadIndex = 0;
thread_local bool t_spawned = false;

// ---------------------------------------------------------------------------
// Property suite

Prop *findProp(OfxPropertySetHandle h, const char *name)
{
    if (!h || !name) return nullptr;
    auto it = h->m.find(name);
    return it == h->m.end() ? nullptr : &it->second;
}

template <typename T>
OfxStatus setAt(OfxPropertySetHandle h, const char *name, int idx, char type, const T &value)
{
    if (!h || !name || idx < 0) return kOfxStatErrBadHandle;
    Prop &p = h->m[name];
    if (p.type != type) {
        // Re-typing an existing property (e.g. int default written as double): convert.
        Prop np;
        np.type = type;
        const int n = p.dim();
        if constexpr (std::is_same_v<T, double>) for (int k = 0; k < n; ++k) np.d.push_back(p.type == 'i' ? p.i[k] : 0.0);
        if constexpr (std::is_same_v<T, int>) for (int k = 0; k < n; ++k) np.i.push_back(p.type == 'd' ? int(p.d[k]) : 0);
        p = np;
    }
    if constexpr (std::is_same_v<T, std::string>) { if ((int)p.s.size() <= idx) p.s.resize(idx + 1); p.s[idx] = value; }
    if constexpr (std::is_same_v<T, double>) { if ((int)p.d.size() <= idx) p.d.resize(idx + 1); p.d[idx] = value; }
    if constexpr (std::is_same_v<T, int>) { if ((int)p.i.size() <= idx) p.i.resize(idx + 1); p.i[idx] = value; }
    if constexpr (std::is_same_v<T, void *>) { if ((int)p.p.size() <= idx) p.p.resize(idx + 1); p.p[idx] = value; }
    return kOfxStatOK;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char *n, int i, void *v) { return setAt(h, n, i, 'p', v); }
OfxStatus propSetString(OfxPropertySetHandle h, const char *n, int i, const char *v) { return setAt(h, n, i, 's', std::string(v ? v : "")); }
OfxStatus propSetDouble(OfxPropertySetHandle h, const char *n, int i, double v) { return setAt(h, n, i, 'd', v); }
OfxStatus propSetInt(OfxPropertySetHandle h, const char *n, int i, int v) { return setAt(h, n, i, 'i', v); }
OfxStatus propSetPointerN(OfxPropertySetHandle h, const char *n, int c, void *const *v)
{
    if (!h || !n) return kOfxStatErrBadHandle;
    Prop p; p.type = 'p'; p.p.assign(v, v + c); h->m[n] = p; return kOfxStatOK;
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char *n, int c, const char *const *v)
{
    if (!h || !n) return kOfxStatErrBadHandle;
    Prop p; p.type = 's'; for (int k = 0; k < c; ++k) p.s.push_back(v[k] ? v[k] : ""); h->m[n] = p; return kOfxStatOK;
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char *n, int c, const double *v)
{
    if (!h || !n) return kOfxStatErrBadHandle;
    Prop p; p.type = 'd'; p.d.assign(v, v + c); h->m[n] = p; return kOfxStatOK;
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char *n, int c, const int *v)
{
    if (!h || !n) return kOfxStatErrBadHandle;
    Prop p; p.type = 'i'; p.i.assign(v, v + c); h->m[n] = p; return kOfxStatOK;
}
OfxStatus propGetPointer(OfxPropertySetHandle h, const char *n, int i, void **v)
{
    Prop *p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type != 'p') return kOfxStatErrValue;
    if (i < 0 || i >= (int)p->p.size()) return kOfxStatErrBadIndex;
    *v = p->p[i];
    return kOfxStatOK;
}
OfxStatus propGetString(OfxPropertySetHandle h, const char *n, int i, char **v)
{
    Prop *p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type != 's') return kOfxStatErrValue;
    if (i < 0 || i >= (int)p->s.size()) return kOfxStatErrBadIndex;
    *v = const_cast<char *>(p->s[i].c_str());
    return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle h, const char *n, int i, double *v)
{
    Prop *p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type == 'd') { if (i < 0 || i >= (int)p->d.size()) return kOfxStatErrBadIndex; *v = p->d[i]; return kOfxStatOK; }
    if (p->type == 'i') { if (i < 0 || i >= (int)p->i.size()) return kOfxStatErrBadIndex; *v = p->i[i]; return kOfxStatOK; }
    return kOfxStatErrValue;
}
OfxStatus propGetInt(OfxPropertySetHandle h, const char *n, int i, int *v)
{
    Prop *p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    if (p->type == 'i') { if (i < 0 || i >= (int)p->i.size()) return kOfxStatErrBadIndex; *v = p->i[i]; return kOfxStatOK; }
    if (p->type == 'd') { if (i < 0 || i >= (int)p->d.size()) return kOfxStatErrBadIndex; *v = (int)p->d[i]; return kOfxStatOK; }
    return kOfxStatErrValue;
}
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char *n, int c, void **v)
{
    for (int k = 0; k < c; ++k) if (OfxStatus s = propGetPointer(h, n, k, &v[k])) return s;
    return kOfxStatOK;
}
OfxStatus propGetStringN(OfxPropertySetHandle h, const char *n, int c, char **v)
{
    for (int k = 0; k < c; ++k) if (OfxStatus s = propGetString(h, n, k, &v[k])) return s;
    return kOfxStatOK;
}
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char *n, int c, double *v)
{
    for (int k = 0; k < c; ++k) if (OfxStatus s = propGetDouble(h, n, k, &v[k])) return s;
    return kOfxStatOK;
}
OfxStatus propGetIntN(OfxPropertySetHandle h, const char *n, int c, int *v)
{
    for (int k = 0; k < c; ++k) if (OfxStatus s = propGetInt(h, n, k, &v[k])) return s;
    return kOfxStatOK;
}
OfxStatus propReset(OfxPropertySetHandle h, const char *n)
{
    Prop *p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    for (auto &x : p->d) x = 0;
    for (auto &x : p->i) x = 0;
    for (auto &x : p->s) x.clear();
    for (auto &x : p->p) x = nullptr;
    return kOfxStatOK;
}
OfxStatus propGetDimension(OfxPropertySetHandle h, const char *n, int *c)
{
    Prop *p = findProp(h, n);
    if (!p) return kOfxStatErrUnknown;
    *c = p->dim();
    return kOfxStatOK;
}

const OfxPropertySuiteV1 g_propSuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt, propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
    propGetPointer, propGetString, propGetDouble, propGetInt, propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
    propReset, propGetDimension};

// ---------------------------------------------------------------------------
// Image effect suite

void defaultClipProps(OfxPropertySetStruct &p, const std::string &name)
{
    p.setS(kOfxPropType, kOfxTypeClip);
    p.setS(kOfxPropName, name);
    p.setS(kOfxPropLabel, name);
    p.setS(kOfxPropShortLabel, name);
    p.setS(kOfxPropLongLabel, name);
    p.setS(kOfxImageEffectPropSupportedComponents, std::vector<std::string>{});
    p.setI(kOfxImageEffectPropTemporalClipAccess, {0});
    p.setI(kOfxImageClipPropOptional, {0});
    p.setI(kOfxImageClipPropIsMask, {0});
    p.setS(kOfxImageClipPropFieldExtraction, "OfxImageFieldDoubled");
    p.setI(kOfxImageEffectPropSupportsTiles, {1});
}

OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle *ph)
{
    if (!e) return kOfxStatErrBadHandle;
    *ph = &e->props;
    return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle *ps)
{
    if (!e) return kOfxStatErrBadHandle;
    *ps = &e->params;
    return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle e, const char *name, OfxPropertySetHandle *ph)
{
    if (!e || !name) return kOfxStatErrBadHandle;
    OfxImageClipStruct *c = e->clip(name);
    if (!c) {
        auto nc = std::make_unique<OfxImageClipStruct>();
        nc->name = name;
        nc->effect = e;
        defaultClipProps(nc->props, name);
        c = nc.get();
        e->clips.push_back(std::move(nc));
    }
    if (ph) *ph = &c->props;
    return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle e, const char *name, OfxImageClipHandle *clip, OfxPropertySetHandle *ph)
{
    if (!e || !name) return kOfxStatErrBadHandle;
    OfxImageClipStruct *c = e->clip(name);
    if (!c) return kOfxStatErrUnknown;
    if (clip) *clip = c;
    if (ph) *ph = &c->props;
    return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle *ph)
{
    if (!c) return kOfxStatErrBadHandle;
    *ph = &c->props;
    return kOfxStatOK;
}
OfxStatus clipGetImage(OfxImageClipHandle c, OfxTime, const OfxRectD *, OfxPropertySetHandle *img)
{
    if (!c || !c->effect || !img) return kOfxStatErrBadHandle;
    RenderState *rs = c->effect->rs;
    if (!rs) return kOfxStatFailed;
    float *data = nullptr;
    if (c->name == kOfxImageEffectOutputClipName) data = rs->dst.data();
    else if (c->name == kOfxImageEffectSimpleSourceClipName) data = rs->src.data();
    else return kOfxStatFailed;   // extra inputs are not connected
    auto *im = new ImageProps;
    im->setS(kOfxPropType, kOfxTypeImage);
    im->setP(kOfxImagePropData, data);
    im->setI(kOfxImagePropBounds, {0, 0, rs->w, rs->h});
    im->setI(kOfxImagePropRegionOfDefinition, {0, 0, rs->w, rs->h});
    im->setI(kOfxImagePropRowBytes, {rs->w * 4 * int(sizeof(float))});
    im->setD(kOfxImagePropPixelAspectRatio, {1.0});
    im->setS(kOfxImagePropField, kOfxImageFieldNone);
    im->setS(kOfxImagePropUniqueIdentifier, c->name + ":" + std::to_string(rs->time));
    im->setD(kOfxImageEffectPropRenderScale, {rs->scale, rs->scale});
    im->setS(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
    im->setS(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    im->setS(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    g_images.insert(im);
    *img = im;
    return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle h)
{
    auto *im = static_cast<ImageProps *>(h);
    if (!g_images.count(im)) return kOfxStatErrBadHandle;
    g_images.erase(im);
    delete im;
    return kOfxStatOK;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle c, OfxTime, OfxRectD *b)
{
    if (!c || !c->effect || !b) return kOfxStatErrBadHandle;
    const RenderState *rs = c->effect->rs;
    const double W = rs ? rs->projW : c->effect->props.d(kOfxImageEffectPropProjectSize, 0, 1920);
    const double H = rs ? rs->projH : c->effect->props.d(kOfxImageEffectPropProjectSize, 1, 1080);
    *b = {0, 0, W, H};
    return kOfxStatOK;
}
int effectAbort(OfxImageEffectHandle) { return 0; }
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle *h)
{
    auto *m = new (std::nothrow) OfxImageMemoryStruct;
    if (!m) return kOfxStatErrMemory;
    try { m->buf.resize(n); } catch (...) { delete m; return kOfxStatErrMemory; }
    *h = m;
    return kOfxStatOK;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle h) { delete h; return kOfxStatOK; }
OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void **ptr)
{
    if (!h) return kOfxStatErrBadHandle;
    *ptr = h->buf.data();
    return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle h) { return h ? kOfxStatOK : kOfxStatErrBadHandle; }

const OfxImageEffectSuiteV1 g_effectSuite = {getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage,
                                             clipReleaseImage, clipGetRegionOfDefinition, effectAbort, imageMemoryAlloc,
                                             imageMemoryFree, imageMemoryLock, imageMemoryUnlock};

// ---------------------------------------------------------------------------
// Parameter suite

void defaultParamProps(OfxParamStruct &p)
{
    auto &q = p.props;
    q.setS(kOfxPropType, kOfxTypeParameter);
    q.setS(kOfxParamPropType, p.type);
    q.setS(kOfxPropName, p.name);
    q.setS(kOfxPropLabel, p.name);
    q.setS(kOfxPropShortLabel, p.name);
    q.setS(kOfxPropLongLabel, p.name);
    q.setS(kOfxParamPropScriptName, p.name);
    q.setS(kOfxParamPropHint, "");
    q.setS(kOfxParamPropParent, "");
    q.setI(kOfxParamPropSecret, {0});
    q.setI(kOfxParamPropEnabled, {1});
    q.setI(kOfxParamPropAnimates, {1});
    q.setI(kOfxParamPropIsAnimating, {0});
    q.setI(kOfxParamPropCanUndo, {1});
    q.setI(kOfxParamPropEvaluateOnChange, {1});
    q.setI(kOfxParamPropPersistant, {1});
    q.setI(kOfxParamPropPluginMayWrite, {0});
    q.setS(kOfxParamPropCacheInvalidation, kOfxParamInvalidateValueChange);
    q.setP(kOfxParamPropDataPtr, nullptr);
    q.setP(kOfxParamPropInteractV1, nullptr);
    q.setI(kOfxParamPropHasHostOverlayHandle, {0});
    q.setI(kOfxParamPropUseHostOverlayHandle, {0});
    const int n = p.dims();
    if (p.isString()) {
        q.setS(kOfxParamPropDefault, "");
        q.setS(kOfxParamPropStringMode, kOfxParamStringIsSingleLine);
        q.setI(kOfxParamPropStringFilePathExists, {1});
    } else if (p.isInt()) {
        q.setI(kOfxParamPropDefault, std::vector<int>(n, 0));
        q.setI(kOfxParamPropMin, std::vector<int>(n, INT_MIN));
        q.setI(kOfxParamPropMax, std::vector<int>(n, INT_MAX));
        q.setI(kOfxParamPropDisplayMin, std::vector<int>(n, INT_MIN));
        q.setI(kOfxParamPropDisplayMax, std::vector<int>(n, INT_MAX));
    } else {
        q.setD(kOfxParamPropDefault, std::vector<double>(n, 0.0));
        q.setD(kOfxParamPropMin, std::vector<double>(n, -DBL_MAX));
        q.setD(kOfxParamPropMax, std::vector<double>(n, DBL_MAX));
        q.setD(kOfxParamPropDisplayMin, std::vector<double>(n, -DBL_MAX));
        q.setD(kOfxParamPropDisplayMax, std::vector<double>(n, DBL_MAX));
        q.setD(kOfxParamPropIncrement, {1.0});
        q.setI(kOfxParamPropDigits, {2});
        q.setS(kOfxParamPropDoubleType, kOfxParamDoubleTypePlain);
        q.setS(kOfxParamPropDefaultCoordinateSystem, kOfxParamCoordinatesCanonical);
    }
    if (p.type == kOfxParamTypeChoice || p.type == kOfxParamTypeStrChoice)
        q.setS(kOfxParamPropChoiceOption, std::vector<std::string>{});
    if (p.type == kOfxParamTypeRGBA) q.setS(kOfxParamPropDimensionLabel, std::vector<std::string>{"r", "g", "b", "a"});
    else if (p.type == kOfxParamTypeRGB) q.setS(kOfxParamPropDimensionLabel, std::vector<std::string>{"r", "g", "b"});
    else if (n == 2) q.setS(kOfxParamPropDimensionLabel, std::vector<std::string>{"x", "y"});
    else if (n == 3) q.setS(kOfxParamPropDimensionLabel, std::vector<std::string>{"x", "y", "z"});
    if (p.type == kOfxParamTypeGroup) q.setI(kOfxParamPropGroupOpen, {1});
    if (p.type == kOfxParamTypePage) q.setS(kOfxParamPropPageChild, std::vector<std::string>{});
}

void resetToDefault(OfxParamStruct &p)
{
    if (p.isString()) {
        p.str = p.props.s(kOfxParamPropDefault);
        return;
    }
    for (int k = 0; k < p.dims(); ++k) p.v[k] = p.props.d(kOfxParamPropDefault, k);
}

OfxStatus paramDefine(OfxParamSetHandle ps, const char *type, const char *name, OfxPropertySetHandle *ph)
{
    if (!ps || !type || !name) return kOfxStatErrBadHandle;
    if (ps->find(name)) return kOfxStatErrExists;
    auto p = std::make_unique<OfxParamStruct>();
    p->type = type;
    p->name = name;
    defaultParamProps(*p);
    if (ph) *ph = &p->props;
    ps->list.push_back(std::move(p));
    return kOfxStatOK;
}
OfxStatus paramGetHandle(OfxParamSetHandle ps, const char *name, OfxParamHandle *h, OfxPropertySetHandle *ph)
{
    if (!ps || !name) return kOfxStatErrBadHandle;
    OfxParamStruct *p = ps->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (h) *h = p;
    if (ph) *ph = &p->props;
    return kOfxStatOK;
}
OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle *ph)
{
    if (!ps) return kOfxStatErrBadHandle;
    *ph = &ps->props;
    return kOfxStatOK;
}
OfxStatus paramGetPropertySet(OfxParamHandle p, OfxPropertySetHandle *ph)
{
    if (!p) return kOfxStatErrBadHandle;
    *ph = &p->props;
    return kOfxStatOK;
}

OfxStatus readValue(OfxParamStruct *p, va_list ap, double scale = 1.0)
{
    if (!p) return kOfxStatErrBadHandle;
    if (p->isString()) {
        *va_arg(ap, char **) = const_cast<char *>(p->str.c_str());
        return kOfxStatOK;
    }
    if (p->type == kOfxParamTypeGroup || p->type == kOfxParamTypePage || p->type == kOfxParamTypePushButton ||
        p->type == kOfxParamTypeParametric || p->type == kOfxParamTypeBytes)
        return kOfxStatErrUnsupported;
    for (int k = 0; k < p->dims(); ++k) {
        if (p->isInt()) *va_arg(ap, int *) = (int)std::lround(p->v[k] * scale);
        else *va_arg(ap, double *) = p->v[k] * scale;
    }
    return kOfxStatOK;
}
OfxStatus writeValue(OfxParamStruct *p, va_list ap)
{
    if (!p) return kOfxStatErrBadHandle;
    if (p->isString()) {
        const char *s = va_arg(ap, const char *);
        p->str = s ? s : "";
        return kOfxStatOK;
    }
    for (int k = 0; k < p->dims(); ++k)
        p->v[k] = p->isInt() ? double(va_arg(ap, int)) : va_arg(ap, double);
    return kOfxStatOK;
}
OfxStatus paramGetValue(OfxParamHandle p, ...)
{
    va_list ap;
    va_start(ap, p);
    OfxStatus s = readValue(p, ap);
    va_end(ap);
    return s;
}
OfxStatus paramGetDerivative(OfxParamHandle p, OfxTime time, ...)
{
    va_list ap;
    va_start(ap, time);
    OfxStatus s = readValue(p, ap, 0.0);
    va_end(ap);
    return s;
}
OfxStatus paramGetIntegral(OfxParamHandle p, OfxTime t1, OfxTime t2, ...)
{
    va_list ap;
    va_start(ap, t2);
    OfxStatus s = readValue(p, ap, t2 - t1);
    va_end(ap);
    return s;
}
OfxStatus paramSetValue(OfxParamHandle p, ...)
{
    va_list ap;
    va_start(ap, p);
    OfxStatus s = writeValue(p, ap);
    va_end(ap);
    return s;
}
OfxStatus paramSetValueAtTime(OfxParamHandle p, OfxTime time, ...)
{
    va_list ap;
    va_start(ap, time);
    OfxStatus s = writeValue(p, ap);
    va_end(ap);
    return s;
}
OfxStatus paramGetNumKeys(OfxParamHandle p, unsigned int *n)
{
    if (!p) return kOfxStatErrBadHandle;
    *n = 0;
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime *) { return kOfxStatErrBadIndex; }
OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int *) { return kOfxStatFailed; }
OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatErrBadIndex; }
OfxStatus paramDeleteAllKeys(OfxParamHandle p) { return p ? kOfxStatOK : kOfxStatErrBadHandle; }
OfxStatus paramCopy(OfxParamHandle to, OfxParamHandle from, OfxTime, const OfxRangeD *)
{
    if (!to || !from || to->type != from->type) return kOfxStatErrValue;
    std::copy(std::begin(from->v), std::end(from->v), std::begin(to->v));
    to->str = from->str;
    return kOfxStatOK;
}
OfxStatus paramEditBegin(OfxParamSetHandle, const char *) { return kOfxStatOK; }
OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

// paramGetValueAtTime: values are evaluated by the host at the render time
// (Raccord keyframes), so any requested time returns the current value.
OfxStatus paramGetValueAtTimeImpl(OfxParamHandle p, OfxTime time, ...)
{
    va_list ap;
    va_start(ap, time);
    OfxStatus s = readValue(p, ap);
    va_end(ap);
    return s;
}

const OfxParameterSuiteV1 g_paramSuite = {paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet,
                                          paramGetValue, paramGetValueAtTimeImpl, paramGetDerivative, paramGetIntegral,
                                          paramSetValue, paramSetValueAtTime, paramGetNumKeys, paramGetKeyTime,
                                          paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys, paramCopy, paramEditBegin,
                                          paramEditEnd};

// ---------------------------------------------------------------------------
// Memory, threads, messages, progress, timeline

OfxStatus memoryAlloc(void *, size_t n, void **out)
{
    *out = std::malloc(n ? n : 1);
    return *out ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFree(void *p)
{
    std::free(p);
    return kOfxStatOK;
}
const OfxMemorySuiteV1 g_memorySuite = {memoryAlloc, memoryFree};

unsigned int cpuCount() { return std::max(1u, std::thread::hardware_concurrency()); }

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int n, void *arg)
{
    if (!func) return kOfxStatFailed;
    if (n == 0) n = cpuCount();
    if (n == 1 || t_spawned) {
        // Nested calls run inline.
        const unsigned int oldIdx = t_threadIndex;
        for (unsigned int k = 0; k < n; ++k) { t_threadIndex = k; func(k, n, arg); }
        t_threadIndex = oldIdx;
        return kOfxStatOK;
    }
    std::vector<std::thread> threads;
    for (unsigned int k = 1; k < n; ++k)
        threads.emplace_back([=] { t_threadIndex = k; t_spawned = true; func(k, n, arg); });
    t_threadIndex = 0;
    t_spawned = true;
    func(0, n, arg);
    t_spawned = false;
    for (auto &t : threads) t.join();
    return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int *n) { *n = cpuCount(); return kOfxStatOK; }
OfxStatus multiThreadIndex(unsigned int *i) { *i = t_threadIndex; return kOfxStatOK; }
int multiThreadIsSpawnedThread() { return t_spawned ? 1 : 0; }
OfxStatus mutexCreate(OfxMutexHandle *m, int lockCount)
{
    *m = new OfxMutex;
    for (int k = 0; k < lockCount; ++k) (*m)->m.lock();
    return kOfxStatOK;
}
OfxStatus mutexDestroy(const OfxMutexHandle m) { delete m; return kOfxStatOK; }
OfxStatus mutexLock(const OfxMutexHandle m) { if (!m) return kOfxStatErrBadHandle; m->m.lock(); return kOfxStatOK; }
OfxStatus mutexUnLock(const OfxMutexHandle m) { if (!m) return kOfxStatErrBadHandle; m->m.unlock(); return kOfxStatOK; }
OfxStatus mutexTryLock(const OfxMutexHandle m) { if (!m) return kOfxStatErrBadHandle; return m->m.try_lock() ? kOfxStatOK : kOfxStatFailed; }
const OfxMultiThreadSuiteV1 g_threadSuite = {multiThread, multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread,
                                             mutexCreate, mutexDestroy, mutexLock, mutexUnLock, mutexTryLock};

QStringList *g_messages = nullptr;
void logMessage(const char *type, const char *fmt, va_list ap)
{
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
    std::fprintf(stderr, "[ofx] %s: %s\n", type ? type : "", buf);
    if (g_messages && type && (!std::strcmp(type, kOfxMessageError) || !std::strcmp(type, kOfxMessageFatal)))
        g_messages->push_back(QString::fromUtf8(buf));
}
OfxStatus message(void *, const char *type, const char *, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logMessage(type, fmt, ap);
    va_end(ap);
    // Questions get the default answer (no UI from the render thread).
    return type && !std::strcmp(type, kOfxMessageQuestion) ? kOfxStatReplyYes : kOfxStatOK;
}
OfxStatus setPersistentMessage(void *, const char *type, const char *, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logMessage(type, fmt, ap);
    va_end(ap);
    return kOfxStatOK;
}
OfxStatus clearPersistentMessage(void *) { return kOfxStatOK; }
const OfxMessageSuiteV2 g_messageSuite = {message, setPersistentMessage, clearPersistentMessage};

OfxStatus progressStart1(void *, const char *) { return kOfxStatOK; }
OfxStatus progressStart2(void *, const char *, const char *) { return kOfxStatOK; }
OfxStatus progressUpdate(void *, double) { return kOfxStatOK; }
OfxStatus progressEnd(void *) { return kOfxStatOK; }
const OfxProgressSuiteV1 g_progress1 = {progressStart1, progressUpdate, progressEnd};
const OfxProgressSuiteV2 g_progress2 = {progressStart2, progressUpdate, progressEnd};

OfxStatus getTime(void *inst, double *t)
{
    auto *e = static_cast<OfxImageEffectStruct *>(inst);
    *t = e && e->rs ? e->rs->time : 0;
    return kOfxStatOK;
}
OfxStatus gotoTime(void *, double) { return kOfxStatOK; }
OfxStatus getTimeBounds(void *inst, double *a, double *b)
{
    auto *e = static_cast<OfxImageEffectStruct *>(inst);
    *a = 0;
    *b = e ? e->props.d(kOfxImageEffectInstancePropEffectDuration) : 0;
    return kOfxStatOK;
}
const OfxTimeLineSuiteV1 g_timeline = {getTime, gotoTime, getTimeBounds};

OfxPropertySetStruct g_hostProps;

const void *fetchSuite(OfxPropertySetHandle, const char *name, int version)
{
    if (!name) return nullptr;
    const std::string n = name;
    if (n == kOfxPropertySuite && version == 1) return &g_propSuite;
    if (n == kOfxImageEffectSuite && version == 1) return &g_effectSuite;
    if (n == kOfxParameterSuite && version == 1) return &g_paramSuite;
    if (n == kOfxMemorySuite && version == 1) return &g_memorySuite;
    if (n == kOfxMultiThreadSuite && version == 1) return &g_threadSuite;
    if (n == kOfxMessageSuite && (version == 1 || version == 2)) return &g_messageSuite;   // V2 extends V1
    if (n == kOfxProgressSuite && version == 1) return &g_progress1;
    if (n == kOfxProgressSuite && version == 2) return &g_progress2;
    if (n == kOfxTimeLineSuite && version == 1) return &g_timeline;
    return nullptr;
}

OfxHost g_host = {&g_hostProps, fetchSuite};

void initHostProps()
{
    auto &h = g_hostProps;
    h.setS(kOfxPropType, kOfxTypeImageEffectHost);
    h.setS(kOfxPropName, "fr.raccord.Raccord");
    h.setS(kOfxPropLabel, "Raccord");
    h.setI(kOfxPropAPIVersion, {1, 4});
    h.setI(kOfxPropVersion, {0, 1, 0});
    h.setS(kOfxPropVersionLabel, "0.1.0");
    h.setP(kOfxPropHostOSHandle, nullptr);
    h.setI(kOfxImageEffectHostPropIsBackground, {0});
    h.setS(kOfxImageEffectHostPropNativeOrigin, kOfxHostNativeOriginBottomLeft);
    h.setI(kOfxImageEffectPropSupportsOverlays, {0});
    h.setI(kOfxImageEffectPropSupportsMultiResolution, {1});
    h.setI(kOfxImageEffectPropSupportsTiles, {0});
    h.setI(kOfxImageEffectPropTemporalClipAccess, {0});
    h.setS(kOfxImageEffectPropSupportedComponents, std::vector<std::string>{kOfxImageComponentRGBA});
    h.setS(kOfxImageEffectPropSupportedContexts, std::vector<std::string>{kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral});
    h.setS(kOfxImageEffectPropSupportedPixelDepths, std::vector<std::string>{kOfxBitDepthFloat});
    h.setI(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
    h.setI(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
    h.setI(kOfxImageEffectPropSetableFrameRate, {0});
    h.setI(kOfxImageEffectPropSetableFielding, {0});
    h.setI(kOfxImageEffectInstancePropSequentialRender, {0});
    h.setI(kOfxImageEffectPropRenderQualityDraft, {0});
    h.setS(kOfxImageEffectPropOpenGLRenderSupported, "false");
    h.setS(kOfxImageEffectPropCudaRenderSupported, "false");
    h.setS(kOfxImageEffectPropOpenCLRenderSupported, "false");
    h.setS(kOfxImageEffectPropMetalRenderSupported, "false");
    h.setI(kOfxParamHostPropSupportsCustomInteract, {0});
    h.setI(kOfxParamHostPropSupportsStringAnimation, {0});
    h.setI(kOfxParamHostPropSupportsChoiceAnimation, {0});
    h.setI(kOfxParamHostPropSupportsBooleanAnimation, {0});
    h.setI(kOfxParamHostPropSupportsCustomAnimation, {0});
    h.setI(kOfxParamHostPropSupportsStrChoice, {0});
    h.setI(kOfxParamHostPropSupportsStrChoiceAnimation, {0});
    h.setI(kOfxParamHostPropSupportsParametricAnimation, {0});
    h.setI(kOfxParamHostPropMaxParameters, {-1});
    h.setI(kOfxParamHostPropMaxPages, {0});
    h.setI(kOfxParamHostPropPageRowColumnCount, {0, 0});
}

void defaultEffectProps(OfxPropertySetStruct &p)
{
    p.setS(kOfxPropType, kOfxTypeImageEffect);
    p.setS(kOfxPropLabel, "");
    p.setS(kOfxPropShortLabel, "");
    p.setS(kOfxPropLongLabel, "");
    p.setS(kOfxPropPluginDescription, "");
    p.setI(kOfxPropVersion, {0});
    p.setS(kOfxPropVersionLabel, "");
    p.setS(kOfxImageEffectPluginPropGrouping, "");
    p.setS(kOfxImageEffectPropSupportedContexts, std::vector<std::string>{});
    p.setS(kOfxImageEffectPropSupportedPixelDepths, std::vector<std::string>{});
    p.setI(kOfxImageEffectPluginPropSingleInstance, {0});
    p.setS(kOfxImageEffectPluginRenderThreadSafety, "OfxImageEffectRenderInstanceSafe");
    p.setI(kOfxImageEffectPluginPropHostFrameThreading, {0});
    p.setI(kOfxImageEffectPropSupportsMultiResolution, {1});
    p.setI(kOfxImageEffectPropSupportsTiles, {1});
    p.setI(kOfxImageEffectPropTemporalClipAccess, {0});
    p.setI(kOfxImageEffectPluginPropFieldRenderTwiceAlways, {1});
    p.setI(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
    p.setI(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
    p.setP(kOfxImageEffectPluginPropOverlayInteractV1, nullptr);
}

OfxStatus call(Plugin &pl, const char *action, const void *handle, OfxPropertySetHandle in = nullptr, OfxPropertySetHandle out = nullptr)
{
    if (!pl.api || !pl.api->mainEntry) return kOfxStatFailed;
    try {
        return pl.api->mainEntry(action, handle, in, out);
    } catch (...) {
        std::fprintf(stderr, "[ofx] %s threw during %s\n", qPrintable(pl.id), action);
        return kOfxStatErrFatal;
    }
}

bool isNormalisedXY(const std::string &t) { return t == kOfxParamDoubleTypeNormalisedXY || t == kOfxParamDoubleTypeNormalisedXYAbsolute; }
bool isCanonicalXY(const std::string &t) { return t == kOfxParamDoubleTypeXY || t == kOfxParamDoubleTypeXYAbsolute; }

} // namespace

// ---------------------------------------------------------------------------

struct Host::Impl {
    std::vector<std::unique_ptr<Plugin>> plugins;
    std::map<QString, Plugin *> byEffect;
    QStringList errors;
    bool loaded = false;

    void loadBinary(const QString &path);
    bool describe(Plugin &pl);
    OfxImageEffectStruct *instance(Plugin &pl, const QString &key, const RenderArgs &a, QString *err);
};

Host &Host::instance()
{
    static Host h;
    return h;
}

Host::Host() : d(std::make_unique<Impl>())
{
    initHostProps();
    g_messages = &d->errors;
}

Host::~Host()
{
    g_messages = nullptr;
    // Plugins are intentionally left loaded: unloading at exit races with their static destructors.
}

QStringList Host::searchPaths() const
{
    QStringList dirs;
    for (const QString &p : qEnvironmentVariable("OFX_PLUGIN_PATH").split(':', Qt::SkipEmptyParts)) dirs << p;
#ifdef Q_OS_WIN
    const QString common = qEnvironmentVariable("CommonProgramFiles", "C:/Program Files/Common Files");
    dirs << QDir::fromNativeSeparators(common) + "/OFX/Plugins";
    dirs
#else
    dirs << "/usr/OFX/Plugins" << "/usr/local/OFX/Plugins"
#endif
         << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/raccord/ofx";
    return dirs;
}

QStringList Host::loadedPlugins() const
{
    QStringList out;
    for (const auto &p : d->plugins) out << p->id;
    return out;
}

QStringList Host::errors() const { return d->errors; }

int Host::loadPlugins(const QStringList &extra)
{
    if (d->loaded) return int(d->plugins.size());
    d->loaded = true;
    QStringList dirs = extra + searchPaths();
    dirs.removeDuplicates();
    for (const QString &dir : dirs) {
        QDir root(dir);
        if (!root.exists()) continue;
        for (const QFileInfo &bundle : root.entryInfoList({"*.ofx.bundle"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
#ifdef Q_OS_WIN
            QDir bin(bundle.absoluteFilePath() + "/Contents/Win64");
#else
            QDir bin(bundle.absoluteFilePath() + "/Contents/Linux-x86-64");
#endif
            for (const QFileInfo &f : bin.entryInfoList({"*.ofx"}, QDir::Files))
                d->loadBinary(f.absoluteFilePath());
        }
    }
    return int(d->plugins.size());
}

void Host::Impl::loadBinary(const QString &path)
{
    auto *lib = new QLibrary(path);
    lib->setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!lib->load()) {
        errors << QStringLiteral("%1: %2").arg(path, lib->errorString());
        delete lib;
        return;
    }
    auto count = reinterpret_cast<OfxGetNumberOfPluginsFn>(lib->resolve("OfxGetNumberOfPlugins"));
    auto get = reinterpret_cast<OfxGetPluginFn>(lib->resolve("OfxGetPlugin"));
    auto setHost = reinterpret_cast<OfxSetHostFn>(lib->resolve("OfxSetHost"));
    if (!count || !get) {
        errors << QStringLiteral("%1: not an OpenFX plugin").arg(path);
        lib->unload();
        delete lib;
        return;
    }
    if (setHost) setHost(&g_host);
    const int n = count();
    for (int k = 0; k < n; ++k) {
        OfxPlugin *api = get(k);
        if (!api || !api->pluginApi || std::strcmp(api->pluginApi, kOfxImageEffectPluginApi) != 0 || api->apiVersion != 1)
            continue;
        auto pl = std::make_unique<Plugin>();
        pl->lib = lib;
        pl->api = api;
        pl->id = QString::fromUtf8(api->pluginIdentifier);
        if (api->setHost) api->setHost(&g_host);
        if (describe(*pl)) {
            byEffect["ofx." + pl->id] = pl.get();
            plugins.push_back(std::move(pl));
        }
    }
}

bool Host::Impl::describe(Plugin &pl)
{
    OfxStatus st = call(pl, kOfxActionLoad, nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        errors << QStringLiteral("%1: load failed (%2)").arg(pl.id).arg(st);
        return false;
    }
    auto desc = std::make_unique<OfxImageEffectStruct>();
    desc->plugin = &pl;
    defaultEffectProps(desc->props);
    st = call(pl, kOfxActionDescribe, desc.get());
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        errors << QStringLiteral("%1: describe failed (%2)").arg(pl.id).arg(st);
        return false;
    }
    const auto contexts = desc->props.sv(kOfxImageEffectPropSupportedContexts);
    auto supports = [&](const char *c) { return std::find(contexts.begin(), contexts.end(), c) != contexts.end(); };
    if (supports(kOfxImageEffectContextFilter)) pl.context = kOfxImageEffectContextFilter;
    else if (supports(kOfxImageEffectContextGeneral)) pl.context = kOfxImageEffectContextGeneral;
    else {
        errors << QStringLiteral("%1: no filter/general context (generators and transitions are not supported yet)").arg(pl.id);
        return false;
    }
    // Pixel depth: we only provide float images.
    const auto depths = desc->props.sv(kOfxImageEffectPropSupportedPixelDepths);
    if (!depths.empty() && std::find(depths.begin(), depths.end(), kOfxBitDepthFloat) == depths.end()) {
        errors << QStringLiteral("%1: float images not supported by the plugin").arg(pl.id);
        return false;
    }
    auto ctx = std::make_unique<OfxImageEffectStruct>();
    ctx->plugin = &pl;
    ctx->props = desc->props;
    OfxPropertySetStruct in;
    in.setS(kOfxImageEffectPropContext, pl.context);
    st = call(pl, kOfxImageEffectActionDescribeInContext, ctx.get(), &in);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        errors << QStringLiteral("%1: describe in context failed (%2)").arg(pl.id).arg(st);
        return false;
    }
    if (!ctx->clip(kOfxImageEffectOutputClipName) || !ctx->clip(kOfxImageEffectSimpleSourceClipName)) {
        errors << QStringLiteral("%1: missing Source/Output clip").arg(pl.id);
        return false;
    }

    // Effect definition for the registry.
    EffectDef def;
    def.id = "ofx." + pl.id;
    QString label = QString::fromStdString(ctx->props.s(kOfxPropLabel));
    if (label.isEmpty()) label = pl.id.section('.', -1);
    def.name = label;
    const QString grouping = QString::fromStdString(ctx->props.s(kOfxImageEffectPluginPropGrouping));
    def.category = grouping.isEmpty() ? QStringLiteral("OpenFX") : QStringLiteral("OpenFX/") + grouping;
    def.kind = EffectKind::Video;
    def.supportsMask = true;
    def.description = QString::fromStdString(ctx->props.s(kOfxPropPluginDescription));
    std::map<std::string, std::string> groupLabels;
    for (const auto &p : ctx->params.list)
        if (p->type == kOfxParamTypeGroup) groupLabels[p->name] = p->props.s(kOfxPropLabel, 0, p->name);
    for (const auto &p : ctx->params.list) {
        if (p->props.i(kOfxParamPropSecret)) continue;
        ParamDef pd;
        pd.id = QString::fromStdString(p->name);
        pd.name = QString::fromStdString(p->props.s(kOfxPropLabel, 0, p->name));
        const std::string parent = p->props.s(kOfxParamPropParent);
        if (!parent.empty() && groupLabels.count(parent)) pd.group = QString::fromStdString(groupLabels[parent]);
        pd.animatable = p->props.i(kOfxParamPropAnimates, 0, 1) != 0;
        auto range = [&](double fallbackMin, double fallbackMax) {
            double lo = p->props.d(kOfxParamPropDisplayMin, 0, -DBL_MAX), hi = p->props.d(kOfxParamPropDisplayMax, 0, DBL_MAX);
            if (lo <= -1e300 || lo <= INT_MIN) lo = p->props.d(kOfxParamPropMin, 0, fallbackMin);
            if (hi >= 1e300 || hi >= INT_MAX) hi = p->props.d(kOfxParamPropMax, 0, fallbackMax);
            if (lo <= -1e300 || lo <= INT_MIN) lo = fallbackMin;
            if (hi >= 1e300 || hi >= INT_MAX) hi = fallbackMax;
            pd.min = lo;
            pd.max = hi;
        };
        const std::string &t = p->type;
        if (t == kOfxParamTypeDouble) {
            const std::string dt = p->props.s(kOfxParamPropDoubleType);
            pd.type = dt == kOfxParamDoubleTypeAngle ? ParamType::Angle : ParamType::Float;
            const double def0 = p->props.d(kOfxParamPropDefault);
            range(std::min(0.0, def0), std::max(1.0, std::abs(def0) * 2));
            pd.def = Value(def0);
            pd.step = std::max(1e-4, p->props.d(kOfxParamPropIncrement, 0, (pd.max - pd.min) / 100));
        } else if (t == kOfxParamTypeInteger) {
            pd.type = ParamType::Int;
            const double def0 = p->props.d(kOfxParamPropDefault);
            range(std::min(0.0, def0), std::max(100.0, std::abs(def0) * 2));
            pd.def = Value(def0);
            pd.step = 1;
        } else if (t == kOfxParamTypeBoolean) {
            pd.type = ParamType::Bool;
            pd.def = Value(double(p->props.i(kOfxParamPropDefault)));
        } else if (t == kOfxParamTypeChoice) {
            pd.type = ParamType::Enum;
            for (const auto &o : p->props.sv(kOfxParamPropChoiceOption)) pd.options << QString::fromStdString(o);
            pd.def = Value(double(p->props.i(kOfxParamPropDefault)));
        } else if (t == kOfxParamTypeRGBA || t == kOfxParamTypeRGB) {
            pd.type = ParamType::Color;
            pd.def = Value(p->props.d(kOfxParamPropDefault, 0), p->props.d(kOfxParamPropDefault, 1), p->props.d(kOfxParamPropDefault, 2),
                           t == kOfxParamTypeRGBA ? p->props.d(kOfxParamPropDefault, 3, 1.0) : 1.0);
        } else if (t == kOfxParamTypeDouble2D) {
            pd.type = ParamType::Point;
            const std::string dt = p->props.s(kOfxParamPropDoubleType);
            double x = p->props.d(kOfxParamPropDefault, 0), y = p->props.d(kOfxParamPropDefault, 1);
            const bool normDefault = p->props.s(kOfxParamPropDefaultCoordinateSystem) == kOfxParamCoordinatesNormalised;
            if (isCanonicalXY(dt) && !normDefault) { x /= 1920.0; y /= 1080.0; }
            if (isCanonicalXY(dt) || isNormalisedXY(dt)) y = 1.0 - y;   // OFX y is up, Raccord y is down
            pd.def = Value(x, y);
        } else if (t == kOfxParamTypeString) {
            const std::string mode = p->props.s(kOfxParamPropStringMode);
            if (mode == kOfxParamStringIsLabel) continue;
            pd.type = mode == kOfxParamStringIsFilePath ? ParamType::File : mode == kOfxParamStringIsMultiLine ? ParamType::Text : ParamType::String;
            pd.def = Value(QString::fromStdString(p->props.s(kOfxParamPropDefault)));
            pd.animatable = false;
        } else {
            continue;   // groups, pages, buttons, custom, parametric, 3D: not exposed
        }
        def.params.push_back(pd);
    }
    EffectRegistry::instance().add(def);
    pl.descriptor = std::move(ctx);
    return true;
}

OfxImageEffectStruct *Host::Impl::instance(Plugin &pl, const QString &key, const RenderArgs &a, QString *err)
{
    auto it = pl.instances.find(key);
    if (it != pl.instances.end()) return it->second.get();
    const OfxImageEffectStruct &dc = *pl.descriptor;
    auto inst = std::make_unique<OfxImageEffectStruct>();
    inst->plugin = &pl;
    inst->props = dc.props;
    auto &p = inst->props;
    p.setS(kOfxPropType, kOfxTypeImageEffectInstance);
    p.setS(kOfxImageEffectPropContext, pl.context);
    p.setI(kOfxPropIsInteractive, {0});
    p.setP(kOfxPropInstanceData, nullptr);
    p.setI(kOfxImageEffectInstancePropSequentialRender, {0});
    const double W = a.width / std::max(1e-6, a.renderScale), H = a.height / std::max(1e-6, a.renderScale);
    p.setD(kOfxImageEffectPropProjectSize, {W, H});
    p.setD(kOfxImageEffectPropProjectExtent, {W, H});
    p.setD(kOfxImageEffectPropProjectOffset, {0, 0});
    p.setD(kOfxImageEffectPropProjectPixelAspectRatio, {1.0});
    p.setD(kOfxImageEffectInstancePropEffectDuration, {a.duration});
    p.setD(kOfxImageEffectPropFrameRate, {a.fps});
    for (const auto &dp : dc.params.list) {
        auto np = std::make_unique<OfxParamStruct>();
        np->type = dp->type;
        np->name = dp->name;
        np->props = dp->props;
        np->props.setS(kOfxPropType, kOfxTypeParameterInstance);
        resetToDefault(*np);
        inst->params.list.push_back(std::move(np));
    }
    for (const auto &dclip : dc.clips) {
        auto c = std::make_unique<OfxImageClipStruct>();
        c->name = dclip->name;
        c->effect = inst.get();
        c->props = dclip->props;
        const bool connected = c->name == kOfxImageEffectOutputClipName || c->name == kOfxImageEffectSimpleSourceClipName;
        auto &q = c->props;
        q.setI(kOfxImageClipPropConnected, {connected ? 1 : 0});
        q.setS(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
        q.setS(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
        q.setS(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
        q.setD(kOfxImagePropPixelAspectRatio, {1.0});
        q.setD(kOfxImageEffectPropFrameRate, {a.fps});
        q.setD(kOfxImageEffectPropFrameRange, {0, a.duration});
        q.setS(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
        q.setS(kOfxImageClipPropUnmappedComponents, kOfxImageComponentRGBA);
        q.setS(kOfxImageClipPropUnmappedPixelDepth, kOfxBitDepthFloat);
        q.setD(kOfxImageEffectPropUnmappedFrameRange, {0, a.duration});
        q.setD(kOfxImageEffectPropUnmappedFrameRate, {a.fps});
        q.setI(kOfxImageClipPropContinuousSamples, {0});
        inst->clips.push_back(std::move(c));
    }
    const OfxStatus st = call(pl, kOfxActionCreateInstance, inst.get());
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        if (err) *err = QStringLiteral("%1: create instance failed (%2)").arg(pl.id).arg(st);
        return nullptr;
    }
    OfxImageEffectStruct *raw = inst.get();
    pl.instances[key] = std::move(inst);
    return raw;
}

bool Host::render(const EffectInstance &e, Ticks local, const QString &instanceKey, const RenderArgs &a, QString *error)
{
    auto it = d->byEffect.find(e.type);
    if (it == d->byEffect.end() || !a.src || !a.dst || a.width <= 0 || a.height <= 0) return false;
    Plugin &pl = *it->second;
    std::lock_guard<std::mutex> lock(g_renderMutex);
    OfxImageEffectStruct *inst = d->instance(pl, instanceKey, a, error);
    if (!inst) return false;
    RenderState rs;
    rs.w = a.width;
    rs.h = a.height;
    rs.scale = a.renderScale;
    rs.time = a.time;
    rs.projW = a.width / std::max(1e-6, a.renderScale);
    rs.projH = a.height / std::max(1e-6, a.renderScale);
    inst->props.setD(kOfxImageEffectPropProjectSize, {rs.projW, rs.projH});
    inst->props.setD(kOfxImageEffectPropProjectExtent, {rs.projW, rs.projH});
    // Parameters at this time.
    const EffectDef *def = EffectRegistry::instance().find(e.type);
    for (const auto &p : inst->params.list) {
        const QString pid = QString::fromStdString(p->name);
        if (!def || !def->param(pid)) continue;
        const Value v = e.value(pid, local);
        if (p->isString()) {
            p->str = v.s.toStdString();
        } else if (p->type == kOfxParamTypeDouble2D) {
            const std::string dt = p->props.s(kOfxParamPropDoubleType);
            if (isCanonicalXY(dt)) { p->v[0] = v.v[0] * rs.projW; p->v[1] = (1.0 - v.v[1]) * rs.projH; }
            else if (isNormalisedXY(dt)) { p->v[0] = v.v[0]; p->v[1] = 1.0 - v.v[1]; }
            else { p->v[0] = v.v[0]; p->v[1] = v.v[1]; }
        } else {
            for (int k = 0; k < p->dims(); ++k) p->v[k] = v.v[k];
        }
    }
    // Bottom-up copies (OFX origin is bottom left).
    const size_t row = size_t(a.width) * 4;
    rs.src.resize(row * a.height);
    rs.dst.assign(row * a.height, 0.f);
    for (int y = 0; y < a.height; ++y)
        std::memcpy(&rs.src[size_t(y) * row], a.src + size_t(a.height - 1 - y) * row, row * sizeof(float));
    inst->rs = &rs;

    OfxPropertySetStruct seqArgs;
    seqArgs.setD(kOfxImageEffectPropFrameRange, {a.time, a.time});
    seqArgs.setD(kOfxImageEffectPropFrameStep, {1.0});
    seqArgs.setI(kOfxPropIsInteractive, {0});
    seqArgs.setD(kOfxImageEffectPropRenderScale, {a.renderScale, a.renderScale});
    seqArgs.setI(kOfxImageEffectPropSequentialRenderStatus, {0});
    seqArgs.setI(kOfxImageEffectPropInteractiveRenderStatus, {0});
    call(pl, kOfxImageEffectActionBeginSequenceRender, inst, &seqArgs);

    OfxPropertySetStruct args;
    args.setD(kOfxPropTime, {a.time});
    args.setS(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
    args.setI(kOfxImageEffectPropRenderWindow, {0, 0, a.width, a.height});
    args.setD(kOfxImageEffectPropRenderScale, {a.renderScale, a.renderScale});
    args.setI(kOfxImageEffectPropSequentialRenderStatus, {0});
    args.setI(kOfxImageEffectPropInteractiveRenderStatus, {0});
    args.setI(kOfxImageEffectPropRenderQualityDraft, {0});
    const OfxStatus st = call(pl, kOfxImageEffectActionRender, inst, &args);

    call(pl, kOfxImageEffectActionEndSequenceRender, inst, &seqArgs);
    inst->rs = nullptr;
    if (st != kOfxStatOK) {
        if (error) *error = QStringLiteral("%1: render failed (%2)").arg(pl.id).arg(st);
        return false;
    }
    for (int y = 0; y < a.height; ++y)
        std::memcpy(a.dst + size_t(a.height - 1 - y) * row, &rs.dst[size_t(y) * row], row * sizeof(float));
    return true;
}

} // namespace rc::ofx
