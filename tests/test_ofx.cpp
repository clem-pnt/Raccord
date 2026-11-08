#include <catch2/catch.hpp>
#include "ofx/OfxHost.h"
#include <cmath>
#include <vector>

using namespace rc;

TEST_CASE("OpenFX host loads, describes and renders a plugin")
{
    auto &host = ofx::Host::instance();
    host.loadPlugins({QStringLiteral(RACCORD_OFX_TEST_DIR)});
    INFO(host.errors().join("\n").toStdString());
    REQUIRE(host.loadedPlugins().contains("fr.raccord.test.Invert"));
    const EffectDef *d = EffectRegistry::instance().find("ofx.fr.raccord.test.Invert");
    REQUIRE(d);
    CHECK(d->name == "Raccord Test Invert");
    CHECK(d->category == "OpenFX/Raccord Tests");
    REQUIRE(d->params.size() == 3);   // the secret parameter is hidden
    CHECK(d->param("amount")->type == ParamType::Float);
    CHECK(d->param("amount")->def.f() == 1.0);
    CHECK(d->param("tint")->type == ParamType::Color);
    CHECK(d->param("mode")->type == ParamType::Enum);
    CHECK(d->param("mode")->options.size() == 2);

    // 3x2 image, top-down rows; pixel (x, y) = (x/4, y/2, 0.25, 1).
    const int w = 3, h = 2;
    std::vector<float> src(w * h * 4), dst(w * h * 4, -1.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = &src[(y * w + x) * 4];
            p[0] = x / 4.f; p[1] = y / 2.f; p[2] = 0.25f; p[3] = 1.f;
        }
    EffectInstance e = EffectInstance::create("ofx.fr.raccord.test.Invert");
    e.uid = "u1";
    ofx::RenderArgs a;
    a.src = src.data();
    a.dst = dst.data();
    a.width = w;
    a.height = h;
    QString err;
    REQUIRE(host.render(e, 0, e.uid, a, &err));
    // Full inversion, row order preserved (bottom-up conversion is internal).
    for (int i = 0; i < w * h; ++i) {
        CHECK(std::abs(dst[i * 4 + 0] - (1.f - src[i * 4 + 0])) < 1e-6);
        CHECK(std::abs(dst[i * 4 + 1] - (1.f - src[i * 4 + 1])) < 1e-6);
        CHECK(dst[i * 4 + 3] == 1.f);
    }
    // Parameters from the effect instance reach the plugin.
    e.param("amount").setStaticValue(Value(0.0));
    e.param("tint").setStaticValue(Value(0.5, 1.0, 1.0, 1.0));
    e.param("mode").setStaticValue(Value(1.0));
    REQUIRE(host.render(e, 0, e.uid, a, &err));
    // amount 0, red halved then swapped with blue: out.r = src.b, out.b = src.r * 0.5.
    for (int i = 0; i < w * h; ++i) {
        CHECK(std::abs(dst[i * 4 + 0] - src[i * 4 + 2]) < 1e-6);
        CHECK(std::abs(dst[i * 4 + 2] - src[i * 4 + 0] * 0.5f) < 1e-6);
        CHECK(std::abs(dst[i * 4 + 1] - src[i * 4 + 1]) < 1e-6);
    }
}
