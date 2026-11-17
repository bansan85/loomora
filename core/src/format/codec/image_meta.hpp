// Container-level handling of the images stored in a project (research R9): the format layer never
// decodes a pixel. It checks the signature and the structure of the container (PNG chunks, JPEG
// markers, WebP RIFF), strips the metadata (SC-009: no GPS, camera, Exif, XMP, IPTC or text left
// in any embedded image) by copying the pixel-bearing parts byte for byte, reads the size of a PNG
// from its IHDR, and checks a thumbnail against the limits (FR-011, FR-017).
//
// What is kept (everything else is dropped; the allow-list is deliberately strict):
//   PNG   every critical chunk (IHDR, PLTE, IDAT, IEND and any other upper-case first letter), and
//         the colour and pixel-interpretation chunks tRNS, gAMA, cHRM, sRGB, iCCP, sBIT, cICP,
//         mDCV, cLLI, pHYs, bKGD, hIST, sPLT and the APNG chunks acTL, fcTL, fdAT. Dropped: eXIf,
//         tEXt, zTXt, iTXt (XMP, GPS and any text), tIME, and every other ancillary chunk.
//   JPEG  SOI, EOI, every non-APPn, non-COM marker segment with its entropy-coded data (DQT, DHT,
//         SOFn, SOS and the scan, DRI, RSTn...), APP0 "JFIF", APP2 "ICC_PROFILE", APP14 "Adobe".
//         Dropped: APP1 (Exif, XMP), APP13 (Photoshop, IPTC), COM, and every other APPn (APP2 that
//         is not ICC, such as the multi-picture MPF; APP3 to APP12, APP15; JFXX).
//   WebP  VP8, VP8L, VP8X, ALPH, ANIM, ANMF, ICCP chunks. Dropped: EXIF, XMP, and any other chunk.
//         The RIFF size is rewritten and the Exif and XMP flags of VP8X are cleared (the only bytes
//         that change).
// Bytes after the end of the image (after PNG IEND, JPEG EOI, the RIFF size) are dropped too.
// Stripping an image that has no metadata gives the same bytes back.
//
// Every function throws FormatError(LOOMORA_ERR_INVALID_ITEM) when the bytes are not a well-formed
// image of the expected container (the message says where); for stripping, that is the signature
// of an image the writer must not store. No exception other than FormatError / std::bad_alloc.
#ifndef LOOMORA_FORMAT_CODEC_IMAGE_META_HPP
#define LOOMORA_FORMAT_CODEC_IMAGE_META_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace loomora {
namespace format {

enum ImageFormat { kImageUnknown = 0, kImagePng = 1, kImageJpeg = 2, kImageWebp = 3 };

// The format whose signature starts `data` (PNG 89 50 4E 47 0D 0A 1A 0A, JPEG FF D8 FF, WebP
// "RIFF"....."WEBP"), kImageUnknown otherwise. Looks at the first bytes only.
ImageFormat detect_image_format(const std::uint8_t *data, std::size_t len);

// "png", "jpeg", "webp" (the `format` values of image.json), "" for kImageUnknown.
const char *image_format_name(ImageFormat format);

// Returns `data` without metadata (see above), structure fully checked (PNG chunk CRCs, JPEG segment
// lengths and EOI, WebP chunk sizes). `format` kImageUnknown detects it from the signature; a
// given `format` that does not match the signature is an error (the image.json `format` must be
// true). `*removed_bytes`, when not null, receives the number of bytes dropped.
std::vector<std::uint8_t> strip_image_metadata(const std::uint8_t *data, std::size_t len,
                                               ImageFormat format = kImageUnknown,
                                               std::size_t *removed_bytes = nullptr);

struct PngHeader {
    std::uint32_t width;
    std::uint32_t height;
    std::uint8_t bit_depth;
    std::uint8_t color_type;
    PngHeader() : width(0), height(0), bit_depth(0), color_type(0) {}
};

// Reads the IHDR of a PNG (signature, first chunk IHDR of length 13, its CRC, width and height
// between 1 and 2^31 - 1). Does not walk the other chunks: it is cheap, for thumbnails.
PngHeader read_png_header(const std::uint8_t *data, std::size_t len);

// True when `data` is a PNG whose IHDR width and height are at most limits().thumb_max_side (256)
// and whose size is at most limits().thumb_max_bytes (64 KiB). Never throws (false for anything
// that is not a readable PNG): the reader uses it to report warn.thumbnail_size.
bool thumbnail_within_limits(const std::uint8_t *data, std::size_t len);

// The write-side check: throws FormatError(LOOMORA_ERR_INVALID_ITEM) saying which limit is
// exceeded (a thumbnail wider or taller than the maximum side, larger than the maximum size, or
// not a PNG). It does not strip: call strip_image_metadata too.
void require_thumbnail(const std::uint8_t *data, std::size_t len);

} // namespace format
} // namespace loomora

#endif
