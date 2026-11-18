// Image (data-model.md, "Image"): one source picture of a project and its metadata. The pixels
// are in separate archive entries (source.<ext>, target.png, thumb.png).
#ifndef LOOMORA_FORMAT_MODEL_IMAGE_HPP
#define LOOMORA_FORMAT_MODEL_IMAGE_HPP

#include <cstdint>
#include <string>

#include "format/model/common.hpp"
#include "format/model/project.hpp"

namespace loomora {
namespace format {
namespace model {

enum ColorMode { kColorBw = 0, kColorGray = 1, kColorColor = 2 };
enum ImageShape { kShapeCircle = 0, kShapeLandscape = 1, kShapePortrait = 2, kShapeSquare = 3, kShapeFree = 4 };

const char *to_string(ColorMode m);
const char *to_string(ImageShape s);
bool parse_color_mode(const std::string &text, ColorMode &out);
bool parse_image_shape(const std::string &text, ImageShape &out);

// JSON key order: id, name, color_mode, shape, width_px, height_px, source, target, thumbnail.
struct Image {
    std::uint32_t id = 0;
    std::string name;                 // up to 256 bytes
    ColorMode color_mode = kColorColor;
    ImageShape shape = kShapeFree;
    std::uint64_t width_px = 0;       // >= 1, of the source image
    std::uint64_t height_px = 0;      // >= 1
    bool has_source = false;          // full profile only: {entry, format png|jpeg|webp, size}
    EntryRef source;
    bool has_target = false;          // full profile only, optional: the processed image (png)
    EntryRef target;
    EntryRef thumbnail;               // both profiles: {entry} (png within 256 x 256 px and 64 KiB)
};

// `profile` light: no source and no target.
void check_image(const Image &image, Profile profile, const std::string &entry, Report &report);

} // namespace model
} // namespace format
} // namespace loomora

#endif
