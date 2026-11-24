// Original Cell Framework implementation; see LICENSE.
#include <cell/core.hpp>
#include <AAX.h>
#include <AAX_Errors.h>
#include <AAX_CEffectParameters.h>
#include <AAX_ICollection.h>
#include <AAX_IACFController.h>
#include <AAX_UIDs.h>
#include <acfextras.h>
#include <AAX_IComponentDescriptor.h>
#include <AAX_IEffectDescriptor.h>
#include <AAX_IPropertyMap.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

AAX_Result GetEffectDescriptions(AAX_ICollection*);

namespace {
constexpr auto unsupported = AAX_ERROR_UNIMPLEMENTED;

struct Properties final : AAX_IPropertyMap {
    std::map<AAX_EProperty, AAX_CPropertyValue> values;
    AAX_CBoolean GetProperty(AAX_EProperty key, AAX_CPropertyValue* value) const override {
        const auto found = values.find(key);
        if (!value || found == values.end())
            return false;
        *value = found->second;
        return true;
    }
    AAX_Result AddProperty(AAX_EProperty key, AAX_CPropertyValue value) override {
        return values.emplace(key, value).second ? AAX_SUCCESS : AAX_ERROR_INVALID_ARGUMENT;
    }
    AAX_CBoolean GetPointerProperty(AAX_EProperty, const void**) const override { return false; }
    AAX_Result AddPointerProperty(AAX_EProperty, const void*) override { return unsupported; }
    AAX_Result AddPointerProperty(AAX_EProperty, const char*) override { return unsupported; }
    AAX_Result RemoveProperty(AAX_EProperty) override { return unsupported; }
    AAX_Result AddPropertyWithIDArray(AAX_EProperty, const AAX_SPlugInIdentifierTriad*, uint32_t) override { return unsupported; }
    AAX_CBoolean GetPropertyWithIDArray(AAX_EProperty, const AAX_SPlugInIdentifierTriad**, uint32_t*) const override { return false; }
    IACFUnknown* GetIUnknown() override { return nullptr; }
};

struct Component final : AAX_IComponentDescriptor {
    mutable Properties properties;
    std::set<AAX_CFieldIndex> fields;
    AAX_CProcessProc render{};
    AAX_CFieldIndex input{}, output{}, length{}, owner{}, token{};
    bool rejectRender{};

    AAX_Result field(AAX_CFieldIndex index) {
        return fields.insert(index).second ? AAX_SUCCESS : AAX_ERROR_INVALID_ARGUMENT;
    }

    AAX_Result AddAudioIn(AAX_CFieldIndex index) override { input = index; return field(index); }
    AAX_Result AddAudioOut(AAX_CFieldIndex index) override { output = index; return field(index); }
    AAX_Result AddAudioBufferLength(AAX_CFieldIndex index) override { length = index; return field(index); }
    AAX_Result AddDataInPort(AAX_CFieldIndex index, uint32_t bytes, AAX_EDataInPortType) override {
        token = index;
        return bytes == sizeof(uint64_t) ? field(index) : AAX_ERROR_INVALID_ARGUMENT;
    }
    AAX_Result AddPrivateData(AAX_CFieldIndex index, int32_t bytes, uint32_t) override {
        owner = index;
        return bytes == sizeof(void*) ? field(index) : AAX_ERROR_INVALID_ARGUMENT;
    }
    AAX_IPropertyMap* NewPropertyMap() const override { return &properties; }
    AAX_Result AddProcessProc_Native(AAX_CProcessProc procedure, AAX_IPropertyMap* map,
                                    AAX_CInstanceInitProc, AAX_CBackgroundProc, AAX_CSelector*) override {
        if (rejectRender)
            return AAX_ERROR_INVALID_ARGUMENT;
        if (!procedure || map != &properties)
            return AAX_ERROR_NULL_ARGUMENT;
        render = procedure;
        return AAX_SUCCESS;
    }

    AAX_Result Clear() override { return unsupported; }
    AAX_Result AddSampleRate(AAX_CFieldIndex) override { return unsupported; }
    AAX_Result AddClock(AAX_CFieldIndex) override { return unsupported; }
    AAX_Result AddSideChainIn(AAX_CFieldIndex) override { return unsupported; }
    AAX_Result AddAuxOutputStem(AAX_CFieldIndex, int32_t, const char*) override { return unsupported; }
    AAX_Result AddTemporaryData(AAX_CFieldIndex, uint32_t) override { return unsupported; }
    AAX_Result AddDmaInstance(AAX_CFieldIndex, AAX_IDma::EMode) override { return unsupported; }
    AAX_Result AddMeters(AAX_CFieldIndex, const AAX_CTypeID*, uint32_t) override { return unsupported; }
    AAX_Result AddMIDINode(AAX_CFieldIndex, AAX_EMIDINodeType, const char*, uint32_t) override { return unsupported; }
    AAX_Result AddReservedField(AAX_CFieldIndex, uint32_t) override { return unsupported; }
    AAX_IPropertyMap* DuplicatePropertyMap(AAX_IPropertyMap*) const override { return nullptr; }
    AAX_Result AddProcessProc_TI(const char*, const char*, AAX_IPropertyMap*,
                                const char*, const char*, AAX_CSelector*) override { return unsupported; }
    AAX_Result AddProcessProc(AAX_IPropertyMap*, AAX_CSelector*, int32_t) override { return unsupported; }
};

struct Effect final : AAX_IEffectDescriptor {
    std::array<Component, 2> components;
    std::size_t created{}, registered{};
    std::string name;
    using ParameterFactory = AAX_CEffectParameters* (AAX_CALLBACK *)() noexcept;
    ParameterFactory parameterFactory{};

    AAX_IComponentDescriptor* NewComponentDescriptor() override {
        return created < components.size() ? &components[created++] : nullptr;
    }
    AAX_Result AddComponent(AAX_IComponentDescriptor* component) override {
        if (registered >= created || component != &components[registered])
            return AAX_ERROR_INVALID_ARGUMENT;
        ++registered;
        return AAX_SUCCESS;
    }
    AAX_Result AddName(const char* effectName) override { name = effectName; return AAX_SUCCESS; }
    AAX_Result AddCategory(uint32_t) override { return AAX_SUCCESS; }
    AAX_Result AddProcPtr(void* pointer, AAX_CProcPtrID id) override {
        if (!pointer || id != kAAX_ProcPtrID_Create_EffectParameters || parameterFactory)
            return AAX_ERROR_INVALID_ARGUMENT;
        parameterFactory = reinterpret_cast<ParameterFactory>(pointer);
        return AAX_SUCCESS;
    }

    AAX_Result AddCategoryBypassParameter(uint32_t, AAX_CParamID) override { return unsupported; }
    AAX_IPropertyMap* NewPropertyMap() override { return nullptr; }
    AAX_Result SetProperties(AAX_IPropertyMap*) override { return unsupported; }
    AAX_Result AddResourceInfo(AAX_EResourceType, const char*) override { return unsupported; }
    AAX_Result AddMeterDescription(AAX_CTypeID, const char*, AAX_IPropertyMap*) override { return unsupported; }
    AAX_Result AddControlMIDINode(AAX_CTypeID, AAX_EMIDINodeType, const char*, uint32_t) override { return unsupported; }
};

struct Collection final : AAX_ICollection {
    Effect effect;
    std::string id, manufacturer;
    std::vector<std::string> packageNames;
    uint32_t version{};

    AAX_IEffectDescriptor* NewDescriptor() override { return &effect; }
    AAX_Result AddEffect(const char* effectId, AAX_IEffectDescriptor* descriptor) override {
        if (descriptor != &effect || !id.empty())
            return AAX_ERROR_INVALID_ARGUMENT;
        id = effectId;
        return AAX_SUCCESS;
    }
    AAX_Result SetManufacturerName(const char* name) override { manufacturer = name; return AAX_SUCCESS; }
    AAX_Result AddPackageName(const char* name) override { packageNames.emplace_back(name); return AAX_SUCCESS; }
    AAX_Result SetPackageVersion(uint32_t number) override { version = number; return AAX_SUCCESS; }
    AAX_IPropertyMap* NewPropertyMap() override { return nullptr; }
    AAX_Result SetProperties(AAX_IPropertyMap*) override { return unsupported; }
    AAX_Result GetHostVersion(uint32_t*) const override { return unsupported; }
    AAX_IDescriptionHost* DescriptionHost() override { return nullptr; }
    const AAX_IDescriptionHost* DescriptionHost() const override { return nullptr; }
    IACFDefinition* HostDefinition() const override { return nullptr; }
};

struct RenderController final : AAX_IACFController {
    AAX_EStemFormat layout{};
    AAX_CSampleRate rate{};
    int32_t latency{-1};
    acfUInt32 refs{1};
    AAX_CFieldIndex packetField{};
    std::uint64_t packetToken{};
    unsigned packetAttempts{};
    bool rejectPacket{};

    ACFRESULT ACFMETHODCALLTYPE QueryInterface(const acfIID& iid, void** out) override {
        *out = nullptr;
        if (iid != IID_IAAXControllerV1 && iid != IID_IACFUnknown)
            return ACF_E_NOINTERFACE;
        *out = static_cast<AAX_IACFController*>(this);
        AddRef();
        return ACF_OK;
    }
    acfUInt32 ACFMETHODCALLTYPE AddRef() override { return ++refs; }
    acfUInt32 ACFMETHODCALLTYPE Release() override { return --refs; }

    AAX_Result GetSampleRate(AAX_CSampleRate* out) const override { *out = rate; return AAX_SUCCESS; }
    AAX_Result GetInputStemFormat(AAX_EStemFormat* out) const override { *out = layout; return AAX_SUCCESS; }
    AAX_Result GetOutputStemFormat(AAX_EStemFormat* out) const override { *out = layout; return AAX_SUCCESS; }
    AAX_Result SetSignalLatency(int32_t value) override { latency = value; return AAX_SUCCESS; }
    AAX_Result GetEffectID(AAX_IString*) const override { return unsupported; }
    AAX_Result GetSignalLatency(int32_t*) const override { return unsupported; }
    AAX_Result GetCycleCount(AAX_EProperty, AAX_CPropertyValue*) const override { return unsupported; }
    AAX_Result GetTODLocation(AAX_CTimeOfDay*) const override { return unsupported; }
    AAX_Result SetCycleCount(AAX_EProperty*, AAX_CPropertyValue*, int32_t) override { return unsupported; }
    AAX_Result PostPacket(AAX_CFieldIndex field, const void* data, uint32_t size) override {
        if (!data || size != sizeof(packetToken))
            return AAX_ERROR_INVALID_ARGUMENT;
        ++packetAttempts;
        if (rejectPacket)
            return AAX_ERROR_FIFO_FULL;
        packetField = field;
        std::memcpy(&packetToken, data, sizeof(packetToken));
        return AAX_SUCCESS;
    }
    AAX_Result GetCurrentMeterValue(AAX_CTypeID, float*) const override { return unsupported; }
    AAX_Result GetMeterPeakValue(AAX_CTypeID, float*) const override { return unsupported; }
    AAX_Result ClearMeterPeakValue(AAX_CTypeID) const override { return unsupported; }
    AAX_Result GetMeterClipped(AAX_CTypeID, AAX_CBoolean*) const override { return unsupported; }
    AAX_Result ClearMeterClipped(AAX_CTypeID) const override { return unsupported; }
    AAX_Result GetMeterCount(uint32_t*) const override { return unsupported; }
    AAX_Result GetNextMIDIPacket(AAX_CFieldIndex*, AAX_CMidiPacket*) override { return unsupported; }
};

bool checkRender(Effect::ParameterFactory create, const Component& component,
                 AAX_EStemFormat layout, AAX_CSampleRate rate) {
    RenderController controller;
    controller.layout = layout;
    controller.rate = rate;
    const std::unique_ptr<AAX_CEffectParameters> parameters(create());
    if (!parameters)
        return false;
    const auto initialized = parameters->Initialize(&controller);
    const auto exercise = [&] {
        if (initialized != AAX_SUCCESS || controller.latency != 0)
            return false;
        void* owner{};
        std::uint64_t token{};
        if (parameters->ResetFieldData(component.owner, &owner, sizeof(owner)) != AAX_SUCCESS || !owner ||
            parameters->ResetFieldData(component.token, &token, sizeof(token)) != AAX_SUCCESS)
            return false;

        constexpr int32_t frames = 64;
        int32_t length = frames;
        std::array<float, frames> left{}, right{}, leftOut{}, rightOut{};
        for (int32_t n = 0; n < frames; ++n) {
            left[n] = float(n) / frames;
            right[n] = -left[n];
        }
        rightOut.fill(2);
        float* inputs[]{left.data(), right.data()};
        float* outputs[]{leftOut.data(), rightOut.data()};
        // AAX supplies a pointer slot for each registered field index.
        std::array<void*, 16> context{};
        if (component.fields.empty() || *component.fields.rbegin() >= context.size())
            return false;
        context[component.input] = inputs;
        context[component.output] = outputs;
        context[component.length] = &length;
        context[component.owner] = &owner;
        context[component.token] = &token;
        void* instances[]{nullptr, context.data()};
        component.render(instances, instances + 2);
        const auto unity = [&] {
            return leftOut == left && (layout == AAX_eStemFormat_Stereo ? rightOut == right :
                std::all_of(rightOut.begin(), rightOut.end(), [](float sample) { return sample == 2; }));
        };
        if (!unity() || parameters->UpdateParameterNormalizedValue(
                "gain", .75, AAX_eUpdateSource_Parameter) != AAX_SUCCESS)
            return false;
        controller.rejectPacket = true;
        if (parameters->GenerateCoefficients() != AAX_ERROR_FIFO_FULL)
            return false;
        component.render(instances, instances + 2);
        if (!unity())
            return false;
        controller.rejectPacket = false;
        if (parameters->GenerateCoefficients() != AAX_SUCCESS || controller.packetAttempts != 2 ||
            controller.packetField != component.token || controller.packetToken <= token)
            return false;
        token = controller.packetToken;
        for (unsigned block = 0; block < 20; ++block)
            component.render(instances, instances + 2);
        const float settled = std::pow(10.f, -6.f / 20.f);
        return std::abs(leftOut.back() - left.back() * settled) < 1e-6f &&
            (layout == AAX_eStemFormat_Stereo ?
                std::abs(rightOut.back() - right.back() * settled) < 1e-6f : rightOut.back() == 2);
    };
    const bool rendered = exercise();
    return parameters->Uninitialize() == AAX_SUCCESS && controller.refs == 1 && rendered;
}

bool checkStateFactory(Effect::ParameterFactory create) {
    const std::unique_ptr<AAX_CEffectParameters> parameters(create());
    if (!parameters)
        return false;

    constexpr auto chunkId = CELL_AAX_CHUNK_ID;
    int32_t count{};
    AAX_CTypeID id{};
    uint32_t bytes{};
    alignas(AAX_SPlugInChunk) std::array<std::byte, 512> storage{};
    auto* chunk = new (storage.data()) AAX_SPlugInChunk{};
    AAX_CBoolean equal{};
    if (parameters->GetNumberOfChunks(&count) != AAX_SUCCESS || count != 1 ||
        parameters->GetChunkIDFromIndex(1, &id) != AAX_ERROR_INVALID_CHUNK_INDEX ||
        parameters->GetChunkIDFromIndex(0, &id) != AAX_SUCCESS || id != chunkId ||
        parameters->GetChunkSize(chunkId, &bytes) != AAX_SUCCESS ||
        bytes == 0 || bytes > storage.size() - offsetof(AAX_SPlugInChunk, fData) ||
        parameters->GetChunk(chunkId, chunk) != AAX_SUCCESS ||
        chunk->fSize != static_cast<int32_t>(bytes) || chunk->fVersion != 1 ||
        chunk->fChunkID != chunkId || chunk->fManufacturerID != CELL_AAX_MANUFACTURER_ID ||
        chunk->fProductID != CELL_AAX_PRODUCT_ID || chunk->fPlugInID != CELL_AAX_STEREO_ID ||
        parameters->CompareActiveChunk(chunk, &equal) != AAX_SUCCESS || !equal)
        return false;

    // A corrupt serialized payload must be rejected without changing live state.
    chunk->fData[0] ^= 1;
    if (parameters->CompareActiveChunk(chunk, &equal) != AAX_SUCCESS || equal ||
        parameters->SetChunk(chunkId, chunk) == AAX_SUCCESS)
        return false;
    chunk->fData[0] ^= 1;
    if (parameters->CompareActiveChunk(chunk, &equal) != AAX_SUCCESS || !equal)
        return false;

    cell::ByteWriter edited;
    edited.u32(0x4c4c4543u); // CELL
    edited.u32(1);
    edited.u32(cell::stableId("com.example.cell.gain"));
    edited.u32(1);
    edited.u32(cell::stableId("gain"));
    edited.f64(-6);
    if (edited.bytes.size() != bytes)
        return false;
    std::memcpy(chunk->fData, edited.bytes.data(), bytes);
    return parameters->CompareActiveChunk(chunk, &equal) == AAX_SUCCESS && !equal &&
        parameters->SetChunk(chunkId, chunk) == AAX_SUCCESS &&
        parameters->CompareActiveChunk(chunk, &equal) == AAX_SUCCESS && equal;
}

bool matches(const Component& component, AAX_EStemFormat layout, AAX_CTypeID nativeId) {
    const auto value = [&component](AAX_EProperty key, AAX_CPropertyValue expected) {
        AAX_CPropertyValue actual{};
        return component.properties.GetProperty(key, &actual) && actual == expected;
    };
    return component.render && component.fields.size() == 5 &&
        value(AAX_eProperty_InputStemFormat, layout) && value(AAX_eProperty_OutputStemFormat, layout) &&
        value(AAX_eProperty_ManufacturerID, CELL_AAX_MANUFACTURER_ID) &&
        value(AAX_eProperty_ProductID, CELL_AAX_PRODUCT_ID) &&
        value(AAX_eProperty_PlugInID_Native, nativeId) &&
        value(AAX_eProperty_CanBypass, false) && value(AAX_eProperty_UsesClientGUI, true);
}
} // namespace

extern "C"
#if defined(_WIN32)
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
bool cell_aax_test_describe() noexcept {
    try {
        Collection collection;
        if (GetEffectDescriptions(nullptr) != AAX_ERROR_NULL_ARGUMENT ||
            GetEffectDescriptions(&collection) != AAX_SUCCESS ||
            collection.id != "com.example.cell.gain" || collection.manufacturer != "Cell SDK Example" ||
            collection.version != CELL_PLUGIN_VERSION_CODE || collection.effect.name != "Cell Gain Example" ||
            collection.packageNames.size() != 2 || collection.packageNames.front() != "Cell Gain Example" ||
            collection.packageNames.back().empty() || collection.packageNames.back().size() > 16 ||
            !collection.effect.parameterFactory || !checkStateFactory(collection.effect.parameterFactory) ||
            collection.effect.registered != 2 ||
            !matches(collection.effect.components[0], AAX_eStemFormat_Stereo, CELL_AAX_STEREO_ID) ||
            !matches(collection.effect.components[1], AAX_eStemFormat_Mono, CELL_AAX_MONO_ID))
            return false;
        if (!checkRender(collection.effect.parameterFactory, collection.effect.components[0], AAX_eStemFormat_Stereo, 48000) ||
            !checkRender(collection.effect.parameterFactory, collection.effect.components[1], AAX_eStemFormat_Mono, 96000))
            return false;
        Collection rejected;
        rejected.effect.components[0].rejectRender = true;
        return GetEffectDescriptions(&rejected) == AAX_ERROR_INVALID_ARGUMENT &&
            rejected.id.empty() && rejected.effect.registered == 0;
    } catch (...) {
        return false;
    }
}
