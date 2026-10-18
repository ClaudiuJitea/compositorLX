// Synthetic HEVC fixtures, no camera files or external assets. Build with
// libheif-dev. Pass the output directory and, optionally, the path to an x265
// encoder plugin.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <libheif/heif.h>
#include <string>
void check(heif_error e) {
  if (e.code != heif_error_Ok) {
    std::fprintf(stderr, "%s\n", e.message);
    std::exit(1);
  }
}
int main(int argc, char **argv) {
  if (argc < 2)
    return 1;
  check(heif_init(nullptr));
  const heif_plugin_info *plugin = nullptr;
  if (argc > 2)
    check(heif_load_plugin(argv[2], &plugin));
  for (int bits : {8, 10}) {
    const int width = bits == 8 ? 64 : 512, height = 32;
    heif_context *context = heif_context_alloc();
    heif_encoder *encoder = nullptr;
    heif_image *image = nullptr;
    check(heif_context_get_encoder_for_format(context, heif_compression_HEVC,
                                              &encoder));
    check(heif_encoder_set_lossless(encoder, 1));
    check(heif_image_create(width, height, heif_colorspace_RGB,
                            bits == 8 ? heif_chroma_interleaved_RGB
                                      : heif_chroma_interleaved_RRGGBB_LE,
                            &image));
    check(heif_image_add_plane(image, heif_channel_interleaved, width, height,
                               bits));
    int stride = 0;
    uint8_t *pixels =
        heif_image_get_plane(image, heif_channel_interleaved, &stride);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        for (int c = 0; c < 3; ++c) {
          if (bits == 8)
            pixels[y * stride + x * 3 + c] = uint8_t(40 + x * 3);
          else {
            const uint16_t v = uint16_t(250 + x);
            pixels[y * stride + x * 6 + c * 2] = uint8_t(v);
            pixels[y * stride + x * 6 + c * 2 + 1] = uint8_t(v >> 8);
          }
        }
    heif_image_handle *handle = nullptr;
    check(heif_context_encode_image(context, image, encoder, nullptr, &handle));
    heif_image_handle_release(handle);
    const std::string path =
        std::string(argv[1]) +
        (bits == 8 ? "/generated-rgb8.heic" : "/generated-rgb10.hif");
    check(heif_context_write_to_file(context, path.c_str()));
    heif_image_release(image);
    heif_encoder_release(encoder);
    heif_context_free(context);
  }
}
