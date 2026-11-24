#include <cell/ui_effects.hpp>
#include <iostream>
using namespace cell::ui;
int main(int argc,char** argv) {
    try {
        Surface output;output.resize(1000,620);EffectCache effects;AlphaMask panel,button,label;
        panel.rasterize(380,330,[](Canvas& c){c.rounded({15,15,350,300},22,Color(0xffffffff));});
        button.rasterize(290,78,[](Canvas& c){c.rounded({8,8,274,62},14,Color(0xffffffff));});
        label.rasterize(500,80,[](Canvas& c){c.text({8,54},"CELL GRAPHICS",45,Color(0xffffffff));});
        CachedLayer detail;
        output.render([&](Canvas& c){
            c.gradient({0,0,1000,620},Color(0xff15232b),Color(0xff090e17));
            effects.draw(c,label,{65,30},{9,{0,0},Color(0x9064e5ff)});
            c.text({73,84},"CELL GRAPHICS",45,Color(0xffdbf8ff));
            c.text({75,125},"Independent CPU effects / cached vector layers",18,Color(0xff8aabb8));
            for(int col=0;col<2;++col){
                Point origin{65.+col*480,165};
                effects.draw(c,panel,origin,{16,{0,16},Color(0xb0000000)});
                effects.draw(c,panel,origin,{9,{0,0},Color(col?0x556affbd:0x5550cfff)});
                c.rounded({origin.x+15,180,350,300},22,Color(0xff243640));
                if(col)effects.draw(c,panel,origin,{5,{0,5},Color(0xcc05080c),1,EffectKind::innerShadow});
                c.text({origin.x+40,221},col?"INNER SHADOW":"SOFT SHADOW + GLOW",18,Color(0xffd9eff1));
                detail.draw(c,{origin.x+40,245,300,110},1,1,[](Canvas& local){
                    local.line({0,82},{300,82},Color(0xff49616e));
                    local.polyline(std::array<Point,7>{{{0,78},{40,75},{80,28},{125,15},{170,55},{225,70},{300,72}}},Color(0xff77d9eb),3);
                    local.text({0,108},"Same cached layer, two positions",14,Color(0xff8ba9b4));
                });
                Point buttonAt{origin.x+42,377};
                effects.draw(c,button,buttonAt,{7,{0,8},Color(0xaa000000)});
                c.gradient({buttonAt.x+8,385,274,62},Color(0xff385d65),Color(0xff1c353e),14);
                effects.draw(c,button,buttonAt,{2,{0,2},Color(0xaaffffff),.6,EffectKind::innerShadow});
                constexpr std::string_view caption = "CACHED / SCALE AWARE";
                c.text({buttonAt.x+145-c.textWidth(caption,17)/2,422},caption,17,Color(0xffdffcff));
            }
            c.text({75,553},"Paths, text and masks share the same effects cache.",17,Color(0xffa9c4ce));
            c.text({75,582},"Software renderer today. GPU backend remains separate work.",15,Color(0xff7394a2));
        });
        if(!output.writePng(argc>1?argv[1]:"cell-effects-gallery.png"))return 1;
        auto stats=effects.stats();
        std::cout<<"effects="<<stats.entries<<" bytes="<<stats.retainedBytes<<" blur_builds="<<stats.blurBuilds<<" layer_paints="<<detail.paintCount()<<'\n';
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
