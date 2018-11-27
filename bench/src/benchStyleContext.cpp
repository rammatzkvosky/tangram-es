#include "benchmark/benchmark.h"

#include "scene/styleContext.h"
#include "data/tileData.h"

#define ITERATIONS ->Iterations(10)
//#define ITERATIONS

#define RUN(FIXTURE, NAME)                                              \
    BENCHMARK_DEFINE_F(FIXTURE, NAME)(benchmark::State& st) { while (st.KeepRunning()) { run(); } } \
    BENCHMARK_REGISTER_F(FIXTURE, NAME) ITERATIONS;

using namespace Tangram;

template<size_t jscontext>
class JSGetPropertyFixture : public benchmark::Fixture {
public:
    std::unique_ptr<StyleContext> ctx;
    Feature feature;
    void SetUp(const ::benchmark::State& state) override {
        ctx.reset(new StyleContext(jscontext));

        feature.props.set("message", "Hello World!");
        feature.props.set("message2", "Hello World!");
        ctx->setFeature(feature);
        ctx->setFunctions({
                R"(function () { return feature.message; })",
                R"(function () { return feature.message2; })"
            });
    }
    __attribute__ ((noinline)) void run() {
        StyleParam::Value value;
        benchmark::DoNotOptimize(value);
        ctx->evalStyle(0, StyleParamKey::text_source, value);
        // ctx->evalStyle(1, StyleParamKey::text_source, value);
        // ctx->evalFilter(0);
        // ctx->evalFilter(1);
     }
};

using DuktapeGetPropertyFixture = JSGetPropertyFixture<0>;
RUN(DuktapeGetPropertyFixture, DuktapeGetPropertyBench)

using JSCoreGetPropertyFixture = JSGetPropertyFixture<1>;
RUN(JSCoreGetPropertyFixture, JSCoreGetPropertyBench)

#if 0

const char scene_file[] = "bubble-wrap-style.zip";
const char tile_file[] = "res/tile.mvt";
std::shared_ptr<Scene> scene;
std::shared_ptr<TileSource> source;
std::shared_ptr<TileData> tileData;

void globalSetup() {
    static bool initialized = false;
    if (initialized) { return; }
    initialized = true;

    std::shared_ptr<MockPlatform> platform = std::make_shared<MockPlatform>();
    Url sceneUrl(scene_file);
    platform->putMockUrlContents(sceneUrl, MockPlatform::getBytesFromFile(scene_file));

    scene = std::make_shared<Scene>(platform, sceneUrl);
    Importer importer(scene);
    try {
        scene->config() = importer.applySceneImports(platform);
    }
    catch (const YAML::ParserException& e) {
        LOGE("Parsing scene config '%s'", e.what());
        exit(-1);
    }
    if (!scene->config()) {
        LOGE("Invalid scene file '%s'", scene_file);
        exit(-1);
    }
    SceneLoader::applyConfig(platform, scene);
    scene->fontContext()->loadFonts();
    for (auto& s : scene->tileSources()) {
        source = s;
        if (source->generateGeometry()) { break; }
    }

    Tile tile({0,0,10,10});
    auto task = source->createTask(tile.getID());
    auto& t = dynamic_cast<BinaryTileTask&>(*task);
    auto rawTileData = MockPlatform::getBytesFromFile(tile_file);
    t.rawTileData = std::make_shared<std::vector<char>>(rawTileData);
    tileData = source->parse(*task);
    if (!tileData) {
        LOGE("Invalid tile file '%s'", tile_file);
        exit(-1);
    }
}

template<size_t jscontext>
class FilterFunctionsFixture : public benchmark::Fixture {
public:
    std::unique_ptr<StyleContext> ctx;
    void SetUp(const ::benchmark::State& state) override {
        globalSetup();
        ctx.reset(new StyleContext(jscontext))
    }

    __attribute__ ((noinline)) void run() {
        for (auto& layer : tileData->layers()) {
            for (auto& feature : layer) {
                for (int i = 0; i < scene->functions().size(); i++) {
                    ctx->
                }
            }
        }
        result = tileBuilder->build({0,0,10,10}, *tileData, *source);
    }
};

using DUKFilterFunctionsFixture = FilterFunctionsFixture<0>;
using JSCFilterFunctionsFixture = FilterFunctionsFixture<1>;

RUN(DUKFilterFunctionsFixture, DUKFilterFunctionsBench)
RUN(JSCFilterFunctionsFixture, JSCFilterFunctionsBench)

#endif


class DirectGetPropertyFixture : public benchmark::Fixture {
public:
    Feature feature;
    void SetUp(const ::benchmark::State& state) override {
        feature.props.set("message", "Hello World!");
    }
    void TearDown(const ::benchmark::State& state) override {}
};
BENCHMARK_DEFINE_F(DirectGetPropertyFixture, DirectGetPropertyBench)(benchmark::State& st) {
    StyleParam::Value value;

    while (st.KeepRunning()) {
        const auto v = feature.props.get("message");
        if (v.is<std::string>()) {
            value = v.get<std::string>();
        } else {
            printf("eerrr\n");
        }
    }
}
BENCHMARK_REGISTER_F(DirectGetPropertyFixture, DirectGetPropertyBench) ITERATIONS;

BENCHMARK_MAIN();
