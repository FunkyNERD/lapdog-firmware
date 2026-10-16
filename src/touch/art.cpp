#if LAPDOG_TOUCH

#include "art.h"

#include "touch_art.h"

// RGB565A8: the RGB565 plane (2 bytes a pixel, so that's the stride), then the alpha plane.
#define ART_IMAGE(name, pixels, W, H)                                                                    \
    const lv_image_dsc_t name = {                                                                      \
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .flags = 0, .w = W, \
                   .h = H, .stride = (W) * 2, .reserved_2 = 0},                                         \
        .data_size = sizeof(pixels),                                                                     \
        .data = pixels,                                                                                  \
        .reserved = nullptr,                                                                           \
        .reserved_2 = nullptr,                                                                         \
    }

namespace art
{

ART_IMAGE(dog, touch_art::dog, touch_art::DOG_W, touch_art::DOG_H);
ART_IMAGE(wordmark, touch_art::wordmark, touch_art::WORDMARK_W, touch_art::WORDMARK_H);
ART_IMAGE(lockup, touch_art::lockup, touch_art::LOCKUP_W, touch_art::LOCKUP_H);

const float SCALE = touch_art::SCALE, ORIGIN_X = touch_art::ORIGIN_X, ORIGIN_Y = touch_art::ORIGIN_Y;

} // namespace art

#endif // LAPDOG_TOUCH
