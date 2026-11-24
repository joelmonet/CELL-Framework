// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/streams.hpp>
#include <cell/text_list.hpp>
#include <filesystem>
#include <memory>
#include <cstring>

namespace cell {
class FileInputStream;class FileOutputStream;
enum class FileWriteMode {createNew,truncate,append};
struct FileTime {std::int64_t milliseconds{};std::int64_t toMilliseconds()const noexcept{return milliseconds;}bool operator==(const FileTime&)const=default;};

// Native paths and OS-backed streams. No foreign file or stream is retained.
// File and directory operations belong on control/worker threads.
class File {
    std::filesystem::path path_;
    Text foreignPath()const;
public:
    File()=default;explicit File(Text path);explicit File(std::filesystem::path);
    explicit File(const char*path):File(Text(path)){}explicit File(std::string path):File(Text(std::move(path))){}
    template<class T>requires requires(const T&value){value.getFullPathName().toRawUTF8();}
    File(const T&value):File(Text(value.getFullPathName())){}
    template<class T>requires(!std::is_same_v<T,File>&&std::is_constructible_v<T,const char*>&&requires(const T&value){value.getFullPathName();value.getParentDirectory();})
    operator T()const{return T(foreignPath().toRawUTF8());} // temporary foreign API boundary
    const std::filesystem::path&nativePath()const noexcept{return path_;}
    Text getFullPathName()const;Text getFileName()const;Text getFileNameWithoutExtension()const;Text getFileExtension()const;
    File getParentDirectory()const;File getChildFile(const Text&)const;File getSiblingFile(const Text&)const;File withFileExtension(const Text&)const;
    File getNonexistentChildFile(const Text&,const Text&,bool brackets=true)const;File getNonexistentSibling(bool brackets=true)const;
    Text getRelativePathFrom(const File&)const;
    bool isAChildOf(const File&)const;
    Text fitChildFileName(const Text&)const;
    bool exists()const noexcept;bool existsAsFile()const noexcept;bool isDirectory()const noexcept;bool hasFileExtension(const Text&)const;
    std::int64_t getSize()const noexcept;FileTime getLastModificationTime()const noexcept;
    bool setLastModificationTime(FileTime)const;
    template<class T>requires requires(const T&value){value.toMilliseconds();}
    bool setLastModificationTime(const T&value)const{return setLastModificationTime(FileTime{value.toMilliseconds()});}
    Result createDirectory()const;Result create()const;
    bool deleteFile()const noexcept;bool deleteRecursively()const noexcept;bool moveFileTo(const File&)const;bool copyFileTo(const File&)const;
    bool replaceWithText(const Text&)const;bool replaceWithData(const void*,std::size_t)const;bool appendText(const Text&)const;
    Text loadFileAsString()const;bool loadFileAsData(ByteBlock&)const;
    template<class Block>requires requires(Block&block){block.setSize(std::size_t{},false);block.getData();}
    bool loadFileAsData(Block&destination)const{ByteBlock bytes;if(!loadFileAsData(bytes))return false;destination.setSize(bytes.getSize(),false);if(bytes.getSize())std::memcpy(destination.getData(),bytes.getData(),bytes.getSize());return true;}
    std::unique_ptr<FileInputStream>createInputStream()const;
    std::unique_ptr<FileOutputStream>createOutputStream(FileWriteMode=FileWriteMode::createNew)const;
    enum SpecialLocationType {userHomeDirectory,userDocumentsDirectory,userApplicationDataDirectory,tempDirectory,currentExecutableFile,currentApplicationFile,currentModuleFile};
    static File getSpecialLocation(SpecialLocationType);static File getCurrentWorkingDirectory();static File createTempFile(const Text&extension);
    static bool isAbsolutePath(const Text&);static Text createLegalFileName(const Text&);
    enum TypesOfFileToFind {findDirectories=1,findFiles=2,findFilesAndDirectories=3};
    template<class Container>void findChildFiles(Container&output,int types,bool recursive,const Text&patterns="*")const{
        for(auto&file:children(types,recursive,patterns))if constexpr(requires{output.add(file);})output.add(file);else output.push_back(file);
    }
    std::vector<File>children(int types,bool recursive,const Text&patterns="*")const;
    std::vector<File>findChildFiles(int types,bool recursive,const Text&patterns="*")const{return children(types,recursive,patterns);}
    bool operator==(const File&)const;bool operator<(const File&)const;
    template<class T>requires(!std::is_same_v<std::remove_cvref_t<T>,File>&&requires(const T&value){value.getFullPathName().toRawUTF8();})
    bool operator==(const T&other)const{return *this==File(other);}
};
class FileInputStream final:public InputStream {
    struct Impl;std::unique_ptr<Impl>impl_;
public:
    explicit FileInputStream(const File&);~FileInputStream();
    bool openedOk()const noexcept;bool failedToOpen()const noexcept{return !openedOk();}const Result&getStatus()const noexcept;
    int read(void*,int)override;std::int64_t getPosition()const override;bool setPosition(std::int64_t)override;
    std::int64_t getTotalLength()const override;bool isExhausted()const override;
};
class FileOutputStream final:public OutputStream {
    struct Impl;std::unique_ptr<Impl>impl_;
public:
    FileOutputStream(const File&,FileWriteMode,std::size_t bufferBytes=65536);~FileOutputStream();
    bool openedOk()const noexcept;bool failedToOpen()const noexcept{return !openedOk();}const Result&getStatus()const noexcept;
    bool write(const void*,std::size_t)override;void flush()override;std::int64_t getPosition()const override;bool setPosition(std::int64_t)override;Result truncate();
};
class TemporaryFile {
    File target_,temporary_;bool committed_{};
public:
    explicit TemporaryFile(const File&);~TemporaryFile();
    TemporaryFile(const TemporaryFile&)=delete;TemporaryFile&operator=(const TemporaryFile&)=delete;
    const File&getTemporaryFile()const noexcept{return temporary_;}
    const File&getFile()const noexcept{return temporary_;}
    const File&getTargetFile()const noexcept{return target_;}
    bool overwriteTargetFileWithTemporary();
};
}
