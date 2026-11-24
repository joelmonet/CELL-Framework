#include "sparse_fixture.hpp"
// Original Cell Framework implementation; see LICENSE.
#include <cell/file.hpp>
#include <cell/files.hpp>
#include <cell/uuid.hpp>
#include <iostream>
namespace {void need(bool value,const char*message){if(!value)throw std::runtime_error(message);}}
int main(){cell::File root;try{
    root=cell::File::getSpecialLocation(cell::File::tempDirectory).getChildFile("cell-file-test-"+cell::Uuid().toString());need(root.createDirectory().wasOk(),"Could not create owned test directory");
    const auto name=cell::Text::fromUTF8(u8"Élise_Ж_声_🎙.txt");const auto file=root.getChildFile(name);
    need(file.replaceWithText("abc")&&file.appendText("def")&&file.loadFileAsString()=="abcdef","Native text append/replace changed bytes");
    need(file.getFileName()==name&&file.getRelativePathFrom(root)==name&&file.hasFileExtension("TXT"),"Unicode path or suffix changed");
    need(!file.createOutputStream()&&file.loadFileAsString()=="abcdef","Exclusive output creation overwrote an existing file");
    cell::ByteBlock data;need(file.loadFileAsData(data)&&data.getSize()==6,"Native binary load failed");
    const auto longName=root.getChildFile(cell::Text(std::string(240,'a'))+".txt");need(longName.replaceWithText("long name"),"Atomic replacement imposed a shorter temporary-name limit");
    const auto fitted=root.fitChildFileName(cell::Text(std::string(400,'a'))+"声.mid");need(fitted.endsWith(".mid")&&fitted.contains("~")&&root.getChildFile(fitted).replaceWithText("fitted"),"Filesystem-aware naming lost the extension or failed to fit");
    const auto numbered=root.getNonexistentChildFile(cell::File(fitted).getFileNameWithoutExtension(),".mid");need(numbered!=root.getChildFile(fitted)&&numbered.replaceWithText("numbered"),"Numbered collision name exceeded the filesystem capacity");
    const auto modification=file.getLastModificationTime();const cell::FileTime changed{modification.toMilliseconds()-10000};need(file.setLastModificationTime(changed)&&std::abs(file.getLastModificationTime().toMilliseconds()-changed.toMilliseconds())<=2,"Native modification time did not round-trip");need(!file.setLastModificationTime(cell::FileTime{INT64_MAX}),"Unrepresentable modification time was accepted");
    for(const std::int64_t stamp:{INT64_C(-1000000000123),INT64_C(0),INT64_C(1700000000123),INT64_C(4102444800123)})need(file.setLastModificationTime(cell::FileTime{stamp})&&std::abs(file.getLastModificationTime().toMilliseconds()-stamp)<=2,"Absolute file timestamp epoch or millisecond precision changed");
    need(root.findChildFiles(cell::File::findFiles,false,"*.mid").size()==2,"Native file enumeration lost matching files");
    need(file.isAChildOf(root)&&!root.isAChildOf(root)&&!root.isAChildOf(file),"Directory ancestry is incorrect");
    {cell::TemporaryFile temporary(file);need(temporary.getTemporaryFile().replaceWithText("committed")&&temporary.overwriteTargetFileWithTemporary(),"Native temporary publication failed");}
    need(file.loadFileAsString()=="committed","Published content was lost");
    const auto source=root.getChildFile("source.bin");need(source.replaceWithText("ready"),"Could not prepare publication source");
    need(cell::files::publish(source.nativePath(),file.nativePath(),false).failed()&&source.existsAsFile()&&file.loadFileAsString()=="committed","Exclusive publication replaced a destination or lost its source");
    {
        cell::FileInputStream oldReader(file);
        need(oldReader.openedOk(), "Could not retain the old file before publication");
        need(cell::files::publish(source.nativePath(),file.nativePath(),true).wasOk()&&!source.exists()&&file.loadFileAsString()=="ready","Explicit atomic replacement failed");
        std::array<std::byte, 9> oldBytes{};
        need(oldReader.readFully(oldBytes) &&
                 std::string_view(reinterpret_cast<const char*>(oldBytes.data()), oldBytes.size()) == "committed",
             "Retained reader lost its original bytes during publication");
    }
    const auto sparse=root.getChildFile("beyond-4GiB.bin");constexpr std::int64_t position=(INT64_C(5)<<30)+123;
    {cell::FileOutputStream output(sparse,cellTest::prepareSparseFixture(sparse));need(output.openedOk()&&output.setPosition(position)&&output.writeInt(0x78563412),"64-bit buffered output seek/write failed");output.flush();need(output.getStatus().wasOk()&&output.getPosition()==position+4,"64-bit output position or flush failed");}
    {cell::FileInputStream input(sparse);std::array<std::byte,4>bytes{};need(input.openedOk()&&input.getTotalLength()==position+4&&input.setPosition(position)&&input.readFully(bytes)&&bytes[0]==std::byte{0x12}&&bytes[3]==std::byte{0x78},"64-bit native read used a narrowed offset");need(input.isExhausted()&&input.setPosition(0)&&input.readFully(bytes)&&bytes[0]==std::byte{}&&!input.isExhausted(),"Sparse holes or EOF/seek state changed");}
    const auto utf16=root.getChildFile("utf16.txt");const std::u16string text=u"声 🎙";std::vector<std::byte>encoded{std::byte{0xff},std::byte{0xfe}};for(auto unit:text){encoded.push_back(std::byte(unit&255));encoded.push_back(std::byte(unit>>8));}need(utf16.replaceWithData(encoded.data(),encoded.size())&&utf16.loadFileAsString()==cell::Text::fromUTF8(u8"声 🎙"),"UTF-16 import lost supplementary characters");
    const auto directory=root.getChildFile("folder/inner");need(directory.createDirectory().wasOk(),"Could not create symlink test layout");need(root.getChildFile("folder/marker").replaceWithText("right")&&root.getChildFile("marker").replaceWithText("wrong"),"Could not create path markers");std::error_code error;std::filesystem::create_directory_symlink(directory.nativePath(),root.getChildFile("link").nativePath(),error);if(!error)need(root.getChildFile("link/../marker").loadFileAsString()=="right","Lexical normalization selected a different symlink target");
    need(cell::File::createLegalFileName(cell::Text::fromUTF8(u8"声/é?.wav"))==cell::Text::fromUTF8(u8"声é.wav")&&cell::File::createLegalFileName("CON.txt")=="_CON.txt","Portable filename sanitization corrupted Unicode or reserved names");
    need(cell::File::getSpecialLocation(cell::File::currentExecutableFile).existsAsFile()&&cell::File::getSpecialLocation(cell::File::currentModuleFile).existsAsFile(),"Native executable/module discovery failed");
    need(root.deleteRecursively(),"Could not remove owned test directory");
    std::cout<<"PASS native files: Unicode paths/text,UTF-16,sparse>4GiB offsets, buffered streams, EOF/seek, exclusive/atomic output, long names, symlinks and module paths\n";return 0;
}catch(const std::exception&error){root.deleteRecursively();std::cerr<<error.what()<<'\n';return 1;}}
