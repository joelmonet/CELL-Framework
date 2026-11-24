// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <bit>
#include <cell/ksp.hpp>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>

namespace cell::ksp {
namespace {
enum class Op {
  push,
  load,
  store,
  polyLoad,
  polyStore,
  arrayLoad,
  arrayStore,
  cc,
  builtin,
  add,
  sub,
  mul,
  div,
  mod,
  neg,
  logicalNot,
  bitNot,
  eq,
  ne,
  lt,
  le,
  gt,
  ge,
  logicalAnd,
  logicalOr,
  bitAnd,
  bitOr,
  bitXor,
  jump,
  jumpFalse,
  call,
  drop,
  duplicate,
  halt
};
enum class Function {
  ignore,
  changeNote,
  changeVelocity,
  tune,
  volume,
  pan,
  allow,
  disallow,
  play,
  noteOff,
  wait,
  random,
  abs,
  min,
  max,
  inRange
};
enum class Builtin {
  eventId,
  eventNote,
  eventVelocity,
  ccNumber,
  uptime,
  noteHeld,
  callbackId
};
struct Instruction {
  Op op{};
  std::int32_t a{}, b{};
  unsigned line{};
};
enum class SymbolKind { global, poly, array, constant };
struct Symbol {
  SymbolKind kind{};
  unsigned index{}, length{};
  std::int32_t value{};
  bool constant{};
};
struct FunctionInfo {
  Function function;
  unsigned arguments;
  bool returns{};
};
const std::map<std::string, FunctionInfo, std::less<>> functions{
    {"ignore_event", {Function::ignore, 1}},
    {"change_note", {Function::changeNote, 2}},
    {"change_velo", {Function::changeVelocity, 2}},
    {"change_tune", {Function::tune, 3}},
    {"change_vol", {Function::volume, 3}},
    {"change_pan", {Function::pan, 3}},
    {"allow_group", {Function::allow, 1}},
    {"disallow_group", {Function::disallow, 1}},
    {"play_note", {Function::play, 4, true}},
    {"note_off", {Function::noteOff, 1}},
    {"wait", {Function::wait, 1}},
    {"random", {Function::random, 2, true}},
    {"abs", {Function::abs, 1, true}},
    {"min", {Function::min, 2, true}},
    {"max", {Function::max, 2, true}},
    {"in_range", {Function::inRange, 3, true}}};
const std::map<std::string, Builtin, std::less<>> builtins{
    {"$EVENT_ID", Builtin::eventId},
    {"$EVENT_NOTE", Builtin::eventNote},
    {"$EVENT_VELOCITY", Builtin::eventVelocity},
    {"$CC_NUM", Builtin::ccNumber},
    {"$ENGINE_UPTIME", Builtin::uptime},
    {"$NOTE_HELD", Builtin::noteHeld},
    {"$NI_CALLBACK_ID", Builtin::callbackId}};
struct Token {
  std::string text;
  unsigned line{};
};
void validateLimits(const Limits &l) {
  if (!l.sourceBytes || l.sourceBytes > 16 * 1024 * 1024 || !l.instructions ||
      l.instructions > 1048576 || !l.variables || l.variables > 128 ||
      !l.arrayValues || l.arrayValues > 131072 || !l.voices ||
      l.voices > 1024 || !l.callbacks || l.callbacks > 256 ||
      !l.instructionsPerBlock || !l.instructionsPerCallback)
    throw std::invalid_argument("Invalid KSP runtime limits");
}
std::vector<Token> lex(std::string_view source, const Limits &limits) {
  if (source.size() > limits.sourceBytes)
    throw CompileError(1, "source byte limit exceeded");
  std::vector<Token> result;
  unsigned line = 1;
  auto add = [&](std::string_view s) {
    if (result.size() >= limits.instructions * 8ull)
      throw CompileError(line, "token limit exceeded");
    result.push_back({std::string(s), line});
  };
  for (std::size_t p = 0; p < source.size();) {
    const char c = source[p];
    if (c == '\r' || c == ' ' || c == '\t') {
      ++p;
      continue;
    }
    if (c == '\n') {
      add("\n");
      ++line;
      ++p;
      continue;
    }
    if (c == '{') {
      const auto start = line;
      ++p;
      while (p < source.size() && source[p] != '}') {
        if (source[p++] == '\n')
          ++line;
      }
      if (p == source.size())
        throw CompileError(start, "unterminated comment");
      ++p;
      continue;
    }
    if (c == '/' && p + 1 < source.size() && source[p + 1] == '/') {
      while (p < source.size() && source[p] != '\n')
        ++p;
      continue;
    }
    if (c == '.' && source.substr(p, 3) == "...") {
      p += 3;
      while (p < source.size() &&
             (source[p] == ' ' || source[p] == '\t' || source[p] == '\r'))
        ++p;
      if (p == source.size() || source[p] != '\n')
        throw CompileError(line, "line continuation must end the line");
      ++line;
      ++p;
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
        c == '$' || c == '%') {
      const auto begin = p++;
      while (p < source.size()) {
        const char n = source[p];
        if (!((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
              (n >= '0' && n <= '9') || n == '_'))
          break;
        ++p;
      }
      add(source.substr(begin, p - begin));
      continue;
    }
    if (c >= '0' && c <= '9') {
      const auto begin = p++;
      while (p < source.size() && ((source[p] >= '0' && source[p] <= '9') || (source[p] >= 'a' && source[p] <= 'f') || (source[p] >= 'A' && source[p] <= 'F')))
        ++p;
      if(p<source.size()&&(source[p]=='h'||source[p]=='H'))++p;
      add(source.substr(begin, p - begin));
      continue;
    }
    if (c == '.') {
      const auto end = source.find('.', p + 1);
      if (end != source.npos) {
        add(source.substr(p, end - p + 1));
        p = end + 1;
        continue;
      }
    }
    if (p + 1 < source.size() &&
        (source.substr(p, 2) == ":=" || source.substr(p, 2) == "<=" ||
         source.substr(p, 2) == ">=")) {
      add(source.substr(p, 2));
      p += 2;
      continue;
    }
    if (std::string_view("()+-*/[]=#<>,").find(c) != std::string_view::npos) {
      add(source.substr(p++, 1));
      continue;
    }
    throw CompileError(line, "unsupported character or string syntax");
  }
  result.push_back({"\n", line});
  result.push_back({"<eof>", line});
  return result;
}
std::int32_t wrapped(std::uint32_t n) noexcept {
  return std::bit_cast<std::int32_t>(n);
}
} // namespace
struct ProgramData {
  std::vector<Instruction> code;
  std::map<std::string, Symbol, std::less<>> symbols;
  std::array<int, 4> entry{-1, -1, -1, -1};
  unsigned globals{}, poly{}, arrayCount{};
  std::array<std::int32_t, 128> polyDefaults{};
};
namespace {
class Compiler {
  Limits limits_;
  std::vector<Token> tokens_;
  std::size_t at_{};
  unsigned callback_{}, depth_{};
  std::map<std::string,std::size_t,std::less<>> userFunctions_;
  std::vector<std::vector<unsigned>> functionExits_;
  std::shared_ptr<ProgramData> p_ = std::make_shared<ProgramData>();
  const Token &token() const { return tokens_.at(at_); }
  bool is(std::string_view s) const { return token().text == s; }
  bool take(std::string_view s) {
    if (!is(s))
      return false;
    ++at_;
    return true;
  }
  [[noreturn]] void fail(std::string reason) const {
    throw CompileError(token().line, std::move(reason));
  }
  void require(std::string_view s) {
    if (!take(s))
      fail("expected '" + std::string(s) + "', got '" + token().text + "'");
  }
  void newlines() {
    while (take("\n")) {
    };
  }
  unsigned emit(Op op, std::int32_t a = 0, std::int32_t b = 0) {
    if (p_->code.size() >= limits_.instructions)
      fail("instruction limit exceeded");
    p_->code.push_back({op, a, b, token().line});
    return unsigned(p_->code.size() - 1);
  }
  std::int32_t literal() {
    const bool negative = take("-");
    take("+");
    std::uint32_t n{};
    const auto &s = token().text;
    const bool hex=!s.empty()&&(s.back()=='h'||s.back()=='H');const auto end=s.data()+s.size()-unsigned(hex);
    const auto parsed = std::from_chars(s.data(), end, n,hex?16:10);
    if (s.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != end)
      fail("integer constant expected");
    ++at_;
    return negative ? wrapped(0u - n) : wrapped(n);
  }
  Symbol symbol(std::string_view name) {
    auto it = p_->symbols.find(name);
    if (it == p_->symbols.end())
      fail("unknown variable '" + std::string(name) + "'");
    return it->second;
  }
  void load(Symbol s) {
    if (s.kind == SymbolKind::constant)
      emit(Op::push, s.value);
    else
      emit(s.kind == SymbolKind::poly ? Op::polyLoad : Op::load, int(s.index));
  }
  void store(Symbol s) {
    if (s.constant || s.kind == SymbolKind::constant)
      fail("cannot assign a constant");
    emit(s.kind == SymbolKind::poly ? Op::polyStore : Op::store, int(s.index));
  }
  static std::optional<std::pair<int, Op>> binary(std::string_view s) {
    if (s == "or")
      return {{1, Op::logicalOr}};
    if (s == "and")
      return {{2, Op::logicalAnd}};
    if (s == "=")
      return {{3, Op::eq}};
    if (s == "#")
      return {{3, Op::ne}};
    if (s == "<")
      return {{3, Op::lt}};
    if (s == "<=")
      return {{3, Op::le}};
    if (s == ">")
      return {{3, Op::gt}};
    if (s == ">=")
      return {{3, Op::ge}};
    if (s == ".or.")
      return {{4, Op::bitOr}};
    if (s == ".xor.")
      return {{4, Op::bitXor}};
    if (s == ".and.")
      return {{5, Op::bitAnd}};
    if (s == "+")
      return {{6, Op::add}};
    if (s == "-")
      return {{6, Op::sub}};
    if (s == "*")
      return {{7, Op::mul}};
    if (s == "/")
      return {{7, Op::div}};
    if (s == "mod")
      return {{7, Op::mod}};
    return {};
  }
  void expression(int minimum = 0) {
    if (++depth_ > 64)
      fail("expression nesting limit exceeded");
    if (take("-")) {
      expression(8);
      emit(Op::neg);
    } else if (take("+"))
      expression(8);
    else if (take("not")) {
      expression(8);
      emit(Op::logicalNot);
    } else if (take(".not.")) {
      expression(8);
      emit(Op::bitNot);
    } else if (take("(")) {
      expression();
      require(")");
    } else if (!token().text.empty() && token().text.front() >= '0' &&
               token().text.front() <= '9')
      emit(Op::push, literal());
    else {
      const auto name = token().text;
      ++at_;
      if (name == "$ALL_GROUPS" || name == "$ALL_EVENTS")
        emit(Op::push, -1);
      else if (auto b = builtins.find(name); b != builtins.end())
        emit(Op::builtin, int(b->second));
      else if (name == "%CC") {
        require("[");
        expression();
        require("]");
        emit(Op::cc);
      } else if (!name.empty() &&
                 (name.front() == '$' || name.front() == '%')) {
        const auto s = symbol(name);
        if (s.kind == SymbolKind::array) {
          require("[");
          expression();
          require("]");
          emit(Op::arrayLoad, int(s.index), int(s.length));
        } else
          load(s);
      } else
        function(name, true);
    }
    while (const auto op = binary(token().text)) {
      if (op->first < minimum)
        break;
      ++at_;
      expression(op->first + 1);
      emit(op->second);
    }
    --depth_;
  }
  void function(const std::string &name, bool value) {
    const auto it = functions.find(name);
    if (it == functions.end())
      fail("unsupported command '" + name + "'");
    const auto f = it->second;
    if (value && !f.returns)
      fail("command has no return value: " + name);
    if (callback_ == 0 &&
        (f.function == Function::play || f.function == Function::wait ||
         f.function == Function::allow || f.function == Function::disallow ||
         f.function == Function::ignore))
      fail("command is unavailable in on init: " + name);
    if ((f.function == Function::allow || f.function == Function::disallow) &&
        (callback_ != 1 && callback_ != 2 && callback_ != 4))
      fail("group selection requires on note/release");
    if ((f.function == Function::changeNote ||
         f.function == Function::changeVelocity) &&
        callback_ != 1 && callback_ != 4)
      fail("note/velocity changes require on note");
    require("(");
    for (unsigned n = 0; n < f.arguments; ++n) {
      if (n)
        require(",");
      expression();
    }
    require(")");
    emit(Op::call, int(f.function), int(f.arguments));
    if (f.returns && !value)
      emit(Op::drop);
  }
  void declaration() {
    if (callback_ != 0)
      fail("declarations must be in on init");
    const bool poly = take("polyphonic");
    const bool constant = take("const");
    const auto name = token().text;
    if (name.size() < 2 || (name.front() != '$' && name.front() != '%') ||
        builtins.contains(name) || name == "%CC" || name == "$ALL_GROUPS" ||
        name == "$ALL_EVENTS" || p_->symbols.contains(name))
      fail("invalid or duplicate variable '" + name + "'");
    ++at_;
    Symbol s;
    s.constant = constant;
    if (name.front() == '%') {
      if (poly)
        fail("polyphonic arrays are not implemented");
      require("[");
      const auto length = literal();
      require("]");
      if (length <= 0 || std::uint64_t(p_->arrayCount) + unsigned(length) >
                             limits_.arrayValues)
        fail("array storage limit exceeded");
      s.kind = SymbolKind::array;
      s.index = p_->arrayCount;
      s.length = unsigned(length);
      p_->arrayCount += s.length;
      p_->symbols[name] = s;
      if (take(":=")) {
        require("(");
        unsigned n = 0;
        do {
          if (n >= s.length)
            fail("too many array initializers");
          emit(Op::push, int(n++));
          expression();
          emit(Op::arrayStore, int(s.index), int(s.length));
        } while (take(","));
        require(")");
      }
      return;
    }
    if (poly) {
      if (constant)
        fail("polyphonic constants are not implemented");
      if (p_->poly >= limits_.variables)
        fail("polyphonic variable limit exceeded");
      s.kind = SymbolKind::poly;
      s.index = p_->poly++;
      if (take(":="))
        p_->polyDefaults[s.index] = literal();
      p_->symbols[name] = s;
      return;
    }
    if (constant) {
      require(":=");
      s.kind = SymbolKind::constant;
      s.value = literal();
      p_->symbols[name] = s;
      return;
    }
    if (p_->globals >= limits_.variables)
      fail("global variable limit exceeded");
    s.kind = SymbolKind::global;
    s.index = p_->globals++;
    p_->symbols[name] = s;
    if (take(":=")) {
      expression();
      store(s);
    }
  }
  std::int32_t caseConstant(){if(!token().text.empty()&&token().text.front()=='$'){const auto value=symbol(token().text);if(value.kind!=SymbolKind::constant)fail("case bounds must be constants");++at_;return value.value;}return literal();}
  void inlineFunction(const std::string& name){
    const auto found=userFunctions_.find(name);if(found==userFunctions_.end())fail("function must be declared before it is called: "+name);if(functionExits_.size()>=32)fail("function nesting limit exceeded");
    const auto after=at_;at_=found->second;functionExits_.emplace_back();block();require("end");require("function");for(const auto jump:functionExits_.back())p_->code[jump].a=int(p_->code.size());functionExits_.pop_back();at_=after;
  }
  void block(bool caseBody=false) {
    if (++depth_ > 64)
      fail("statement nesting limit exceeded");
    newlines();
    while (!is("end") && !is("else") && !is("<eof>") && !(caseBody&&is("case"))) {
      if (take("declare"))
        declaration();
      else if (take("if")) {
        require("(");
        expression();
        require(")");
        const auto branch = emit(Op::jumpFalse);
        require("\n");
        block();
        if (take("else")) {
          const auto end = emit(Op::jump);
          p_->code[branch].a = int(p_->code.size());
          require("\n");
          block();
          p_->code[end].a = int(p_->code.size());
        } else
          p_->code[branch].a = int(p_->code.size());
        require("end");
        require("if");
      } else if (take("while")) {
        const auto begin = unsigned(p_->code.size());
        require("(");
        expression();
        require(")");
        const auto branch = emit(Op::jumpFalse);
        require("\n");
        block();
        require("end");
        require("while");
        emit(Op::jump, int(begin));
        p_->code[branch].a = int(p_->code.size());
      } else if(take("select")){
        require("(");expression();require(")");require("\n");newlines();std::vector<unsigned> exits;
        while(take("case")){
          const auto low=caseConstant();const bool range=take("to");const auto high=range?caseConstant():low;if(high<low)fail("case range is reversed");
          emit(Op::duplicate,0);emit(Op::push,low);emit(range?Op::ge:Op::eq);
          if(range){emit(Op::duplicate,1);emit(Op::push,high);emit(Op::le);emit(Op::logicalAnd);}
          const auto branch=emit(Op::jumpFalse);emit(Op::drop);require("\n");block(true);exits.push_back(emit(Op::jump));p_->code[branch].a=int(p_->code.size());
        }
        require("end");require("select");emit(Op::drop);for(const auto jump:exits)p_->code[jump].a=int(p_->code.size());
      } else if(take("call")){
        const auto name=token().text;++at_;if(take("("))require(")");inlineFunction(name);
      } else if (take("exit")){
        if(functionExits_.empty())emit(Op::halt);else functionExits_.back().push_back(emit(Op::jump));
      }
      else if (is("inc") || is("dec")) {
        const bool increment = take("inc");
        if (!increment)
          require("dec");
        require("(");
        const auto s = symbol(token().text);
        if (s.kind == SymbolKind::array)
          fail("inc/dec requires a scalar");
        ++at_;
        require(")");
        load(s);
        emit(Op::push, 1);
        emit(increment ? Op::add : Op::sub);
        store(s);
      } else if (!token().text.empty() &&
                 (token().text.front() == '$' || token().text.front() == '%')) {
        const auto s = symbol(token().text);
        ++at_;
        if (s.constant)
          fail("cannot assign a constant");
        if (s.kind == SymbolKind::array) {
          require("[");
          expression();
          require("]");
          require(":=");
          expression();
          emit(Op::arrayStore, int(s.index), int(s.length));
        } else {
          require(":=");
          expression();
          store(s);
        }
      } else {
        const auto name = token().text;
        ++at_;
        function(name, false);
      }
      require("\n");
      newlines();
    }
    --depth_;
  }

public:
  Compiler(std::string_view source, const Limits &limits)
      : limits_(limits), tokens_(lex(source, limits)) {}
  std::shared_ptr<const ProgramData> run() {
    newlines();
    while (!is("<eof>")) {
      if(take("function")){
        const auto name=token().text;if(name.empty()||!((name.front()>='a'&&name.front()<='z')||(name.front()>='A'&&name.front()<='Z')||name.front()=='_')||userFunctions_.contains(name)||functions.contains(name))fail("invalid or duplicate function name");++at_;if(take("("))require(")");require("\n");const auto begin=at_;const auto codeSize=p_->code.size();callback_=4;functionExits_.emplace_back();block();require("end");require("function");functionExits_.pop_back();p_->code.resize(codeSize);userFunctions_.emplace(name,begin);require("\n");newlines();continue;
      }
      require("on");
      const auto name = token().text;
      callback_ = name == "init"         ? 0
                  : name == "note"       ? 1
                  : name == "release"    ? 2
                  : name == "controller" ? 3
                                         : 4;
      if (callback_ == 4)
        fail("unsupported callback '" + name + "'");
      ++at_;
      if (p_->entry[callback_] >= 0)
        fail("duplicate callback");
      p_->entry[callback_] = int(p_->code.size());
      require("\n");
      block();
      require("end");
      require("on");
      emit(Op::halt);
      require("\n");
      newlines();
    }
    return p_;
  }
};
} // namespace
Program compile(std::string_view source, const Limits &limits) {
  validateLimits(limits);
  Program program;
  program.data_ = Compiler(source, limits).run();
  return program;
}

struct Runtime::Impl {
  std::shared_ptr<const ProgramData> program;
  Limits limits;
  double rate{};
  unsigned groupCount{};
  Fault error{};
  bool initialized{};
  std::uint64_t clock{}, now{}, callbackSerial{};
  std::int32_t nextEvent = 1;
  unsigned budget{}, frame{};
  std::uint32_t randomState = 0x91e10da5;
  struct Voice {
    bool used{}, emitted{}, released{}, input{}, keyReleased{}, ignored{};
    std::int32_t id{}, parent{};
    unsigned source{}, note{}, velocity{}, channel{};
    std::uint64_t end = UINT64_MAX;
    VoiceCommand start;
    std::array<std::int32_t, 128> poly{};
  };
  struct Task {
    bool used{}, waited{}, ignoreRelease{};
    unsigned pc{}, sp{}, kind{}, ccNumber{}, channel = 1;
    std::int32_t event{};
    std::uint64_t wake{}, serial{};
    std::array<std::int32_t, 128> stack{};
  };
  std::vector<Voice> voices;
  std::vector<Task> tasks;
  std::vector<std::int32_t> globals, arrays;
  std::array<std::int32_t, 128> cc{};
  Output *output{};
  Voice *voice(std::int32_t id) noexcept {
    for (auto &v : voices)
      if (v.used && v.id == id)
        return &v;
    return nullptr;
  }
  void fault(Fault e) noexcept {
    if (error != Fault::none)
      return;
    error = e;
    if (output) {
      VoiceCommand c;
      c.kind = CommandKind::panic;
      c.frame = frame;
      (void)output->push(c);
    }
  }
  bool send(VoiceCommand c) noexcept {
    c.frame = frame;
    if (!output || !output->push(c)) {
      fault(Fault::outputCapacity);
      return false;
    }
    return true;
  }
  std::array<std::uint64_t, 4> allGroups() const noexcept {
    std::array<std::uint64_t, 4> result{};
    for (unsigned n = 0; n < groupCount; ++n)
      result[n / 64] |= std::uint64_t(1) << (n % 64);
    return result;
  }
  Voice *create(unsigned note, unsigned velocity, unsigned channel,
                bool input) noexcept {
    if (nextEvent == INT32_MAX) {
      fault(Fault::eventIdentity);
      return nullptr;
    }
    for (auto &v : voices)
      if (!v.used) {
        v = {};
        v.used = true;
        v.id = nextEvent++;
        v.source = v.note = note;
        v.velocity = velocity;
        v.channel = channel;
        v.input = input;
        v.poly = program->polyDefaults;
        v.start.kind = CommandKind::start;
        v.start.id = v.id;
        v.start.note = note;
        v.start.velocity = velocity;
        v.start.channel = channel;
        v.start.groups = allGroups();
        return &v;
      }
    fault(Fault::voiceCapacity);
    return nullptr;
  }
  void start(Voice &v) noexcept {
    if (v.emitted || v.ignored || v.released)
      return;
    v.emitted = send(v.start);
  }
  void release(Voice &v) noexcept {
    if (v.released)
      return;
    v.released = true;
    if (v.emitted) {
      auto c = v.start;
      c.kind = CommandKind::release;
      send(c);
    }
  }
  void children(std::int32_t parent) noexcept {
    for (auto &v : voices)
      if (v.used && v.parent == parent)
        release(v);
  }
  void collect() noexcept {
    for (auto &v : voices)
      if (v.used && v.released && (!v.input || v.keyReleased)) {
        bool pending = false;
        for (const auto &t : tasks)
          pending |= t.used && t.event == v.id;
        if (!pending)
          v.used = false;
      }
  }
  void flush(Task &task) noexcept {
    auto *v = voice(task.event);
    if (!v)
      return;
    if (task.kind == 1)
      start(*v);
    else if (task.kind == 2 && !task.ignoreRelease)
      release(*v);
  }
  std::uint64_t delay(std::int32_t us) noexcept {
    return std::max<std::uint64_t>(
        1, std::uint64_t(std::ceil(double(us) * rate / 1000000.)));
  }
  bool push(Task &t, std::int32_t value) noexcept {
    if (t.sp == t.stack.size()) {
      fault(Fault::instructionBudget);
      return false;
    }
    t.stack[t.sp++] = value;
    return true;
  }
  std::int32_t pop(Task &t) noexcept {
    if (!t.sp) {
      fault(Fault::arithmetic);
      return 0;
    }
    return t.stack[--t.sp];
  }
  std::int32_t builtin(Task &t, Builtin b) noexcept {
    auto *v = voice(t.event);
    switch (b) {
    case Builtin::eventId:
      return t.event;
    case Builtin::eventNote:
      return v ? int(v->note) : 0;
    case Builtin::eventVelocity:
      return v ? int(v->velocity) : 0;
    case Builtin::noteHeld:
      return v && !v->keyReleased;
    case Builtin::ccNumber:
      return int(t.ccNumber);
    case Builtin::uptime:
      return wrapped(std::uint32_t(double(now) * 1000 / rate));
    case Builtin::callbackId:
      return wrapped(std::uint32_t(t.serial));
    }
    return 0;
  }
  std::int32_t calculate(Op op, std::int32_t a, std::int32_t b) noexcept {
    const auto x = std::uint32_t(a), y = std::uint32_t(b);
    switch (op) {
    case Op::add:
      return wrapped(x + y);
    case Op::sub:
      return wrapped(x - y);
    case Op::mul:
      return wrapped(x * y);
    case Op::div:
    case Op::mod:
      if (!b) {
        fault(Fault::arithmetic);
        return 0;
      }
      if (a == INT32_MIN && b == -1)
        return op == Op::div ? INT32_MIN : 0;
      return op == Op::div ? a / b : a % b;
    case Op::eq:
      return a == b;
    case Op::ne:
      return a != b;
    case Op::lt:
      return a < b;
    case Op::le:
      return a <= b;
    case Op::gt:
      return a > b;
    case Op::ge:
      return a >= b;
    case Op::logicalAnd:
      return a && b;
    case Op::logicalOr:
      return a || b;
    case Op::bitAnd:
      return wrapped(x & y);
    case Op::bitOr:
      return wrapped(x | y);
    case Op::bitXor:
      return wrapped(x ^ y);
    default:
      fault(Fault::arithmetic);
      return 0;
    }
  }
  void call(Task &t, Function fn, unsigned argc) noexcept {
    std::array<std::int32_t, 4> a{};
    for (unsigned n = argc; n; --n)
      a[n - 1] = pop(t);
    if (error != Fault::none)
      return;
    auto *current = voice(t.event);
    if (fn == Function::wait) {
      if (a[0] < 1) {
        fault(Fault::invalidArgument);
        return;
      }
      flush(t);
      t.waited = true;
      t.wake = now + delay(a[0]);
      return;
    }
    if (fn == Function::play) {
      if (a[0] < 0 || a[0] > 127 || a[1] < 1 || a[1] > 127 || a[2] < 0 ||
          a[3] < -1 || (a[3] == -1 && (!current || t.kind != 1))) {
        fault(Fault::invalidArgument);
        return;
      }
      auto *generated = create(unsigned(a[0]), unsigned(a[1]),
                               current ? current->channel : t.channel, false);
      if (!generated)
        return;
      if (current)
        generated->start.groups = current->start.groups;
      generated->start.offsetMicroseconds = unsigned(a[2]);
      generated->start.oneShot = a[3] == 0;
      if (a[3] > 0)
        generated->end = now + delay(a[3]);
      if (a[3] == -1) {
        generated->parent = current->id;
        if (current->keyReleased)
          generated->end = now;
      }
      start(*generated);
      push(t, generated->id);
      return;
    }
    if (fn == Function::allow || fn == Function::disallow) {
      if (!current || t.waited) {
        fault(Fault::invalidArgument);
        return;
      }
      if (a[0] == -1)
        current->start.groups = fn == Function::allow
                                    ? allGroups()
                                    : std::array<std::uint64_t, 4>{};
      else if (a[0] >= 0 && unsigned(a[0]) < groupCount) {
        auto &group = current->start.groups[unsigned(a[0]) / 64];
        const auto bit = std::uint64_t(1) << (unsigned(a[0]) % 64);
        if (fn == Function::allow)
          group |= bit;
        else
          group &= ~bit;
      } else
        fault(Fault::invalidArgument);
      return;
    }
    if (fn == Function::random) {
      if (a[0] > a[1]) {
        fault(Fault::invalidArgument);
        return;
      }
      randomState ^= randomState << 13;
      randomState ^= randomState >> 17;
      randomState ^= randomState << 5;
      const auto width = std::uint64_t(std::int64_t(a[1]) - a[0]) + 1;
      push(t, wrapped(std::uint32_t(std::int64_t(a[0]) +
                                    std::int64_t(randomState % width))));
      return;
    }
    if (fn == Function::abs) {
      if (a[0] == INT32_MIN) {
        fault(Fault::arithmetic);
        return;
      }
      push(t, a[0] < 0 ? -a[0] : a[0]);
      return;
    }
    if (fn == Function::min || fn == Function::max || fn == Function::inRange) {
      push(t, fn == Function::min   ? std::min(a[0], a[1])
              : fn == Function::max ? std::max(a[0], a[1])
                                    : int(a[0] >= a[1] && a[0] <= a[2]));
      return;
    }
    if (fn == Function::noteOff && a[0] == -1) {
      for (auto &v : voices)
        if (v.used)
          release(v);
      return;
    }
    if (a[0] == -1 && (fn == Function::tune || fn == Function::volume ||
                       fn == Function::pan)) {
      for (auto &v : voices)
        if (v.used && !v.released) {
          auto child = t;
          push(child, v.id);
          push(child, a[1]);
          push(child, a[2]);
          call(child, fn, 3);
        }
      return;
    }
    auto *v = voice(a[0]);
    if (!v)
      return;
    if (fn == Function::ignore) {
      if (t.kind == 1 && !t.waited && !v->emitted)
        v->ignored = true;
      else if (t.kind == 2 && v == current)
        t.ignoreRelease = true;
      else
        fault(Fault::invalidArgument);
      return;
    }
    if (fn == Function::noteOff) {
      release(*v);
      return;
    }
    if (fn == Function::changeNote || fn == Function::changeVelocity) {
      if (a[1] < 0 || a[1] > 127) {
        fault(Fault::invalidArgument);
        return;
      }
      if (fn == Function::changeNote)
        v->note = unsigned(a[1]);
      else
        v->velocity = unsigned(a[1]);
      if (!t.waited && !v->emitted) {
        v->start.note = v->note;
        v->start.velocity = v->velocity;
      }
      return;
    }
    if (a[2] != 0 && a[2] != 1) {
      fault(Fault::invalidArgument);
      return;
    }
    if (!v->emitted) {
      auto &value = fn == Function::tune     ? v->start.tuneMillicents
                    : fn == Function::volume ? v->start.volumeMillidB
                                             : v->start.panUnits;
      value = a[2] ? wrapped(std::uint32_t(value) + std::uint32_t(a[1])) : a[1];
      return;
    }
    auto command = v->start;
    command.kind = fn == Function::tune     ? CommandKind::tune
                   : fn == Function::volume ? CommandKind::volume
                                            : CommandKind::pan;
    command.value = a[1];
    command.relative = a[2] != 0;
    send(command);
  }
  void run(Task &t) noexcept {
    unsigned executed = 0;
    while (t.used && error == Fault::none) {
      if (!budget || ++executed > limits.instructionsPerCallback) {
        fault(Fault::instructionBudget);
        break;
      }
      --budget;
      if (t.pc >= program->code.size()) {
        fault(Fault::arithmetic);
        break;
      }
      const auto ins = program->code[t.pc++];
      switch (ins.op) {
      case Op::push:
        push(t, ins.a);
        break;
      case Op::load:
        push(t, globals[unsigned(ins.a)]);
        break;
      case Op::store:
        globals[unsigned(ins.a)] = pop(t);
        break;
      case Op::polyLoad:
      case Op::polyStore: {
        auto *v = voice(t.event);
        if (!v) {
          fault(Fault::invalidArgument);
          break;
        }
        if (ins.op == Op::polyLoad)
          push(t, v->poly[unsigned(ins.a)]);
        else
          v->poly[unsigned(ins.a)] = pop(t);
        break;
      }
      case Op::arrayLoad:
      case Op::arrayStore: {
        const auto value = ins.op == Op::arrayStore ? pop(t) : 0,
                   index = pop(t);
        if (index < 0 || index >= ins.b) {
          fault(Fault::arrayBounds);
          break;
        }
        auto &cell = arrays[unsigned(ins.a + index)];
        if (ins.op == Op::arrayLoad)
          push(t, cell);
        else
          cell = value;
        break;
      }
      case Op::cc: {
        const auto n = pop(t);
        if (n < 0 || n > 127)
          fault(Fault::arrayBounds);
        else
          push(t, cc[unsigned(n)]);
        break;
      }
      case Op::builtin:
        push(t, builtin(t, Builtin(ins.a)));
        break;
      case Op::neg: {
        const auto n = pop(t);
        push(t, wrapped(0u - std::uint32_t(n)));
        break;
      }
      case Op::logicalNot: {
        const auto n = pop(t);
        push(t, !n);
        break;
      }
      case Op::bitNot: {
        const auto n = pop(t);
        push(t, wrapped(~std::uint32_t(n)));
        break;
      }
      case Op::jump:
        t.pc = unsigned(ins.a);
        break;
      case Op::jumpFalse:
        if (!pop(t))
          t.pc = unsigned(ins.a);
        break;
      case Op::drop:
        pop(t);
        break;
      case Op::duplicate:
        if(ins.a<0||unsigned(ins.a)>=t.sp)fault(Fault::arithmetic);else {const auto value=t.stack[t.sp-1-unsigned(ins.a)];push(t,value);}
        break;
      case Op::call:
        call(t, Function(ins.a), unsigned(ins.b));
        if (Function(ins.a) == Function::wait)
          return;
        break;
      case Op::halt:
        flush(t);
        t.used = false;
        break;
      default: {
        const auto b = pop(t), a = pop(t);
        push(t, calculate(ins.op, a, b));
        break;
      }
      }
    }
  }
  void callback(unsigned kind, std::int32_t event = 0, unsigned controller = 0,
                unsigned channel = 1) noexcept {
    const auto pc = program->entry[kind];
    if (pc < 0) {
      if (auto *v = voice(event)) {
        if (kind == 1)
          start(*v);
        else if (kind == 2)
          release(*v);
      }
      return;
    }
    for (auto &t : tasks)
      if (!t.used) {
        t = {};
        t.used = true;
        t.kind = kind;
        t.pc = unsigned(pc);
        t.event = event;
        t.wake = now;
        t.serial = ++callbackSerial;
        t.ccNumber = controller;
        t.channel = channel;
        run(t);
        return;
      }
    fault(Fault::callbackCapacity);
  }
  void input(MidiEvent e) noexcept {
    const auto kind = e.data[0] & 0xf0;
    const unsigned channel = (e.data[0] & 15) + 1;
    if (kind == 0x90 && e.data[2]) {
      if (auto *v = create(e.data[1], e.data[2], channel, true))
        callback(1, v->id);
    } else if (kind == 0x80 || (kind == 0x90 && !e.data[2])) {
      Voice *found = nullptr;
      for (auto &v : voices)
        if (v.used && v.input && !v.keyReleased && v.channel == channel &&
            v.source == e.data[1] && (!found || v.id < found->id))
          found = &v;
      if (found) {
        found->keyReleased = true;
        children(found->id);
        callback(2, found->id);
        if (found->ignored)
          found->released = true;
      }
    } else {
      if (kind == 0xb0) {
        cc[e.data[1]] = e.data[2];
        if (e.data[1] == 120 || e.data[1] == 123) {
          for (auto &v : voices)
            if (v.used && v.channel == channel) {
              v.keyReleased = true;
              release(v);
            }
        }
        callback(3, 0, e.data[1], channel);
      }
      VoiceCommand c;
      c.kind = CommandKind::midi;
      c.channel = channel;
      c.midi = e;
      send(c);
    }
  }
  void reset() noexcept {
    for (auto &v : voices)
      v = {};
    for (auto &t : tasks)
      t = {};
    std::fill(globals.begin(), globals.end(), 0);
    std::fill(arrays.begin(), arrays.end(), 0);
    cc = {};
    clock = now = callbackSerial = 0;
    nextEvent = 1;
    initialized = false;
    error = Fault::none;
    randomState = 0x91e10da5;
    output = nullptr;
  }
};
Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
void Runtime::prepare(const Program &program, double rate, unsigned groups,
                      const Limits &limits) {
  validateLimits(limits);
  if (!program.data_ || !std::isfinite(rate) || rate < 1000 || rate > 768000 ||
      !groups || groups > 256 ||
      program.data_->code.size() > limits.instructions ||
      program.data_->globals > limits.variables ||
      program.data_->poly > limits.variables ||
      program.data_->arrayCount > limits.arrayValues)
    throw std::invalid_argument("Invalid KSP preparation");
  auto next = std::make_unique<Impl>();
  next->program = program.data_;
  next->limits = limits;
  next->rate = rate;
  next->groupCount = groups;
  next->voices.resize(limits.voices);
  next->tasks.resize(limits.callbacks);
  next->globals.resize(program.data_->globals);
  next->arrays.resize(program.data_->arrayCount);
  next->reset();
  impl_ = std::move(next);
}
void Runtime::reset() noexcept { impl_->reset(); }
void Runtime::voiceFinished(std::int32_t id) noexcept {
  if (auto *v = impl_->voice(id))
    v->released = true;
  impl_->collect();
}
bool Runtime::process(std::span<const MidiEvent> input, unsigned frames,
                      Output &output) noexcept {
  auto &p = *impl_;
  if (!p.program || !frames || frames > (1u << 20) ||
      input.size() > midi::eventCapacity)
    return false;
  if (p.error != Fault::none)
    return false;
  p.output = &output;
  p.frame = 0;
  p.now = p.clock;
  p.budget = p.limits.instructionsPerBlock;
  if (!p.initialized) {
    p.initialized = true;
    p.callback(0);
  }
  std::size_t at = 0;
  std::uint32_t previous = 0;
  while (p.error == Fault::none) {
    while (at < input.size() &&
           (!midi::valid(input[at]) || input[at].frame >= frames ||
            input[at].frame < previous))
      ++at;
    auto next =
        at < input.size() ? p.clock + input[at].frame : p.clock + frames;
    for (const auto &t : p.tasks)
      if (t.used)
        next = std::min(next, t.wake);
    for (const auto &v : p.voices)
      if (v.used && !v.released)
        next = std::min(next, v.end);
    if (next >= p.clock + frames)
      break;
    p.now = next;
    p.frame = unsigned(next - p.clock);
    for (auto &v : p.voices)
      if (v.used && !v.released && v.end <= next)
        p.release(v);
    for (;;) {
      Impl::Task *due = nullptr;
      for (auto &t : p.tasks)
        if (t.used && t.wake <= next && (!due || t.serial < due->serial))
          due = &t;
      if (!due || p.error != Fault::none)
        break;
      p.run(*due);
    }
    while (at < input.size() && input[at].frame == p.frame &&
           p.error == Fault::none) {
      previous = input[at].frame;
      p.input(input[at++]);
    }
    p.collect();
  }
  p.clock += frames;
  p.output = nullptr;
  return p.error == Fault::none;
}
Fault Runtime::fault() const noexcept { return impl_->error; }
std::size_t Runtime::storageBytes() const noexcept {
  return sizeof(Impl) + impl_->voices.capacity() * sizeof(Impl::Voice) +
         impl_->tasks.capacity() * sizeof(Impl::Task) +
         (impl_->globals.capacity() + impl_->arrays.capacity()) *
             sizeof(std::int32_t);
}
std::int32_t Runtime::variable(std::string_view name) const {
  const auto &p = *impl_;
  if (!p.program)
    throw std::logic_error("KSP runtime is not prepared");
  const auto it = p.program->symbols.find(name);
  if (it == p.program->symbols.end() ||
      (it->second.kind != SymbolKind::global &&
       it->second.kind != SymbolKind::constant))
    throw std::invalid_argument("KSP global integer variable not found");
  return it->second.kind == SymbolKind::constant ? it->second.value
                                                 : p.globals[it->second.index];
}
} // namespace cell::ksp
