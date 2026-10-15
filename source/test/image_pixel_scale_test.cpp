#include "StarAssets.hpp"
#include "StarDrawable.hpp"
#include "StarDrawablePainter.hpp"
#include "StarFile.hpp"
#include "StarImage.hpp"
#include "StarImageMetadataDatabase.hpp"
#include "StarImageProcessing.hpp"
#include "StarImageScaling.hpp"
#include "StarRoot.hpp"

#include "gtest/gtest.h"

using namespace Star;

namespace {

class TestTexture : public Texture {
public:
  explicit TestTexture(Vec2U size) : m_size(size) {}
  Vec2U size() const override { return m_size; }
  TextureFiltering filtering() const override { return TextureFiltering::Nearest; }
  TextureAddressing addressing() const override { return TextureAddressing::Clamp; }
private:
  Vec2U m_size;
};

class TestTextureGroup : public TextureGroup {
public:
  TextureFiltering filtering() const override { return TextureFiltering::Nearest; }
  TexturePtr create(Image const& image) override { return make_ref<TestTexture>(image.size()); }
};

// Capture the production painter's submitted vertices/UVs without a GL context.
class TestRenderer : public Renderer {
public:
  String rendererId() const override { return "test"; }
  Vec2U screenSize() const override { return {800, 600}; }
  void loadConfig(Json const&) override {}
  void loadEffectConfig(String const&, Json const&, StringMap<String> const&) override {}
  void setEffectParameter(String const&, RenderEffectParameter const&) override {}
  void setEffectScriptableParameter(String const&, String const&, RenderEffectParameter const&) override {}
  Maybe<RenderEffectParameter> getEffectScriptableParameter(String const&, String const&) override { return {}; }
  Maybe<VariantTypeIndex> getEffectScriptableParameterType(String const&, String const&) override { return {}; }
  void setEffectTexture(String const&, ImageView const&) override {}
  bool switchEffectConfig(String const&) override { return true; }
  void setScissorRect(Maybe<RectI> const&) override {}
  TexturePtr createTexture(Image const& image, TextureAddressing, TextureFiltering) override { return make_ref<TestTexture>(image.size()); }
  void setSizeLimitEnabled(bool) override {}
  void setMultiTexturingEnabled(bool) override {}
  void setMultiSampling(unsigned) override {}
  TextureGroupPtr createTextureGroup(TextureGroupSize, TextureFiltering) override { return make_shared<TestTextureGroup>(); }
  RenderBufferPtr createRenderBuffer() override { return {}; }
  List<RenderPrimitive>& immediatePrimitives() override { return primitives; }
  void render(RenderPrimitive primitive) override { primitives.append(std::move(primitive)); }
  void renderBuffer(RenderBufferPtr const&, Mat3F const&) override {}
  void flush(Mat3F const&) override {}
  List<RenderPrimitive> primitives;
};

class ImagePixelScaleTest : public testing::Test {
protected:
  void SetUp() override {
    directory = File::temporaryDirectory();
    source = File::relativeTo(directory, "assets");
    File::makeDirectory(source);
    Root::Settings settings{};
    settings.assetSources = {source};
    settings.storageDirectory = File::relativeTo(directory, "storage");
    settings.defaultConfiguration = JsonObject();
    settings.assetsSettings.assetTimeToLive = 60.0f;
    settings.assetsSettings.audioDecompressLimit = 0.0f;
    settings.assetsSettings.workerPoolSize = 0;
    settings.assetsSettings.skipDigest = true;
    settings.assetsSettings.skipPreload = true;
    settings.logLevel = LogLevel::Error;
    root = make_unique<Root>(std::move(settings));
    for (unsigned scale : {1u, 2u, 4u}) {
      auto name = "sprite" + toString(scale);
      writeImage(name, Image::filled(Vec2U::filled(8 * scale), {240, 80, 40, 255}));
      writeFrames(name, scale, Vec2U::filled(8 * scale));
    }
    writeImage("legacy", Image::filled({8, 8}, {240, 80, 40, 255}));
  }

  void TearDown() override {
    root.reset();
    File::removeDirectoryRecursive(directory);
  }

  void writeImage(String const& name, Image const& image) {
    image.writePng(File::open(File::relativeTo(source, name + ".png"), IOMode::Write));
  }

  void writeFrames(String const& name, unsigned scale, Vec2U size) {
    Json frames = JsonObject{
      {"pixelScale", scale},
      {"frameGrid", JsonObject{{"size", JsonArray{size[0], size[1]}},
          {"dimensions", JsonArray{1, 1}}, {"names", JsonArray{Json(JsonArray{"idle"})}}}},
      {"aliases", JsonObject{{"default", "idle"}}}
    };
    File::writeFile(frames.repr(), File::relativeTo(source, name + ".frames"));
  }

  String directory, source;
  unique_ptr<Root> root;
};

TEST_F(ImagePixelScaleTest, FramesUsePhysicalCoordinatesAndAliasesKeepDensity) {
  for (unsigned scale : {1u, 2u, 4u}) {
    String path = "/sprite" + toString(scale) + ".png";
    auto frames = root->assets()->imageFrames(path);
    EXPECT_EQ(frames->pixelScale, scale);
    EXPECT_EQ(frames->getRect("default")->size(), Vec2U::filled(8 * scale));
    EXPECT_EQ(root->assets()->image(path + ":default")->size(), Vec2U::filled(8 * scale));
    EXPECT_EQ(root->imageMetadataDatabase()->logicalImageSize(path + ":default"), Vec2F::filled(8));
    EXPECT_EQ(frames->toJson().contains("pixelScale"), scale != 1);
  }
}

TEST_F(ImagePixelScaleTest, MissingDensityAndFilenameSuffixKeepLegacySize) {
  writeImage("unmarked@2x", Image::filled({16, 16}, {255, 255, 255, 255}));
  auto metadata = root->imageMetadataDatabase();
  EXPECT_EQ(metadata->imagePixelScale("/legacy.png"), 1u);
  EXPECT_EQ(metadata->logicalImageSize("/legacy.png"), Vec2F(8, 8));
  // The new file must be scanned, but no filename convention opts it in.
  root->reload();
  EXPECT_EQ(root->imageMetadataDatabase()->imagePixelScale("/unmarked@2x.png"), 1u);
  EXPECT_EQ(root->imageMetadataDatabase()->logicalImageSize("/unmarked@2x.png"), Vec2F(16, 16));
}

TEST_F(ImagePixelScaleTest, RejectsUnsupportedDensity) {
  StringList paths;
  for (double value : {0.0, -1.0, 3.0, 2.5, 8.0}) {
    String name = "bad" + toString(paths.size());
    writeImage(name, Image::filled({8, 8}, {255, 255, 255, 255}));
    File::writeFile(Json(JsonObject{{"pixelScale", value}}).repr(), File::relativeTo(source, name + ".frames"));
    paths.append("/" + name + ".png");
  }
  for (auto const& path : paths)
    EXPECT_THROW(root->assets()->imageFrames(path), AssetException);
}

TEST_F(ImagePixelScaleTest, WorldAndPortraitBoundsStayIdentical) {
  for (float pixelSize : {1.0f, 1.0f / 8.0f}) {
    Drawable legacy = Drawable::makeImage("/legacy.png", pixelSize, true, {3, 5});
    for (unsigned scale : {1u, 2u, 4u}) {
      String image = "/sprite" + toString(scale) + ".png:idle";
      auto hd = Drawable::makeImage(image, pixelSize, true, {3, 5});
      EXPECT_EQ(hd.boundBox(false), legacy.boundBox(false));
      EXPECT_EQ(hd.boundBox(true), legacy.boundBox(true));
      hd.rotate(0.7f, {0.25f, -1.0f});
      auto rotated = legacy;
      rotated.rotate(0.7f, {0.25f, -1.0f});
      EXPECT_EQ(hd.boundBox(false), rotated.boundBox(false));
      Drawable roundTrip(hd.toJson());
      EXPECT_EQ(roundTrip.boundBox(false), hd.boundBox(false));
    }
  }
}

TEST_F(ImagePixelScaleTest, PainterKeepsGeometryButUsesEveryTexturePixel) {
  auto renderer = make_shared<TestRenderer>();
  auto textures = make_shared<AssetTextureGroup>(make_shared<TestTextureGroup>());
  DrawablePainter painter(renderer, textures);
  for (unsigned scale : {1u, 2u, 4u}) {
    String path = "/sprite" + toString(scale) + ".png:idle";
    auto drawable = Drawable::makeImage(path, 1.0f / 8.0f, true, {});
    painter.drawDrawable(drawable, 3.0f, {100, 200}, 3.0f);
    auto const& quad = renderer->primitives.last().get<RenderQuad>();
    EXPECT_EQ(quad.a.screenCoordinate, Vec2F(98.5f, 198.5f));
    EXPECT_EQ(quad.c.screenCoordinate, Vec2F(101.5f, 201.5f));
    EXPECT_EQ(quad.c.textureCoordinate, Vec2F::filled(8 * scale));
    unsigned cachedScale = 99;
    EXPECT_TRUE((bool)textures->loadTexture(path, &cachedScale));
    EXPECT_EQ(cachedScale, scale);
  }
}

TEST_F(ImagePixelScaleTest, LegacyManualScaleAndMatrixRoundTripKeepTheirMeaning) {
  Drawable legacy(JsonObject{{"image", "/legacy.png"}, {"scale", 0.5f}});
  EXPECT_EQ(legacy.boundBox(false), RectF(-2, -2, 2, 2));
  Drawable explicitMatrix(legacy.toJson());
  EXPECT_EQ(explicitMatrix.boundBox(false), legacy.boundBox(false));
}

TEST_F(ImagePixelScaleTest, MixedDensityMasksStayAlignedInBothDirections) {
  for (unsigned maskScale : {1u, 2u, 4u}) {
    Image mask = Image::filled(Vec2U::filled(8 * maskScale), {255, 255, 255, 0});
    for (unsigned y = 0; y < 8 * maskScale; ++y)
      for (unsigned x = 0; x < 4 * maskScale; ++x)
        mask.set(Vec2U(x, y), Vec4B(255, 255, 255, 255));
    auto name = "mask" + toString(maskScale);
    writeImage(name, mask);
    writeFrames(name, maskScale, Vec2U::filled(8 * maskScale));
  }
  for (unsigned scale : {1u, 2u, 4u}) {
    for (unsigned maskScale : {1u, 2u, 4u}) {
      String path = "/sprite" + toString(scale) + ".png:default?addmask=/mask" + toString(maskScale) + ".png;0;0";
      auto image = root->assets()->image(path);
      EXPECT_EQ(image->size(), Vec2U::filled(8 * scale));
      for (unsigned x = 0; x < 8 * scale; ++x)
        EXPECT_EQ(image->get(x, 0)[3], x < 4 * scale ? 255 : 0);
      auto drawable = Drawable::makeImage(path, 1.0f, true, {});
      EXPECT_EQ(drawable.boundBox(true), RectF(-4, -4, 0, 4));
    }
  }
}

TEST_F(ImagePixelScaleTest, LogicalCropAndBorderMatchRealImageSize) {
  for (unsigned scale : {1u, 2u, 4u}) {
    String base = "/sprite" + toString(scale) + ".png:idle";
    auto cropped = base + "?crop=1;2;5;6";
    auto bordered = base + "?border=1;ffffffff;ffffffff";
    for (auto path : {cropped, bordered}) {
      auto actual = root->assets()->image(path);
      EXPECT_EQ(root->imageMetadataDatabase()->imageSize(path), actual->size());
    }
    EXPECT_EQ(root->imageMetadataDatabase()->logicalImageSize(cropped), Vec2F(4, 4));
    EXPECT_EQ(root->imageMetadataDatabase()->logicalImageSize(bordered), Vec2F(10, 10));
    Drawable drawable = Drawable::makeImage(base, 1.0f, true, {10, 20});
    drawable.imagePart().addDirectives("border=1;ffffffff;ffffffff", true);
    EXPECT_EQ(drawable.boundBox(false).center(), Vec2F(10, 20));
  }
}

TEST_F(ImagePixelScaleTest, MixedDensityMaskOffsetsUseLogicalPixels) {
  for (unsigned scale : {1u, 2u, 4u}) {
    for (unsigned maskScale : {1u, 2u, 4u}) {
      String path = "/sprite" + toString(scale) + ".png:idle?addmask=/sprite" + toString(maskScale) + ".png;1;0";
      auto image = root->assets()->image(path);
      for (unsigned x = 0; x < 8 * scale; ++x)
        EXPECT_EQ(image->get(x, 0)[3], x < 7 * scale ? 255 : 0);
      EXPECT_EQ(Drawable::makeImage(path, 1.0f, true, {}).boundBox(true), RectF(-4, -4, 3, 4));
    }
  }
}

TEST_F(ImagePixelScaleTest, ImageSpacesRetainWorldFootprint) {
  for (unsigned scale : {1u, 2u, 4u}) {
    String path = "/sprite" + toString(scale) + ".png:idle";
    EXPECT_EQ(root->imageMetadataDatabase()->imageSpaces(path, {}, 0.5f, false), List<Vec2I>({{0, 0}}));
    EXPECT_EQ(root->imageMetadataDatabase()->imageSpaces(path, {8, 0}, 0.5f, true), List<Vec2I>({{1, 0}}));
  }
}

TEST_F(ImagePixelScaleTest, RepeatedMaskReferencesAreResampledOnlyOnce) {
  for (unsigned scale : {1u, 2u, 4u}) {
    String path = "/sprite" + toString(scale) + ".png:idle?addmask=/legacy.png;1;0?addmask=/legacy.png;0;1";
    auto drawable = Drawable::makeImage(path, 1.0f, true, {});
    EXPECT_EQ(drawable.boundBox(true), RectF(-4, -4, 3, 3));
  }
}

TEST_F(ImagePixelScaleTest, MixedDensityBlendsMatchLegacyLogicalOffsets) {
  auto expected = root->assets()->image("/legacy.png?blendmult=/legacy.png;1;0");
  for (unsigned scale : {1u, 2u, 4u}) {
    for (unsigned referenceScale : {1u, 2u, 4u}) {
      String path = "/sprite" + toString(scale) + ".png:idle?blendmult=/sprite" + toString(referenceScale) + ".png;1;0";
      auto image = root->assets()->image(path);
      for (unsigned x = 0; x < 8 * scale; ++x)
        EXPECT_EQ(image->get(x, 0), expected->get(x / scale, 0));
    }
  }
}

TEST_F(ImagePixelScaleTest, MetadataDensityIsInvalidatedByHotReload) {
  auto metadata = root->imageMetadataDatabase();
  EXPECT_EQ(metadata->logicalImageSize("/sprite2.png:idle"), Vec2F(8, 8));
  writeFrames("sprite2", 1, {16, 16});
  root->hotReload();
  EXPECT_EQ(metadata->imagePixelScale("/sprite2.png:idle"), 1u);
  EXPECT_EQ(metadata->logicalImageSize("/sprite2.png:idle"), Vec2F(16, 16));
}

}
