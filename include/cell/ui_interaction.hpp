#pragma once
#include <cell/ui.hpp>
#include <optional>

namespace cell::ui {

// GUI-thread ownership; device kind is part of the identity so a pen cannot
// take a mouse or touch contact with the same platform-specific numeric ID.
class GestureBroker {
public:
    static constexpr std::size_t maxPointers=16,maxParameters=4;
    explicit GestureBroker(Engine& engine):engine_(engine){}
    ~GestureBroker(){cancelAll();}
    GestureBroker(const GestureBroker&)=delete;
    GestureBroker& operator=(const GestureBroker&)=delete;
    bool begin(PointerIdentity pointer,std::span<const ParamId> ids)noexcept{
        if(find(pointer)||ids.empty()||ids.size()>maxParameters)return false;
        auto free=std::find_if(contacts_.begin(),contacts_.end(),[](const auto& item){return !item.count;});
        if(free==contacts_.end()||!engine_.beginEdits(ids))return false;
        free->pointer=pointer;free->generation=engine_.stateGeneration();free->count=ids.size();std::copy(ids.begin(),ids.end(),free->ids.begin());return true;
    }
    bool set(PointerIdentity pointer,std::span<const std::pair<ParamId,double>> values)noexcept{
        const auto* contact=find(pointer);if(contact&&contact->generation!=engine_.stateGeneration()){finish(pointer);return false;}if(!contact||values.empty()||values.size()>contact->count)return false;
        for(const auto& [id,value]:values){(void)value;if(std::find(contact->ids.begin(),contact->ids.begin()+contact->count,id)==contact->ids.begin()+contact->count)return false;}
        return engine_.editValues(values);
    }
    // Cancellation closes automation at its last accepted value. It does not
    // write the initial value back over a newer host automation event.
    bool finish(PointerIdentity pointer)noexcept{
        auto* contact=find(pointer);if(!contact)return false;
        if(!engine_.endEdits({contact->ids.data(),contact->count}))return false;
        contact->count=0;return true;
    }
    bool cancelAll()noexcept{bool ok=true;for(auto& contact:contacts_)if(contact.count)ok=finish(contact.pointer)&&ok;return ok;}
    bool active(PointerIdentity pointer)const noexcept{return find(pointer)!=nullptr;}
    std::size_t size()const noexcept{return std::count_if(contacts_.begin(),contacts_.end(),[](const auto& item){return item.count!=0;});}
private:
    struct Contact{PointerIdentity pointer;std::array<ParamId,maxParameters> ids{};std::size_t count{};std::uint64_t generation{};};
    Contact* find(PointerIdentity pointer)noexcept{for(auto& contact:contacts_)if(contact.count&&contact.pointer==pointer)return &contact;return nullptr;}
    const Contact* find(PointerIdentity pointer)const noexcept{for(const auto& contact:contacts_)if(contact.count&&contact.pointer==pointer)return &contact;return nullptr;}
    Engine& engine_;std::array<Contact,maxPointers> contacts_{};
};

// Select one axis at gesture start. Summing x/y makes diagonal pointer jitter
// cancel or double a rotary movement. No pixel dead zone or value quantization
// is applied here; callers retain the fractional value until commit succeeds.
class RotaryMotion {
public:
 double delta(double right,double up)noexcept{
  if(!std::isfinite(right)||!std::isfinite(up))return 0;
  if(!axis_&&(right||up))axis_=std::abs(right)>std::abs(up)?1:2;
  return axis_==1?right:axis_==2?up:0;
 }
 void reset()noexcept{axis_=0;}
private:int axis_{};
};

class ValueDrag {
public:
 enum class Scale {linear,logarithmic,stepped};
 ValueDrag(double value,double y,double minimum,double maximum,double speed,Scale scale=Scale::linear):value_(std::clamp(value,minimum,maximum)),lastY_(y),minimum_(minimum),maximum_(maximum),speed_(speed),scale_(scale){}
 double move(double y,bool fine=false)noexcept{
  if(!std::isfinite(y))return value();const double delta=(lastY_-y)*speed_*(fine?.1:1.);lastY_=y;
  value_=std::clamp(scale_==Scale::logarithmic?value_*std::exp(std::clamp(delta,-40.,40.)):value_+delta,minimum_,maximum_);return value();
 }
 double value()const noexcept{return scale_==Scale::stepped?std::round(value_):value_;}
private:
 double value_,lastY_,minimum_,maximum_,speed_;Scale scale_;
};

struct FocusTarget {std::uint64_t identity{};bool enabled=true;};
inline std::optional<std::size_t> moveFocus(std::span<const FocusTarget> targets,std::uint64_t current,bool reverse=false)noexcept{
    if(targets.empty())return {};
    std::size_t at=targets.size();for(std::size_t i=0;i<targets.size();++i)if(targets[i].identity==current){at=i;break;}
    if(at==targets.size())at=reverse?0:targets.size()-1;
    for(std::size_t step=0;step<targets.size();++step){at=reverse?(at?at-1:targets.size()-1):(at+1)%targets.size();if(targets[at].enabled&&targets[at].identity)return at;}
    return {};
}

enum class ParameterAction {reset,copy,paste,host};
class ParameterClipboard {
public:
    bool copy(const ParameterBank& bank,ParamId id){const auto* field=bank.find(id);if(!field)return false;const auto value=bank.get(id);if(!std::isfinite(value))return false;unit_=field->unit;value_=value;return true;}
    bool canPaste(const Parameter& field)const noexcept{return value_&&unit_==field.unit&&*value_>=field.minimum&&*value_<=field.maximum;}
    std::optional<double> paste(const Parameter& field)const noexcept{return canPaste(field)?std::optional<double>(field.clamp(*value_)):std::nullopt;}
private:
    std::optional<double> value_;std::string unit_;
};

} // namespace cell::ui
