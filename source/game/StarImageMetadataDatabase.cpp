#include "StarImageMetadataDatabase.hpp"
#include "StarFile.hpp"
#include "StarImage.hpp"
#include "StarImageProcessing.hpp"
#include "StarLogging.hpp"
#include "StarEncode.hpp"
#include "StarGameTypes.hpp"
#include "StarRoot.hpp"
#include "StarAssets.hpp"
#include "StarImagePixelScale.hpp"

namespace Star {

ImageMetadataDatabase::ImageMetadataDatabase() {
  MutexLocker locker(m_mutex);
  int timeSmear = 2000;
  int64_t timeToLive = 60000;
  m_sizeCache.setTimeSmear(timeSmear);
  m_spacesCache.setTimeSmear(timeSmear);
  m_regionCache.setTimeSmear(timeSmear);
  m_pixelScaleCache.setTimeSmear(timeSmear);
  m_sizeCache.setTimeToLive(timeToLive);
  m_spacesCache.setTimeToLive(timeToLive);
  m_regionCache.setTimeToLive(timeToLive);
  m_pixelScaleCache.setTimeToLive(timeToLive);
  m_reloadTracker = make_shared<TrackerListener>();
  Root::singleton().registerReloadListener(m_reloadTracker);
}

Vec2U ImageMetadataDatabase::imageSize(AssetPath const& path) const {
  checkReload();
  MutexLocker locker(m_mutex);
  if (auto cached = m_sizeCache.ptr(path))
    return *cached;

  locker.unlock();
  Vec2U size = calculateImageSize(path);

  locker.lock();
  m_sizeCache.set(path, size);
  return size;
}

unsigned ImageMetadataDatabase::imagePixelScale(AssetPath const& path) const {
  checkReload();
  MutexLocker locker(m_mutex);
  if (auto cached = m_pixelScaleCache.ptr(path.basePath))
    return *cached;
  locker.unlock();
  auto frames = Root::singleton().assets()->imageFrames(path.basePath);
  unsigned scale = frames ? frames->pixelScale : 1;
  locker.lock();
  m_pixelScaleCache.set(path.basePath, scale);
  return scale;
}

Vec2F ImageMetadataDatabase::logicalImageSize(AssetPath const& path) const {
  Vec2F size(imageSize(path));
  unsigned scale = imagePixelScale(path);
  size[0] = logicalImagePixels(size[0], scale);
  size[1] = logicalImagePixels(size[1], scale);
  return size;
}

void ImageMetadataDatabase::checkReload() const {
  if (m_reloadTracker->pullTriggered()) {
    MutexLocker locker(m_mutex);
    m_sizeCache.clear();
    m_pixelScaleCache.clear();
    m_spacesCache.clear();
    m_regionCache.clear();
  }
}

List<Vec2I> ImageMetadataDatabase::imageSpaces(AssetPath const& path, Vec2F position, float fillLimit, bool flip) const {
  checkReload();
  SpacesEntry key = make_tuple(path, Vec2I::round(position), fillLimit, flip);

  MutexLocker locker(m_mutex);
  if (auto cached = m_spacesCache.ptr(key)) {
    return *cached;
  }

  auto filteredPath = filterProcessing(path);
  SpacesEntry filteredKey = make_tuple(filteredPath, Vec2I::round(position), fillLimit, flip);

  if (auto cached = m_spacesCache.ptr(filteredKey)) {
    m_spacesCache.set(key, *cached);
    return *cached;
  }

  locker.unlock();

  auto image = Root::singleton().assets()->image(filteredPath);
  int imageWidth = image->width();
  int imageHeight = image->height();
  unsigned pixelScale = imagePixelScale(filteredPath);
  int tilePixels = (int)TilePixels * pixelScale;

  Vec2I min((position / TilePixels).floor());
  Vec2F logicalSize(logicalImagePixels(imageWidth, pixelScale), logicalImagePixels(imageHeight, pixelScale));
  Vec2I max(((logicalSize + position) / TilePixels).ceil());

  List<Vec2I> spaces;

  for (int yspace = min[1]; yspace < max[1]; ++yspace) {
    for (int xspace = min[0]; xspace < max[0]; ++xspace) {
      float fillRatio = 0.0f;

      for (int y = 0; y < tilePixels; ++y) {
        int ypixel = round(yspace * tilePixels + y - position[1] * pixelScale);
        if (ypixel < 0 || ypixel >= imageHeight)
          continue;

        for (int x = 0; x < tilePixels; ++x) {
          int xpixel = round(xspace * tilePixels + x - position[0] * pixelScale);
          if (flip)
            xpixel = imageWidth - 1 - xpixel;

          if (xpixel < 0 || xpixel >= imageWidth)
            continue;

          if (image->get(xpixel, ypixel)[3] > 0)
            fillRatio += 1.0f / square((float)tilePixels);
        }
      }

      if (fillRatio >= fillLimit)
        spaces.append(Vec2I(xspace, yspace));
    }
  }

  locker.lock();
  m_spacesCache.set(key, spaces);
  m_spacesCache.set(filteredKey, spaces);

  return spaces;
}

RectU ImageMetadataDatabase::nonEmptyRegion(AssetPath const& path) const {
  checkReload();
  MutexLocker locker(m_mutex);
  
  if (auto cached = m_regionCache.ptr(path)) {
    return *cached;
  }

  auto filteredPath = filterProcessing(path);
  if (auto cached = m_regionCache.ptr(filteredPath)) {
    m_regionCache.set(path, *cached);
    return *cached;
  }

  locker.unlock();
  auto image = Root::singleton().assets()->image(filteredPath);
  RectU region = RectU::null();
  image->forEachPixel([&region](unsigned x, unsigned y, Vec4B const& pixel) {
    if (pixel[3] > 0)
      region.combine(RectU::withSize({x, y}, {1, 1}));
  });

  locker.lock();
  m_regionCache.set(path, region);
  m_regionCache.set(filteredPath, region);

  return region;
}

void ImageMetadataDatabase::cleanup() const {
  MutexLocker locker(m_mutex);

  m_sizeCache.cleanup();
  m_pixelScaleCache.cleanup();
  m_spacesCache.cleanup();
  m_regionCache.cleanup();
}

AssetPath ImageMetadataDatabase::filterProcessing(AssetPath const& path) {
  AssetPath newPath = { path.basePath, path.subPath, {} };

  String filtered;
  for (auto& directives : path.directives.list())
    directives.loadOperations();
  path.directives.forEach([&](auto const& entry, Directives const& directives) {
    ImageOperation const& operation = entry.operation;
    if (!(operation.is<HueShiftImageOperation>()           ||
          operation.is<SaturationShiftImageOperation>()    ||
          operation.is<BrightnessMultiplyImageOperation>() ||
          operation.is<FadeToColorImageOperation>()        ||
          operation.is<ScanLinesImageOperation>()          ||
          operation.is<SetColorImageOperation>())) {
      filtered += "?";
      filtered += entry.string(*directives);
    }
    });

  newPath.directives = std::move(filtered);
  return newPath;
}

Vec2U ImageMetadataDatabase::calculateImageSize(AssetPath const& path) const {
  // Carefully calculate an image's size while trying not to actually load it.
  // In error cases, this will fall back to calling Assets::image, so that image
  // can possibly produce a missing image asset or properly report the error.

  auto assets = Root::singleton().assets();

  auto fallback = [&assets, &path]() {
    return assets->image(path)->size();
  };

  if (!assets->assetExists(path.basePath)) {
    return fallback();
  }

  Vec2U imageSize;
  if (path.subPath) {
    auto frames = assets->imageFrames(path.basePath);
    if (!frames)
      return fallback();

    if (auto rect = frames->getRect(*path.subPath))
      imageSize = rect->size();
    else
      return fallback();
  } else {
    // We ensure that the base image size is cached even when given directives,
    // so we don't have to call Image::readPngMetadata on the same file more
    // than once.
    MutexLocker locker(m_mutex);
    if (auto size = m_sizeCache.ptr(path.basePath)) {
      imageSize = *size;
    } else {
      locker.unlock();
      auto file = assets->openFile(path.basePath);
      if (Image::isPng(file))
        imageSize = get<0>(Image::readPngMetadata(file));
      else
        imageSize = fallback();
      locker.lock();
      m_sizeCache.set(path.basePath, imageSize);
    }
  }

  struct OperationSizeAdjust {
    Vec2U& imageSize;
    bool hasError;

    OperationSizeAdjust(Vec2U& size) : imageSize(size), hasError(false) {};

    void operator()(NullImageOperation const&) {}

    void operator()(ErrorImageOperation const&) {}

    void operator()(HueShiftImageOperation const&) {}

    void operator()(SaturationShiftImageOperation const&) {}

    void operator()(BrightnessMultiplyImageOperation const&) {}

    void operator()(FadeToColorImageOperation const&) {}

    void operator()(ScanLinesImageOperation const&) {}

    void operator()(SetColorImageOperation const&) {}

    void operator()(ColorReplaceImageOperation const&) {}

    void operator()(AlphaMaskImageOperation const&) {}

    void operator()(BlendImageOperation const&) {}

    void operator()(MultiplyImageOperation const&) {}

    void operator()(BorderImageOperation const& bio) {
      imageSize += Vec2U::filled(bio.pixels * 2);
    }

    void operator()(ScaleImageOperation const& sio) {
      imageSize = Vec2U::round(vmult(Vec2F(imageSize), sio.scale));
    }

    void operator()(CropImageOperation const& cio) {
      if (cio.subset.isEmpty() ||
          cio.subset.xMin() < 0 ||
          cio.subset.yMin() < 0 ||
          (unsigned)cio.subset.xMax() > imageSize[0] ||
          (unsigned)cio.subset.yMax() > imageSize[1]) {
        hasError = true;
      } else {
        imageSize = Vec2U(cio.subset.size());
      }
    }

    void operator()(FlipImageOperation const&) {}
  };

  OperationSizeAdjust osa(imageSize);
  unsigned pixelScale = imagePixelScale(path);

  for (auto& directives : path.directives.list())
    directives.loadOperations();

  bool complete = path.directives.forEachAbortable([&](auto const& entry, Directives const&) -> bool {
    if (pixelScale == 1)
      entry.operation.call(osa);
    else
      imageOperationForPixelScale(entry.operation, pixelScale).call(osa);
    return !osa.hasError;
  });

  if (!complete)
    return fallback();

  return imageSize;
}

}
