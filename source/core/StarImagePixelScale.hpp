#pragma once

#include <cstdint>

namespace Star {

// Opt-in image density. A logical pixel keeps the retail coordinate system;
// physical pixels are the actual texels in the PNG and frame rectangles.
inline bool validImagePixelScale(double scale) {
  return scale == 1.0 || scale == 2.0 || scale == 4.0;
}

inline float logicalImagePixels(float physicalPixels, unsigned pixelScale) {
  return pixelScale == 1 ? physicalPixels : physicalPixels / pixelScale;
}

// Nearest neighbour at logical pixel centres, for masks/blends of different
// densities. In particular, 1x -> 2x repeats each pixel exactly twice.
inline unsigned imageReferencePixel(unsigned pixel, unsigned referenceScale, unsigned targetScale) {
  return (uint64_t(pixel) * 2 + 1) * referenceScale / (uint64_t(targetScale) * 2);
}

}
