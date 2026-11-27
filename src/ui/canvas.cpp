#include <cell/text.hpp>
#include <cell/ui_palette.hpp>
#include <cell/ui.hpp>
#include <blend2d/blend2d.h>
#include "cell_font.hpp"
#include "cell_font_bold.hpp"
#include "cell_font_mono.hpp"
#include "cell_font_brand_grotesk.hpp"
#include "cell_font_brand_condensed.hpp"
#include <filesystem>
#include <cstdlib>
#include <charconv>
#include <cctype>
#include <cstring>
#include <mutex>

namespace cell::ui {
namespace {
BLContext& ctx(void* p) { return *static_cast<BLContext*>(p); }
BLRgba32 rgba(Color c) { return BLRgba32(PaletteScope::map(c).argb); }
BLRect rect(Rect r) { return {r.x,r.y,r.width,r.height}; }
}

Canvas::Canvas(void* context,void* fontFace,void* boldFace,void* monoFace,void* brandGrotesk,void* brandCondensed):context_(context),fontFace_(fontFace),boldFace_(boldFace),monoFace_(monoFace),brandGrotesk_(brandGrotesk),brandCondensed_(brandCondensed){}
void*Canvas::face(Typeface role,float weight)const noexcept{if(role==Typeface::brandGrotesk&&brandGrotesk_)return brandGrotesk_;if(role==Typeface::brandCondensed&&brandCondensed_)return brandCondensed_;if(role==Typeface::monospace&&monoFace_)return monoFace_;return weight>=650&&boldFace_?boldFace_:fontFace_;}
void Canvas::save() { ctx(context_).save(); }
void Canvas::restore() { ctx(context_).restore(); }
void Canvas::scale(double x, double y) { ctx(context_).scale(x,y); }
void Canvas::translate(double x, double y) { ctx(context_).translate(x,y); }
void Canvas::skew(double x,double y){ctx(context_).skew(x,y);}
void Canvas::rotate(double radians){ctx(context_).rotate(radians);}
void Canvas::transform(double a,double b,double c,double d,double e,double f){
    ctx(context_).apply_transform(BLMatrix2D(a,d,b,e,c,f));
}
void Canvas::opacity(double amount){ctx(context_).set_global_alpha(std::clamp(amount,0.,1.));}
void Canvas::clip(Rect r) { ctx(context_).clip_to_rect(rect(r)); }
void Canvas::fill(Rect r, Color c) { ctx(context_).fill_rect(rect(r),rgba(c)); }
void Canvas::rounded(Rect r, double radius, Color c) { ctx(context_).fill_round_rect(BLRoundRect(r.x,r.y,r.width,r.height,radius),rgba(c)); }
void Canvas::strokeRounded(Rect r,double radius,Color c,double width){ctx(context_).set_stroke_width(width);ctx(context_).stroke_round_rect(BLRoundRect(r.x,r.y,r.width,r.height,radius),rgba(c));}
void Canvas::gradient(Rect r, Color top, Color bottom, double radius) {
    BLGradient g(BLLinearGradientValues(r.x,r.y,r.x,r.y+r.height));
    g.add_stop(0.0,rgba(top)); g.add_stop(1.0,rgba(bottom));
    if (radius>0) ctx(context_).fill_round_rect(BLRoundRect(r.x,r.y,r.width,r.height,radius),g);
    else ctx(context_).fill_rect(rect(r),g);
}
void Canvas::circle(Point p, double radius, Color c) { ctx(context_).fill_circle(BLCircle(p.x,p.y,radius),rgba(c)); }
void Canvas::strokeCircle(Point p,double radius,Color c,double width){ctx(context_).set_stroke_width(width);ctx(context_).stroke_circle(BLCircle(p.x,p.y,radius),rgba(c));}
void Canvas::ellipse(Rect r,Color color,double width){
    const BLEllipse ellipse(r.x+r.width*.5,r.y+r.height*.5,r.width*.5,r.height*.5);
    if(width>0){ctx(context_).set_stroke_width(width);ctx(context_).stroke_ellipse(ellipse,rgba(color));}
    else ctx(context_).fill_ellipse(ellipse,rgba(color));
}
void Canvas::path(std::span<const VectorCommand> commands,Color color,double width,bool rounded){
    BLPath path;
    for(const auto& command:commands)switch(command.kind){
        case VectorCommand::Kind::move:path.move_to(command.first.x,command.first.y);break;
        case VectorCommand::Kind::line:path.line_to(command.first.x,command.first.y);break;
        case VectorCommand::Kind::quadratic:path.quad_to(command.first.x,command.first.y,command.second.x,command.second.y);break;
        case VectorCommand::Kind::cubic:path.cubic_to(command.first.x,command.first.y,command.second.x,command.second.y,command.third.x,command.third.y);break;
        case VectorCommand::Kind::close:path.close();break;
    }
    if(width<=0){ctx(context_).fill_path(path,rgba(color));return;}
    ctx(context_).save();ctx(context_).set_stroke_width(width);
    ctx(context_).set_stroke_caps(rounded?BL_STROKE_CAP_ROUND:BL_STROKE_CAP_BUTT);
    ctx(context_).set_stroke_join(rounded?BL_STROKE_JOIN_ROUND:BL_STROKE_JOIN_MITER_CLIP);
    ctx(context_).stroke_path(path,rgba(color));ctx(context_).restore();
}
void Canvas::pathGradient(std::span<const VectorCommand> commands,std::span<const GradientStop> stops,Point from,Point to,bool radial){
    BLPath path;
    for(const auto& command:commands)switch(command.kind){
        case VectorCommand::Kind::move:path.move_to(command.first.x,command.first.y);break;
        case VectorCommand::Kind::line:path.line_to(command.first.x,command.first.y);break;
        case VectorCommand::Kind::quadratic:path.quad_to(command.first.x,command.first.y,command.second.x,command.second.y);break;
        case VectorCommand::Kind::cubic:path.cubic_to(command.first.x,command.first.y,command.second.x,command.second.y,command.third.x,command.third.y);break;
        case VectorCommand::Kind::close:path.close();break;
    }
    BLGradient gradient;
    if(radial)gradient.create(BLRadialGradientValues(from.x,from.y,from.x,from.y,std::max(1.e-9,std::hypot(to.x-from.x,to.y-from.y))));
    else gradient.create(BLLinearGradientValues(from.x,from.y,to.x,to.y));
    for(const auto& stop:stops)gradient.add_stop(stop.offset,rgba(stop.color));
    ctx(context_).fill_path(path,gradient);
}
void Canvas::gradient(Rect r,std::span<const GradientStop> stops,Point from,Point to,double radius,bool ellipse){
    BLGradient gradient(BLLinearGradientValues(from.x,from.y,to.x,to.y));
    for(const auto& stop:stops)gradient.add_stop(stop.offset,rgba(stop.color));
    if(ellipse)ctx(context_).fill_ellipse(BLEllipse(r.x+r.width/2,r.y+r.height/2,r.width/2,r.height/2),gradient);
    else if(radius>0)ctx(context_).fill_round_rect(BLRoundRect(r.x,r.y,r.width,r.height,radius),gradient);
    else ctx(context_).fill_rect(rect(r),gradient);
}
void Canvas::gradientPolygon(std::span<const Point> points,Color first,Color last,Point from,Point to){
    if(points.empty())return;BLPath path;path.move_to(points.front().x,points.front().y);
    for(std::size_t i=1;i<points.size();++i)path.line_to(points[i].x,points[i].y);path.close();
    BLGradient gradient(BLLinearGradientValues(from.x,from.y,to.x,to.y));gradient.add_stop(0,rgba(first));gradient.add_stop(1,rgba(last));ctx(context_).fill_path(path,gradient);
}
void Canvas::line(Point a, Point b, Color c, double thickness) {
    ctx(context_).set_stroke_width(thickness);
    ctx(context_).stroke_line(a.x,a.y,b.x,b.y,rgba(c));
}
void Canvas::polyline(std::span<const Point> points, Color c, double thickness, bool closed, bool filled) {
    if(points.empty()) return;
    BLPath path;
    path.move_to(points.front().x,points.front().y);
    for(std::size_t i=1;i<points.size();++i) path.line_to(points[i].x,points[i].y);
    if(closed) path.close();
    if(filled) ctx(context_).fill_path(path,rgba(c));
    else { ctx(context_).set_stroke_width(thickness); ctx(context_).stroke_path(path,rgba(c)); }
}
void Canvas::text(Point baseline, std::string_view text, double size, Color c) {
    BLFont font;
    if(font.create_from_face(*static_cast<BLFontFace*>(fontFace_),float(size)) != BL_SUCCESS) return;
    ctx(context_).fill_utf8_text(BLPoint(baseline.x,baseline.y),font,text.data(),text.size(),rgba(c));
}
void Canvas::textWeighted(Point baseline,std::string_view text,double size,Color c,float weight,Typeface role){
    // Static OFL font instance: this also works on raster backends without
    // variable-glyph outline support. Both faces are bundled on every OS.
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(face(role,weight)),float(size))!=BL_SUCCESS)return;
    ctx(context_).fill_utf8_text(BLPoint(baseline.x,baseline.y),font,text.data(),text.size(),rgba(c));
}
TextMetrics Canvas::textMetrics(std::string_view text,double size,float weight,Typeface role)const{
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(face(role,weight)),float(size))!=BL_SUCCESS)return {};
    const auto metrics=font.metrics();BLGlyphBuffer glyphs;glyphs.set_utf8_text(text.data(),text.size());
    if(font.shape(glyphs)!=BL_SUCCESS)return {};
    BLTextMetrics bounds{};if(font.get_text_metrics(glyphs,bounds)!=BL_SUCCESS)return {};
    return {bounds.advance.x,metrics.ascent,metrics.descent,bounds.bounding_box.x0,bounds.bounding_box.y0,bounds.bounding_box.x1,bounds.bounding_box.y1};
}
void Canvas::fittedText(Rect bounds,std::string_view text,Color color,float weight,double shear,Typeface role){
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(face(role,weight)),72)!=BL_SUCCESS)return;
    BLGlyphBuffer glyphs;glyphs.set_utf8_text(text.data(),text.size());if(font.shape(glyphs)!=BL_SUCCESS)return;
    BLPath path;if(font.get_glyph_run_outlines(glyphs.glyph_run(),path)!=BL_SUCCESS)return;
    path.transform(BLMatrix2D(1,0,shear,1,0,0));BLBox box;if(path.get_bounding_box(&box)!=BL_SUCCESS||box.x1<=box.x0||box.y1<=box.y0)return;
    auto& context=ctx(context_);context.save();context.translate(bounds.x,bounds.y);context.scale(bounds.width/(box.x1-box.x0),bounds.height/(box.y1-box.y0));context.translate(-box.x0,-box.y0);context.fill_path(path,rgba(color));context.restore();
}
double Canvas::textWidth(std::string_view text,double size,float weight,Typeface role)const{
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(face(role,weight)),float(size))!=BL_SUCCESS)return 0;
    BLGlyphBuffer glyphs;glyphs.set_utf8_text(text.data(),text.size());font.shape(glyphs);BLTextMetrics metrics{};font.get_text_metrics(glyphs,metrics);return metrics.advance.x;
}
std::size_t Canvas::missingGlyphs(std::string_view text,Typeface role)const{
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(face(role)),16)!=BL_SUCCESS)throw std::runtime_error("Cannot prepare coverage font");
    BLGlyphBuffer glyphs;glyphs.set_utf8_text(text.data(),text.size());BLGlyphMappingState state{};
    if(font.map_text_to_glyphs(glyphs,state)!=BL_SUCCESS)throw std::runtime_error("Cannot map font coverage");return state.undefined_count;
}
void Canvas::textMono(Point baseline,std::string_view text,double size,Color c){
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(monoFace_?monoFace_:fontFace_),float(size))!=BL_SUCCESS)return;
    ctx(context_).fill_utf8_text(BLPoint(baseline.x,baseline.y),font,text.data(),text.size(),rgba(c));
}
double Canvas::monoWidth(std::string_view text,double size)const{
    BLFont font;if(font.create_from_face(*static_cast<BLFontFace*>(monoFace_?monoFace_:fontFace_),float(size))!=BL_SUCCESS)return 0;
    BLGlyphBuffer glyphs;glyphs.set_utf8_text(text.data(),text.size());font.shape(glyphs);BLTextMetrics metrics{};font.get_text_metrics(glyphs,metrics);return metrics.advance.x;
}
bool Canvas::svgPath(std::string_view data,Rect bounds,Color color,double stroke){
    // Small, strict SVG centreline reader for the owned 24-unit rack glyphs.
    // No renderer or parser code from the original UI framework is reused.
    BLPath path;std::size_t offset=0;char command=0;double x=0,y=0;
    auto spaces=[&]{while(offset<data.size()&&(std::isspace(static_cast<unsigned char>(data[offset]))||data[offset]==','))++offset;};
    auto number=[&](double& n){spaces();auto first=data.data()+offset;auto result=cell::detail::parseDoubleChars(first,data.data()+data.size(),n);if(result.ec!=std::errc{}||result.ptr==first||!std::isfinite(n))return false;offset=std::size_t(result.ptr-data.data());return true;};
    while(true){spaces();if(offset==data.size())break;if(std::isalpha(static_cast<unsigned char>(data[offset])))command=data[offset++];
        if(command=='Z'){path.close();command=0;continue;}
        unsigned count=command=='H'||command=='V'?1:command=='M'||command=='L'?2:command=='S'?4:command=='C'?6:command=='A'?7:0;if(!count)return false;
        std::array<double,7> n{};for(unsigned i=0;i<count;++i)if(!number(n[i]))return false;
        switch(command){case 'M':x=n[0];y=n[1];path.move_to(x,y);command='L';break;case 'L':x=n[0];y=n[1];path.line_to(x,y);break;case 'H':x=n[0];path.line_to(x,y);break;case 'V':y=n[0];path.line_to(x,y);break;case 'C':path.cubic_to(n[0],n[1],n[2],n[3],n[4],n[5]);x=n[4];y=n[5];break;case 'S':path.smooth_cubic_to(n[0],n[1],n[2],n[3]);x=n[2];y=n[3];break;case 'A':path.elliptic_arc_to(n[0],n[1],n[2],n[3]!=0,n[4]!=0,n[5],n[6]);x=n[5];y=n[6];break;default:return false;}
    }
    auto& context=ctx(context_);context.save();double size=std::min(bounds.width,bounds.height);context.translate(bounds.x+(bounds.width-size)/2,bounds.y+(bounds.height-size)/2);context.scale(size/24,size/24);context.set_stroke_width(stroke);context.set_stroke_caps(BL_STROKE_CAP_ROUND);context.set_stroke_join(BL_STROKE_JOIN_ROUND);context.stroke_path(path,rgba(color));context.restore();return true;
}

namespace {
struct SharedFonts {
    BLFontFace font,bold,mono,brandGrotesk,brandCondensed;
    explicit SharedFonts(const std::string& custom) {
        BLFontData groteskData,condensedData;
        if(groteskData.create_from_data(cell::assets::fontBrandGrotesk,sizeof(cell::assets::fontBrandGrotesk))!=BL_SUCCESS||brandGrotesk.create_from_data(groteskData,0)!=BL_SUCCESS)throw std::runtime_error("Bundled brand grotesk failed");
        if(condensedData.create_from_data(cell::assets::fontBrandCondensed,sizeof(cell::assets::fontBrandCondensed))!=BL_SUCCESS||brandCondensed.create_from_data(condensedData,0)!=BL_SUCCESS)throw std::runtime_error("Bundled brand condensed failed");
        BLFontData monoData;
        if(monoData.create_from_data(cell::assets::fontMono,sizeof(cell::assets::fontMono))!=BL_SUCCESS||mono.create_from_data(monoData,0)!=BL_SUCCESS)throw std::runtime_error("Bundled value font failed");
        BLFontData boldData;
        if(boldData.create_from_data(cell::assets::fontBold,sizeof(cell::assets::fontBold))!=BL_SUCCESS||bold.create_from_data(boldData,0)!=BL_SUCCESS)throw std::runtime_error("Bundled bold font failed");
        BLFontData bundled;
        if(bundled.create_from_data(cell::assets::font,sizeof(cell::assets::font))==BL_SUCCESS&&
           font.create_from_data(bundled,0)==BL_SUCCESS&&custom.empty())return;
        std::vector<std::string> paths;
        if(!custom.empty()) paths.push_back(custom);
#if defined(_WIN32)
        paths.emplace_back("C:/Windows/Fonts/segoeui.ttf");
#elif defined(__APPLE__)
        paths.emplace_back("/System/Library/Fonts/Supplemental/Arial.ttf");
#else
        paths.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        paths.emplace_back("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
#endif
        bool loaded=false;
        for(const auto& path:paths) if(font.create_from_file(path.c_str())==BL_SUCCESS) { loaded=true;break; }
        if(!loaded) throw std::runtime_error("No font available. Set CELL_FONT to a trusted TTF font.");
    }
};

std::shared_ptr<SharedFonts> sharedFonts() {
    static std::mutex mutex;
    struct Cached { std::string key; std::shared_ptr<SharedFonts> fonts; std::uint64_t used{}; };
    static std::vector<Cached> cache; static std::uint64_t clock{};
    const char* environment = std::getenv("CELL_FONT");const std::string key = environment ? environment : "";
    std::lock_guard lock(mutex);
    for (auto& item : cache) if (item.key == key) { item.used = ++clock; return item.fonts; }
    auto fonts = std::make_shared<SharedFonts>(key);
    if (cache.size() == 4) cache.erase(std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return a.used < b.used; }));
    cache.push_back({key, fonts, ++clock}); return fonts;
}
}
struct Surface::Impl {
    BLImage image;
    BLImageData data{};
    std::shared_ptr<SharedFonts> fonts; // raster-only surfaces do not parse fonts
    bool painting{};
};
Surface::Surface() : impl_(std::make_unique<Impl>()) {}
Surface::~Surface() = default;
DecodedImage decodeImage(std::span<const std::byte> bytes,std::size_t maximumPixels,unsigned maximumEdge){
    if(bytes.empty()||bytes.size()>32u*1024*1024||!maximumPixels||maximumPixels>64u*1024*1024)throw std::invalid_argument("encoded image bounds");
    BLImageCodec codec;BLImageDecoder decoder;BLImageInfo info{};BLImage image;
    if(codec.find_by_data(bytes.data(),bytes.size())!=BL_SUCCESS||codec.create_decoder(&decoder)!=BL_SUCCESS||decoder.read_info(info,bytes.data(),bytes.size())!=BL_SUCCESS||info.size.w<1||info.size.h<1||info.size.w>8192||info.size.h>8192||std::size_t(info.size.w)>maximumPixels/std::size_t(info.size.h))throw std::runtime_error("unsupported or oversized image");
    if(decoder.read_frame(image,bytes.data(),bytes.size())!=BL_SUCCESS||image.width()!=info.size.w||image.height()!=info.size.h||image.convert(BL_FORMAT_PRGB32)!=BL_SUCCESS)throw std::runtime_error("image decode failed");
    if(maximumEdge&&unsigned(std::max(image.width(),image.height()))>maximumEdge){const double ratio=double(maximumEdge)/std::max(image.width(),image.height());BLImage scaled;const BLSizeI size{std::max(1,int(std::lround(image.width()*ratio))),std::max(1,int(std::lround(image.height()*ratio)))};if(BLImage::scale(scaled,image,size,BL_IMAGE_SCALE_FILTER_LANCZOS)!=BL_SUCCESS)throw std::runtime_error("image scaling failed");image=std::move(scaled);}
    BLImageData data{};if(image.get_data(&data)!=BL_SUCCESS)throw std::runtime_error("image pixels unavailable");DecodedImage result{image.width(),image.height(),std::vector<std::uint32_t>(std::size_t(image.width())*std::size_t(image.height()))};
    for(int row=0;row<result.height;++row)std::memcpy(result.pixels.data()+std::size_t(row)*std::size_t(result.width),static_cast<const std::byte*>(data.pixel_data)+row*data.stride,std::size_t(result.width)*4);return result;
}
void Surface::resize(int width,int height) {
    if(width<1||height<1||width>8192||height>8192) throw std::invalid_argument("surface dimensions");
    if(width==impl_->image.width()&&height==impl_->image.height())return;
    if(impl_->painting)throw std::logic_error("Cannot resize a surface during painting");
    if(impl_->image.create(width,height,BL_FORMAT_PRGB32)!=BL_SUCCESS) throw std::runtime_error("surface allocation failed");
    impl_->image.get_data(&impl_->data);
}
void Surface::setPixels(std::span<const std::uint32_t> pixels,int width,int height){
    if(impl_->painting)throw std::logic_error("Cannot replace pixels during painting");
    if(width<1||height<1||width>8192||height>8192||pixels.size()!=std::size_t(width)*std::size_t(height))throw std::invalid_argument("pixel dimensions");
    // Preserve an aliased input if resizing or detaching the image replaces it.
    std::vector<std::uint32_t> staged;
    if(impl_->data.pixel_data){const auto first=reinterpret_cast<std::uintptr_t>(impl_->data.pixel_data),input=reinterpret_cast<std::uintptr_t>(pixels.data());const auto size=std::size_t(std::abs(impl_->data.stride))*std::size_t(impl_->image.height());if(input>=first&&input-first<size){staged.assign(pixels.begin(),pixels.end());pixels=staged;}}
    resize(width,height);
    if(impl_->image.make_mutable(&impl_->data)!=BL_SUCCESS)throw std::runtime_error("pixel surface allocation failed");
    for(int row=0;row<height;++row)std::memcpy(static_cast<std::byte*>(impl_->data.pixel_data)+row*impl_->data.stride,pixels.data()+std::size_t(row)*std::size_t(width),std::size_t(width)*4);
}
void Canvas::image(const Surface& source,Rect destination){
    if(source.width()<=0||source.height()<=0||!std::isfinite(destination.x)||!std::isfinite(destination.y)||!std::isfinite(destination.width)||!std::isfinite(destination.height)||destination.width<=0||destination.height<=0)return;
    ctx(context_).blit_image(BLRect(destination.x,destination.y,destination.width,destination.height),source.impl_->image);
}
void Canvas::image(const Surface& source,Rect destination,Rect area,bool nearest){
    if(source.width()<=0||source.height()<=0||!std::isfinite(destination.x)||!std::isfinite(destination.y)||!std::isfinite(destination.width)||!std::isfinite(destination.height)||!std::isfinite(area.x)||!std::isfinite(area.y)||!std::isfinite(area.width)||!std::isfinite(area.height)||destination.width<=0||destination.height<=0||area.width<=0||area.height<=0)return;
    const int left=int(std::clamp(std::floor(area.x),0.,double(source.width()))),top=int(std::clamp(std::floor(area.y),0.,double(source.height())));
    const int right=int(std::clamp(std::ceil(area.x+area.width),double(left),double(source.width()))),bottom=int(std::clamp(std::ceil(area.y+area.height),double(top),double(source.height())));
    if(right<=left||bottom<=top)return;
    ctx(context_).save();ctx(context_).set_pattern_quality(nearest?BL_PATTERN_QUALITY_NEAREST:BL_PATTERN_QUALITY_BILINEAR);
    ctx(context_).blit_image(rect(destination),source.impl_->image,BLRectI(left,top,right-left,bottom-top));ctx(context_).restore();
}
struct Surface::PaintSession::Impl {
    Surface& owner;
    BLContext context;
    Canvas canvas{nullptr,nullptr};
    explicit Impl(Surface& surface):owner(surface){
        if(owner.impl_->painting)throw std::logic_error("A surface already has an active painter");
        if(owner.width()<1||owner.height()<1)throw std::logic_error("Prepare the surface before painting");
        if(!owner.impl_->fonts)owner.impl_->fonts=sharedFonts();
        if(context.begin(owner.impl_->image)!=BL_SUCCESS)throw std::runtime_error("Could not begin surface painting");
        canvas=Canvas(&context,&owner.impl_->fonts->font,&owner.impl_->fonts->bold,&owner.impl_->fonts->mono,&owner.impl_->fonts->brandGrotesk,&owner.impl_->fonts->brandCondensed);
        owner.impl_->painting=true;
    }
    ~Impl(){context.end();owner.impl_->image.get_data(&owner.impl_->data);owner.impl_->painting=false;}
};
Surface::PaintSession::PaintSession(Surface& owner):impl_(std::make_unique<Impl>(owner)){}
Surface::PaintSession::~PaintSession()=default;
Canvas& Surface::PaintSession::canvas()noexcept{return impl_->canvas;}
std::unique_ptr<Surface::PaintSession> Surface::beginPaint(){return std::unique_ptr<PaintSession>(new PaintSession(*this));}
void Surface::render(const std::function<void(Canvas&)>& paint) {
    if(impl_->painting)throw std::logic_error("A surface already has an active painter");
    if(!impl_->fonts)impl_->fonts=sharedFonts();
    BLContext context;if(context.begin(impl_->image)!=BL_SUCCESS)throw std::runtime_error("Could not begin surface painting");
    struct Complete {Impl& surface;BLContext& context;~Complete(){context.end();surface.image.get_data(&surface.data);surface.painting=false;}} complete{*impl_,context};
    impl_->painting=true;
    Canvas canvas(&context,&impl_->fonts->font,&impl_->fonts->bold,&impl_->fonts->mono,&impl_->fonts->brandGrotesk,&impl_->fonts->brandCondensed);
    paint(canvas);
}
bool Surface::writePng(const std::string& path) const { return impl_->image.write_to_file(path.c_str())==BL_SUCCESS; }
const void* Surface::pixels() const { return impl_->data.pixel_data; }
std::intptr_t Surface::stride() const { return impl_->data.stride; }
int Surface::width() const { return impl_->image.width(); }
int Surface::height() const { return impl_->image.height(); }

} // namespace cell::ui
