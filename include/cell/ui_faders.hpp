// SPDX-License-Identifier: Apache-2.0
// Original image-backed fader cap rendering. No product skins or parameter policy.
#pragma once
#include <cell/ui.hpp>
#include <cmath>
#include <numbers>

namespace cell::ui::faders {
// Draw caller-owned immutable pixels in the existing thumb bounds. Horizontal
// travel rotates the artwork by a quarter turn; its center mark then remains
// perpendicular to the track. The source's aspect ratio is retained.
inline void drawCap(Canvas& canvas,Rect bounds,const Surface& image,
                    bool horizontal=false,bool enabled=true) {
    if(!std::isfinite(bounds.x)||!std::isfinite(bounds.y)||
       !std::isfinite(bounds.width)||!std::isfinite(bounds.height)||
       bounds.width<=0||bounds.height<=0||image.width()<=0||image.height()<=0)return;
    const double width=horizontal?bounds.height:bounds.width;
    const double height=horizontal?bounds.width:bounds.height;
    const double scale=std::min(width/image.width(),height/image.height());
    const double w=image.width()*scale,h=image.height()*scale;
    canvas.save();
    if(!enabled)canvas.opacity(.38);
    canvas.translate(bounds.x+bounds.width/2,bounds.y+bounds.height/2);
    if(horizontal)canvas.rotate(-std::numbers::pi/2);
    canvas.image(image,{-w/2,-h/2,w,h});
    canvas.restore();
}

// Construct on the editor/worker thread, before using the cap for painting.
// The owned raster is decoded once and can serve every repaint and DPI size.
// Neither construction nor drawing belongs in an audio callback. The caller
// retains the cap until any paint using it has ended.
class PreparedCap {
public:
    explicit PreparedCap(std::span<const std::byte> encoded,
                         std::size_t maximumPixels=256u*256u,unsigned maximumEdge=256) {
        auto pixels=decodeImage(encoded,maximumPixels,maximumEdge);
        image_.setPixels(pixels.pixels,pixels.width,pixels.height);
    }
    [[nodiscard]] const Surface& image()const noexcept{return image_;}
    void paint(Canvas& canvas,Rect bounds,bool horizontal=false,bool enabled=true)const {
        drawCap(canvas,bounds,image_,horizontal,enabled);
    }
private:
    Surface image_;
};
} // namespace cell::ui::faders
