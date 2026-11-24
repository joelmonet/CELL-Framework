// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#include <memory>
#include <vector>
namespace cell::history {
class Action {
 public:virtual~Action()=default;virtual bool perform()=0;virtual bool undo()=0;
 virtual int getSizeInUnits(){return 1;}
 // The returned action takes ownership of neither argument. It must reproduce
 // their combined before/after state, without mutating the document.
 virtual Action*createCoalescedAction(Action*){return nullptr;}
};
// Control-thread delta history. Actions share immutable source snapshots; this
// manager never serializes a project, copies PCM, or walks old entries per edit.
// There is no fixed transaction count. Application resource policy owns budgets.
class ActionHistory {
 struct Transaction{Text label;std::vector<std::unique_ptr<Action>>actions;std::size_t units{};};
 std::vector<Transaction>entries_;std::size_t cursor_{},units_{};
 Text pendingLabel_;bool newTransaction_=true,active_{},clearPending_{};
 struct Operation{ActionHistory&h;explicit Operation(ActionHistory&v):h(v){h.active_=true;}~Operation(){h.active_=false;if(h.clearPending_)h.clearUndoHistory();}};
 public:
 enum class Error{none,actionFailed,rollbackFailed,reentrant};
 private:Error error_=Error::none;
 public:
 ActionHistory()=default;~ActionHistory()=default;ActionHistory(const ActionHistory&)=delete;ActionHistory&operator=(const ActionHistory&)=delete;
 void beginNewTransaction(Text label={}){pendingLabel_=std::move(label);newTransaction_=true;}
 bool perform(Action*);bool undo();bool redo();
 bool canUndo()const noexcept{return cursor_>0;}bool canRedo()const noexcept{return cursor_<entries_.size();}
 void clearUndoHistory();Text getUndoDescription()const{return canUndo()?entries_[cursor_-1].label:Text{};}
 Text getRedoDescription()const{return canRedo()?entries_[cursor_].label:Text{};}
 std::vector<Text>getUndoDescriptions()const;std::vector<Text>getRedoDescriptions()const;
 std::size_t getNumActionsInCurrentTransaction()const noexcept{return !newTransaction_&&canUndo()?entries_[cursor_-1].actions.size():0;}
 std::size_t getNumberOfUnitsTakenUpByStoredCommands()const noexcept{return units_;}
 std::size_t transactionCount()const noexcept{return entries_.size();}Error lastError()const noexcept{return error_;}
};
}
