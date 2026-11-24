// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/result.hpp>
#include <cell/byte_block.hpp>
#include <span>
#include <bit>
namespace cell {
class InputStream {
public:
    virtual ~InputStream()=default;
    virtual int read(void*,int)=0;
    virtual std::int64_t getPosition()const=0;
    virtual bool setPosition(std::int64_t)=0;
    virtual std::int64_t getTotalLength()const=0;
    virtual bool isExhausted()const=0;
    std::size_t readBytes(std::span<std::byte>bytes){std::size_t at{};while(at<bytes.size()){const auto amount=int(std::min<std::size_t>(bytes.size()-at,INT32_MAX));const auto count=read(bytes.data()+at,amount);if(count<=0)break;at+=std::size_t(count);}return at;}
    bool readFully(std::span<std::byte>bytes){return readBytes(bytes)==bytes.size();}
};
class OutputStream {
public:
    virtual ~OutputStream()=default;
    virtual bool write(const void*,std::size_t)=0;
    virtual void flush()=0;
    virtual std::int64_t getPosition()const=0;
    virtual bool setPosition(std::int64_t)=0;
    bool writeByte(char value){return write(&value,1);}
    template<class T>bool integer(T value,bool bigEndian=false){using U=std::make_unsigned_t<T>;auto bits=std::bit_cast<U>(value);std::array<std::byte,sizeof(T)>bytes;for(std::size_t at=0;at<bytes.size();++at){bytes[bigEndian?bytes.size()-at-1:at]=std::byte(bits&255);bits>>=8;}return write(bytes.data(),bytes.size());}
    bool writeShort(std::int16_t value){return integer(value);}bool writeShortBigEndian(std::int16_t value){return integer(value,true);}
    bool writeInt(std::int32_t value){return integer(value);}bool writeIntBigEndian(std::int32_t value){return integer(value,true);}
    bool writeInt64(std::int64_t value){return integer(value);}bool writeDouble(double value){return integer(std::bit_cast<std::int64_t>(value));}
    bool writeFloat(float value){return integer(std::bit_cast<std::int32_t>(value));}
    bool writeString(const Text&value){return write(value.toRawUTF8(),value.getNumBytesAsUTF8()+1);}
    bool writeFromInputStream(InputStream&input,std::int64_t bytes=-1){std::array<std::byte,65536>buffer{};while(bytes!=0){const auto wanted=bytes<0?buffer.size():std::min<std::uint64_t>(buffer.size(),std::uint64_t(bytes));const auto count=input.read(buffer.data(),int(wanted));if(count<=0)return bytes<=0;if(!write(buffer.data(),std::size_t(count)))return false;if(bytes>0)bytes-=count;}return true;}
};
}
