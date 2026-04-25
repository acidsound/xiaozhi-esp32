#include "lvgl_font.h"
#include <cbin_font.h>


LvglCBinFont::LvglCBinFont(void* data) {
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
}

LvglCBinFont::~LvglCBinFont() {
    if (font_ != nullptr) {
        cbin_font_delete(font_);
    }
}

void LvglCBinFont::SetFallback(std::shared_ptr<LvglFont> fallback) {
    fallback_ = fallback;
    if (font_ != nullptr) {
        font_->fallback = fallback_ ? fallback_->font() : nullptr;
    }
}
