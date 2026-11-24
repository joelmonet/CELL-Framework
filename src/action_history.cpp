// Original Cell Framework implementation; see LICENSE.
#include <cell/action_history.hpp>
#include <limits>
#include <stdexcept>
namespace cell::history {
namespace {template<class T>void reserveSlot(std::vector<T>&values,std::size_t needed){if(needed<=values.capacity())return;const auto spare=values.max_size()-values.capacity(),growth=std::min(spare,values.capacity()/2+1);values.reserve(std::max(needed,values.capacity()+growth));}
std::size_t actionUnits(Action&a){return std::size_t(std::max(0,a.getSizeInUnits()));}void addUnits(std::size_t&to,std::size_t n){if(n>std::numeric_limits<std::size_t>::max()-to)throw std::length_error("History accounting overflow");to+=n;}}
bool ActionHistory::perform(Action*raw){std::unique_ptr<Action>action(raw);if(!action)return false;if(active_){error_=Error::reentrant;return false;}Operation operation(*this);error_=Error::none;
 const bool append=newTransaction_||!cursor_||cursor_!=entries_.size();Transaction candidate;const auto units=actionUnits(*action);if(units>std::numeric_limits<std::size_t>::max()-units_)throw std::length_error("History accounting overflow");
 // Reserve every container slot before mutating the document. Failed admission
 // leaves the existing redo branch and current transaction untouched.
 if(append){candidate.label=pendingLabel_;candidate.actions.reserve(1);reserveSlot(entries_,cursor_+1);}else reserveSlot(entries_[cursor_-1].actions,entries_[cursor_-1].actions.size()+1);
 if(!action->perform()){error_=Error::actionFailed;return false;}
 if(append){while(entries_.size()>cursor_){units_-=entries_.back().units;entries_.pop_back();}candidate.units=units;candidate.actions.push_back(std::move(action));entries_.push_back(std::move(candidate));++cursor_;addUnits(units_,units);newTransaction_=false;return true;}
 auto&entry=entries_[cursor_-1];std::unique_ptr<Action>merged;
 try{if(!entry.actions.empty())merged.reset(entry.actions.back()->createCoalescedAction(action.get()));}catch(const std::bad_alloc&){/* Coalescing is optional; the reserved delta slot remains available. */}
 if(merged){const auto prior=actionUnits(*entry.actions.back()),next=actionUnits(*merged);entry.units-=prior;units_-=prior;addUnits(entry.units,next);addUnits(units_,next);entry.actions.back()=std::move(merged);}
 else {entry.actions.push_back(std::move(action));addUnits(entry.units,units);addUnits(units_,units);}return true;
}
bool ActionHistory::undo(){if(active_){error_=Error::reentrant;return false;}if(!canUndo())return false;Operation operation(*this);error_=Error::none;auto&actions=entries_[cursor_-1].actions;
 for(std::size_t at=actions.size();at>0;--at)if(!actions[at-1]->undo()){error_=Error::actionFailed;for(std::size_t restore=at;restore<actions.size();++restore)if(!actions[restore]->perform())error_=Error::rollbackFailed;return false;}
 --cursor_;newTransaction_=true;return true;
}
bool ActionHistory::redo(){if(active_){error_=Error::reentrant;return false;}if(!canRedo())return false;Operation operation(*this);error_=Error::none;auto&actions=entries_[cursor_].actions;
 for(std::size_t at=0;at<actions.size();++at)if(!actions[at]->perform()){error_=Error::actionFailed;for(std::size_t restore=at;restore>0;--restore)if(!actions[restore-1]->undo())error_=Error::rollbackFailed;return false;}
 ++cursor_;newTransaction_=true;return true;
}
void ActionHistory::clearUndoHistory(){if(active_){clearPending_=true;return;}entries_.clear();cursor_=units_=0;newTransaction_=true;clearPending_=false;pendingLabel_={};error_=Error::none;}
std::vector<Text>ActionHistory::getUndoDescriptions()const{std::vector<Text>result;result.reserve(cursor_);for(auto at=cursor_;at>0;--at)result.push_back(entries_[at-1].label);return result;}
std::vector<Text>ActionHistory::getRedoDescriptions()const{std::vector<Text>result;result.reserve(entries_.size()-cursor_);for(auto at=cursor_;at<entries_.size();++at)result.push_back(entries_[at].label);return result;}
}
