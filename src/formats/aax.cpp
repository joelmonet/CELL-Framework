// Independently written AAX Native adapter. Requires the actual Avid SDK and
// its library/entry points; SDK headers and wrapper libraries are not vendored.
#include <cell/text.hpp>
#include <cell/analysis.hpp>
#include <cell/commands.hpp>
#include <cell/wrapper.hpp>
#include "AAX_CEffectParameters.h"
#include "AAX_CParameter.h"
#include "AAX_ICollection.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IEffectDescriptor.h"
#include "AAX_IController.h"
#include "AAX_IPropertyMap.h"
#include "AAX_ITransport.h"
#include "AAX_TransportTypes.h"
#include "AAX_IMIDINode.h"
#include "AAX_ITaperDelegate.h"
#include "AAX_IDisplayDelegate.h"
#include "AAX_Errors.h"
#if CELL_HAS_UI
#include <cell/ui.hpp>
#include "AAX_CEffectGUI.h"
#include "AAX_IViewContainer.h"
#endif
#include <charconv>
#include <cstring>
#include <unordered_map>

#if !defined(CELL_AAX_MANUFACTURER_ID) || !defined(CELL_AAX_PRODUCT_ID) || !defined(CELL_AAX_STEREO_ID) || !defined(CELL_AAX_MONO_ID) || !defined(CELL_AAX_CHUNK_ID)
#error "AAX requires explicit, stable manufacturer, product, mono, stereo and state-chunk IDs"
#endif
#ifndef CELL_PLUGIN_VERSION_CODE
#define CELL_PLUGIN_VERSION_CODE 10000
#endif

namespace {
using namespace cell;
using wrapper::Result;
constexpr AAX_CTypeID commandData=0x43434d44, queryData=0x43515259, restoreData=0x43525354,
    analysisData=0x43414e41, analysisEnable=0x43414e45, stateRevision=0x43524556, inputData=0x43494e50;
constexpr AAX_Result failed=AAX_ERROR_INVALID_INTERNAL_DATA;

class Taper final : public AAX_ITaperDelegate<double> {
public:
    explicit Taper(const Parameter& parameter):parameter_(parameter) {}
    Taper* Clone() const override {return new Taper(*this);}
    double GetMaximumValue() const override {return parameter_.maximum;}
    double GetMinimumValue() const override {return parameter_.minimum;}
    double ConstrainRealValue(double value) const override {return parameter_.clamp(value);}
    double NormalizedToReal(double value) const override {return parameter_.fromNormalized(value);}
    double RealToNormalized(double value) const override {return parameter_.toNormalized(value);}
private: Parameter parameter_;
};
class Display final : public AAX_IDisplayDelegate<double> {
public:
    explicit Display(const Parameter& parameter):parameter_(parameter) {}
    Display* Clone() const override {return new Display(*this);}
    bool ValueToString(double value,AAX_CString* output) const override {
        if(!output)return false;
        try{const auto text=parameter_.text(value);*output=text.c_str();return true;}catch(...){return false;}
    }
    bool ValueToString(double value,int32_t maximum,AAX_CString* output) const override {
        if(maximum<0||!output)return false;
        try{auto text=parameter_.text(value);if(text.size()>std::size_t(maximum))text.resize(std::size_t(maximum));*output=text.c_str();return true;}catch(...){return false;}
    }
    bool StringToValue(const AAX_CString& input,double* output) const override {
        if(!output)return false;
        const char* text=input.CString();const auto length=std::strlen(text);if(length>128)return false;
        double value{};const auto parsed=cell::detail::parseDoubleChars(text,text+length,value);
        if(parsed.ec!=std::errc{}||!std::isfinite(value))return false;
        std::string_view suffix(parsed.ptr,std::size_t(text+length-parsed.ptr));
        while(!suffix.empty()&&suffix.front()==' ')suffix.remove_prefix(1);
        while(!suffix.empty()&&suffix.back()==' ')suffix.remove_suffix(1);
        if(!suffix.empty()&&suffix!=parameter_.unit)return false;
        *output=parameter_.clamp(value);return true;
    }
private: Parameter parameter_;
};

class Parameters;
struct Context {
    float** input;
    float** output;
    int32_t* frames;
    Parameters** owner;
    std::uint64_t* token;
    int32_t* sidechain;
    AAX_IMIDINode* midi;
};
constexpr AAX_CFieldIndex inputField=AAX_FIELD_INDEX(Context,input),outputField=AAX_FIELD_INDEX(Context,output),
    framesField=AAX_FIELD_INDEX(Context,frames),ownerField=AAX_FIELD_INDEX(Context,owner),
    tokenField=AAX_FIELD_INDEX(Context,token),sideField=AAX_FIELD_INDEX(Context,sidechain),midiField=AAX_FIELD_INDEX(Context,midi);

class Parameters final : public AAX_CEffectParameters {
public:
    static AAX_CEffectParameters* AAX_CALLBACK create() noexcept {
        try{return new Parameters;}catch(...){return nullptr;}
    }
    AAX_Result EffectInit() override {
        try {
            AAX_CSampleRate rate{};AAX_EStemFormat input{},output{};
            if(!Controller()||Controller()->GetSampleRate(&rate)!=AAX_SUCCESS||
                Controller()->GetInputStemFormat(&input)!=AAX_SUCCESS||Controller()->GetOutputStemFormat(&output)!=AAX_SUCCESS||input!=output)return failed;
            channels_=std::uint32_t(AAX_STEM_FORMAT_CHANNEL_COUNT(output));
            if((channels_!=1&&channels_!=2)||runtime_.configure({rate,wrapper::Limits::renderFrames,channels_})!=Result::ok)return failed;
            const auto fields=runtime_.engine().parameters().descriptors();
            dirty_.assign(fields.size(),false);changed_.reserve(fields.size());values_.reserve(fields.size());byKey_.reserve(fields.size());
            for(std::size_t index=0;index<fields.size();++index) {
                const auto& field=fields[index];byKey_.emplace(field.key,index);
                auto parameter=std::make_unique<AAX_CParameter<double>>(field.key.c_str(),AAX_CString(field.name.c_str()),field.defaultValue,Taper(field),Display(field),field.automatable);
                const auto steps=field.step>0?std::min(2147483647.,std::floor((field.maximum-field.minimum)/field.step)+1):0;
                parameter->SetNumberOfSteps(steps>0?int32_t(steps):1024);
                parameter->SetType(field.step>=1?AAX_eParameterType_Discrete:AAX_eParameterType_Continuous);
                mParameterManager.AddParameter(parameter.release());
                if(field.bypass)bypassKey_=field.key;
            }
            if(runtime_.activate()!=Result::ok||runtime_.start()!=Result::ok)return failed;
            return Controller()->SetSignalLatency(runtime_.engine().processor().latency());
        } catch(...) {return failed;}
    }
    AAX_Result GetMasterBypassParameter(AAX_IString* output) const override {
        if(!output)return AAX_ERROR_NULL_ARGUMENT;
        if(bypassKey_.empty())return AAX_ERROR_UNIMPLEMENTED;
        output->Set(bypassKey_.c_str());return AAX_SUCCESS;
    }
    AAX_Result UpdateParameterNormalizedValue(AAX_CParamID key,double normalized,AAX_EUpdateSource source) override {
        if(!key||!std::isfinite(normalized))return AAX_ERROR_INVALID_PARAMETER_ID;
        const auto found=byKey_.find(key);if(found==byKey_.end())return AAX_ERROR_INVALID_PARAMETER_ID;
        const auto result=AAX_CEffectParameters::UpdateParameterNormalizedValue(key,normalized,source);
        if(result!=AAX_SUCCESS)return result;
        std::lock_guard lock(model_);
        if(!dirty_[found->second]){dirty_[found->second]=true;changed_.push_back(found->second);}
        return AAX_SUCCESS;
    }
    AAX_Result UpdateParameterNormalizedRelative(AAX_CParamID key,double delta) override {
        if(!key||!std::isfinite(delta))return AAX_ERROR_INVALID_PARAMETER_ID;
        double current{};const auto result=GetParameterNormalizedValue(key,&current);
        if(result!=AAX_SUCCESS)return result;
        return UpdateParameterNormalizedValue(key,std::clamp(current+delta,0.,1.),AAX_eUpdateSource_Parameter);
    }
    AAX_Result GenerateCoefficients() override {
        try {
            std::lock_guard lock(model_);timeline_.reclaim();
            const auto fields=runtime_.engine().parameters().descriptors();values_.clear();
            for(const auto index:changed_) {
                const auto* parameter=mParameterManager.GetParameterByID(fields[index].key.c_str());
                if(!parameter)return AAX_ERROR_INVALID_PARAMETER_ID;
                values_.push_back({fields[index].id,fields[index].fromNormalized(parameter->GetNormalizedValue())});
            }
            if(!values_.empty()) {
                const auto token=timeline_.publish(runtime_.engine().parameters(),values_);
                if(!token)return AAX_ERROR_FIFO_FULL; // keep changed_/dirty_ for retry
                token_=*token;
                for(const auto index:changed_)dirty_[index]=false;
                changed_.clear();needsPost_=true;
            }
            if(needsPost_) {
                const auto result=Controller()->PostPacket(tokenField,&token_,sizeof(token_));
                if(result!=AAX_SUCCESS)return result;
                needsPost_=false;
            }
            return AAX_CEffectParameters::GenerateCoefficients();
        } catch(...) {return failed;}
    }
    AAX_Result ResetFieldData(AAX_CFieldIndex field,void* data,uint32_t size) const override {
        if(!data)return AAX_ERROR_NULL_ARGUMENT;
        if(field==ownerField) {
            if(size!=sizeof(Parameters*))return failed;
            auto* self=const_cast<Parameters*>(this);std::memcpy(data,&self,sizeof(self));self->runtime_.requestMidiPanic();return AAX_SUCCESS;
        }
        if(field==tokenField) {
            if(size!=sizeof(std::uint64_t))return failed;
            const auto token=timeline_.applied();std::memcpy(data,&token,sizeof(token));return AAX_SUCCESS;
        }
        return AAX_CEffectParameters::ResetFieldData(field,data,size);
    }
    AAX_Result NotificationReceived(AAX_CTypeID type,const void* data,uint32_t size) override {
        if(type==AAX_eNotificationEvent_EnteringOfflineMode)offline_.store(true);
        if(type==AAX_eNotificationEvent_ExitingOfflineMode)offline_.store(false);
        if(type==AAX_eNotificationEvent_TransportStateChanged&&data&&size==sizeof(AAX_TransportStateInfo_V1)) {
            AAX_TransportStateInfo_V1 state;std::memcpy(&state,data,sizeof(state));recording_.store(state.mIsRecording!=0);
        }
        return AAX_CEffectParameters::NotificationReceived(type,data,size);
    }
    AAX_Result TimerWakeup() override {
        try {
            {std::lock_guard lock(model_);timeline_.reclaim();}
            ParameterEdit edit;
            while(hasPendingGenerated_||runtime_.engine().pollGenerated(edit)) {
                if(hasPendingGenerated_)edit=pendingGenerated_;
                if(const auto* field=runtime_.engine().parameters().find(edit.id))
                    if(SetParameterNormalizedValue(field->key.c_str(),field->toNormalized(edit.value))!=AAX_SUCCESS) {
                        pendingGenerated_=edit;hasPendingGenerated_=true;break;
                    }
                hasPendingGenerated_=false;
            }
            const auto latency=runtime_.engine().processor().latency();
            int32_t reported{};
            if(Controller()->GetSignalLatency(&reported)==AAX_SUCCESS&&reported>=0&&std::uint32_t(reported)!=latency)
                Controller()->SetSignalLatency(latency);
            return AAX_CEffectParameters::TimerWakeup();
        } catch(...) {return failed;}
    }
    AAX_Result GetNumberOfChunks(int32_t* count) const override {if(!count)return AAX_ERROR_NULL_ARGUMENT;*count=1;return AAX_SUCCESS;}
    AAX_Result GetChunkIDFromIndex(int32_t index,AAX_CTypeID* id) const override {
        if(!id)return AAX_ERROR_NULL_ARGUMENT;if(index)return AAX_ERROR_INVALID_CHUNK_INDEX;*id=CELL_AAX_CHUNK_ID;return AAX_SUCCESS;
    }
    AAX_Result GetChunkSize(AAX_CTypeID id,uint32_t* size) const override {
        if(id!=CELL_AAX_CHUNK_ID)return AAX_ERROR_INVALID_CHUNK_ID;if(!size)return AAX_ERROR_NULL_ARGUMENT;
        std::lock_guard lock(chunkMutex_);
        if(runtime_.saveState(saved_)!=Result::ok)return failed;
        *size=uint32_t(saved_.size());savedValid_=true;return AAX_SUCCESS;
    }
    AAX_Result GetChunk(AAX_CTypeID id,AAX_SPlugInChunk* chunk) const override {
        if(id!=CELL_AAX_CHUNK_ID)return AAX_ERROR_INVALID_CHUNK_ID;if(!chunk)return AAX_ERROR_NULL_ARGUMENT;
        std::lock_guard lock(chunkMutex_);if(!savedValid_)return failed;
        chunk->fVersion=1;chunk->fChunkID=id;chunk->fSize=int32_t(saved_.size());
        chunk->fManufacturerID=CELL_AAX_MANUFACTURER_ID;chunk->fProductID=CELL_AAX_PRODUCT_ID;
        chunk->fPlugInID=channels_==1?CELL_AAX_MONO_ID:CELL_AAX_STEREO_ID;std::memset(chunk->fName,0,sizeof(chunk->fName));
        if(!saved_.empty())std::memcpy(chunk->fData,saved_.data(),saved_.size());
        return AAX_SUCCESS;
    }
    AAX_Result SetChunk(AAX_CTypeID id,const AAX_SPlugInChunk* chunk) override {
        if(id!=CELL_AAX_CHUNK_ID)return AAX_ERROR_INVALID_CHUNK_ID;
        if(!chunk||chunk->fChunkID!=id||chunk->fSize<0||std::uint32_t(chunk->fSize)>wrapper::Limits::stateBytes)return AAX_ERROR_MALFORMED_CHUNK;
        return restore({reinterpret_cast<const std::byte*>(chunk->fData),std::size_t(chunk->fSize)});
    }
    AAX_Result CompareActiveChunk(const AAX_SPlugInChunk* chunk,AAX_CBoolean* equal) const override {
        if(!chunk||!equal)return AAX_ERROR_NULL_ARGUMENT;
        if(chunk->fChunkID!=CELL_AAX_CHUNK_ID||chunk->fSize<0||std::uint32_t(chunk->fSize)>wrapper::Limits::stateBytes)return AAX_ERROR_MALFORMED_CHUNK;
        std::vector<std::byte> current;if(runtime_.saveState(current)!=Result::ok)return failed;
        *equal=current.size()==std::size_t(chunk->fSize)&&(current.empty()||std::memcmp(current.data(),chunk->fData,current.size())==0);return AAX_SUCCESS;
    }
    AAX_Result GetNumberOfChanges(int32_t* count) const override {
        if(!count)return AAX_ERROR_NULL_ARGUMENT;const auto result=AAX_CEffectParameters::GetNumberOfChanges(count);
        if(result==AAX_SUCCESS)*count=int32_t((std::uint32_t(*count)+changes_.load())&0x7fffffffu);return result;
    }
    AAX_Result GetCustomData(AAX_CTypeID id,uint32_t size,void* data,uint32_t* written) const override {
        if(!data||!written)return AAX_ERROR_NULL_ARGUMENT;*written=0;
        if(id==stateRevision) {
            if(size!=sizeof(std::uint32_t))return failed;const auto revision=changes_.load();
            std::memcpy(data,&revision,sizeof(revision));*written=sizeof(revision);return AAX_SUCCESS;
        }
        if(id==analysisData) {
            if(size!=sizeof(AnalysisFrame))return failed;
            auto* source=dynamic_cast<AnalysisSource*>(&runtime_.engine().processor());AnalysisFrame frame;
            if(!source||!source->pullAnalysis(frame))return AAX_SUCCESS;
            frame.latency=runtime_.engine().processor().latency();std::memcpy(data,&frame,sizeof(frame));*written=sizeof(frame);return AAX_SUCCESS;
        }
        if(id==queryData) {
            std::lock_guard lock(queryMutex_);if(reply_.size()>size)return failed;
            if(!reply_.empty())std::memcpy(data,reply_.data(),reply_.size());*written=uint32_t(reply_.size());return AAX_SUCCESS;
        }
        return AAX_CEffectParameters::GetCustomData(id,size,data,written);
    }
    AAX_Result SetCustomData(AAX_CTypeID id,uint32_t size,const void* data) override {
        if(!data&&size)return AAX_ERROR_NULL_ARGUMENT;
        try {
            const std::span<const std::byte> bytes{static_cast<const std::byte*>(data),size};
            if(id==restoreData)return restore(bytes);
            if(id==analysisEnable) {
                if(size!=sizeof(std::uint32_t))return failed;
                std::uint32_t enabled{};std::memcpy(&enabled,data,sizeof(enabled));if(enabled>1)return failed;
                if(auto* source=dynamic_cast<AnalysisSource*>(&runtime_.engine().processor()))source->enableAnalysis(enabled!=0);
                return AAX_SUCCESS;
            }
            if(id==commandData||id==queryData||id==inputData) {
                if(bytes.empty()||bytes.size()>maxProductQueryBytes)return failed;
                if(id==commandData||id==inputData){if(!runtime_.engine().executeProductCommand(bytes))return failed;if(id==commandData)++changes_;return AAX_SUCCESS;}
                auto reply=runtime_.engine().queryProduct(bytes);if(reply.size()>maxProductReplyBytes)return failed;
                std::lock_guard lock(queryMutex_);reply_=std::move(reply);return AAX_SUCCESS;
            }
            return AAX_CEffectParameters::SetCustomData(id,size,data);
        } catch(...) {return failed;}
    }

    static void AAX_CALLBACK render(Context* const begin[],const void* rawEnd) noexcept {
        const auto* end=static_cast<Context* const*>(rawEnd);
        for(auto cursor=begin;cursor!=end;++cursor)
            if(*cursor&&(*cursor)->owner&&*(*cursor)->owner)(*(*cursor)->owner)->process(**cursor);
    }
private:
    AAX_Result restore(std::span<const std::byte> bytes) {
        std::lock_guard lock(model_);
        if(runtime_.loadStateAtIdle(bytes,&timeline_)!=Result::ok)return failed;
        // Quietly synchronize the AAX model after its owning product has
        // accepted the complete state. Older timed packets were cancelled.
        changed_.clear();std::fill(dirty_.begin(),dirty_.end(),false);
        for(const auto& field:runtime_.engine().parameters().descriptors())
            if(auto* parameter=mParameterManager.GetParameterByID(field.key.c_str()))
                parameter->UpdateNormalizedValue(field.toNormalized(runtime_.engine().parameters().get(field.id)));
        ++changes_;return AAX_SUCCESS;
    }
    cell::Transport readTransport() noexcept {
        cell::Transport result;result.offline=offline_.load();result.recording=recording_.load();result.tempoValid=false;
        if(auto* transport=Transport()) {
            transport->IsTransportPlaying(&result.playing);double bpm=120;
            if(transport->GetCurrentTempo(&bpm)==AAX_SUCCESS&&std::isfinite(bpm)&&bpm>0){result.tempo=bpm;result.tempoValid=true;}
            result.samplePositionValid=transport->GetCurrentNativeSampleLocation(&result.samplePosition)==AAX_SUCCESS;
            int64_t ticks{},start{},end{};uint32_t perQuarter=960000;
            if(transport->GetCurrentTickPosition(&ticks)==AAX_SUCCESS) {
                const auto status=transport->GetTicksPerQuarter(&perQuarter);
                if(perQuarter){result.beatPosition=double(ticks)/perQuarter;result.beatPositionValid=status==AAX_SUCCESS;}
            }
            transport->GetCurrentLoopPosition(&result.looping,&start,&end);
            int32_t numerator=4,denominator=4;
            if(transport->GetCurrentMeter(&numerator,&denominator)==AAX_SUCCESS&&numerator>0&&numerator<=65535&&denominator>0&&denominator<=65535) {
                result.numerator=std::uint16_t(numerator);result.denominator=std::uint16_t(denominator);
            }
        }
        return result;
    }
    void process(Context& context) noexcept {
        if(!context.frames||*context.frames<0||!context.token)return;
        const auto frames=std::uint32_t(*context.frames);
        std::array<const float*,2> input{};std::array<float*,2> output{};
        for(unsigned channel=0;channel<channels_;++channel) {
            input[channel]=context.input?context.input[channel]:nullptr;output[channel]=context.output?context.output[channel]:nullptr;
        }
        std::array<const float*,1> side{};
        if(plugin::descriptor().supportsSidechain&&context.input&&context.sidechain&&*context.sidechain>=int32_t(channels_))side[0]=context.input[*context.sidechain];
        std::size_t count{};bool overflow=false;
        if(plugin::descriptor().acceptsMidi&&context.midi)if(const auto* stream=context.midi->GetNodeBuffer()) {
            if(stream->mBufferSize>65536||(stream->mBufferSize&&!stream->mBuffer))overflow=true;
            else for(uint32_t index=0;index<stream->mBufferSize;++index) {
                const auto& packet=stream->mBuffer[index];const auto status=packet.mData[0];
                if(status<0x80||status>=0xf0||packet.mTimestamp>=frames)continue;
                const auto length=std::uint8_t((status&0xf0)==0xc0||(status&0xf0)==0xd0?2:3);
                if(packet.mLength!=length)continue;
                if(count==midi_.size()){overflow=true;break;}
                midi_[count++]={packet.mTimestamp,{status,packet.mData[1],length==3?packet.mData[2]:std::uint8_t(0)},length};
            }
        }
        if(overflow) {
            runtime_.requestMidiPanic();if(frames<=wrapper::Limits::hostFrames)for(unsigned channel=0;channel<channels_;++channel)if(output[channel])std::fill_n(output[channel],frames,0.f);
            return;
        }
        const auto result=runtime_.process({frames,{input.data(),channels_},{output.data(),channels_},
            side[0]?std::span<const float* const>(side):std::span<const float* const>{},{},{midi_.data(),count},readTransport(),&timeline_,*context.token});
        (void)result;
    }
    mutable wrapper::Runtime runtime_{plugin::descriptor(),plugin::createProcessor()};
    wrapper::ParameterTimeline timeline_;
    std::unordered_map<std::string_view,std::size_t> byKey_;
    std::mutex model_;std::vector<bool> dirty_;std::vector<std::size_t> changed_;
    std::vector<wrapper::ParameterTimeline::Value> values_;
    std::uint64_t token_{};bool needsPost_=true;
    std::uint32_t channels_=2;std::string bypassKey_;
    std::array<MidiEvent,wrapper::Limits::midi> midi_{};
    ParameterEdit pendingGenerated_{};bool hasPendingGenerated_{};
    std::atomic<bool> offline_{},recording_{};std::atomic<std::uint32_t> changes_{};
    mutable std::mutex chunkMutex_,queryMutex_;mutable std::vector<std::byte> saved_;mutable bool savedValid_{};
    std::vector<std::byte> reply_;
};

#if CELL_HAS_UI
class Gui final : public AAX_CEffectGUI {
public:
    Gui() {
        const auto fields=projection_.parameters().descriptors();byKey_.reserve(fields.size());
        for(std::size_t index=0;index<fields.size();++index)byKey_.emplace(fields[index].key,index);
    }
    static AAX_CEffectGUI* AAX_CALLBACK create() noexcept {try{return new Gui;}catch(...){return nullptr;}}
    ~Gui() override {DeleteViewContainer();}
    AAX_Result GetViewSize(AAX_Point* size) const override {
        if(!size)return AAX_ERROR_NULL_ARGUMENT;size->horz=float(width_);size->vert=float(height_);return AAX_SUCCESS;
    }
    AAX_Result TimerWakeup() override {
        try{flushUi();if(window_&&window_->isOpen())window_->poll();return AAX_SUCCESS;}catch(...){return failed;}
    }
    AAX_Result ParameterUpdated(AAX_CParamID key) override {
        if(!key)return AAX_ERROR_INVALID_PARAMETER_ID;
        const auto found=byKey_.find(key);
        if(found!=byKey_.end()) {
            const auto& field=projection_.parameters().descriptors()[found->second];
            double normalized{};if(GetEffectParameters()->GetParameterNormalizedValue(key,&normalized)==AAX_SUCCESS)
                projection_.parameters().set(field.id,field.fromNormalized(normalized));
            if(editor_&&editor_->invalidate)editor_->invalidate();
        }
        return AAX_SUCCESS;
    }
protected:
    void CreateViewContents() override {
        try {
            for(const auto& field:projection_.parameters().descriptors()) {
                double value{};if(GetEffectParameters()->GetParameterNormalizedValue(field.key.c_str(),&value)==AAX_SUCCESS)
                    projection_.parameters().set(field.id,field.fromNormalized(value));
            }
            editor_=plugin::createEditor(projection_);if(!editor_)return;
            const auto size=editor_->preferredSize();width_=int(size.x);height_=int(size.y);
            editor_->editsAvailable=[this]{flushUi();};
            editor_->hostParameterMenuAvailable=[this](ParamId id){const auto* field=projection_.parameters().find(id);return field&&field->automatable&&window_&&window_->isOpen()&&GetViewContainer();};
            editor_->showHostParameterMenu=[this](ParamId id,ui::Point){
                if(!editor_->hostParameterMenuAvailable(id))return false;const auto* field=projection_.parameters().find(id);
                // AAX routes its documented automation-popup modifier gesture
                // through the view container; the host owns the popup itself.
                return GetViewContainer()->HandleParameterMouseDown(field->key.c_str(),AAX_eModifiers_Command|AAX_eModifiers_Control|AAX_eModifiers_Option)==AAX_SUCCESS;
            };
            editor_->productCommand=[this](std::span<const std::byte> bytes){return bytes.size()<=maxProductQueryBytes&&GetEffectParameters()->SetCustomData(commandData,uint32_t(bytes.size()),bytes.data())==AAX_SUCCESS;};
            editor_->transientInput=[this](std::span<const std::byte> bytes){return !bytes.empty()&&bytes.size()<=maxProductQueryBytes&&GetEffectParameters()->SetCustomData(inputData,uint32_t(bytes.size()),bytes.data())==AAX_SUCCESS;};
            editor_->productQuery=[this](std::span<const std::byte> bytes,bool){
                std::vector<std::byte> reply;
                if(bytes.empty()||bytes.size()>maxProductQueryBytes||GetEffectParameters()->SetCustomData(queryData,uint32_t(bytes.size()),bytes.data())!=AAX_SUCCESS)return reply;
                reply.resize(maxProductReplyBytes);uint32_t written{};
                if(GetEffectParameters()->GetCustomData(queryData,uint32_t(reply.size()),reply.data(),&written)!=AAX_SUCCESS||written>reply.size())return std::vector<std::byte>{};
                reply.resize(written);return reply;
            };
            editor_->restoreProductState=[this](std::span<const std::byte> bytes){
                if(bytes.size()>wrapper::Limits::stateBytes||GetEffectParameters()->SetCustomData(restoreData,uint32_t(bytes.size()),bytes.data())!=AAX_SUCCESS)return false;
                return projection_.loadState(bytes);
            };
            editor_->synchronize=[this]{
                std::uint32_t revision{},bytes{};
                if(GetEffectParameters()->GetCustomData(stateRevision,sizeof(revision),&revision,&bytes)==AAX_SUCCESS&&bytes==sizeof(revision)&&revision!=revision_) {
                    for(const auto& field:projection_.parameters().descriptors()) {
                        double value{};if(GetEffectParameters()->GetParameterNormalizedValue(field.key.c_str(),&value)==AAX_SUCCESS)
                            projection_.parameters().set(field.id,field.fromNormalized(value));
                    }
                    revision_=revision;
                }
                auto* target=dynamic_cast<AnalysisSource*>(&projection_.processor());if(!target)return;
                AnalysisFrame frame;uint32_t written{};
                if(GetEffectParameters()->GetCustomData(analysisData,sizeof(frame),&frame,&written)==AAX_SUCCESS&&written==sizeof(frame)) {
                    target->setAnalysisConnection(AnalysisConnection::host);target->receiveAnalysis(frame);
                }
            };
            editor_->visibilityChanged=[this](bool visible){const std::uint32_t enabled=visible;GetEffectParameters()->SetCustomData(analysisEnable,sizeof(enabled),&enabled);};
            editor_->requestSize=[this](int width,int height){
                if(width<1||height<1||width>9600||height>7200||!GetViewContainer())return false;
                AAX_Point size;size.horz=float(width);size.vert=float(height);
                if(GetViewContainer()->SetViewSize(size)!=AAX_SUCCESS)return false;
                if(window_&&window_->isOpen()&&!window_->resize(width,height))return false;
                width_=width;height_=height;return true;
            };
        } catch(...) {editor_.reset();}
    }
    void CreateViewContainer() override {
        try {
            auto* container=GetViewContainer();if(!editor_||!container||!container->GetPtr())return;
#if defined(_WIN32)
            if(container->GetType()!=AAX_eViewContainer_Type_HWND)return;
#elif defined(__APPLE__)
            if(container->GetType()!=AAX_eViewContainer_Type_NSView)return;
#else
            return; // AAX Native is only packaged for supported macOS/Windows hosts.
#endif
            window_=std::make_unique<ui::Window>(*editor_);
            if(!window_->open(reinterpret_cast<std::uintptr_t>(container->GetPtr()),width_,height_)){window_.reset();return;}
            window_->show(true);
        } catch(...) {window_.reset();}
    }
    void DeleteViewContainer() override {
        if(window_){window_->show(false);window_->close();window_.reset();}
    }
private:
    void flushUi() {
        auto* model=GetEffectParameters();if(!model)return;
        ParameterEdit edit;
        while(hasPendingEdit_||projection_.pollUiEdit(edit)) {
            if(hasPendingEdit_)edit=pendingEdit_;
            if(const auto* field=projection_.parameters().find(edit.id)) {
                const auto result=edit.type==EditType::begin?model->TouchParameter(field->key.c_str()):
                    edit.type==EditType::end?model->ReleaseParameter(field->key.c_str()):
                    model->SetParameterNormalizedValue(field->key.c_str(),field->toNormalized(edit.value));
                if(result!=AAX_SUCCESS){pendingEdit_=edit;hasPendingEdit_=true;return;}
            }
            hasPendingEdit_=false;
        }
    }
    Engine projection_{plugin::descriptor(),plugin::createProcessor()};
    std::unique_ptr<ui::Editor> editor_;std::unique_ptr<ui::Window> window_;
    std::unordered_map<std::string_view,std::size_t> byKey_;std::uint32_t revision_{};
    ParameterEdit pendingEdit_{};bool hasPendingEdit_{};
    int width_=int(plugin::descriptor().editorWidth),height_=int(plugin::descriptor().editorHeight);
};
#endif

AAX_Result describe(AAX_IEffectDescriptor& effect,AAX_EStemFormat stem,AAX_CTypeID nativeId) {
    auto* component=effect.NewComponentDescriptor();if(!component)return AAX_ERROR_NULL_OBJECT;
    auto check=[](AAX_Result result){if(result!=AAX_SUCCESS)throw result;};
    check(component->AddAudioIn(inputField));check(component->AddAudioOut(outputField));check(component->AddAudioBufferLength(framesField));
    check(component->AddPrivateData(ownerField,sizeof(Parameters*)));check(component->AddDataInPort(tokenField,sizeof(std::uint64_t)));
    if(plugin::descriptor().supportsSidechain)check(component->AddSideChainIn(sideField));
    if(plugin::descriptor().acceptsMidi)check(component->AddMIDINode(midiField,AAX_eMIDINodeType_LocalInput,"MIDI Input",0xffff));
    auto* properties=component->NewPropertyMap();if(!properties)return AAX_ERROR_NULL_OBJECT;
    check(properties->AddProperty(AAX_eProperty_ManufacturerID,CELL_AAX_MANUFACTURER_ID));
    check(properties->AddProperty(AAX_eProperty_ProductID,CELL_AAX_PRODUCT_ID));check(properties->AddProperty(AAX_eProperty_PlugInID_Native,nativeId));
    check(properties->AddProperty(AAX_eProperty_InputStemFormat,stem));check(properties->AddProperty(AAX_eProperty_OutputStemFormat,stem));
    bool bypass=false;for(const auto& field:plugin::descriptor().parameters)bypass|=field.bypass;
    check(properties->AddProperty(AAX_eProperty_CanBypass,bypass));check(properties->AddProperty(AAX_eProperty_UsesClientGUI,!CELL_HAS_UI));
    check(properties->AddProperty(AAX_eProperty_Constraint_Location,AAX_eConstraintLocationMask_DataModel));
    check(properties->AddProperty(AAX_eProperty_ObservesTransportState,true));
    check(component->AddProcessProc_Native(Parameters::render,properties));return effect.AddComponent(component);
}

std::string shortPackageName(std::string_view name) {
    constexpr std::size_t limit = 16;
    auto end = std::min(name.size(), limit);
    while (end > 0 && end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
        --end;
    return std::string(name.substr(0, end));
}

} // namespace

// The SDK's own AAX_Init entry points call this descriptor function.
AAX_Result GetEffectDescriptions(AAX_ICollection* collection) {
    if(!collection)return AAX_ERROR_NULL_ARGUMENT;
    try {
        const auto& info=cell::plugin::descriptor();
        if(info.channels!=1&&info.channels!=2)return failed;
        auto* effect=collection->NewDescriptor();if(!effect)return AAX_ERROR_NULL_OBJECT;
        auto check=[](AAX_Result result){if(result!=AAX_SUCCESS)throw result;};
        if (!info.name || !*info.name)
            return AAX_ERROR_INVALID_ARGUMENT;
        const std::string_view name(info.name);
        const auto shortName = shortPackageName(name);
        if (shortName.empty())
            return AAX_ERROR_INVALID_ARGUMENT;
        check(effect->AddName(info.name));
        if (name.size() > 31)
            check(effect->AddName(shortName.c_str()));
        check(effect->AddCategory(AAX_ePlugInCategory_None));
        check(describe(*effect,info.channels==1?AAX_eStemFormat_Mono:AAX_eStemFormat_Stereo,info.channels==1?CELL_AAX_MONO_ID:CELL_AAX_STEREO_ID));
        if(info.supportsMono&&info.channels!=1)check(describe(*effect,AAX_eStemFormat_Mono,CELL_AAX_MONO_ID));
        check(effect->AddProcPtr(reinterpret_cast<void*>(Parameters::create),kAAX_ProcPtrID_Create_EffectParameters));
#if CELL_HAS_UI
        check(effect->AddProcPtr(reinterpret_cast<void*>(Gui::create),kAAX_ProcPtrID_Create_EffectGUI));
#endif
        check(collection->AddEffect(info.id,effect));check(collection->SetManufacturerName(info.vendor));
        check(collection->AddPackageName(info.name));
        if (shortName.size() != name.size())
            check(collection->AddPackageName(shortName.c_str()));
        return collection->SetPackageVersion(CELL_PLUGIN_VERSION_CODE);
    } catch(AAX_Result result) {return result;} catch(...) {return failed;}
}
