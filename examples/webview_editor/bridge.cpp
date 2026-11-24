#include "gain.hpp"
#include <cell/web_parameter_bridge.hpp>
#include <iostream>
int main(int argc, char** argv){
    try{
        cell::Engine engine(cell::plugin::descriptor(),cell::plugin::createProcessor());
        engine.prepare(48000,64,2);
        const std::array allowed{cell::example::gainId};
        const std::array actions{cell::web::ActionBinding{cell::example::factoryRecallAction, cell::web::ActionMode::invoke}};
        cell::web::ParameterBridge bridge(engine,allowed,true,actions);
        if (argc == 2 && std::string_view(argv[1]) == "--parameters") {
            std::cout << bridge.snapshot() << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--apply-gestures") {
            std::string line;
            unsigned lineNumber = 0;
            while (std::getline(std::cin, line)) {
                ++lineNumber;
                if (!bridge.apply(line))
                    throw std::runtime_error("Parameter message rejected on line " + std::to_string(lineNumber));
                cell::ParameterEdit edit;
                while (engine.pollEdit(edit)) {}
            }
            if (!std::cin.eof()) throw std::runtime_error("Could not read gesture stream");
            bridge.cancel();
            std::cout << bridge.snapshot() << '\n';
            return 0;
        }
        if (argc != 1)
            throw std::runtime_error("Usage: cell_web_bridge_example [--parameters | --apply-gestures]");
        if(!bridge.apply(R"({"type":"begin","key":"gain"})") ||
           !bridge.apply(R"({"type":"parameter","key":"gain","value":-12.1})") ||
           !bridge.apply(R"({"type":"end","key":"gain"})"))
            throw std::runtime_error("Parameter message rejected");
        if(std::abs(engine.parameters().get(cell::example::gainId)+12.1)>.00001)
            throw std::runtime_error("Parameter message did not reach native engine");
        // In a plugin its wrapper consumes these for host automation. This
        // smoke example owns the engine and drains them on its control thread.
        cell::ParameterEdit edit;unsigned edits=0;while(engine.pollEdit(edit))++edits;
        if(edits!=3)throw std::runtime_error("Missing begin/value/end sequence");
        std::cout<<bridge.snapshot()<<'\n';
        std::cout<<"PASS: native parameter bridge; browser runtime not required\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
