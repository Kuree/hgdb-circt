//===- HWOps.h - Declare HW dialect operations ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the operation classes for the HW dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_HW_OPS_H
#define CIRCT_DIALECT_HW_OPS_H

#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/HWOpInterfaces.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/FunctionInterfaces.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/RegionKindInterface.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/StringExtras.h"

namespace circt {
namespace hw {

class EnumFieldAttr;

/// A module port direction.
enum class PortDirection {
  INPUT = 1,
  OUTPUT = 2,
  INOUT = 3,
};

/// Flip a port direction.
PortDirection flip(PortDirection direction);

/// This holds the name, type, direction of a module's ports
struct PortInfo {
  StringAttr name;
  PortDirection direction;
  Type type;

  /// This is the argument index or the result index depending on the direction.
  /// "0" for an output means the first output, "0" for a in/inout means the
  /// first argument.
  size_t argNum = ~0U;

  /// The optional symbol for this port.
  StringAttr sym = {};

  // The optional debug attribute for this port
  StringAttr debugAttr = {};

  StringRef getName() const { return name.getValue(); }
  bool isInput() const { return direction == PortDirection::INPUT; }
  bool isOutput() const { return direction == PortDirection::OUTPUT; }
  bool isInOut() const { return direction == PortDirection::INOUT; }

  /// Return a unique numeric identifier for this port.
  ssize_t getId() const { return isOutput() ? argNum : (-1 - argNum); };
};

/// This holds a decoded list of input/inout and output ports for a module or
/// instance.
struct ModulePortInfo {
  explicit ModulePortInfo(ArrayRef<PortInfo> inputs, ArrayRef<PortInfo> outputs)
      : inputs(inputs.begin(), inputs.end()),
        outputs(outputs.begin(), outputs.end()) {}

  explicit ModulePortInfo(ArrayRef<PortInfo> mergedPorts) {
    inputs.reserve(mergedPorts.size());
    outputs.reserve(mergedPorts.size());
    for (auto port : mergedPorts) {
      if (port.isOutput())
        outputs.push_back(port);
      else
        inputs.push_back(port);
    }
  }

  /// This contains a list of the input and inout ports.
  SmallVector<PortInfo> inputs;
  /// This is a list of the output ports.
  SmallVector<PortInfo> outputs;
};

/// TODO: Move all these functions to a hw::ModuleLike interface.

/// Return an encapsulated set of information about input and output ports of
/// the specified module or instance.
ModulePortInfo getModulePortInfo(Operation *op);

/// Return an encapsulated set of information about input and output ports of
/// the specified module or instance.  The input ports always come before the
/// output ports in the list.
SmallVector<PortInfo> getAllModulePortInfos(Operation *op);

/// Return the PortInfo for the specified input or inout port.
PortInfo getModuleInOrInoutPort(Operation *op, size_t idx);

/// Return the PortInfo for the specified output port.
PortInfo getModuleOutputPort(Operation *op, size_t idx);

/// Insert and remove ports of a module. The insertion and removal indices must
/// be in ascending order. The indices refer to the port positions before any
/// insertion or removal occurs. Ports inserted at the same index will appear in
/// the module in the same order as they were listed in the `insert*` array.
/// If 'body' is provided, additionally inserts/removes the corresponding
/// block arguments.
void modifyModulePorts(Operation *op,
                       ArrayRef<std::pair<unsigned, PortInfo>> insertInputs,
                       ArrayRef<std::pair<unsigned, PortInfo>> insertOutputs,
                       ArrayRef<unsigned> removeInputs,
                       ArrayRef<unsigned> removeOutputs, Block *body = nullptr);

// Helpers for working with modules.

/// Return true if this is an hw.module, external module, generated module etc.
bool isAnyModule(Operation *module);

/// Return true if isAnyModule or instance.
bool isAnyModuleOrInstance(Operation *module);

/// Return the signature for the specified module as a function type.
FunctionType getModuleType(Operation *module);

/// Return the number of inputs for the specified module/instance.
inline unsigned getModuleNumInputs(Operation *moduleOrInstance) {
  assert(isAnyModuleOrInstance(moduleOrInstance) &&
         "must be called on instance or module");
  return moduleOrInstance->getAttrOfType<ArrayAttr>("argNames").size();
}

/// Return the number of outputs for the specified module/instance.
inline unsigned getModuleNumOutputs(Operation *moduleOrInstance) {
  assert(isAnyModuleOrInstance(moduleOrInstance) &&
         "must be called on instance or module");
  return moduleOrInstance->getAttrOfType<ArrayAttr>("resultNames").size();
}

/// Returns the verilog module name attribute or symbol name of any module-like
/// operations.
StringAttr getVerilogModuleNameAttr(Operation *module);
inline StringRef getVerilogModuleName(Operation *module) {
  return getVerilogModuleNameAttr(module).getValue();
}

/// Return the port name for the specified argument or result.  These can only
/// return a null StringAttr when the IR is invalid.
StringAttr getModuleArgumentNameAttr(Operation *module, size_t argNo);
StringAttr getModuleResultNameAttr(Operation *module, size_t argNo);

static inline StringRef getModuleArgumentName(Operation *module, size_t argNo) {
  auto attr = getModuleArgumentNameAttr(module, argNo);
  return attr ? attr.getValue() : StringRef();
}
static inline StringRef getModuleResultName(Operation *module,
                                            size_t resultNo) {
  auto attr = getModuleResultNameAttr(module, resultNo);
  return attr ? attr.getValue() : StringRef();
}

// Index width should be exactly clog2 (size of array), or either 0 or 1 if the
// array is a singleton.
bool isValidIndexBitWidth(Value index, Value array);

void setModuleArgumentNames(Operation *module, ArrayRef<Attribute> names);
void setModuleResultNames(Operation *module, ArrayRef<Attribute> names);

/// Return true if the specified operation is a combinational logic op.
bool isCombinational(Operation *op);

/// Return true if the specified attribute tree is made up of nodes that are
/// valid in a parameter expression.
bool isValidParameterExpression(Attribute attr, Operation *module);

/// Check parameter specified by `value` to see if it is valid within the scope
/// of the specified module `module`.  If not, emit an error at the location of
/// `usingOp` and return failure, otherwise return success.
///
/// If `disallowParamRefs` is true, then parameter references are not allowed.
LogicalResult checkParameterInContext(Attribute value, Operation *module,
                                      Operation *usingOp,
                                      bool disallowParamRefs = false);

/// Check parameter specified by `value` to see if it is valid according to the
/// module's parameters.  If not, emit an error to the diagnostic provided as an
/// argument to the lambda 'instanceError' and return failure, otherwise return
/// success.
///
/// If `disallowParamRefs` is true, then parameter references are not allowed.
LogicalResult checkParameterInContext(
    Attribute value, ArrayAttr moduleParameters,
    const std::function<void(std::function<bool(InFlightDiagnostic &)>)>
        &instanceError,
    bool disallowParamRefs = false);

/// Return the symbol (if exists, else null) on the corresponding input port
/// argument.
StringAttr getArgSym(Operation *op, unsigned i);

/// Return the symbol (if any, else null) on the corresponding output port
/// argument.
StringAttr getResultSym(Operation *op, unsigned i);

// Check whether an integer value is an offset from a base.
bool isOffset(Value base, Value index, uint64_t offset);

// A class for providing access to the in- and output ports of a module through
// use of the HWModuleBuilder.
class HWModulePortAccessor {

public:
  HWModulePortAccessor(Location loc, const ModulePortInfo &info,
                       Region &bodyRegion);

  // Returns the i'th/named input port of the module.
  Value getInput(unsigned i);
  Value getInput(StringRef name);
  ValueRange getInputs() { return inputArgs; }

  // Assigns the i'th/named output port of the module.
  void setOutput(unsigned i, Value v);
  void setOutput(StringRef name, Value v);

  const ModulePortInfo &getModulePortInfo() const { return info; }
  const llvm::SmallVector<Value> &getOutputOperands() const {
    return outputOperands;
  }

private:
  llvm::StringMap<unsigned> inputIdx, outputIdx;
  llvm::SmallVector<Value> inputArgs;
  llvm::SmallVector<Value> outputOperands;
  ModulePortInfo info;
};

using HWModuleBuilder =
    llvm::function_ref<void(OpBuilder &, HWModulePortAccessor &)>;

} // namespace hw
} // namespace circt

#define GET_OP_CLASSES
#include "circt/Dialect/HW/HW.h.inc"

#endif // CIRCT_DIALECT_HW_OPS_H
