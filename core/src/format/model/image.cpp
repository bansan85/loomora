#include "format/model/image.hpp"

namespace loomora {
namespace format {
namespace model {

namespace {
const char *const kColorNames[] = {"bw", "gray", "color"};
const char *const kShapeNames[] = {"circle", "landscape", "portrait", "square", "free"};
}

const char *to_string(ColorMode m) {
    const int i = static_cast<int>(m);
    return (i >= 0 && i < 3) ? kColorNames[i] : "";
}

const char *to_string(ImageShape s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < 5) ? kShapeNames[i] : "";
}

bool parse_color_mode(const std::string &text, ColorMode &out) {
    int i;
    if (!lookup_name(kColorNames, 3, text, i))
        return false;
    out = static_cast<ColorMode>(i);
    return true;
}

bool parse_image_shape(const std::string &text, ImageShape &out) {
    int i;
    if (!lookup_name(kShapeNames, 5, text, i))
        return false;
    out = static_cast<ImageShape>(i);
    return true;
}

void check_image(const Image &img, Profile profile, const std::string &entry, Report &report) {
    check_length(report, entry, kItemImage, img.id, "name", img.name, kMaxNameBytes);
    if (static_cast<int>(img.color_mode) < 0 || static_cast<int>(img.color_mode) > 2)
        add_value_error(report, entry, kItemImage, img.id, "color_mode must be bw, gray or color");
    if (static_cast<int>(img.shape) < 0 || static_cast<int>(img.shape) > 4)
        add_value_error(report, entry, kItemImage, img.id, "shape is not one of the known shapes");
    if (img.width_px < 1)
        add_value_error(report, entry, kItemImage, img.id, "width_px must be at least 1");
    if (img.height_px < 1)
        add_value_error(report, entry, kItemImage, img.id, "height_px must be at least 1");
    if (img.has_source)
        check_entry_ref(report, entry, kItemImage, img.id, "source", img.source);
    if (img.has_target) {
        check_entry_ref(report, entry, kItemImage, img.id, "target", img.target);
        if (!img.target.format.empty() && img.target.format != "png")
            add_value_error(report, entry, kItemImage, img.id, "target.format must be png");
    }
    check_entry_ref(report, entry, kItemImage, img.id, "thumbnail", img.thumbnail);
    if (profile == kProfileLight && (img.has_source || img.has_target))
        add_value_error(report, entry, kItemImage, img.id, "a light project keeps no source and no target");
}

} // namespace model
} // namespace format
} // namespace loomora
