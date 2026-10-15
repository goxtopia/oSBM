#pragma once

namespace Star {

STAR_CLASS(Image);
Image scaleNearest(Image const& srcImage, Vec2F const& scale);
// Resample between declared image densities without changing logical size.
Image rescaleImagePixels(Image const& image, unsigned sourceScale, unsigned targetScale);
Image scaleBilinear(Image const& srcImage, Vec2F const& scale);
Image scaleBicubic(Image const& srcImage, Vec2F const& scale);

}
