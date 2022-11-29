//===- HWOps.cpp - Implement the HW operations ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implement the HW ops.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/HW/CustomDirectiveImpl.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "circt/Dialect/HW/HWSymCache.h"
#include "circt/Dialect/HW/HWVisitors.h"
#include "circt/Dialect/HW/InstanceImplementation.h"
#include "circt/Dialect/HW/ModuleImplementation.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/FunctionImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

using namespace circt;
using namespace hw;
using mlir::TypedAttr;

/// Flip a port direction.
PortDirection hw::flip(PortDirection direction) {
  switch (direction) {
  case PortDirection::INPUT:
    return PortDirection::OUTPUT;
  case PortDirection::OUTPUT:
    return PortDirection::INPUT;
  case PortDirection::INOUT:
    return PortDirection::INOUT;
  }
  llvm_unreachable("unknown PortDirection");
}

bool hw::isValidIndexBitWidth(Value index, Value array) {
  hw::ArrayType arrayType =
      hw::getCanonicalType(array.getType()).dyn_cast<hw::ArrayType>();
  assert(arrayType && "expected array type");
  unsigned indexWidth = index.getType().getIntOrFloatBitWidth();
  auto requiredWidth = llvm::Log2_64_Ceil(arrayType.getSize());
  return requiredWidth == 0 ? (indexWidth == 0 || indexWidth == 1)
                            : indexWidth == requiredWidth;
}

/// Return true if the specified operation is a combinational logic op.
bool hw::isCombinational(Operation *op) {
  struct IsCombClassifier : public TypeOpVisitor<IsCombClassifier, bool> {
    bool visitInvalidTypeOp(Operation *op) { return false; }
    bool visitUnhandledTypeOp(Operation *op) { return true; }
  };

  return (op->getDialect() && op->getDialect()->getNamespace() == "comb") ||
         IsCombClassifier().dispatchTypeOpVisitor(op);
}

static Value foldStructExtract(Operation *inputOp, StringRef field) {
  // A struct extract of a struct create -> corresponding struct create operand.
  if (auto structCreate = dyn_cast_or_null<StructCreateOp>(inputOp)) {
    auto ty = type_cast<StructType>(structCreate.getResult().getType());
    if (auto idx = ty.getFieldIndex(field))
      return structCreate.getOperand(*idx);
    return {};
  }
  // Extracting injected field -> corresponding field
  if (auto structInject = dyn_cast_or_null<StructInjectOp>(inputOp)) {
    if (structInject.getField() != field)
      return {};
    return structInject.getNewValue();
  }
  return {};
}

/// Get a special name to use when printing the entry block arguments of the
/// region contained by an operation in this dialect.
static void getAsmBlockArgumentNamesImpl(mlir::Region &region,
                                         OpAsmSetValueNameFn setNameFn) {
  if (region.empty())
    return;
  // Assign port names to the bbargs.
  auto *module = region.getParentOp();

  auto *block = &region.front();
  for (size_t i = 0, e = block->getNumArguments(); i != e; ++i) {
    auto name = getModuleArgumentName(module, i);
    if (!name.empty())
      setNameFn(block->getArgument(i), name);
  }
}

enum class Delimiter {
  None,
  Paren,               // () enclosed list
  OptionalLessGreater, // <> enclosed list or absent
};

/// Check parameter specified by `value` to see if it is valid according to the
/// module's parameters.  If not, emit an error to the diagnostic provided as an
/// argument to the lambda 'instanceError' and return failure, otherwise return
/// success.
///
/// If `disallowParamRefs` is true, then parameter references are not allowed.
LogicalResult hw::checkParameterInContext(
    Attribute value, ArrayAttr moduleParameters,
    const instance_like_impl::EmitErrorFn &instanceError,
    bool disallowParamRefs) {
  // Literals are always ok.  Their types are already known to match
  // expectations.
  if (value.isa<IntegerAttr>() || value.isa<FloatAttr>() ||
      value.isa<StringAttr>() || value.isa<ParamVerbatimAttr>())
    return success();

  // Check both subexpressions of an expression.
  if (auto expr = value.dyn_cast<ParamExprAttr>()) {
    for (auto op : expr.getOperands())
      if (failed(checkParameterInContext(op, moduleParameters, instanceError,
                                         disallowParamRefs)))
        return failure();
    return success();
  }

  // Parameter references need more analysis to make sure they are valid within
  // this module.
  if (auto parameterRef = value.dyn_cast<ParamDeclRefAttr>()) {
    auto nameAttr = parameterRef.getName();

    // Don't allow references to parameters from the default values of a
    // parameter list.
    if (disallowParamRefs) {
      instanceError([&](auto &diag) {
        diag << "parameter " << nameAttr
             << " cannot be used as a default value for a parameter";
        return false;
      });
      return failure();
    }

    // Find the corresponding attribute in the module.
    for (auto param : moduleParameters) {
      auto paramAttr = param.cast<ParamDeclAttr>();
      if (paramAttr.getName() != nameAttr)
        continue;

      // If the types match then the reference is ok.
      if (paramAttr.getType() == parameterRef.getType())
        return success();

      instanceError([&](auto &diag) {
        diag << "parameter " << nameAttr << " used with type "
             << parameterRef.getType() << "; should have type "
             << paramAttr.getType();
        return true;
      });
      return failure();
    }

    instanceError([&](auto &diag) {
      diag << "use of unknown parameter " << nameAttr;
      return true;
    });
    return failure();
  }

  instanceError([&](auto &diag) {
    diag << "invalid parameter value " << value;
    return false;
  });
  return failure();
}

/// Check parameter specified by `value` to see if it is valid within the scope
/// of the specified module `module`.  If not, emit an error at the location of
/// `usingOp` and return failure, otherwise return success.  If `usingOp` is
/// null, then no diagnostic is generated.
///
/// If `disallowParamRefs` is true, then parameter references are not allowed.
LogicalResult hw::checkParameterInContext(Attribute value, Operation *module,
                                          Operation *usingOp,
                                          bool disallowParamRefs) {
  instance_like_impl::EmitErrorFn emitError =
      [&](const std::function<bool(InFlightDiagnostic &)> &fn) {
        if (usingOp) {
          auto diag = usingOp->emitOpError();
          if (fn(diag))
            diag.attachNote(module->getLoc()) << "module declared here";
        }
      };

  return checkParameterInContext(value,
                                 module->getAttrOfType<ArrayAttr>("parameters"),
                                 emitError, disallowParamRefs);
}

/// Return true if the specified attribute tree is made up of nodes that are
/// valid in a parameter expression.
bool hw::isValidParameterExpression(Attribute attr, Operation *module) {
  return succeeded(checkParameterInContext(attr, module, nullptr, false));
}

/// Return the symbol (if any, else null) on the corresponding input port
/// argument.
StringAttr hw::getArgSym(Operation *op, unsigned i) {
  assert(isAnyModuleOrInstance(op) &&
         "Can only get module ports from an instance or module");
  StringAttr sym = {};
  auto argAttrs = op->getAttrOfType<ArrayAttr>(
      mlir::function_interface_impl::getArgDictAttrName());
  if (argAttrs && (i < argAttrs.size()))
    if (auto s = argAttrs[i].cast<DictionaryAttr>())
      if (auto symRef = s.get("hw.exportPort"))
        sym = symRef.cast<FlatSymbolRefAttr>().getAttr();
  return sym;
}

/// Return the symbol (if any, else null) on the corresponding output port
/// argument.
StringAttr hw::getResultSym(Operation *op, unsigned i) {
  assert(isAnyModuleOrInstance(op) &&
         "Can only get module ports from an instance or module");
  StringAttr sym = {};
  auto resAttrs = op->getAttrOfType<ArrayAttr>(
      mlir::function_interface_impl::getResultDictAttrName());
  if (resAttrs && (i < resAttrs.size()))
    if (auto s = resAttrs[i].cast<DictionaryAttr>())
      if (auto symRef = s.get("hw.exportPort"))
        sym = symRef.cast<FlatSymbolRefAttr>().getAttr();
  return sym;
}

HWModulePortAccessor::HWModulePortAccessor(Location loc,
                                           const ModulePortInfo &info,
                                           Region &bodyRegion)
    : info(info) {
  inputArgs.resize(info.inputs.size());
  for (auto [i, barg] : llvm::enumerate(bodyRegion.getArguments())) {
    inputIdx[info.inputs[i].name.str()] = i;
    inputArgs[i] = barg;
  }

  outputOperands.resize(info.outputs.size());
  for (auto [i, outputInfo] : llvm::enumerate(info.outputs)) {
    outputIdx[outputInfo.name.str()] = i;
  }
}

void HWModulePortAccessor::setOutput(unsigned i, Value v) {
  assert(outputOperands.size() > i && "invalid output index");
  assert(outputOperands[i] == Value() && "output already set");
  outputOperands[i] = v;
}

Value HWModulePortAccessor::getInput(unsigned i) {
  assert(inputArgs.size() > i && "invalid input index");
  return inputArgs[i];
}
Value HWModulePortAccessor::getInput(StringRef name) {
  return getInput(inputIdx.find(name.str())->second);
}
void HWModulePortAccessor::setOutput(StringRef name, Value v) {
  setOutput(outputIdx.find(name.str())->second, v);
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

void ConstantOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printAttribute(getValueAttr());
  p.printOptionalAttrDict((*this)->getAttrs(), /*elidedAttrs=*/{"value"});
}

ParseResult ConstantOp::parse(OpAsmParser &parser, OperationState &result) {
  IntegerAttr valueAttr;

  if (parser.parseAttribute(valueAttr, "value", result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();

  result.addTypes(valueAttr.getType());
  return success();
}

LogicalResult ConstantOp::verify() {
  // If the result type has a bitwidth, then the attribute must match its width.
  if (getValue().getBitWidth() != getType().cast<IntegerType>().getWidth())
    return emitError(
        "hw.constant attribute bitwidth doesn't match return type");

  return success();
}

/// Build a ConstantOp from an APInt, infering the result type from the
/// width of the APInt.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       const APInt &value) {

  auto type = IntegerType::get(builder.getContext(), value.getBitWidth());
  auto attr = builder.getIntegerAttr(type, value);
  return build(builder, result, type, attr);
}

/// Build a ConstantOp from an APInt, infering the result type from the
/// width of the APInt.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       IntegerAttr value) {
  return build(builder, result, value.getType(), value);
}

/// This builder allows construction of small signed integers like 0, 1, -1
/// matching a specified MLIR IntegerType.  This shouldn't be used for general
/// constant folding because it only works with values that can be expressed in
/// an int64_t.  Use APInt's instead.
void ConstantOp::build(OpBuilder &builder, OperationState &result, Type type,
                       int64_t value) {
  auto numBits = type.cast<IntegerType>().getWidth();
  build(builder, result, APInt(numBits, (uint64_t)value, /*isSigned=*/true));
}

void ConstantOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  auto intTy = getType();
  auto intCst = getValue();

  // Sugar i1 constants with 'true' and 'false'.
  if (intTy.cast<IntegerType>().getWidth() == 1)
    return setNameFn(getResult(), intCst.isZero() ? "false" : "true");

  // Otherwise, build a complex name with the value and type.
  SmallVector<char, 32> specialNameBuffer;
  llvm::raw_svector_ostream specialName(specialNameBuffer);
  specialName << 'c' << intCst << '_' << intTy;
  setNameFn(getResult(), specialName.str());
}

OpFoldResult ConstantOp::fold(ArrayRef<Attribute> constants) {
  assert(constants.empty() && "constant has no operands");
  return getValueAttr();
}

//===----------------------------------------------------------------------===//
// ParamValueOp
//===----------------------------------------------------------------------===//

static ParseResult parseParamValue(OpAsmParser &p, Attribute &value,
                                   Type &resultType) {
  if (p.parseType(resultType) || p.parseEqual() ||
      p.parseAttribute(value, resultType))
    return failure();
  return success();
}

static void printParamValue(OpAsmPrinter &p, Operation *, Attribute value,
                            Type resultType) {
  p << resultType << " = ";
  p.printAttributeWithoutType(value);
}

LogicalResult ParamValueOp::verify() {
  // Check that the attribute expression is valid in this module.
  return checkParameterInContext(
      getValue(), (*this)->getParentOfType<hw::HWModuleOp>(), *this);
}

OpFoldResult ParamValueOp::fold(ArrayRef<Attribute> constants) {
  assert(constants.empty() && "hw.param.value has no operands");
  return getValueAttr();
}

//===----------------------------------------------------------------------===//
// HWModuleOp
//===----------------------------------------------------------------------===/

/// Return true if this is an hw.module, external module, generated module etc.
bool hw::isAnyModule(Operation *module) {
  return isa<HWModuleOp>(module) || isa<HWModuleExternOp>(module) ||
         isa<HWModuleGeneratedOp>(module);
}

/// Return true if isAnyModule or instance.
bool hw::isAnyModuleOrInstance(Operation *moduleOrInstance) {
  return isAnyModule(moduleOrInstance) || isa<InstanceOp>(moduleOrInstance);
}

/// Return the signature for a module as a function type from the module itself
/// or from an hw::InstanceOp.
FunctionType hw::getModuleType(Operation *moduleOrInstance) {
  if (auto instance = dyn_cast<InstanceOp>(moduleOrInstance)) {
    SmallVector<Type> inputs(instance->getOperandTypes());
    SmallVector<Type> results(instance->getResultTypes());
    return FunctionType::get(instance->getContext(), inputs, results);
  }

  assert(isAnyModule(moduleOrInstance) &&
         "must be called on instance or module");
  auto typeAttr =
      moduleOrInstance->getAttrOfType<TypeAttr>(HWModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

/// Return the name to use for the Verilog module that we're referencing
/// here.  This is typically the symbol, but can be overridden with the
/// verilogName attribute.
StringAttr hw::getVerilogModuleNameAttr(Operation *module) {
  auto nameAttr = module->getAttrOfType<StringAttr>("verilogName");
  if (nameAttr)
    return nameAttr;

  return module->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
}

/// Return the port name for the specified argument or result.
StringAttr hw::getModuleArgumentNameAttr(Operation *module, size_t argNo) {
  auto argNames = module->getAttrOfType<ArrayAttr>("argNames");
  // Tolerate malformed IR here to enable debug printing etc.
  if (argNames && argNo < argNames.size())
    return argNames[argNo].cast<StringAttr>();
  return StringAttr();
}

StringAttr hw::getModuleResultNameAttr(Operation *module, size_t resultNo) {
  auto resultNames = module->getAttrOfType<ArrayAttr>("resultNames");
  // Tolerate malformed IR here to enable debug printing etc.
  if (resultNames && resultNo < resultNames.size())
    return resultNames[resultNo].cast<StringAttr>();
  return StringAttr();
}

void hw::setModuleArgumentNames(Operation *module, ArrayRef<Attribute> names) {
  assert(isAnyModule(module) && "Must be called on a module");
  assert(getModuleType(module).getNumInputs() == names.size() &&
         "incorrect number of arguments names specified");
  module->setAttr("argNames", ArrayAttr::get(module->getContext(), names));
}

void hw::setModuleResultNames(Operation *module, ArrayRef<Attribute> names) {
  assert(isAnyModule(module) && "Must be called on a module");
  assert(getModuleType(module).getNumResults() == names.size() &&
         "incorrect number of arguments names specified");
  module->setAttr("resultNames", ArrayAttr::get(module->getContext(), names));
}

// Flag for parsing different module types
enum ExternModKind { PlainMod, ExternMod, GenMod };

static void buildModule(OpBuilder &builder, OperationState &result,
                        StringAttr name, const ModulePortInfo &ports,
                        ArrayAttr parameters,
                        ArrayRef<NamedAttribute> attributes,
                        StringAttr comment) {
  using namespace mlir::function_interface_impl;

  // Add an attribute for the name.
  result.addAttribute(SymbolTable::getSymbolAttrName(), name);

  SmallVector<Attribute> argNames, resultNames;
  SmallVector<Type, 4> argTypes, resultTypes;
  SmallVector<Attribute> argAttrs, resultAttrs, debugAttrs;
  auto exportPortIdent = StringAttr::get(builder.getContext(), "hw.exportPort");
  bool hasDebugAttr = false;

  for (auto elt : ports.inputs) {
    if (elt.direction == PortDirection::INOUT && !elt.type.isa<hw::InOutType>())
      elt.type = hw::InOutType::get(elt.type);
    argTypes.push_back(elt.type);
    argNames.push_back(elt.name);
    Attribute attr;
    if (elt.sym && !elt.sym.getValue().empty())
      attr = builder.getDictionaryAttr(
          {{exportPortIdent, FlatSymbolRefAttr::get(elt.sym)}});
    else
      attr = builder.getDictionaryAttr({});
    argAttrs.push_back(attr);
    debugAttrs.push_back(elt.debugAttr);
    if (elt.debugAttr)
      hasDebugAttr = true;
  }

  for (auto elt : ports.outputs) {
    resultTypes.push_back(elt.type);
    resultNames.push_back(elt.name);
    Attribute attr;
    if (elt.sym && !elt.sym.getValue().empty())
      attr = builder.getDictionaryAttr(
          {{exportPortIdent, FlatSymbolRefAttr::get(elt.sym)}});
    else
      attr = builder.getDictionaryAttr({});
    resultAttrs.push_back(attr);
    debugAttrs.push_back(elt.debugAttr);
    if (elt.debugAttr)
      hasDebugAttr = true;
  }

  // Allow clients to pass in null for the parameters list.
  if (!parameters)
    parameters = builder.getArrayAttr({});

  // Record the argument and result types as an attribute.
  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));
  result.addAttribute("argNames", builder.getArrayAttr(argNames));
  result.addAttribute("resultNames", builder.getArrayAttr(resultNames));
  result.addAttribute("hw.debug.name", builder.getArrayAttr(debugAttrs));
  result.addAttribute(mlir::function_interface_impl::getArgDictAttrName(),
                      builder.getArrayAttr(argAttrs));
  result.addAttribute(mlir::function_interface_impl::getResultDictAttrName(),
                      builder.getArrayAttr(resultAttrs));
  result.addAttribute("parameters", parameters);
  if (!comment)
    comment = builder.getStringAttr("");
  result.addAttribute("comment", comment);
  result.addAttributes(attributes);
  if (hasDebugAttr && !debugAttrs.empty())
    result.addAttribute("hw.debug.name", builder.getArrayAttr(debugAttrs));
  result.addRegion();
}

/// Internal implementation of argument/result insertion and removal on modules.
static void modifyModuleArgs(
    MLIRContext *context, ArrayRef<std::pair<unsigned, PortInfo>> insertArgs,
    ArrayRef<unsigned> removeArgs, ArrayRef<Attribute> oldArgNames,
    ArrayRef<Type> oldArgTypes, ArrayRef<Attribute> oldArgAttrs,
    SmallVector<Attribute> &newArgNames, SmallVector<Type> &newArgTypes,
    SmallVector<Attribute> &newArgAttrs, Block *body = nullptr) {

#ifndef NDEBUG
  // Check that the `insertArgs` and `removeArgs` indices are in ascending
  // order.
  assert(llvm::is_sorted(insertArgs,
                         [](auto &a, auto &b) { return a.first < b.first; }) &&
         "insertArgs must be in ascending order");
  assert(llvm::is_sorted(removeArgs, [](auto &a, auto &b) { return a < b; }) &&
         "removeArgs must be in ascending order");
#endif

  auto oldArgCount = oldArgTypes.size();
  auto newArgCount = oldArgCount + insertArgs.size() - removeArgs.size();
  assert((int)newArgCount >= 0);

  newArgNames.reserve(newArgCount);
  newArgTypes.reserve(newArgCount);
  newArgAttrs.reserve(newArgCount);

  auto exportPortAttrName = StringAttr::get(context, "hw.exportPort");
  auto emptyDictAttr = DictionaryAttr::get(context, {});

  BitVector erasedIndices;
  if (body)
    erasedIndices.resize(oldArgCount + insertArgs.size());

  for (unsigned argIdx = 0, idx = 0; argIdx <= oldArgCount; ++argIdx, ++idx) {
    // Insert new ports at this position.
    while (!insertArgs.empty() && insertArgs[0].first == argIdx) {
      auto port = insertArgs[0].second;
      if (port.direction == PortDirection::INOUT && !port.type.isa<InOutType>())
        port.type = InOutType::get(port.type);
      Attribute attr =
          (port.sym && !port.sym.getValue().empty())
              ? DictionaryAttr::get(
                    context,
                    {{exportPortAttrName, FlatSymbolRefAttr::get(port.sym)}})
              : emptyDictAttr;
      newArgNames.push_back(port.name);
      newArgTypes.push_back(port.type);
      newArgAttrs.push_back(attr);
      insertArgs = insertArgs.drop_front();
      if (body)
        body->insertArgument(idx++, port.type, UnknownLoc::get(context));
    }
    if (argIdx == oldArgCount)
      break;

    // Migrate the old port at this position.
    bool removed = false;
    while (!removeArgs.empty() && removeArgs[0] == argIdx) {
      removeArgs = removeArgs.drop_front();
      removed = true;
    }

    if (removed) {
      if (body)
        erasedIndices.set(idx);
    } else {
      newArgNames.push_back(oldArgNames[argIdx]);
      newArgTypes.push_back(oldArgTypes[argIdx]);
      newArgAttrs.push_back(oldArgAttrs.empty() ? emptyDictAttr
                                                : oldArgAttrs[argIdx]);
    }
  }

  if (body)
    body->eraseArguments(erasedIndices);

  assert(newArgNames.size() == newArgCount);
  assert(newArgTypes.size() == newArgCount);
  assert(newArgAttrs.size() == newArgCount);
}

/// Insert and remove ports of a module. The insertion and removal indices must
/// be in ascending order. The indices refer to the port positions before any
/// insertion or removal occurs. Ports inserted at the same index will appear in
/// the module in the same order as they were listed in the `insert*` array.
///
/// The operation must be any of the module-like operations.
void hw::modifyModulePorts(
    Operation *op, ArrayRef<std::pair<unsigned, PortInfo>> insertInputs,
    ArrayRef<std::pair<unsigned, PortInfo>> insertOutputs,
    ArrayRef<unsigned> removeInputs, ArrayRef<unsigned> removeOutputs,
    Block *body) {
  auto moduleOp = cast<mlir::FunctionOpInterface>(op);

  auto arrayOrEmpty = [](ArrayAttr attr) {
    return attr ? attr.getValue() : ArrayRef<Attribute>{};
  };

  // Dig up the old argument and result data.
  ArrayRef<Attribute> oldArgNames =
      moduleOp->getAttrOfType<ArrayAttr>("argNames").getValue();
  ArrayRef<Type> oldArgTypes = moduleOp.getArgumentTypes();
  ArrayRef<Attribute> oldArgAttrs =
      arrayOrEmpty(moduleOp->getAttrOfType<ArrayAttr>(
          mlir::function_interface_impl::getArgDictAttrName()));

  ArrayRef<Attribute> oldResultNames =
      moduleOp->getAttrOfType<ArrayAttr>("resultNames").getValue();
  ArrayRef<Type> oldResultTypes = moduleOp.getResultTypes();
  ArrayRef<Attribute> oldResultAttrs =
      arrayOrEmpty(moduleOp->getAttrOfType<ArrayAttr>(
          mlir::function_interface_impl::getResultDictAttrName()));

  // Modify the ports.
  SmallVector<Attribute> newArgNames, newResultNames;
  SmallVector<Type> newArgTypes, newResultTypes;
  SmallVector<Attribute> newArgAttrs, newResultAttrs;

  modifyModuleArgs(moduleOp.getContext(), insertInputs, removeInputs,
                   oldArgNames, oldArgTypes, oldArgAttrs, newArgNames,
                   newArgTypes, newArgAttrs, body);

  modifyModuleArgs(moduleOp.getContext(), insertOutputs, removeOutputs,
                   oldResultNames, oldResultTypes, oldResultAttrs,
                   newResultNames, newResultTypes, newResultAttrs);

  // Update the module operation types and attributes.
  moduleOp.setType(
      FunctionType::get(moduleOp.getContext(), newArgTypes, newResultTypes));
  moduleOp->setAttr("argNames",
                    ArrayAttr::get(moduleOp.getContext(), newArgNames));
  moduleOp->setAttr("resultNames",
                    ArrayAttr::get(moduleOp.getContext(), newResultNames));
  moduleOp->setAttr(mlir::function_interface_impl::getArgDictAttrName(),
                    ArrayAttr::get(moduleOp.getContext(), newArgAttrs));
  moduleOp->setAttr(mlir::function_interface_impl::getResultDictAttrName(),
                    ArrayAttr::get(moduleOp.getContext(), newResultAttrs));
}

void HWModuleOp::build(OpBuilder &builder, OperationState &result,
                       StringAttr name, const ModulePortInfo &ports,
                       ArrayAttr parameters,
                       ArrayRef<NamedAttribute> attributes, StringAttr comment,
                       bool shouldEnsureTerminator) {
  buildModule(builder, result, name, ports, parameters, attributes, comment);

  // Create a region and a block for the body.
  auto *bodyRegion = result.regions[0].get();
  Block *body = new Block();
  bodyRegion->push_back(body);

  // Add arguments to the body block.
  for (auto elt : ports.inputs)
    body->addArgument(elt.type, builder.getUnknownLoc());

  if (shouldEnsureTerminator)
    HWModuleOp::ensureTerminator(*bodyRegion, builder, result.location);
}

void HWModuleOp::build(OpBuilder &builder, OperationState &result,
                       StringAttr name, ArrayRef<PortInfo> ports,
                       ArrayAttr parameters,
                       ArrayRef<NamedAttribute> attributes,
                       StringAttr comment) {
  build(builder, result, name, ModulePortInfo(ports), parameters, attributes,
        comment);
}

void HWModuleOp::build(OpBuilder &builder, OperationState &odsState,
                       StringAttr name, const ModulePortInfo &ports,
                       HWModuleBuilder modBuilder, ArrayAttr parameters,
                       ArrayRef<NamedAttribute> attributes,
                       StringAttr comment) {
  build(builder, odsState, name, ports, parameters, attributes, comment,
        /*shouldEnsureTerminator=*/false);
  auto *bodyRegion = odsState.regions[0].get();
  OpBuilder::InsertionGuard guard(builder);
  auto accessor = HWModulePortAccessor(odsState.location, ports, *bodyRegion);
  builder.setInsertionPointToEnd(&bodyRegion->front());
  modBuilder(builder, accessor);
  // Create output operands.
  llvm::SmallVector<Value> outputOperands = accessor.getOutputOperands();
  builder.create<hw::OutputOp>(odsState.location, outputOperands);
}

void HWModuleOp::modifyPorts(
    ArrayRef<std::pair<unsigned, PortInfo>> insertInputs,
    ArrayRef<std::pair<unsigned, PortInfo>> insertOutputs,
    ArrayRef<unsigned> eraseInputs, ArrayRef<unsigned> eraseOutputs) {
  hw::modifyModulePorts(*this, insertInputs, insertOutputs, eraseInputs,
                        eraseOutputs, getBodyBlock());
}

/// Return the name to use for the Verilog module that we're referencing
/// here.  This is typically the symbol, but can be overridden with the
/// verilogName attribute.
StringAttr HWModuleExternOp::getVerilogModuleNameAttr() {
  if (auto vName = getVerilogNameAttr())
    return vName;

  return (*this)->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
}

StringAttr HWModuleGeneratedOp::getVerilogModuleNameAttr() {
  if (auto vName = getVerilogNameAttr()) {
    return vName;
  }
  return (*this)->getAttrOfType<StringAttr>(
      ::mlir::SymbolTable::getSymbolAttrName());
}

void HWModuleExternOp::build(OpBuilder &builder, OperationState &result,
                             StringAttr name, const ModulePortInfo &ports,
                             StringRef verilogName, ArrayAttr parameters,
                             ArrayRef<NamedAttribute> attributes) {
  buildModule(builder, result, name, ports, parameters, attributes, {});

  if (!verilogName.empty())
    result.addAttribute("verilogName", builder.getStringAttr(verilogName));
}

void HWModuleExternOp::build(OpBuilder &builder, OperationState &result,
                             StringAttr name, ArrayRef<PortInfo> ports,
                             StringRef verilogName, ArrayAttr parameters,
                             ArrayRef<NamedAttribute> attributes) {
  build(builder, result, name, ModulePortInfo(ports), verilogName, parameters,
        attributes);
}

void HWModuleExternOp::modifyPorts(
    ArrayRef<std::pair<unsigned, PortInfo>> insertInputs,
    ArrayRef<std::pair<unsigned, PortInfo>> insertOutputs,
    ArrayRef<unsigned> eraseInputs, ArrayRef<unsigned> eraseOutputs) {
  hw::modifyModulePorts(*this, insertInputs, insertOutputs, eraseInputs,
                        eraseOutputs);
}

void HWModuleExternOp::appendOutputs(
    ArrayRef<std::pair<StringAttr, Value>> outputs) {}

void HWModuleGeneratedOp::build(OpBuilder &builder, OperationState &result,
                                FlatSymbolRefAttr genKind, StringAttr name,
                                const ModulePortInfo &ports,
                                StringRef verilogName, ArrayAttr parameters,
                                ArrayRef<NamedAttribute> attributes) {
  buildModule(builder, result, name, ports, parameters, attributes, {});
  result.addAttribute("generatorKind", genKind);
  if (!verilogName.empty())
    result.addAttribute("verilogName", builder.getStringAttr(verilogName));
}

void HWModuleGeneratedOp::build(OpBuilder &builder, OperationState &result,
                                FlatSymbolRefAttr genKind, StringAttr name,
                                ArrayRef<PortInfo> ports, StringRef verilogName,
                                ArrayAttr parameters,
                                ArrayRef<NamedAttribute> attributes) {
  build(builder, result, genKind, name, ModulePortInfo(ports), verilogName,
        parameters, attributes);
}

void HWModuleGeneratedOp::modifyPorts(
    ArrayRef<std::pair<unsigned, PortInfo>> insertInputs,
    ArrayRef<std::pair<unsigned, PortInfo>> insertOutputs,
    ArrayRef<unsigned> eraseInputs, ArrayRef<unsigned> eraseOutputs) {
  hw::modifyModulePorts(*this, insertInputs, insertOutputs, eraseInputs,
                        eraseOutputs);
}

void HWModuleGeneratedOp::appendOutputs(
    ArrayRef<std::pair<StringAttr, Value>> outputs) {}

/// Return an encapsulated set of information about input and output ports of
/// the specified module or instance.  The input ports always come before the
/// output ports in the list.
ModulePortInfo hw::getModulePortInfo(Operation *op) {
  assert(isAnyModuleOrInstance(op) &&
         "Can only get module ports from an instance or module");

  SmallVector<PortInfo> inputs, outputs;
  auto argTypes = getModuleType(op).getInputs();

  auto argNames = op->getAttrOfType<ArrayAttr>("argNames");
  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    bool isInOut = false;
    auto type = argTypes[i];

    if (auto inout = type.dyn_cast<InOutType>()) {
      isInOut = true;
      type = inout.getElementType();
    }

    auto direction = isInOut ? PortDirection::INOUT : PortDirection::INPUT;
    inputs.push_back(
        {argNames[i].cast<StringAttr>(), direction, type, i, getArgSym(op, i)});
  }

  auto resultNames = op->getAttrOfType<ArrayAttr>("resultNames");
  auto resultTypes = getModuleType(op).getResults();
  for (unsigned i = 0, e = resultTypes.size(); i < e; ++i) {
    outputs.push_back({resultNames[i].cast<StringAttr>(), PortDirection::OUTPUT,
                       resultTypes[i], i, getResultSym(op, i)});
  }
  return ModulePortInfo(inputs, outputs);
}

/// Return an encapsulated set of information about input and output ports of
/// the specified module or instance.  The input ports always come before the
/// output ports in the list.
SmallVector<PortInfo> hw::getAllModulePortInfos(Operation *op) {
  assert(isAnyModuleOrInstance(op) &&
         "Can only get module ports from an instance or module");

  SmallVector<PortInfo> results;
  auto argTypes = getModuleType(op).getInputs();
  auto argNames = op->getAttrOfType<ArrayAttr>("argNames");
  auto debugAttrs = op->getAttrOfType<ArrayAttr>("hw.debug.name");
  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    bool isInOut = false;
    auto type = argTypes[i];

    if (auto inout = type.dyn_cast<InOutType>()) {
      isInOut = true;
      type = inout.getElementType();
    }

    auto direction = isInOut ? PortDirection::INOUT : PortDirection::INPUT;
    auto debugAttr = debugAttrs && debugAttrs[i]
                         ? debugAttrs[i].cast<StringAttr>()
                         : StringAttr{};
    results.push_back({argNames[i].cast<StringAttr>(), direction, type, i,
                       getArgSym(op, i), debugAttr});
  }

  auto resultNames = op->getAttrOfType<ArrayAttr>("resultNames");
  auto resultTypes = getModuleType(op).getResults();
  for (unsigned i = 0, e = resultTypes.size(); i < e; ++i) {
    auto idx = i + argTypes.size();
    auto debugAttr = debugAttrs && (debugAttrs.size() > idx) && debugAttrs[idx]
                         ? debugAttrs[idx].cast<StringAttr>()
                         : StringAttr{};
    results.push_back({resultNames[i].cast<StringAttr>(), PortDirection::OUTPUT,
                       resultTypes[i], i, getResultSym(op, i), debugAttr});
  }
  return results;
}

/// Return the PortInfo for the specified input or inout port.
PortInfo hw::getModuleInOrInoutPort(Operation *op, size_t idx) {
  auto argTypes = getModuleType(op).getInputs();
  auto argNames = op->getAttrOfType<ArrayAttr>("argNames");
  bool isInOut = false;
  auto type = argTypes[idx];

  if (auto inout = type.dyn_cast<InOutType>()) {
    isInOut = true;
    type = inout.getElementType();
  }

  auto direction = isInOut ? PortDirection::INOUT : PortDirection::INPUT;
  return {argNames[idx].cast<StringAttr>(), direction, type, idx,
          getArgSym(op, idx)};
}

/// Return the PortInfo for the specified output port.
PortInfo hw::getModuleOutputPort(Operation *op, size_t idx) {
  auto resultNames = op->getAttrOfType<ArrayAttr>("resultNames");
  auto resultTypes = getModuleType(op).getResults();
  assert(idx < resultNames.size() && "invalid result number");
  return {resultNames[idx].cast<StringAttr>(), PortDirection::OUTPUT,
          resultTypes[idx], idx, getResultSym(op, idx)};
}

static bool hasAttribute(StringRef name, ArrayRef<NamedAttribute> attrs) {
  for (auto &argAttr : attrs)
    if (argAttr.getName() == name)
      return true;
  return false;
}

static ParseResult parseHWModuleOp(OpAsmParser &parser, OperationState &result,
                                   ExternModKind modKind = PlainMod) {

  using namespace mlir::function_interface_impl;

  auto loc = parser.getCurrentLocation();

  SmallVector<OpAsmParser::Argument, 4> entryArgs;
  SmallVector<DictionaryAttr> resultAttrs;
  SmallVector<Type, 4> resultTypes;
  ArrayAttr parameters;
  auto &builder = parser.getBuilder();

  // Parse the visibility attribute.
  (void)mlir::impl::parseOptionalVisibilityKeyword(parser, result.attributes);

  // Parse the name as a symbol.
  StringAttr nameAttr;
  if (parser.parseSymbolName(nameAttr, SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  FlatSymbolRefAttr kindAttr;
  if (modKind == GenMod) {
    if (parser.parseComma() ||
        parser.parseAttribute(kindAttr, "generatorKind", result.attributes)) {
      return failure();
    }
  }

  // Parse the function signature.
  bool isVariadic = false;
  SmallVector<Attribute> resultNames;
  if (parseOptionalParameterList(parser, parameters) ||
      module_like_impl::parseModuleFunctionSignature(
          parser, entryArgs, isVariadic, resultTypes, resultAttrs,
          resultNames) ||
      // If function attributes are present, parse them.
      parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();

  // Record the argument and result types as an attribute.  This is necessary
  // for external modules.
  SmallVector<Type> argTypes;
  for (auto &arg : entryArgs)
    argTypes.push_back(arg.type);

  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  auto *context = result.getContext();

  if (hasAttribute("resultNames", result.attributes) ||
      hasAttribute("parameters", result.attributes)) {
    parser.emitError(
        loc, "explicit `resultNames` / `parameters` attributes not allowed");
    return failure();
  }

  // Use the argument and result names if not already specified.
  SmallVector<Attribute> argNames;
  if (!entryArgs.empty()) {
    for (auto &arg : entryArgs)
      argNames.push_back(
          module_like_impl::getPortNameAttr(context, arg.ssaName.name));
  } else if (!argTypes.empty()) {
    // The parser returns empty names in a special way.
    argNames.assign(argTypes.size(), StringAttr::get(context, ""));
  }

  // An explicit `argNames` attribute overrides the MLIR names.  This is how
  // we represent port names that aren't valid MLIR identifiers.  Result and
  // parameter names are printed quoted when they aren't valid identifiers, so
  // they don't need this affordance.
  if (!hasAttribute("argNames", result.attributes))
    result.addAttribute("argNames", ArrayAttr::get(context, argNames));
  result.addAttribute("resultNames", ArrayAttr::get(context, resultNames));
  result.addAttribute("parameters", parameters);
  if (!hasAttribute("comment", result.attributes))
    result.addAttribute("comment", StringAttr::get(context, ""));

  assert(resultAttrs.size() == resultTypes.size());

  // Add the attributes to the function arguments.
  addArgAndResultAttrs(builder, result, entryArgs, resultAttrs);

  // Parse the optional function body.
  auto *body = result.addRegion();
  if (modKind == PlainMod) {
    if (parser.parseRegion(*body, entryArgs))
      return failure();

    HWModuleOp::ensureTerminator(*body, parser.getBuilder(), result.location);
  }
  return success();
}

ParseResult HWModuleOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseHWModuleOp(parser, result);
}

ParseResult HWModuleExternOp::parse(OpAsmParser &parser,
                                    OperationState &result) {
  return parseHWModuleOp(parser, result, ExternMod);
}

ParseResult HWModuleGeneratedOp::parse(OpAsmParser &parser,
                                       OperationState &result) {
  return parseHWModuleOp(parser, result, GenMod);
}

FunctionType getHWModuleOpType(Operation *op) {
  auto typeAttr = op->getAttrOfType<TypeAttr>(HWModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

static void printModuleOp(OpAsmPrinter &p, Operation *op,
                          ExternModKind modKind) {
  using namespace mlir::function_interface_impl;

  FunctionType fnType = getHWModuleOpType(op);
  auto argTypes = fnType.getInputs();
  auto resultTypes = fnType.getResults();

  p << ' ';

  // Print the visibility of the module.
  StringRef visibilityAttrName = SymbolTable::getVisibilityAttrName();
  if (auto visibility = op->getAttrOfType<StringAttr>(visibilityAttrName))
    p << visibility.getValue() << ' ';

  // Print the operation and the function name.
  p.printSymbolName(SymbolTable::getSymbolName(op).getValue());
  if (modKind == GenMod) {
    p << ", ";
    p.printSymbolName(cast<HWModuleGeneratedOp>(op).getGeneratorKind());
  }

  // Print the parameter list if present.
  printOptionalParameterList(p, op, op->getAttrOfType<ArrayAttr>("parameters"));

  bool needArgNamesAttr = false;
  module_like_impl::printModuleSignature(p, op, argTypes, /*isVariadic=*/false,
                                         resultTypes, needArgNamesAttr);

  SmallVector<StringRef, 3> omittedAttrs;
  if (modKind == GenMod)
    omittedAttrs.push_back("generatorKind");
  if (!needArgNamesAttr)
    omittedAttrs.push_back("argNames");
  omittedAttrs.push_back("resultNames");
  omittedAttrs.push_back("parameters");
  omittedAttrs.push_back(visibilityAttrName);
  if (op->getAttrOfType<StringAttr>("comment").getValue().empty())
    omittedAttrs.push_back("comment");

  printFunctionAttributes(p, op, argTypes.size(), resultTypes.size(),
                          omittedAttrs);
}

void HWModuleExternOp::print(OpAsmPrinter &p) {
  printModuleOp(p, *this, ExternMod);
}
void HWModuleGeneratedOp::print(OpAsmPrinter &p) {
  printModuleOp(p, *this, GenMod);
}

void HWModuleOp::print(OpAsmPrinter &p) {
  printModuleOp(p, *this, PlainMod);

  // Print the body if this is not an external function.
  Region &body = getBody();
  if (!body.empty()) {
    p << " ";
    p.printRegion(body, /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
  }
}

static LogicalResult verifyModuleCommon(Operation *module) {
  assert(isAnyModule(module) &&
         "verifier hook should only be called on modules");

  auto moduleType = getModuleType(module);
  auto argNames = module->getAttrOfType<ArrayAttr>("argNames");
  auto resultNames = module->getAttrOfType<ArrayAttr>("resultNames");
  if (argNames.size() != moduleType.getNumInputs())
    return module->emitOpError("incorrect number of argument names");
  if (resultNames.size() != moduleType.getNumResults())
    return module->emitOpError("incorrect number of result names");

  SmallPtrSet<Attribute, 4> paramNames;

  // Check parameter default values are sensible.
  for (auto param : module->getAttrOfType<ArrayAttr>("parameters")) {
    auto paramAttr = param.cast<ParamDeclAttr>();

    // Check that we don't have any redundant parameter names.  These are
    // resolved by string name: reuse of the same name would cause ambiguities.
    if (!paramNames.insert(paramAttr.getName()).second)
      return module->emitOpError("parameter ")
             << paramAttr << " has the same name as a previous parameter";

    // Default values are allowed to be missing, check them if present.
    auto value = paramAttr.getValue();
    if (!value)
      continue;

    auto typedValue = value.dyn_cast<TypedAttr>();
    if (!typedValue)
      return module->emitOpError("parameter ")
             << paramAttr << " should have a typed value; has value " << value;

    if (typedValue.getType() != paramAttr.getType())
      return module->emitOpError("parameter ")
             << paramAttr << " should have type " << paramAttr.getType()
             << "; has type " << typedValue.getType();

    // Verify that this is a valid parameter value, disallowing parameter
    // references.  We could allow parameters to refer to each other in the
    // future with lexical ordering if there is a need.
    if (failed(checkParameterInContext(value, module, module,
                                       /*disallowParamRefs=*/true)))
      return failure();
  }
  return success();
}

LogicalResult HWModuleOp::verify() { return verifyModuleCommon(*this); }

LogicalResult HWModuleExternOp::verify() { return verifyModuleCommon(*this); }

std::pair<StringAttr, BlockArgument>
HWModuleOp::insertInput(unsigned index, StringAttr name, Type ty) {
  // Find a unique name for the wire.
  Namespace ns;
  for (auto port : getAllPorts())
    ns.newName(port.name.getValue());
  auto nameAttr = StringAttr::get(getContext(), ns.newName(name.getValue()));

  // Create a new port for the host clock.
  PortInfo port;
  port.name = nameAttr;
  port.direction = PortDirection::INPUT;
  port.type = ty;
  insertPorts({std::make_pair(index, port)}, {});

  // Add a new argument.
  return {nameAttr, getBody().getArgument(index)};
}

void HWModuleOp::insertOutputs(unsigned index,
                               ArrayRef<std::pair<StringAttr, Value>> outputs) {

  auto output = cast<OutputOp>(getBodyBlock()->getTerminator());
  assert(index <= output->getNumOperands() && "invalid output index");

  // Rewrite the port list of the module.
  SmallVector<std::pair<unsigned, PortInfo>> indexedNewPorts;
  for (auto &[name, value] : outputs) {
    PortInfo port;
    port.name = name;
    port.direction = PortDirection::OUTPUT;
    port.type = value.getType();
    indexedNewPorts.emplace_back(index, port);
  }
  insertPorts({}, indexedNewPorts);

  // Rewrite the output op.
  for (auto &[name, value] : outputs)
    output->insertOperands(index++, value);
}

void HWModuleOp::appendOutputs(ArrayRef<std::pair<StringAttr, Value>> outputs) {
  return insertOutputs(getResultTypes().size(), outputs);
}

void HWModuleOp::getAsmBlockArgumentNames(mlir::Region &region,
                                          mlir::OpAsmSetValueNameFn setNameFn) {
  getAsmBlockArgumentNamesImpl(region, setNameFn);
}

void HWModuleExternOp::getAsmBlockArgumentNames(
    mlir::Region &region, mlir::OpAsmSetValueNameFn setNameFn) {
  getAsmBlockArgumentNamesImpl(region, setNameFn);
}

/// Lookup the generator for the symbol.  This returns null on
/// invalid IR.
Operation *HWModuleGeneratedOp::getGeneratorKindOp() {
  auto topLevelModuleOp = (*this)->getParentOfType<ModuleOp>();
  return topLevelModuleOp.lookupSymbol(getGeneratorKind());
}

LogicalResult
HWModuleGeneratedOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  auto *referencedKind =
      symbolTable.lookupNearestSymbolFrom(*this, getGeneratorKindAttr());

  if (referencedKind == nullptr)
    return emitError("Cannot find generator definition '")
           << getGeneratorKind() << "'";

  if (!isa<HWGeneratorSchemaOp>(referencedKind))
    return emitError("Symbol resolved to '")
           << referencedKind->getName()
           << "' which is not a HWGeneratorSchemaOp";

  auto referencedKindOp = dyn_cast<HWGeneratorSchemaOp>(referencedKind);
  auto paramRef = referencedKindOp.getRequiredAttrs();
  auto dict = (*this)->getAttrDictionary();
  for (auto str : paramRef) {
    auto strAttr = str.dyn_cast<StringAttr>();
    if (!strAttr)
      return emitError("Unknown attribute type, expected a string");
    if (!dict.get(strAttr.getValue()))
      return emitError("Missing attribute '") << strAttr.getValue() << "'";
  }

  return success();
}

LogicalResult HWModuleGeneratedOp::verify() {
  return verifyModuleCommon(*this);
}

void HWModuleGeneratedOp::getAsmBlockArgumentNames(
    mlir::Region &region, mlir::OpAsmSetValueNameFn setNameFn) {
  getAsmBlockArgumentNamesImpl(region, setNameFn);
}

LogicalResult HWModuleOp::verifyBody() { return success(); }

//===----------------------------------------------------------------------===//
// InstanceOp
//===----------------------------------------------------------------------===//

/// Create a instance that refers to a known module.
void InstanceOp::build(OpBuilder &builder, OperationState &result,
                       Operation *module, StringAttr name,
                       ArrayRef<Value> inputs, ArrayAttr parameters,
                       StringAttr sym_name) {
  if (!parameters)
    parameters = builder.getArrayAttr({});

  auto [argNames, resultNames] =
      instance_like_impl::getHWModuleArgAndResultNames(module);
  FunctionType modType = getModuleType(module);
  build(builder, result, modType.getResults(), name,
        FlatSymbolRefAttr::get(SymbolTable::getSymbolName(module)), inputs,
        argNames, resultNames, parameters, sym_name);
}

/// Lookup the module or extmodule for the symbol.  This returns null on
/// invalid IR.
Operation *InstanceOp::getReferencedModule(const HWSymbolCache *cache) {
  return instance_like_impl::getReferencedModule(cache, *this,
                                                 getModuleNameAttr());
}

Operation *InstanceOp::getReferencedModule() {
  return getReferencedModule(/*cache=*/nullptr);
}

LogicalResult InstanceOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  return instance_like_impl::verifyInstanceOfHWModule(
      *this, getModuleNameAttr(), getInputs(), getResultTypes(), getArgNames(),
      getResultNames(), getParameters(), symbolTable);
}

LogicalResult InstanceOp::verify() {
  auto module = (*this)->getParentOfType<HWModuleOp>();
  if (!module)
    return success();

  auto moduleParameters = module->getAttrOfType<ArrayAttr>("parameters");
  instance_like_impl::EmitErrorFn emitError =
      [&](const std::function<bool(InFlightDiagnostic &)> &fn) {
        auto diag = emitOpError();
        if (fn(diag))
          diag.attachNote(module->getLoc()) << "module declared here";
      };
  return instance_like_impl::verifyParameterStructure(
      getParameters(), moduleParameters, emitError);
}

ParseResult InstanceOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr instanceNameAttr;
  StringAttr symNameAttr;
  FlatSymbolRefAttr moduleNameAttr;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> inputsOperands;
  SmallVector<Type, 1> inputsTypes, allResultTypes;
  ArrayAttr argNames, resultNames, parameters;
  auto noneType = parser.getBuilder().getType<NoneType>();

  if (parser.parseAttribute(instanceNameAttr, noneType, "instanceName",
                            result.attributes))
    return failure();

  if (succeeded(parser.parseOptionalKeyword("sym"))) {
    // Parsing an optional symbol name doesn't fail, so no need to check the
    // result.
    (void)parser.parseOptionalSymbolName(
        symNameAttr, InnerName::getInnerNameAttrName(), result.attributes);
  }

  llvm::SMLoc parametersLoc, inputsOperandsLoc;
  if (parser.parseAttribute(moduleNameAttr, noneType, "moduleName",
                            result.attributes) ||
      parser.getCurrentLocation(&parametersLoc) ||
      parseOptionalParameterList(parser, parameters) ||
      parseInputPortList(parser, inputsOperands, inputsTypes, argNames) ||
      parser.resolveOperands(inputsOperands, inputsTypes, inputsOperandsLoc,
                             result.operands) ||
      parser.parseArrow() ||
      parseOutputPortList(parser, allResultTypes, resultNames) ||
      parser.parseOptionalAttrDict(result.attributes)) {
    return failure();
  }

  result.addAttribute("argNames", argNames);
  result.addAttribute("resultNames", resultNames);
  result.addAttribute("parameters", parameters);
  result.addTypes(allResultTypes);
  return success();
}

void InstanceOp::print(OpAsmPrinter &p) {
  p << ' ';
  p.printAttributeWithoutType(getInstanceNameAttr());
  if (auto attr = getInnerSymAttr()) {
    p << " sym ";
    p.printSymbolName(attr.getValue());
  }
  p << ' ';
  p.printAttributeWithoutType(getModuleNameAttr());
  printOptionalParameterList(p, *this, getParameters());
  printInputPortList(p, *this, getInputs(), getInputs().getTypes(),
                     getArgNames());
  p << " -> ";
  printOutputPortList(p, *this, getResultTypes(), getResultNames());

  p.printOptionalAttrDict(
      (*this)->getAttrs(),
      /*elidedAttrs=*/{"instanceName", InnerName::getInnerNameAttrName(),
                       "moduleName", "argNames", "resultNames", "parameters"});
}

/// Return the name of the specified input port or null if it cannot be
/// determined.
StringAttr InstanceOp::getArgumentName(size_t idx) {
  return instance_like_impl::getName(getArgNames(), idx);
}

/// Return the name of the specified result or null if it cannot be
/// determined.
StringAttr InstanceOp::getResultName(size_t idx) {
  return instance_like_impl::getName(getResultNames(), idx);
}

/// Change the name of the specified input port.
void InstanceOp::setArgumentName(size_t i, StringAttr name) {
  setArgumentNames(instance_like_impl::updateName(getArgNames(), i, name));
}

/// Change the name of the specified output port.
void InstanceOp::setResultName(size_t i, StringAttr name) {
  setResultNames(instance_like_impl::updateName(getResultNames(), i, name));
}

/// Suggest a name for each result value based on the saved result names
/// attribute.
void InstanceOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
  instance_like_impl::getAsmResultNames(setNameFn, instanceName(),
                                        getResultNames(), getResults());
}

//===----------------------------------------------------------------------===//
// HWOutputOp
//===----------------------------------------------------------------------===//

/// Verify that the num of operands and types fit the declared results.
LogicalResult OutputOp::verify() {
  // Check that the we (hw.output) have the same number of operands as our
  // region has results.
  auto *opParent = (*this)->getParentOp();
  FunctionType modType = getModuleType(opParent);
  ArrayRef<Type> modResults = modType.getResults();
  OperandRange outputValues = getOperands();
  if (modResults.size() != outputValues.size()) {
    emitOpError("must have same number of operands as region results.");
    return failure();
  }

  // Check that the types of our operands and the region's results match.
  for (size_t i = 0, e = modResults.size(); i < e; ++i) {
    if (modResults[i] != outputValues[i].getType()) {
      emitOpError("output types must match module. In "
                  "operand ")
          << i << ", expected " << modResults[i] << ", but got "
          << outputValues[i].getType() << ".";
      return failure();
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

LogicalResult
GlobalRefOp::verifySymbolUses(mlir::SymbolTableCollection &symTables) {
  Operation *parent = (*this)->getParentOp();
  SymbolTable &symTable = symTables.getSymbolTable(parent);
  StringAttr symNameAttr = (*this).getSymNameAttr();
  auto hasGlobalRef = [&](Attribute attr) -> bool {
    if (!attr)
      return false;
    for (auto ref : attr.cast<ArrayAttr>().getAsRange<GlobalRefAttr>())
      if (ref.getGlblSym().getAttr() == symNameAttr)
        return true;
    return false;
  };
  // For all inner refs in the namepath, ensure they have a corresponding
  // GlobalRefAttr to this GlobalRefOp.
  for (auto innerRef : getNamepath().getAsRange<hw::InnerRefAttr>()) {
    StringAttr modName = innerRef.getModule();
    StringAttr innerSym = innerRef.getName();
    Operation *mod = symTable.lookup(modName);
    if (!mod) {
      (*this)->emitOpError("module:'" + modName.str() + "' not found");
      return failure();
    }
    bool glblSymNotFound = true;
    bool innerSymOpNotFound = true;
    mod->walk([&](Operation *op) -> WalkResult {
      StringAttr attr = op->getAttrOfType<StringAttr>("inner_sym");
      // If this is one of the ops in the instance path for the GlobalRefOp.
      if (attr && attr == innerSym) {
        innerSymOpNotFound = false;
        // Each op can have an array of GlobalRefAttr, check if this op is one
        // of them.
        if (hasGlobalRef(op->getAttr(GlobalRefAttr::DialectAttrName))) {
          glblSymNotFound = false;
          return WalkResult::interrupt();
        }
        // If cannot find the ref, then its an error.
        return failure();
      }
      return WalkResult::advance();
    });
    if (glblSymNotFound) {
      // TODO: Doesn't yet work for symbls on FIRRTL module ports. Need to
      // implement an interface.
      if (isa<HWModuleOp, HWModuleExternOp>(mod)) {
        if (auto argAttrs = mod->getAttr(
                mlir::function_interface_impl::getArgDictAttrName()))
          for (auto attr :
               argAttrs.cast<ArrayAttr>().getAsRange<DictionaryAttr>())
            if (auto symRef = attr.get("hw.exportPort"))
              if (symRef.cast<FlatSymbolRefAttr>().getValue() == innerSym)
                if (hasGlobalRef(attr.get(GlobalRefAttr::DialectAttrName)))
                  return success();

        if (auto resAttrs = mod->getAttr(
                mlir::function_interface_impl::getResultDictAttrName()))
          for (auto attr :
               resAttrs.cast<ArrayAttr>().getAsRange<DictionaryAttr>())
            if (auto symRef = attr.get("hw.exportPort"))
              if (symRef.cast<FlatSymbolRefAttr>().getValue() == innerSym)
                if (hasGlobalRef(attr.get(GlobalRefAttr::DialectAttrName)))
                  return success();
      }
    }
    if (innerSymOpNotFound)
      return (*this)->emitOpError("operation:'" + innerSym.str() +
                                  "' in module:'" + modName.str() +
                                  "' could not be found");
    if (glblSymNotFound)
      return (*this)->emitOpError(
          "operation:'" + innerSym.str() + "' in module:'" + modName.str() +
          "' does not contain a reference to '" + symNameAttr.str() + "'");
  }
  return success();
}

static ParseResult parseSliceTypes(OpAsmParser &p, Type &srcType,
                                   Type &idxType) {
  Type type;
  if (p.parseType(type))
    return p.emitError(p.getCurrentLocation(), "Expected type");
  auto arrType = type_dyn_cast<ArrayType>(type);
  if (!arrType)
    return p.emitError(p.getCurrentLocation(), "Expected !hw.array type");
  srcType = type;
  unsigned idxWidth = llvm::Log2_64_Ceil(arrType.getSize());
  idxType = IntegerType::get(p.getBuilder().getContext(), idxWidth);
  return success();
}

static void printSliceTypes(OpAsmPrinter &p, Operation *, Type srcType,
                            Type idxType) {
  p.printType(srcType);
}

ParseResult ArrayCreateOp::parse(OpAsmParser &parser, OperationState &result) {
  llvm::SMLoc inputOperandsLoc = parser.getCurrentLocation();
  llvm::SmallVector<OpAsmParser::UnresolvedOperand, 16> operands;
  Type elemType;

  if (parser.parseOperandList(operands) ||
      parser.parseOptionalAttrDict(result.attributes) || parser.parseColon() ||
      parser.parseType(elemType))
    return failure();

  if (operands.size() == 0)
    return parser.emitError(inputOperandsLoc,
                            "Cannot construct an array of length 0");
  result.addTypes({ArrayType::get(elemType, operands.size())});

  for (auto operand : operands)
    if (parser.resolveOperand(operand, elemType, result.operands))
      return failure();
  return success();
}

void ArrayCreateOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printOperands(getInputs());
  p.printOptionalAttrDict((*this)->getAttrs());
  p << " : " << getInputs()[0].getType();
}

void ArrayCreateOp::build(OpBuilder &b, OperationState &state,
                          ValueRange values) {
  assert(values.size() > 0 && "Cannot build array of zero elements");
  Type elemType = values[0].getType();
  assert(llvm::all_of(
             values,
             [elemType](Value v) -> bool { return v.getType() == elemType; }) &&
         "All values must have same type.");
  build(b, state, ArrayType::get(elemType, values.size()), values);
}

LogicalResult ArrayCreateOp::verify() {
  unsigned returnSize = getType().cast<ArrayType>().getSize();
  if (getInputs().size() != returnSize)
    return failure();
  return success();
}

// Check whether an integer value is an offset from a base.
bool hw::isOffset(Value base, Value index, uint64_t offset) {
  if (auto constBase = base.getDefiningOp<hw::ConstantOp>()) {
    if (auto constIndex = index.getDefiningOp<hw::ConstantOp>()) {
      // If both values are a constant, check if index == base + offset.
      // To account for overflow, the addition is performed with an extra bit
      // and the offset is asserted to fit in the bit width of the base.
      auto baseValue = constBase.getValue();
      auto indexValue = constIndex.getValue();

      unsigned bits = baseValue.getBitWidth();
      assert(bits == indexValue.getBitWidth() && "mismatched widths");

      if (bits < 64 && offset >= (1ull << bits))
        return false;

      APInt baseExt = baseValue.zextOrTrunc(bits + 1);
      APInt indexExt = indexValue.zextOrTrunc(bits + 1);
      return baseExt + offset == indexExt;
    }
  }
  return false;
}

// Canonicalize a create of consecutive elements to a slice.
static LogicalResult foldCreateToSlice(ArrayCreateOp op,
                                       PatternRewriter &rewriter) {
  // Do not canonicalize create of get into a slice.
  auto arrayTy = hw::type_cast<ArrayType>(op.getType());
  if (arrayTy.getSize() <= 1)
    return failure();
  auto elemTy = arrayTy.getElementType();

  // Check if create arguments are consecutive elements of the same array.
  // Attempt to break a create of gets into a sequence of consecutive intervals.
  struct Chunk {
    Value input;
    Value index;
    size_t size;
  };
  SmallVector<Chunk> chunks;
  for (Value value : llvm::reverse(op.getInputs())) {
    auto get = value.getDefiningOp<ArrayGetOp>();
    if (!get)
      return failure();

    Value input = get.getInput();
    Value index = get.getIndex();
    if (!chunks.empty()) {
      auto &c = *chunks.rbegin();
      if (c.input == get.getInput() && isOffset(c.index, index, c.size)) {
        c.size++;
        continue;
      }
    }

    chunks.push_back(Chunk{input, index, 1});
  }

  // If there is a single slice, eliminate the create.
  if (chunks.size() == 1) {
    auto &chunk = chunks[0];
    rewriter.replaceOp(op, rewriter.createOrFold<ArraySliceOp>(
                               op.getLoc(), arrayTy, chunk.input, chunk.index));
    return success();
  }

  // If the number of chunks is significantly less than the number of
  // elements, replace the create with a concat of the identified slices.
  if (chunks.size() * 2 < arrayTy.getSize()) {
    SmallVector<Value> slices;
    for (auto &chunk : llvm::reverse(chunks)) {
      auto sliceTy = ArrayType::get(elemTy, chunk.size);
      slices.push_back(rewriter.createOrFold<ArraySliceOp>(
          op.getLoc(), sliceTy, chunk.input, chunk.index));
    }
    rewriter.replaceOpWithNewOp<ArrayConcatOp>(op, arrayTy, slices);
    return success();
  }

  return failure();
}

LogicalResult ArrayCreateOp::canonicalize(ArrayCreateOp op,
                                          PatternRewriter &rewriter) {
  if (succeeded(foldCreateToSlice(op, rewriter)))
    return success();
  return failure();
}

Value ArrayCreateOp::getUniformElement() {
  if (!getInputs().empty() && llvm::all_equal(getInputs()))
    return getInputs()[0];
  return {};
}

static Optional<uint64_t> getUIntFromValue(Value value) {
  auto idxOp = dyn_cast_or_null<ConstantOp>(value.getDefiningOp());
  if (!idxOp)
    return llvm::None;
  APInt idxAttr = idxOp.getValue();
  if (idxAttr.getBitWidth() > 64)
    return llvm::None;
  return idxAttr.getLimitedValue();
}

LogicalResult ArraySliceOp::verify() {
  unsigned inputSize = type_cast<ArrayType>(getInput().getType()).getSize();
  if (llvm::Log2_64_Ceil(inputSize) !=
      getLowIndex().getType().getIntOrFloatBitWidth())
    return emitOpError(
        "ArraySlice: index width must match clog2 of array size");
  return success();
}

OpFoldResult ArraySliceOp::fold(ArrayRef<Attribute> constants) {
  // If we are slicing the entire input, then return it.
  if (getType() == getInput().getType())
    return getInput();
  return {};
}

LogicalResult ArraySliceOp::canonicalize(ArraySliceOp op,
                                         PatternRewriter &rewriter) {
  auto sliceTy = hw::type_cast<ArrayType>(op.getType());
  auto elemTy = sliceTy.getElementType();
  uint64_t sliceSize = sliceTy.getSize();
  assert(sliceSize != 0 && "empty slice");

  if (sliceSize == 1) {
    // slice(a, n) -> create(a[n])
    auto get = rewriter.create<ArrayGetOp>(op.getLoc(), op.getInput(),
                                           op.getLowIndex());
    rewriter.replaceOpWithNewOp<ArrayCreateOp>(op, op.getType(),
                                               get.getResult());
    return success();
  }

  auto offsetOpt = getUIntFromValue(op.getLowIndex());
  if (!offsetOpt)
    return failure();

  auto inputOp = op.getInput().getDefiningOp();
  if (auto inputSlice = dyn_cast_or_null<ArraySliceOp>(inputOp)) {
    // slice(slice(a, n), m) -> slice(a, n + m)
    if (inputSlice == op)
      return failure();

    auto inputIndex = inputSlice.getLowIndex();
    auto inputOffsetOpt = getUIntFromValue(inputIndex);
    if (!inputOffsetOpt)
      return failure();

    uint64_t offset = *offsetOpt + *inputOffsetOpt;
    auto lowIndex =
        rewriter.create<ConstantOp>(op.getLoc(), inputIndex.getType(), offset);
    rewriter.replaceOpWithNewOp<ArraySliceOp>(op, op.getType(),
                                              inputSlice.getInput(), lowIndex);
    return success();
  }

  if (auto inputCreate = dyn_cast_or_null<ArrayCreateOp>(inputOp)) {
    // slice(create(a0, a1, ..., an), m) -> create(am, ...)
    auto inputs = inputCreate.getInputs();

    uint64_t begin = inputs.size() - *offsetOpt - sliceSize;
    rewriter.replaceOpWithNewOp<ArrayCreateOp>(op, op.getType(),
                                               inputs.slice(begin, sliceSize));
    return success();
  }

  if (auto inputConcat = dyn_cast_or_null<ArrayConcatOp>(inputOp)) {
    // slice(concat(a1, a2, ...)) -> concat(a2, slice(a3, ..), ...)
    SmallVector<Value> chunks;
    uint64_t sliceStart = *offsetOpt;
    for (auto input : llvm::reverse(inputConcat.getInputs())) {
      // Check whether the input intersects with the slice.
      uint64_t inputSize = hw::type_cast<ArrayType>(input.getType()).getSize();
      if (inputSize == 0 || inputSize <= sliceStart) {
        sliceStart -= inputSize;
        continue;
      }

      // Find the indices to slice from this input by intersection.
      uint64_t cutEnd = std::min(inputSize, sliceStart + sliceSize);
      uint64_t cutSize = cutEnd - sliceStart;
      assert(cutSize != 0 && "slice cannot be empty");

      if (cutSize == inputSize) {
        // The whole input fits in the slice, add it.
        assert(sliceStart == 0 && "invalid cut size");
        chunks.push_back(input);
      } else {
        // Slice the required bits from the input.
        unsigned width = inputSize == 1 ? 1 : llvm::Log2_64_Ceil(inputSize);
        auto lowIndex = rewriter.create<ConstantOp>(
            op.getLoc(), rewriter.getIntegerType(width), sliceStart);
        chunks.push_back(rewriter.create<ArraySliceOp>(
            op.getLoc(), hw::ArrayType::get(elemTy, cutSize), input, lowIndex));
      }

      sliceStart = 0;
      sliceSize -= cutSize;
      if (sliceSize == 0)
        break;
    }

    assert(chunks.size() > 0 && "missing sliced items");
    if (chunks.size() == 1)
      rewriter.replaceOp(op, chunks[0]);
    else
      rewriter.replaceOpWithNewOp<ArrayConcatOp>(
          op, llvm::to_vector(llvm::reverse(chunks)));
    return success();
  }
  return failure();
}

//===----------------------------------------------------------------------===//
// ArrayConcatOp
//===----------------------------------------------------------------------===//

static ParseResult parseArrayConcatTypes(OpAsmParser &p,
                                         SmallVectorImpl<Type> &inputTypes,
                                         Type &resultType) {
  Type elemType;
  uint64_t resultSize = 0;

  auto parseElement = [&]() -> ParseResult {
    Type ty;
    if (p.parseType(ty))
      return failure();
    auto arrTy = type_dyn_cast<ArrayType>(ty);
    if (!arrTy)
      return p.emitError(p.getCurrentLocation(), "Expected !hw.array type");
    if (elemType && elemType != arrTy.getElementType())
      return p.emitError(p.getCurrentLocation(), "Expected array element type ")
             << elemType;

    elemType = arrTy.getElementType();
    inputTypes.push_back(ty);
    resultSize += arrTy.getSize();
    return success();
  };

  if (p.parseCommaSeparatedList(parseElement))
    return failure();

  resultType = ArrayType::get(elemType, resultSize);
  return success();
}

static void printArrayConcatTypes(OpAsmPrinter &p, Operation *,
                                  TypeRange inputTypes, Type resultType) {
  llvm::interleaveComma(inputTypes, p, [&p](Type t) { p << t; });
}

void ArrayConcatOp::build(OpBuilder &b, OperationState &state,
                          ValueRange values) {
  assert(!values.empty() && "Cannot build array of zero elements");
  ArrayType arrayTy = values[0].getType().cast<ArrayType>();
  Type elemTy = arrayTy.getElementType();
  assert(llvm::all_of(values,
                      [elemTy](Value v) -> bool {
                        return v.getType().isa<ArrayType>() &&
                               v.getType().cast<ArrayType>().getElementType() ==
                                   elemTy;
                      }) &&
         "All values must be of ArrayType with the same element type.");

  uint64_t resultSize = 0;
  for (Value val : values)
    resultSize += val.getType().cast<ArrayType>().getSize();
  build(b, state, ArrayType::get(elemTy, resultSize), values);
}

// Flatten a concatenation of array creates into a single create.
static bool flattenConcatOp(ArrayConcatOp op, PatternRewriter &rewriter) {
  for (auto input : op.getInputs())
    if (!input.getDefiningOp<ArrayCreateOp>())
      return false;

  SmallVector<Value> items;
  for (auto input : op.getInputs()) {
    auto create = cast<ArrayCreateOp>(input.getDefiningOp());
    for (auto item : create.getInputs())
      items.push_back(item);
  }

  rewriter.replaceOpWithNewOp<ArrayCreateOp>(op, items);
  return true;
}

// Merge consecutive slice expressions in a concatenation.
static bool mergeConcatSlices(ArrayConcatOp op, PatternRewriter &rewriter) {
  struct Slice {
    Value input;
    Value index;
    size_t size;
    Value op;
    SmallVector<Location> locs;
  };

  SmallVector<Value> items;
  Optional<Slice> last;
  bool changed = false;

  auto concatenate = [&] {
    // If there is only one op in the slice, place it to the items list.
    if (!last)
      return;
    if (last->op) {
      items.push_back(last->op);
      last.reset();
      return;
    }

    // Otherwise, create a new slice of with the given size and place it.
    // In this case, the concat op is replaced, using the new argument.
    changed = true;
    auto loc = FusedLoc::get(op.getContext(), last->locs);
    auto origTy = hw::type_cast<ArrayType>(last->input.getType());
    auto arrayTy = ArrayType::get(origTy.getElementType(), last->size);
    items.push_back(rewriter.createOrFold<ArraySliceOp>(
        loc, arrayTy, last->input, last->index));

    last.reset();
  };

  auto append = [&](Value op, Value input, Value index, size_t size) {
    // If this slice is an extension of the previous one, extend the size
    // saved.  In this case, a new slice of is created and the concatenation
    // operator is rewritten.  Otherwise, flush the last slice.
    if (last) {
      if (last->input == input && isOffset(last->index, index, last->size)) {
        last->size += size;
        last->op = {};
        last->locs.push_back(op.getLoc());
        return;
      }
      concatenate();
    }
    last.emplace(Slice{input, index, size, op, {op.getLoc()}});
  };

  for (auto item : llvm::reverse(op.getInputs())) {
    if (auto slice = item.getDefiningOp<ArraySliceOp>()) {
      auto size = hw::type_cast<ArrayType>(slice.getType()).getSize();
      append(item, slice.getInput(), slice.getLowIndex(), size);
      continue;
    }

    if (auto create = item.getDefiningOp<ArrayCreateOp>()) {
      if (create.getInputs().size() == 1) {
        if (auto get = create.getInputs()[0].getDefiningOp<ArrayGetOp>()) {
          append(item, get.getInput(), get.getIndex(), 1);
          continue;
        }
      }
    }

    concatenate();
    items.push_back(item);
  }
  concatenate();

  if (!changed)
    return false;

  if (items.size() == 1) {
    rewriter.replaceOp(op, items[0]);
  } else {
    std::reverse(items.begin(), items.end());
    rewriter.replaceOpWithNewOp<ArrayConcatOp>(op, items);
  }
  return true;
}

LogicalResult ArrayConcatOp::canonicalize(ArrayConcatOp op,
                                          PatternRewriter &rewriter) {
  // concat(create(a1, ...), create(a3, ...), ...) -> create(a1, ..., a3, ...)
  if (flattenConcatOp(op, rewriter))
    return success();

  // concat(slice(a, n, m), slice(a, n + m, p)) -> concat(slice(a, n, m + p))
  if (mergeConcatSlices(op, rewriter))
    return success();

  return success();
}

//===----------------------------------------------------------------------===//
// EnumConstantOp
//===----------------------------------------------------------------------===//

ParseResult EnumConstantOp::parse(OpAsmParser &parser, OperationState &result) {
  // Parse a Type instead of an EnumType since the type might be a type alias.
  // The validity of the canonical type is checked during construction of the
  // EnumFieldAttr.
  Type type;
  StringRef field;

  auto loc = parser.getEncodedSourceLoc(parser.getCurrentLocation());
  if (parser.parseKeyword(&field) || parser.parseColonType(type))
    return failure();

  auto fieldAttr = EnumFieldAttr::get(
      loc, StringAttr::get(parser.getContext(), field), type);

  if (!fieldAttr)
    return failure();

  result.addAttribute("field", fieldAttr);
  result.addTypes(type);

  return success();
}

void EnumConstantOp::print(OpAsmPrinter &p) {
  p << " " << getField().getField().getValue() << " : "
    << getField().getType().getValue();
}

void EnumConstantOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), getField().getField().str());
}

OpFoldResult EnumConstantOp::fold(ArrayRef<Attribute> constants) {
  assert(constants.empty() && "constant has no operands");
  return getFieldAttr();
}

//===----------------------------------------------------------------------===//
// StructCreateOp
//===----------------------------------------------------------------------===//

ParseResult StructCreateOp::parse(OpAsmParser &parser, OperationState &result) {
  llvm::SMLoc inputOperandsLoc = parser.getCurrentLocation();
  llvm::SmallVector<OpAsmParser::UnresolvedOperand, 4> operands;
  Type declOrAliasType;

  if (parser.parseLParen() || parser.parseOperandList(operands) ||
      parser.parseRParen() || parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declOrAliasType))
    return failure();

  auto declType = type_dyn_cast<StructType>(declOrAliasType);
  if (!declType)
    return parser.emitError(parser.getNameLoc(),
                            "expected !hw.struct type or alias");

  llvm::SmallVector<Type, 4> structInnerTypes;
  declType.getInnerTypes(structInnerTypes);
  result.addTypes(declOrAliasType);

  if (parser.resolveOperands(operands, structInnerTypes, inputOperandsLoc,
                             result.operands))
    return failure();
  return success();
}

void StructCreateOp::print(OpAsmPrinter &printer) {
  printer << " (";
  printer.printOperands(getInput());
  printer << ")";
  printer.printOptionalAttrDict((*this)->getAttrs());
  printer << " : " << getType();
}

LogicalResult StructCreateOp::verify() {
  auto elements = hw::type_cast<StructType>(getType()).getElements();

  if (elements.size() != getInput().size())
    return emitOpError("structure field count mismatch");

  for (const auto &[field, value] : llvm::zip(elements, getInput()))
    if (field.type != value.getType())
      return emitOpError("structure field `")
             << field.name << "` type does not match";

  return success();
}

//===----------------------------------------------------------------------===//
// StructExplodeOp
//===----------------------------------------------------------------------===//

ParseResult StructExplodeOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  OpAsmParser::UnresolvedOperand operand;
  Type declType;

  if (parser.parseOperand(operand) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();
  auto structType = type_dyn_cast<StructType>(declType);
  if (!structType)
    return parser.emitError(parser.getNameLoc(),
                            "invalid kind of type specified");

  llvm::SmallVector<Type, 4> structInnerTypes;
  structType.getInnerTypes(structInnerTypes);
  result.addTypes(structInnerTypes);

  if (parser.resolveOperand(operand, declType, result.operands))
    return failure();
  return success();
}

void StructExplodeOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printOperand(getInput());
  printer.printOptionalAttrDict((*this)->getAttrs());
  printer << " : " << getInput().getType();
}

LogicalResult StructExplodeOp::canonicalize(StructExplodeOp op,
                                            PatternRewriter &rewriter) {
  auto *inputOp = op.getInput().getDefiningOp();
  auto elements = type_cast<StructType>(op.getInput().getType()).getElements();
  for (auto [element, res] : llvm::zip(elements, op.getResults())) {
    if (auto foldResult = foldStructExtract(inputOp, element.name.str()))
      res.replaceAllUsesWith(foldResult);
  }
  return failure();
}

void StructExplodeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  auto structType = type_cast<StructType>(getInput().getType());
  for (auto [res, field] : llvm::zip(getResults(), structType.getElements()))
    setNameFn(res, field.name.str());
}

//===----------------------------------------------------------------------===//
// StructExtractOp
//===----------------------------------------------------------------------===//

/// Use the same parser for both struct_extract and union_extract since the
/// syntax is identical.
template <typename AggregateType>
static ParseResult parseExtractOp(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::UnresolvedOperand operand;
  StringAttr fieldName;
  Type declType;

  if (parser.parseOperand(operand) || parser.parseLSquare() ||
      parser.parseAttribute(fieldName, "field", result.attributes) ||
      parser.parseRSquare() ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();
  auto aggType = type_dyn_cast<AggregateType>(declType);
  if (!aggType)
    return parser.emitError(parser.getNameLoc(),
                            "invalid kind of type specified");

  Type resultType = aggType.getFieldType(fieldName.getValue());
  if (!resultType) {
    parser.emitError(parser.getNameLoc(), "invalid field name specified");
    return failure();
  }
  result.addTypes(resultType);

  if (parser.resolveOperand(operand, declType, result.operands))
    return failure();
  return success();
}

/// Use the same printer for both struct_extract and union_extract since the
/// syntax is identical.
template <typename AggType>
static void printExtractOp(OpAsmPrinter &printer, AggType op) {
  printer << " ";
  printer.printOperand(op.getInput());
  printer << "[\"" << op.getField() << "\"]";
  printer.printOptionalAttrDict(op->getAttrs(), {"field"});
  printer << " : " << op.getInput().getType();
}

ParseResult StructExtractOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  return parseExtractOp<StructType>(parser, result);
}

void StructExtractOp::print(OpAsmPrinter &printer) {
  printExtractOp(printer, *this);
}

void StructExtractOp::build(OpBuilder &builder, OperationState &odsState,
                            Value input, StructType::FieldInfo field) {
  build(builder, odsState, field.type, input, field.name);
}

void StructExtractOp::build(OpBuilder &builder, OperationState &odsState,
                            Value input, StringAttr fieldAttr) {
  auto structType = type_cast<StructType>(input.getType());
  auto resultType = structType.getFieldType(fieldAttr);
  build(builder, odsState, resultType, input, fieldAttr);
}

OpFoldResult StructExtractOp::fold(ArrayRef<Attribute> operands) {
  if (auto foldResult =
          foldStructExtract(getInput().getDefiningOp(), getField()))
    return foldResult;
  return {};
}

LogicalResult StructExtractOp::canonicalize(StructExtractOp op,
                                            PatternRewriter &rewriter) {
  auto inputOp = op.getInput().getDefiningOp();

  // b = extract(inject(x["a"], v0)["b"]) => extract(x, "b")
  if (auto structInject = dyn_cast_or_null<StructInjectOp>(inputOp)) {
    if (structInject.getField() != op.getField()) {
      rewriter.replaceOpWithNewOp<StructExtractOp>(
          op, op.getType(), structInject.getInput(), op.getField());
      return success();
    }
  }

  return failure();
}

void StructExtractOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  auto structType = type_cast<StructType>(getInput().getType());
  for (auto field : structType.getElements()) {
    if (field.name == getField()) {
      setNameFn(getResult(), field.name.str());
      return;
    }
  }
}

//===----------------------------------------------------------------------===//
// StructInjectOp
//===----------------------------------------------------------------------===//

ParseResult StructInjectOp::parse(OpAsmParser &parser, OperationState &result) {
  llvm::SMLoc inputOperandsLoc = parser.getCurrentLocation();
  OpAsmParser::UnresolvedOperand operand, val;
  StringAttr fieldName;
  Type declType;

  if (parser.parseOperand(operand) || parser.parseLSquare() ||
      parser.parseAttribute(fieldName, "field", result.attributes) ||
      parser.parseRSquare() || parser.parseComma() ||
      parser.parseOperand(val) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();
  auto structType = type_dyn_cast<StructType>(declType);
  if (!structType)
    return parser.emitError(inputOperandsLoc, "invalid kind of type specified");

  Type resultType = structType.getFieldType(fieldName.getValue());
  if (!resultType) {
    parser.emitError(inputOperandsLoc, "invalid field name specified");
    return failure();
  }
  result.addTypes(declType);

  if (parser.resolveOperands({operand, val}, {declType, resultType},
                             inputOperandsLoc, result.operands))
    return failure();
  return success();
}

void StructInjectOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printOperand(getInput());
  printer << "[\"" << getField() << "\"], ";
  printer.printOperand(getNewValue());
  printer.printOptionalAttrDict((*this)->getAttrs(), {"field"});
  printer << " : " << getInput().getType();
}

LogicalResult StructInjectOp::canonicalize(StructInjectOp op,
                                           PatternRewriter &rewriter) {
  // Canonicalize multiple injects into a create op and eliminate overwrites.
  SmallPtrSet<Operation *, 4> injects;
  DenseMap<StringAttr, Value> fields;

  // Chase a chain of injects. Bail out if cycles are present.
  StructInjectOp inject = op;
  Value input;
  do {
    if (!injects.insert(inject).second)
      return failure();

    fields.try_emplace(inject.getFieldAttr(), inject.getNewValue());
    input = inject.getInput();
    inject = dyn_cast_or_null<StructInjectOp>(input.getDefiningOp());
  } while (inject);
  assert(input && "missing input to inject chain");

  auto ty = hw::type_cast<StructType>(op.getType());
  auto elements = ty.getElements();

  // If the inject chain sets all fields, canonicalize to create.
  if (fields.size() == elements.size()) {
    SmallVector<Value> createFields;
    for (const auto &field : elements) {
      auto it = fields.find(field.name);
      assert(it != fields.end() && "missing field");
      createFields.push_back(it->second);
    }
    rewriter.replaceOpWithNewOp<StructCreateOp>(op, ty, createFields);
    return success();
  }

  // Nothing to canonicalize, only the original inject in the chain.
  if (injects.size() == fields.size())
    return failure();

  // Eliminate overwrites. The hash map contains the last write to each field.
  for (const auto &field : elements) {
    auto it = fields.find(field.name);
    if (it == fields.end())
      continue;
    input = rewriter.create<StructInjectOp>(op.getLoc(), ty, input, field.name,
                                            it->second);
  }

  rewriter.replaceOp(op, input);
  return success();
}

//===----------------------------------------------------------------------===//
// UnionCreateOp
//===----------------------------------------------------------------------===//

ParseResult UnionCreateOp::parse(OpAsmParser &parser, OperationState &result) {
  Type declOrAliasType;
  StringAttr field;
  OpAsmParser::UnresolvedOperand input;
  llvm::SMLoc fieldLoc = parser.getCurrentLocation();

  if (parser.parseAttribute(field, "field", result.attributes) ||
      parser.parseComma() || parser.parseOperand(input) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declOrAliasType))
    return failure();

  auto declType = type_dyn_cast<UnionType>(declOrAliasType);
  if (!declType)
    return parser.emitError(parser.getNameLoc(),
                            "expected !hw.union type or alias");

  Type inputType = declType.getFieldType(field.getValue());
  if (!inputType) {
    parser.emitError(fieldLoc, "cannot find union field '")
        << field.getValue() << '\'';
    return failure();
  }

  if (parser.resolveOperand(input, inputType, result.operands))
    return failure();
  result.addTypes({declOrAliasType});
  return success();
}

void UnionCreateOp::print(OpAsmPrinter &printer) {
  printer << " \"" << getField() << "\", ";
  printer.printOperand(getInput());
  printer.printOptionalAttrDict((*this)->getAttrs(), {"field"});
  printer << " : " << getType();
}

//===----------------------------------------------------------------------===//
// UnionExtractOp
//===----------------------------------------------------------------------===//

ParseResult UnionExtractOp::parse(OpAsmParser &parser, OperationState &result) {
  return parseExtractOp<UnionType>(parser, result);
}

void UnionExtractOp::print(OpAsmPrinter &printer) {
  printExtractOp(printer, *this);
}

//===----------------------------------------------------------------------===//
// ArrayGetOp
//===----------------------------------------------------------------------===//

void ArrayGetOp::build(OpBuilder &builder, OperationState &result, Value input,
                       Value index) {
  auto resultType = type_cast<ArrayType>(input.getType()).getElementType();
  build(builder, result, resultType, input, index);
}

// An array_get of an array_create with a constant index can just be the
// array_create operand at the constant index. If the array_create has a
// single uniform value for each element, just return that value regardless of
// the index. If the array is constructed from a constant by a bitcast
// operation, we can fold into a constant.
OpFoldResult ArrayGetOp::fold(ArrayRef<Attribute> operands) {
  // array_get(bitcast(c), i) -> c[i*w+w-1:i*w]
  if (auto bitcast = getInput().getDefiningOp<hw::BitcastOp>()) {
    auto intTy = getType().dyn_cast<IntegerType>();
    if (!intTy)
      return {};
    auto inputConsatnt = bitcast.getInput().getDefiningOp<hw::ConstantOp>();
    if (!inputConsatnt)
      return {};
    IntegerAttr constIdx = operands[1].dyn_cast_or_null<IntegerAttr>();
    if (!constIdx)
      return {};
    auto constant = inputConsatnt.getValue();
    // Calculate the index. Make sure to zero-extend the index value before
    // multiplying the element width.
    auto startIdx = constIdx.getValue().zext(constant.getBitWidth()) *
                    getType().getIntOrFloatBitWidth();
    // Extract [startIdx + width - 1: startIdx].
    return IntegerAttr::get(
        intTy, constant.lshr(startIdx).trunc(intTy.getIntOrFloatBitWidth()));
  }

  auto inputCreate =
      dyn_cast_or_null<ArrayCreateOp>(getInput().getDefiningOp());
  if (!inputCreate)
    return {};

  if (auto uniformValue = inputCreate.getUniformElement())
    return uniformValue;

  IntegerAttr constIdx = operands[1].dyn_cast_or_null<IntegerAttr>();
  if (!constIdx || constIdx.getValue().getBitWidth() > 64)
    return {};

  uint64_t idx = constIdx.getValue().getLimitedValue();
  auto createInputs = inputCreate.getInputs();
  if (idx >= createInputs.size())
    return {};
  return createInputs[createInputs.size() - idx - 1];
}

LogicalResult ArrayGetOp::canonicalize(ArrayGetOp op,
                                       PatternRewriter &rewriter) {
  auto idxOpt = getUIntFromValue(op.getIndex());
  if (!idxOpt)
    return failure();

  auto *inputOp = op.getInput().getDefiningOp();
  if (auto inputSlice = dyn_cast_or_null<ArraySliceOp>(inputOp)) {
    // get(slice(a, n), m) -> get(a, n + m)
    auto offsetOp = inputSlice.getLowIndex();
    auto offsetOpt = getUIntFromValue(offsetOp);
    if (!offsetOpt)
      return failure();

    uint64_t offset = *offsetOpt + *idxOpt;
    auto newOffset =
        rewriter.create<ConstantOp>(op.getLoc(), offsetOp.getType(), offset);
    rewriter.replaceOpWithNewOp<ArrayGetOp>(op, inputSlice.getInput(),
                                            newOffset);
    return success();
  }

  if (auto inputConcat = dyn_cast_or_null<ArrayConcatOp>(inputOp)) {
    // get(concat(a0, a1, ...), m) -> get(an, m - s0 - s1 - ...)
    uint64_t elemIndex = *idxOpt;
    for (auto input : llvm::reverse(inputConcat.getInputs())) {
      size_t size = hw::type_cast<ArrayType>(input.getType()).getSize();
      if (elemIndex >= size) {
        elemIndex -= size;
        continue;
      }

      unsigned indexWidth = size == 1 ? 1 : llvm::Log2_64_Ceil(size);
      auto newIdxOp = rewriter.create<ConstantOp>(
          op.getLoc(), rewriter.getIntegerType(indexWidth), elemIndex);

      rewriter.replaceOpWithNewOp<ArrayGetOp>(op, input, newIdxOp);
      return success();
    }
    return failure();
  }

  return failure();
}

//===----------------------------------------------------------------------===//
// TypedeclOp
//===----------------------------------------------------------------------===//

StringRef TypedeclOp::getPreferredName() {
  return getVerilogName().value_or(getName());
}

Type TypedeclOp::getAliasType() {
  auto parentScope = cast<hw::TypeScopeOp>(getOperation()->getParentOp());
  return hw::TypeAliasType::get(
      SymbolRefAttr::get(parentScope.getSymNameAttr(),
                         {FlatSymbolRefAttr::get(*this)}),
      getType());
}

//===----------------------------------------------------------------------===//
// BitcastOp
//===----------------------------------------------------------------------===//

OpFoldResult BitcastOp::fold(ArrayRef<Attribute> operands) {
  // Identity.
  // bitcast(%a) : A -> A ==> %a
  if (getOperand().getType() == getType())
    return getOperand();

  return {};
}

LogicalResult BitcastOp::canonicalize(BitcastOp op, PatternRewriter &rewriter) {
  // Composition.
  // %b = bitcast(%a) : A -> B
  //      bitcast(%b) : B -> C
  // ===> bitcast(%a) : A -> C
  auto inputBitcast =
      dyn_cast_or_null<BitcastOp>(op.getInput().getDefiningOp());
  if (!inputBitcast)
    return failure();
  auto bitcast = rewriter.createOrFold<BitcastOp>(op.getLoc(), op.getType(),
                                                  inputBitcast.getInput());
  rewriter.replaceOp(op, bitcast);
  return success();
}

LogicalResult BitcastOp::verify() {
  if (getBitWidth(getInput().getType()) != getBitWidth(getResult().getType()))
    return this->emitOpError("Bitwidth of input must match result");
  return success();
}

//===----------------------------------------------------------------------===//
// TableGen generated logic.
//===----------------------------------------------------------------------===//

// Provide the autogenerated implementation guts for the Op classes.
#define GET_OP_CLASSES
#include "circt/Dialect/HW/HW.cpp.inc"
