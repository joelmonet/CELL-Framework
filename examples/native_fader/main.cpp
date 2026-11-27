// SPDX-License-Identifier: Apache-2.0
#include <cell/ui_faders.hpp>
#include <cell/files.hpp>
#include <iostream>

int main(int argc,char** argv)try {
    if(argc!=3)throw std::invalid_argument("usage: cell_native_fader cap.png preview.png");
    const auto bytes=cell::files::readBounded(argv[1],256u*1024u);
    const cell::ui::faders::PreparedCap cap(bytes);
    cell::ui::Surface preview;preview.resize(640,360);
    preview.render([&](cell::ui::Canvas& canvas) {
        using cell::ui::Color;
        canvas.fill({0,0,640,360},Color(0xff152329));
        canvas.text({30,34},"One prepared image, both fader orientations",17,Color(0xffedf0e9));
        canvas.rounded({108,72,6,230},3,Color(0xff080f12));
        cap.paint(canvas,{91,138,40,24});
        canvas.rounded({220,150,340,6},3,Color(0xff080f12));
        cap.paint(canvas,{380,133,24,40},true);
        cap.paint(canvas,{220,250,40,24},false,false);
        canvas.text({280,268},"Disabled",13,Color(0xffa2b7b9));
    });
    if(!preview.writePng(argv[2]))throw std::runtime_error("preview write failed");
    std::cout<<"Rendered prepared native cap in both orientations\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
