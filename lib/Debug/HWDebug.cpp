#define GET_OP_CLASSES

#include <memory>

#include "circt/Debug/HWDebug.h"
#include "circt/Dialect/Comb/CombVisitors.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWVisitors.h"
#include "circt/Dialect/SV/SVOps.h"
#include "circt/Dialect/SV/SVVisitors.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"
#include "llvm/Support/JSON.h"

mlir::StringRef getPortVerilogName(mlir::Operation *module,
                                   circt::hw::PortInfo port);

namespace circt::hw {
mlir::StringAttr getVerilogModuleNameAttr(mlir::Operation *module);
} // namespace circt::hw

namespace circt::ExportVerilog {
bool isVerilogExpression(Operation *op);
} // namespace circt::ExportVerilog

namespace circt::debug {

#define GEN_PASS_CLASSES
#include "circt/Debug/DebugPasses.h.inc"

struct HWDebugScope;
class HWDebugContext;
class HWDebugBuilder;

void setEntryLocation(HWDebugScope &scope, const mlir::Location &location);

enum class HWDebugScopeType { None, Assign, Declare, Module, Block };

mlir::StringRef toString(HWDebugScopeType type) {
  switch (type) {
  case HWDebugScopeType::None:
    return "none";
  case HWDebugScopeType::Assign:
    return "assign";
  case HWDebugScopeType::Declare:
    return "decl";
  case HWDebugScopeType::Module:
    return "module";
  case HWDebugScopeType::Block:
    return "block";
  }
  llvm_unreachable("unknown scope type");
}

/// Return the verilog name of the operations that can define a symbol.
/// Except for <WireOp, RegOp, LogicOp, LocalParamOp, InstanceOp>, check global
/// state `getDeclarationVerilogName` for them.
static StringRef getSymOpName(Operation *symOp) {
  using namespace circt::hw;
  using namespace circt::sv;
  // Typeswitch of operation types which can define a symbol.
  // If legalizeNames has renamed it, then the attribute must be set.
  if (auto attr = symOp->getAttrOfType<StringAttr>("hw.verilogName"))
    return attr.getValue();
  return TypeSwitch<Operation *, StringRef>(symOp)
      .Case<HWModuleOp, HWModuleExternOp, HWModuleGeneratedOp>(
          [](Operation *op) { return getVerilogModuleName(op); })
      .Case<InterfaceOp>([&](InterfaceOp op) {
        return getVerilogModuleNameAttr(op).getValue();
      })
      .Case<InterfaceSignalOp>(
          [&](InterfaceSignalOp op) { return op.getSymName(); })
      .Case<InterfaceModportOp>(
          [&](InterfaceModportOp op) { return op.getSymName(); })
      .Default([&](Operation *op) {
        if (auto attr = op->getAttrOfType<StringAttr>("name"))
          return attr.getValue();
        if (auto attr = op->getAttrOfType<StringAttr>("instanceName"))
          return attr.getValue();
        if (auto attr =
                op->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()))
          return attr.getValue();
        return StringRef("");
      });
}

struct HWDebugScope {
public:
  explicit HWDebugScope(HWDebugContext &context, mlir::Operation *op)
      : context(context), op(op) {}

  llvm::SmallVector<HWDebugScope *> scopes;

  mlir::StringRef filename;
  uint32_t line = 0;
  uint32_t column = 0;
  mlir::StringAttr condition;

  HWDebugScope *parent = nullptr;

  HWDebugContext &context;

  mlir::Operation *op;

  // NOLINTNEXTLINE
  [[nodiscard]] virtual llvm::json::Value toJSON() const {
    auto res = getScopeJSON(type() == HWDebugScopeType::Block);
    return res;
  }

  [[nodiscard]] virtual HWDebugScopeType type() const {
    return scopes.empty() ? HWDebugScopeType::None : HWDebugScopeType::Block;
  }

  virtual ~HWDebugScope() = default;

protected:
  // NOLINTNEXTLINE
  [[nodiscard]] llvm::json::Object getScopeJSON(bool includeScope) const {
    llvm::json::Object res;
    auto scopeType = type();
    if (scopeType != HWDebugScopeType::Block &&
        scopeType != HWDebugScopeType::Module) {
      // block and module does not have line number
      res["line"] = line;
      if (column > 0) {
        res["column"] = column;
      }
    }

    res["type"] = toString(scopeType);
    if (includeScope) {
      setScope(res);
    }
    if (type() == HWDebugScopeType::Block && !filename.empty()) {
      res["filename"] = filename;
    }
    if (condition && condition.size() != 0) {
      res["condition"] = condition.strref();
    }
    return res;
  }

  // NOLINTNEXTLINE
  void setScope(llvm::json::Object &obj) const {
    llvm::json::Array array;
    array.reserve(scopes.size());
    for (auto const *scope : scopes) {
      if (scope)
        array.emplace_back(scope->toJSON());
    }
    obj["scope"] = std::move(array);
  }
};

struct HWDebugLineInfo : HWDebugScope {
  enum class LineType {
    None = static_cast<int>(HWDebugScopeType::None),
    Assign = static_cast<int>(HWDebugScopeType::Assign),
    Declare = static_cast<int>(HWDebugScopeType::Declare),
  };

  LineType lineType;

  HWDebugLineInfo(HWDebugContext &context, LineType type, mlir::Operation *op)
      : HWDebugScope(context, op), lineType(type) {}

  [[nodiscard]] llvm::json::Value toJSON() const override {
    auto res = getScopeJSON(false);
    if (condition && condition.size() != 0) {
      res["condition"] = condition.strref();
    }
    return res;
  }

  [[nodiscard]] HWDebugScopeType type() const override {
    return static_cast<HWDebugScopeType>(lineType);
  }
};

struct HWDebugVarDef {
  mlir::StringRef name;
  mlir::StringRef value;
  // for how it's always RTL value
  bool rtl = true;
  mlir::StringAttr id;

  HWDebugVarDef(mlir::StringRef name, mlir::StringRef value, bool rtl,
                mlir::StringAttr id)
      : name(name), value(value), rtl(rtl), id(id) {}

  [[nodiscard]] llvm::json::Value toJSON() const {
    if (id) {
      return llvm::json::Value(id);
    }
    return llvm::json::Object({{"name", name}, {"value", value}, {"rtl", rtl}});
  }

  [[nodiscard]] llvm::json::Value toJSONDefinition() const {
    return llvm::json::Object(
        {{"name", name}, {"value", value}, {"rtl", rtl}, {"id", id.strref()}});
  }
};

struct HWModuleInfo : public HWDebugScope {
public:
  // module names
  mlir::StringRef name;

  mlir::SmallVector<const HWDebugVarDef *> variables;
  llvm::DenseMap<mlir::StringRef, mlir::StringRef> instances;
  mlir::SmallVector<const HWDebugVarDef *> outputVars;

  explicit HWModuleInfo(HWDebugContext &context,
                        circt::hw::HWModuleOp *moduleOp)
      : HWDebugScope(context, moduleOp->getOperation()) {
    // index the port names by value
    auto portInfo = moduleOp->getAllPorts();
    outputVars.resize(moduleOp->getNumOutputs());
    for (auto &port : portInfo) {
      StringRef n = getPortVerilogName(*moduleOp, port);
      mlir::Value value;
      if (!port.isOutput()) {
        value = moduleOp->getArgument(port.argNum);
        portNames[value] = n;
      }
      // also add to the generator variables
      if (port.debugAttr) {
        outputPorts.emplace_back(std::make_unique<HWDebugVarDef>(
            port.debugAttr.strref(), n, true, mlir::StringAttr{}));
        variables.emplace_back(outputPorts.back().get());
        outputVars[port.argNum] = outputPorts.back().get();
      }
    }
  }

  [[nodiscard]] llvm::json::Value toJSON() const override {
    auto res = getScopeJSON(true);
    res["name"] = name;

    llvm::json::Array vars;
    vars.reserve(variables.size());
    for (auto const *varDef : variables) {
      vars.emplace_back(varDef->toJSON());
    }
    res["variables"] = std::move(vars);

    if (!instances.empty()) {
      llvm::json::Array insts;
      insts.reserve(instances.size());
      for (auto const &[n, def] : instances) {
        insts.emplace_back(llvm::json::Object{{"name", n}, {"module", def}});
      }
      res["instances"] = std::move(insts);
    }

    return res;
  }

  [[nodiscard]] HWDebugScopeType type() const override {
    return HWDebugScopeType::Module;
  }

  mlir::StringRef getPortName(mlir::Value value) const {
    return portNames.lookup(value);
  }

private:
  llvm::DenseMap<mlir::Value, mlir::StringRef> portNames;
  llvm::SmallVector<std::unique_ptr<HWDebugVarDef>> outputPorts;
};

struct HWDebugVarDeclareLineInfo : public HWDebugLineInfo {
  HWDebugVarDeclareLineInfo(HWDebugContext &context, mlir::Operation *op)
      : HWDebugLineInfo(context, LineType::Declare, op) {}

  HWDebugVarDef *variable;

  [[nodiscard]] llvm::json::Value toJSON() const override {
    auto res = HWDebugLineInfo::toJSON();
    (*res.getAsObject())["variable"] = variable->toJSON();
    return res;
  }
};

struct HWDebugVarAssignLineInfo : public HWDebugLineInfo {
  // This also encodes mapping information
  HWDebugVarAssignLineInfo(HWDebugContext &context, mlir::Operation *op)
      : HWDebugLineInfo(context, LineType::Assign, op) {}

  HWDebugVarDef *variable;

  [[nodiscard]] llvm::json::Value toJSON() const override {
    auto res = HWDebugLineInfo::toJSON();
    (*res.getAsObject())["variable"] = variable->toJSON();
    return res;
  }
};

mlir::StringRef findTop(const llvm::SmallVector<HWModuleInfo *> &modules) {
  llvm::SmallDenseSet<mlir::StringRef> names;
  for (auto const *mod : modules) {
    names.insert(mod->name);
  }
  for (auto const *m : modules) {
    for (auto const &[_, name] : m->instances) {
      names.erase(name);
    }
  }
  assert(names.size() == 1);
  return *names.begin();
}

class HWDebugContext {
public:
  [[nodiscard]] llvm::json::Value toJSON() const {
    llvm::json::Object res{{"generator", "circt"}};
    llvm::json::Array array;
    array.reserve(modules.size());
    for (auto const *module : modules) {
      array.emplace_back(std::move(module->toJSON()));
    }
    res["table"] = std::move(array);
    // if we have variable reference in the context
    if (!vars.empty()) {
      array.clear();
      array.reserve(vars.size());
      for (auto const &[_, var] : vars) {
        array.emplace_back(std::move(var->toJSONDefinition()));
      }
      res["variables"] = std::move(array);
    }
    auto top = findTop(modules);
    res["top"] = top;
    return res;
  }

  template <typename T, typename... Args>
  T *createScope(Args &&...args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto *res = ptr.get();
    scopes.emplace_back(std::move(ptr));
    if constexpr (std::is_same<T, HWModuleInfo>::value) {
      modules.emplace_back(res);
    }
    return res;
  }

  HWDebugScope *getNormalScope(::mlir::Operation *op) {
    auto *ptr = createScope<HWDebugScope>(*this, op);
    if (op)
      setEntryLocation(*ptr, op->getLoc());
    return ptr;
  }

  [[nodiscard]] const llvm::SmallVector<HWModuleInfo *> &getModules() const {
    return modules;
  }

private:
  llvm::SmallVector<HWModuleInfo *> modules;
  llvm::SmallVector<std::unique_ptr<HWDebugScope>> scopes;
  llvm::DenseMap<const mlir::Operation *, std::unique_ptr<HWDebugVarDef>> vars;

  friend HWDebugBuilder;
};

// NOLINTNEXTLINE
mlir::FileLineColLoc getFileLineColLocFromLoc(const mlir::Location &location) {
  // if it's a fused location, the first one will be used
  if (auto const fileLoc = location.dyn_cast<mlir::FileLineColLoc>()) {
    return fileLoc;
  }
  if (auto const fusedLoc = location.dyn_cast<mlir::FusedLoc>()) {
    auto const &locs = fusedLoc.getLocations();
    for (auto const &loc : locs) {
      auto res = getFileLineColLocFromLoc(loc);
      if (res)
        return res;
    }
  }
  return {};
}

void setEntryLocation(HWDebugScope &scope, const mlir::Location &location) {
  // need to get the containing module, as well as the line number
  // information
  auto const fileLoc = getFileLineColLocFromLoc(location);
  if (fileLoc) {
    auto const line = fileLoc.getLine();
    auto const column = fileLoc.getColumn();

    scope.line = line;
    scope.column = column;
  }
}

class HWDebugBuilder {
public:
  HWDebugBuilder(HWDebugContext &context) : context(context) {}

  HWDebugVarDeclareLineInfo *createVarDeclaration(::mlir::Value value) {
    auto loc = value.getLoc();
    auto *op = value.getDefiningOp();
    auto *targetOp = getDebugOp(value, op);
    if (!targetOp)
      return nullptr;

    // need to get the containing module, as well as the line number
    // information
    auto *info = context.createScope<HWDebugVarDeclareLineInfo>(context, op);
    setEntryLocation(*info, loc);
    info->variable = createVarDef(targetOp);
    return info;
  }

  HWDebugVarAssignLineInfo *createAssign(::mlir::Value value,
                                         ::mlir::Operation *op) {
    // only create assign if the target has frontend variable
    auto *targetOp = getDebugOp(value, op);
    if (!targetOp)
      return nullptr;

    auto loc = op->getLoc();

    auto *assign = context.createScope<HWDebugVarAssignLineInfo>(context, op);
    setEntryLocation(*assign, loc);

    assign->variable = createVarDef(targetOp);

    return assign;
  }

  HWDebugVarDef *createVarDef(::mlir::Operation *op) {
    auto it = context.vars.find(op);
    if (it == context.vars.end()) {
      // The OP has to have this attr. need to check before calling this
      // function
      auto frontEndName =
          op->getAttr("hw.debug.name").cast<mlir::StringAttr>().strref();
      llvm::StringRef rtlName;
      if (auto rtlValue = op->getAttr("hw.debug.value")) {
        rtlName = rtlValue.cast<mlir::StringAttr>().strref();
      } else {
        rtlName = getSymOpName(op);
      }
      // For now, we use the size of the map as ID
      auto id = mlir::StringAttr::get(op->getContext(),
                                      std::to_string(context.vars.size()));
      auto var =
          std::make_unique<HWDebugVarDef>(frontEndName, rtlName, true, id);

      it = context.vars.insert(std::make_pair(op, std::move(var))).first;
    }

    return it->second.get();
  }

  HWModuleInfo *createModule(circt::hw::HWModuleOp *op) {
    auto *info = context.createScope<HWModuleInfo>(context, op);
    setEntryLocation(*info, op->getLoc());
    return info;
  }

  HWDebugScope *createScope(::mlir::Operation *op,
                            HWDebugScope *parent = nullptr) {
    auto *res = context.getNormalScope(op);
    if (parent) {
      res->parent = parent;
      parent->scopes.emplace_back(res);
    }
    return res;
  }

private:
  HWDebugContext &context;

  static mlir::Operation *getDebugOp(mlir::Value value, mlir::Operation *op) {
    auto *valueOP = value.getDefiningOp();
    if (valueOP && valueOP->hasAttr("hw.debug.name")) {
      return valueOP;
    }
    if (op && op->hasAttr("hw.debug.name")) {
      return op;
    }
    return nullptr;
  }
};

// hgdb only supports a subsets of operations, nor does it support sign
// conversion. if the expression is complex we need to insert another pass that
// generate a temp wire that holds the expression and use that in hgdb instead.
// for now this is not an issue since Chisel is already doing this (somewhat)
class DebugExprPrinter
    : public circt::comb::CombinationalVisitor<DebugExprPrinter>,
      public circt::hw::TypeOpVisitor<DebugExprPrinter>,
      public circt::sv::Visitor<DebugExprPrinter> {
public:
  explicit DebugExprPrinter(llvm::raw_ostream &os, HWModuleInfo *module)
      : os(os), module(module) {}

  void printExpr(mlir::Value value) {
    auto *op = value.getDefiningOp();
    if (op) {
      auto name = getSymOpName(op);
      if (!name.empty()) {
        os << name;
        return;
      }
    }
    if (op) {
      dispatchCombinationalVisitor(op);
    } else {
      auto ref = module->getPortName(value);
      os << ref;
    }
  }
  // comb ops
  using CombinationalVisitor::visitComb;
  // supported
  void visitComb(circt::comb::AddOp op) { visitBinary(op, "+"); }
  void visitComb(circt::comb::SubOp op) { visitBinary(op, "-"); }
  void visitComb(circt::comb::MulOp op) {
    assert(op.getNumOperands() == 2 && "prelowering should handle variadics");
    return visitBinary(op, "*");
  }
  void visitComb(circt::comb::DivUOp op) { visitBinary(op, "/"); }
  void visitComb(circt::comb::DivSOp op) { visitBinary(op, "/"); }
  void visitComb(circt::comb::ModUOp op) { visitBinary(op, "%"); }
  void visitComb(circt::comb::ModSOp op) { visitBinary(op, "%"); }

  void visitComb(circt::comb::AndOp op) { visitBinary(op, "&"); }
  void visitComb(circt::comb::OrOp op) { visitBinary(op, "|"); }
  void visitComb(circt::comb::XorOp op) {
    if (op.isBinaryNot())
      visitUnary(op, "~");
    else
      visitBinary(op, "^");
  }

  void visitComb(circt::comb::ICmpOp op) {
    // this is copied from ExportVerilog.cpp. probably need some
    // code refactoring since this is duplicated logic
    const char *symop[] = {"==", "!=", "<",  "<=", ">",
                           ">=", "<",  "<=", ">",  ">="};
    auto pred = static_cast<uint64_t>(op.getPredicate());
    visitBinary(op, symop[pred]);
  }

  // type ops
  using TypeOpVisitor::visitTypeOp;
  // supported
  void visitTypeOp(circt::hw::ConstantOp op) { printConstant(op.getValue()); }

  // SV ops
  using Visitor::visitSV;
  void visitSV(circt::sv::ReadInOutOp op) {
    auto value = op->getOperand(0);
    printExpr(value);
  }

  // dispatch
  void visitInvalidComb(Operation *op) { return dispatchTypeOpVisitor(op); }
  void visitInvalidTypeOp(Operation *op) { return dispatchSVVisitor(op); }

  // handle some sv construct
  void visitInvalidSV(Operation *op) {
    op->emitOpError("Unsupported operation in debug expression");
    abort();
  }

  void visitBinary(mlir::Operation *op, mlir::StringRef opStr) {
    // always emit paraphrases
    os << '(';
    auto left = op->getOperand(0);
    printExpr(left);
    os << ' ' << opStr << ' ';
    auto right = op->getOperand(1);
    printExpr(right);
    os << ')';
  }

  void visitUnary(mlir::Operation *op, mlir::StringRef opStr) {
    // always emit paraphrases
    os << '(';
    os << opStr;
    auto target = op->getOperand(0);
    printExpr(target);
    os << ')';
  }

private:
  llvm::raw_ostream &os;
  HWModuleInfo *module;

  void printConstant(const llvm::APInt &value) {
    bool isNegated = false;
    if (value.isNegative() && !value.isMinSignedValue()) {
      os << '-';
      isNegated = true;
    }
    SmallString<32> valueStr;
    if (isNegated) {
      (-value).toStringUnsigned(valueStr, 10);
    } else {
      value.toStringUnsigned(valueStr, 10);
    }
    os << valueStr;
  }
};

class DebugStmtVisitor : public circt::hw::StmtVisitor<DebugStmtVisitor>,
                         public circt::sv::Visitor<DebugStmtVisitor, void> {
public:
  DebugStmtVisitor(HWDebugBuilder &builder, HWModuleInfo *module)
      : builder(builder), module(module), currentScope(module) {}

  using StmtVisitor::visitStmt;

  void visitStmt(circt::hw::InstanceOp op) {
    auto instNameRef = getSymOpName(op);
    // need to find definition names
    auto *mod = op.getReferencedModule(nullptr);
    auto moduleNameStr = circt::hw::getVerilogModuleNameAttr(mod).strref();
    module->instances.insert(std::make_pair(instNameRef, moduleNameStr));
  }

  using Visitor::visitSV;

  void visitSV(circt::sv::RegOp op) {
    // we treat this as a generator variable
    // only generate if we have annotated in the frontend
    if (hasDebug(op)) {
      auto *var = builder.createVarDef(op);
      module->variables.emplace_back(var);

      if (currentScope) {
        auto *varDecl = builder.createVarDeclaration(op);
        currentScope->scopes.emplace_back(varDecl);
      }
    }
  }

  void visitSV(circt::sv::WireOp op) {
    if (hasDebug(op)) {
      auto *var = builder.createVarDef(op);
      module->variables.emplace_back(var);

      if (currentScope) {
        auto *varDecl = builder.createVarDeclaration(op);
        currentScope->scopes.emplace_back(varDecl);
      }
    }
  }

  // assignment
  // we only care about the target of the assignment
  void visitSV(circt::sv::AssignOp op) {
    if (hasDebug(op)) {
      handleAssign(op.getDest(), op.getSrc(), op.getSrc().getDefiningOp(), op);
    }
  }

  void visitSV(circt::sv::BPAssignOp op) {
    if (hasDebug(op)) {
      handleAssign(op.getDest(), op.getSrc(), op.getSrc().getDefiningOp(), op);
    }
  }

  void visitSV(circt::sv::PAssignOp op) {
    if (hasDebug(op)) {
      handleAssign(op.getDest(), op.getSrc(), op.getSrc().getDefiningOp(), op);
    }
  }

  void visitStmt(circt::hw::OutputOp op) {
    hw::HWModuleOp parent = op->getParentOfType<hw::HWModuleOp>();
    for (auto i = 0u; i < parent.getPorts().outputs.size(); i++) {
      auto operand = op.getOperand(i);
      // if the module has been annotated, we can still set the debug attr.
      // this is because output doesn't need to have SSA form
      if (auto *defOp = operand.getDefiningOp()) {
        if (!hasDebug(defOp)) {
          auto *varDef = module->outputVars[i];
          defOp->setAttr(
              "hw.debug.name",
              mlir::StringAttr::get(defOp->getContext(), varDef->name));
          defOp->setAttr(
              "hw.debug.value",
              mlir::StringAttr::get(defOp->getContext(), varDef->value));
        }
      }
      auto *assign = builder.createAssign(operand, op);
      if (assign)
        currentScope->scopes.emplace_back(assign);
    }
  }

  // visit blocks
  void visitSV(circt::sv::AlwaysOp op) {
    // creating a scope
    auto *scope = builder.createScope(op, currentScope);
    auto *temp = currentScope;
    currentScope = scope;
    visitBlock(*op.getBodyBlock());
    currentScope = temp;
  }

  void visitSV(circt::sv::AlwaysCombOp op) { // creating a scope
    auto *scope = builder.createScope(op, currentScope);
    auto *temp = currentScope;
    currentScope = scope;
    visitBlock(*op.getBodyBlock());
    currentScope = temp;
  }

  void visitSV(circt::sv::AlwaysFFOp op) {
    if (op.getResetBlock()) {
      // creating a scope
      auto *scope = builder.createScope(op, currentScope);
      auto *temp = currentScope;
      currentScope = scope;
      visitBlock(*op.getResetBlock());
      currentScope = temp;
    }
    {
      // creating a scope
      auto *scope = builder.createScope(op, currentScope);
      auto *temp = currentScope;
      currentScope = scope;
      visitBlock(*op.getBodyBlock());
      currentScope = temp;
    }
  }

  void visitSV(circt::sv::InitialOp op) { // creating a scope
    auto *scope = builder.createScope(op, currentScope);
    auto *temp = currentScope;
    currentScope = scope;
    visitBlock(*op.getBodyBlock());
    currentScope = temp;
  }

  void visitSV(circt::sv::IfOp op) {
    // first, the statement itself is a line
    auto cond = getCondString(op.getCond());
    if (cond.empty()) {
      op.getCond().getDefiningOp()->emitError(
          "Unsupported if statement condition");
      cond = getCondString(op.getCond());
      return;
    }
    builder.createScope(op, currentScope);
    if (auto *body = op.getThenBlock()) {
      // true
      if (!body->empty()) {
        auto *trueBlock = builder.createScope(op, currentScope);
        auto *temp = currentScope;
        currentScope = trueBlock;
        trueBlock->condition = mlir::StringAttr::get(op.getContext(), cond);
        visitBlock(*body);
        currentScope = temp;
      }
    }
    if (op.hasElse()) {
      auto *elseBody = op.getElseBlock();
      // false
      if (!elseBody->empty()) {
        auto *elseBlock = builder.createScope(op, currentScope);
        auto *temp = currentScope;
        currentScope = elseBlock;
        elseBlock->condition =
            mlir::StringAttr::get(op->getContext(), "!" + cond);
        visitBlock(*elseBody);
        currentScope = temp;
      }
    }
  }

  void visitSV(circt::sv::CaseOp op) {
    auto cond = getCondString(op.getCond());
    if (cond.empty()) {
      op->emitError("Unsupported case statement condition");
      return;
    }

    auto addScope = [op, this](const std::string &caseCond,
                               mlir::Block *block) {
      // use nullptr since it's auxiliary
      auto *scope = builder.createScope(nullptr, currentScope);
      auto *temp = currentScope;
      currentScope = scope;
      scope->condition = StringAttr::get(op->getContext(), caseCond);
      visitBlock(*block);
      currentScope = temp;
    };

    mlir::Block *defaultBlock = nullptr;
    llvm::SmallVector<uint64_t> values;
    for (auto const &caseInfo : op.getCases()) {
      auto const &pattern = caseInfo.pattern;
      if (pattern->getKind() ==
          circt::sv::CasePattern::CasePatternKind::CPK_default) {
        defaultBlock = caseInfo.block;
      } else {
        // Currently, hgdb doesn't support z or ?
        // so we have to turn the pattern into an integer
        auto attr = pattern->attr();
        if (auto value = mlir::isa<mlir::IntegerAttr>(attr)) {
          auto caseCond = cond + " == " + std::to_string(value);
          addScope(caseCond, caseInfo.block);
          values.emplace_back(value);
        }
      }
    }
    if (defaultBlock) {
      // negate all values
      std::string defaultCond;
      for (auto i = 0u; i < values.size(); i++) {
        defaultCond.append("(" + cond + " != " + std::to_string(values[i]) +
                           ")");
        if (i != (values.size() - 1)) {
          defaultCond.append(" && ");
        }
      }
      addScope(defaultCond, defaultBlock);
    }
  }

  // ignore invalid stuff
  void visitInvalidStmt(Operation *op) { dispatchSVVisitor(op); }
  void visitInvalidSV(Operation *) {}
  void visitUnhandledStmt(Operation *) {}
  void visitUnhandledSV(Operation *) {}

  void dispatch(mlir::Operation *op) { dispatchStmtVisitor(op); }

  void visitBlock(mlir::Block &block) {
    for (auto &op : block) {
      dispatch(&op);
    }
  }

private:
  HWDebugBuilder &builder;
  HWModuleInfo *module;
  HWDebugScope *currentScope;

  bool hasDebug(mlir::Operation *op) {
    auto r = op && op->hasAttr("hw.debug.name");
    return r;
  }

  std::string getCondString(mlir::Value value) const {
    std::string cond;
    llvm::raw_string_ostream os(cond);
    DebugExprPrinter p(os, module);
    p.printExpr(value);
    return cond;
  }

  // NOLINTNEXTLINE
  void handleAssign(mlir::Value target, mlir::Value value, mlir::Operation *op,
                    mlir::Operation *assignOp) {
    bool handled = false;
    if (op) {
      mlir::TypeSwitch<Operation *, void>(op).Case<circt::comb::MuxOp>(
          [&](circt::comb::MuxOp mux) {
            // create a new scope, which can be merged later on
            auto *temp = currentScope;
            // true
            {
              auto *scope =
                  builder.createScope(mux.getOperation(), currentScope);
              auto cond = getCondString(mux.getCond());
              if (cond.empty()) {
                mux->emitError("Unable to obtain mux condition expression");
              }
              scope->condition = StringAttr::get(op->getContext(), cond);
              currentScope = scope;
              handleAssign(target, mux.getTrueValue(),
                           mux.getTrueValue().getDefiningOp(), assignOp);
            }
            currentScope = temp;

            {
              auto *scope =
                  builder.createScope(mux.getOperation(), currentScope);
              auto cond = getCondString(mux.getCond());
              if (cond.empty()) {
                mux->emitError("Unable to obtain mux condition expression");
              }
              scope->condition =
                  mlir::StringAttr::get(op->getContext(), "!" + cond);
              currentScope = scope;
              handleAssign(target, mux.getFalseValue(),
                           mux.getFalseValue().getDefiningOp(), assignOp);
            }
            currentScope = temp;
            handled = true;
          });
    }
    // not handled yet, create an assignment
    if (!handled) {
      auto *assign = builder.createAssign(target, op ? op : assignOp);
      if (assign)
        currentScope->scopes.emplace_back(assign);
    }
  }
};

mlir::StringAttr getFilenameFromScopeOp(HWDebugScope *scope) {
  // if it's fused location, the
  auto loc = getFileLineColLocFromLoc(scope->op->getLoc());
  return loc ? loc.getFilename() : mlir::StringAttr{};
}

// NOLINTNEXTLINE
void setScopeFilename(HWDebugScope *scope, HWDebugBuilder &builder) {
  // assuming the current scope is already fixed
  auto scopeFilename = getFilenameFromScopeOp(scope);
  for (auto i = 0u; i < scope->scopes.size(); i++) {
    auto *entry = scope->scopes[i];
    auto entryFilename = getFilenameFromScopeOp(entry);
    // unable to determine this scope's filename
    if (!entryFilename) {
      // delete it from the scope
      scope->scopes[i] = nullptr;
      continue;
    }
    if (entryFilename != scopeFilename ||
        scope->type() != HWDebugScopeType::Block) {
      // need to set this entry's filename
      if (entry->type() == HWDebugScopeType::Block) {
        // set its filename
        entry->filename = entryFilename;
      } else {
        // need to create a scope to contain this one
        auto *newScope = builder.createScope(entry->op);
        // set the line to 0 since it's an artificial scope
        newScope->line = 0;
        newScope->filename = entryFilename;
        newScope->scopes.emplace_back(entry);
        entry->parent = newScope;
        newScope->parent = scope;
        scope->scopes[i] = newScope;
      }
    }
  }
  // merge scopes with the same filename
  // we assume at this stage most of the entries are block entry now
  // we can only merge
  llvm::DenseMap<std::pair<mlir::StringRef, mlir::StringAttr>, HWDebugScope *>
      filenameMapping;
  for (auto i = 0u; i < scope->scopes.size(); i++) {
    auto *entry = scope->scopes[i];
    if (!entry)
      continue;
    if (entry->type() == HWDebugScopeType::Block) {
      auto filename = entry->filename;
      auto cond = entry->condition;
      auto keyEntry = std::make_pair(filename, cond);
      if (filenameMapping.find(keyEntry) == filenameMapping.end()) {
        filenameMapping[keyEntry] = entry;
      } else {
        auto *parent = filenameMapping[keyEntry];
        // merge
        parent->scopes.reserve(entry->scopes.size() + parent->scopes.size());
        for (auto *p : entry->scopes) {
          parent->scopes.emplace_back(p);
          p->parent = p;
        }

        // delete the old entry
        scope->scopes[i] = nullptr;
      }
    }
  }
  // clear the nullptr
  scope->scopes.erase(std::remove_if(scope->scopes.begin(), scope->scopes.end(),
                                     [](auto *p) { return !p; }),
                      scope->scopes.end());

  // recursively setting filenames
  for (auto *entry : scope->scopes) {
    setScopeFilename(entry, builder);
  }
}

void exportDebugTable(mlir::ModuleOp moduleOp, Optional<std::string> filename) {
  // collect all the files
  HWDebugContext context;
  HWDebugBuilder builder(context);
  for (auto &op : *moduleOp.getBody()) {
    mlir::TypeSwitch<mlir::Operation *>(&op).Case<circt::hw::HWModuleOp>(
        [&builder](circt::hw::HWModuleOp mod) {
          // get verilog name
          auto defName = circt::hw::getVerilogModuleNameAttr(mod);
          auto *module = builder.createModule(&mod);
          module->name = defName;
          DebugStmtVisitor visitor(builder, module);
          auto *body = mod.getBodyBlock();
          visitor.visitBlock(*body);
        });
  }
  // Fixing filenames and other scope ordering
  auto const &modules = context.getModules();
  for (auto *m : modules) {
    setScopeFilename(m, builder);
  }
  auto json = context.toJSON();

  if (filename) {
    std::error_code error;
    if (*filename == "-") {
      llvm::outs() << json;
    } else {
      llvm::raw_fd_ostream os(*filename, error);
      if (!error) {
        os << json;
      }
      os.close();
    }
  } else {
    llvm::outs() << json;
  }
}

struct ExportDebugTablePass
    : public circt::debug::HWExportHGDBBase<ExportDebugTablePass> {
  ExportDebugTablePass(llvm::Optional<std::string> filename)
      : filename(filename) {}

  void runOnOperation() override {
    exportDebugTable(getOperation(), filename);
    markAllAnalysesPreserved();
  }

private:
  Optional<std::string> filename;
};

std::unique_ptr<mlir::Pass>
createExportHGDBPass(Optional<std::string> filename) {
  return std::make_unique<ExportDebugTablePass>(filename);
}

} // namespace circt::debug