//===- LowerTypes.cpp - Lower Aggregate Types -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the LowerTypes pass.  This pass replaces aggregate types
// with expanded values.
//
// This pass walks the operations in reverse order. This lets it visit users
// before defs. Users can usually be expanded out to multiple operations (think
// mux of a bundle to muxes of each field) with a temporary subWhatever op
// inserted. When processing an aggregate producer, we blow out the op as
// appropriate, then walk the users, often those are subWhatever ops which can
// be bypassed and deleted. Function arguments are logically last on the
// operation visit order and walked left to right, being peeled one layer at a
// time with replacements inserted to the right of the original argument.
//
// Each processing of an op peels one layer of aggregate type off.  Because new
// ops are inserted immediately above the current up, the walk will visit them
// next, effectively recusing on the aggregate types, without recusing.  These
// potentially temporary ops(if the aggregate is complex) effectively serve as
// the worklist.  Often aggregates are shallow, so the new ops are the final
// ones.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAttributes.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/NLATable.h"
#include "circt/Dialect/FIRRTL/Namespace.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Threading.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Parallel.h"

#define DEBUG_TYPE "firrtl-lower-types"

using namespace circt;
using namespace firrtl;

// TODO: check all argument types
namespace {
/// This represents a flattened bundle field element.
struct FlatBundleFieldEntry {
  /// This is the underlying ground type of the field.
  FIRRTLBaseType type;
  /// The index in the parent type
  size_t index;
  /// The fieldID
  unsigned fieldID;
  /// This is a suffix to add to the field name to make it unique.
  SmallString<16> suffix;
  /// This indicates whether the field was flipped to be an output.
  bool isOutput;

  FlatBundleFieldEntry(const FIRRTLBaseType &type, size_t index,
                       unsigned fieldID, StringRef suffix, bool isOutput)
      : type(type), index(index), fieldID(fieldID), suffix(suffix),
        isOutput(isOutput) {}

  void dump() const {
    llvm::errs() << "FBFE{" << type << " index<" << index << "> fieldID<"
                 << fieldID << "> suffix<" << suffix << "> isOutput<"
                 << isOutput << ">}\n";
  }
};
} // end anonymous namespace

/// Return true if the type has more than zero bitwidth.
static bool hasZeroBitWidth(FIRRTLType type) {
  return TypeSwitch<FIRRTLType, bool>(type)
      .Case<BundleType>([&](auto bundle) {
        for (size_t i = 0, e = bundle.getNumElements(); i < e; ++i) {
          auto elt = bundle.getElement(i);
          if (hasZeroBitWidth(elt.type))
            return true;
        }
        return bundle.getNumElements() == 0;
      })
      .Case<FVectorType>([&](auto vector) {
        if (vector.getNumElements() == 0)
          return true;
        return hasZeroBitWidth(vector.getElementType());
      })
      .Case<FIRRTLBaseType>([](auto groundType) {
        return firrtl::getBitWidth(groundType).value_or(0) == 0;
      })
      .Case<RefType>([](auto ref) { return hasZeroBitWidth(ref.getType()); })
      .Default([](auto) { return false; });
}

/// Return true if the type is a 1d vector type or ground type.
static bool isOneDimVectorType(FIRRTLType type) {
  return TypeSwitch<FIRRTLType, bool>(type)
      .Case<BundleType>([&](auto bundle) { return false; })
      .Case<FVectorType>([&](FVectorType vector) {
        // When the size is 1, lower the vector into a scalar.
        return vector.getElementType().isGround() &&
               vector.getNumElements() > 1;
      })
      .Default([](auto groundType) { return true; });
}

/// Return true if the type has a bundle type as subtype.
static bool containsBundleType(FIRRTLType type) {
  return TypeSwitch<FIRRTLType, bool>(type)
      .Case<BundleType>([&](auto bundle) { return true; })
      .Case<FVectorType>([&](FVectorType vector) {
        return containsBundleType(vector.getElementType());
      })
      .Default([](auto groundType) { return false; });
}

/// Return true if we can preserve the type.
static bool isPreservableAggregateType(Type type,
                                       PreserveAggregate::PreserveMode mode) {
  // Return false if no aggregate value is preserved.
  if (mode == PreserveAggregate::None)
    return false;

  auto firrtlType = type.isa<RefType>() ? type.cast<RefType>().getType()
                                        : type.dyn_cast<FIRRTLBaseType>();
  if (!firrtlType)
    return false;

  // We can a preserve the type iff (i) the type is not passive, (ii) the type
  // doesn't contain analog and (iii) type don't contain zero bitwidth.
  if (!firrtlType.isPassive() || firrtlType.containsAnalog() ||
      hasZeroBitWidth(firrtlType))
    return false;

  switch (mode) {
  case PreserveAggregate::All:
    return true;
  case PreserveAggregate::OneDimVec:
    return isOneDimVectorType(firrtlType);
  case PreserveAggregate::Vec:
    return !containsBundleType(firrtlType);
  default:
    llvm_unreachable("unexpected mode");
  }
}

/// Peel one layer of an aggregate type into its components.  Type may be
/// complex, but empty, in which case fields is empty, but the return is true.
static bool peelType(Type type, SmallVectorImpl<FlatBundleFieldEntry> &fields,
                     PreserveAggregate::PreserveMode mode) {
  // If the aggregate preservation is enabled and the type is preservable,
  // then just return.
  if (isPreservableAggregateType(type, mode))
    return false;

  if (auto refType = type.dyn_cast<RefType>())
    type = refType.getType();
  return TypeSwitch<Type, bool>(type)
      .Case<BundleType>([&](auto bundle) {
        SmallString<16> tmpSuffix;
        // Otherwise, we have a bundle type.  Break it down.
        for (size_t i = 0, e = bundle.getNumElements(); i < e; ++i) {
          auto elt = bundle.getElement(i);
          // Construct the suffix to pass down.
          tmpSuffix.resize(0);
          tmpSuffix.push_back('_');
          tmpSuffix.append(elt.name.getValue());
          fields.emplace_back(elt.type, i, bundle.getFieldID(i), tmpSuffix,
                              elt.isFlip);
        }
        return true;
      })
      .Case<FVectorType>([&](auto vector) {
        // Increment the field ID to point to the first element.
        for (size_t i = 0, e = vector.getNumElements(); i != e; ++i) {
          fields.emplace_back(vector.getElementType(), i, vector.getFieldID(i),
                              "_" + std::to_string(i), false);
        }
        return true;
      })
      .Default([](auto op) { return false; });
}

/// Return if something is not a normal subaccess.  Non-normal includes
/// zero-length vectors and constant indexes (which are really subindexes).
static bool isNotSubAccess(Operation *op) {
  SubaccessOp sao = dyn_cast<SubaccessOp>(op);
  if (!sao)
    return true;
  ConstantOp arg = dyn_cast_or_null<ConstantOp>(sao.getIndex().getDefiningOp());
  if (arg && sao.getInput().getType().cast<FVectorType>().getNumElements() != 0)
    return true;
  return false;
}

/// Look through and collect subfields leading to a subaccess.
static SmallVector<Operation *> getSAWritePath(Operation *op) {
  SmallVector<Operation *> retval;
  auto defOp = op->getOperand(0).getDefiningOp();
  while (isa_and_nonnull<SubfieldOp, SubindexOp, SubaccessOp>(defOp)) {
    retval.push_back(defOp);
    defOp = defOp->getOperand(0).getDefiningOp();
  }
  // Trim to the subaccess
  while (!retval.empty() && isNotSubAccess(retval.back()))
    retval.pop_back();
  return retval;
}

/// Returns whether the given annotation requires precise tracking of the field
/// ID as it gets replicated across lowered operations.
static bool isAnnotationSensitiveToFieldID(Annotation anno) {
  return anno.isClass(signalDriverAnnoClass);
}

/// If an annotation on one operation is replicated across multiple IR
/// operations as a result of type lowering, the replicated annotations may want
/// to track which field ID they were applied to. This function adds a fieldID
/// to such a replicated operation, if the annotation in question requires it.
static Attribute updateAnnotationFieldID(MLIRContext *ctxt, Attribute attr,
                                         unsigned fieldID, Type i64ty) {
  DictionaryAttr dict = attr.cast<DictionaryAttr>();

  // No need to do anything if the annotation applies to the entire field.
  if (fieldID == 0)
    return attr;

  // Only certain annotations require precise tracking of field IDs.
  Annotation anno(dict);
  if (!isAnnotationSensitiveToFieldID(anno))
    return attr;

  // Add the new ID to the existing field ID in the annotation.
  if (auto existingFieldID = anno.getMember<IntegerAttr>("fieldID"))
    fieldID += existingFieldID.getValue().getZExtValue();
  NamedAttrList fields(dict);
  fields.set("fieldID", IntegerAttr::get(i64ty, fieldID));
  return DictionaryAttr::get(ctxt, fields);
}

static MemOp cloneMemWithNewType(ImplicitLocOpBuilder *b, MemOp op,
                                 FlatBundleFieldEntry field) {
  SmallVector<Type, 8> ports;
  SmallVector<Attribute, 8> portNames;

  auto oldPorts = op.getPorts();
  for (size_t portIdx = 0, e = oldPorts.size(); portIdx < e; ++portIdx) {
    auto port = oldPorts[portIdx];
    ports.push_back(
        MemOp::getTypeForPort(op.getDepth(), field.type, port.second));
    portNames.push_back(port.first);
  }

  // It's easier to duplicate the old annotations, then fix and filter them.
  auto newMem = b->create<MemOp>(
      ports, op.getReadLatency(), op.getWriteLatency(), op.getDepth(),
      op.getRuw(), portNames, (op.getName() + field.suffix).str(),
      op.getNameKind(), op.getAnnotations().getValue(),
      op.getPortAnnotations().getValue(), op.getInnerSymAttr());
  if (auto oldName = getInnerSymName(op))
    newMem.setInnerSymAttr(hw::InnerSymAttr::get(StringAttr::get(
        b->getContext(), oldName.getValue() + (op.getName() + field.suffix))));

  SmallVector<Attribute> newAnnotations;
  for (size_t portIdx = 0, e = newMem.getNumResults(); portIdx < e; ++portIdx) {
    auto portType = newMem.getResult(portIdx).getType().cast<BundleType>();
    auto oldPortType = op.getResult(portIdx).getType().cast<BundleType>();
    SmallVector<Attribute> portAnno;
    for (auto attr : newMem.getPortAnnotation(portIdx)) {
      Annotation anno(attr);
      if (auto annoFieldID = anno.getFieldID()) {
        auto targetIndex = oldPortType.getIndexForFieldID(annoFieldID);

        // Apply annotations to all elements if the target is the whole
        // sub-field.
        if (annoFieldID == oldPortType.getFieldID(targetIndex)) {
          anno.setMember(
              "circt.fieldID",
              b->getI32IntegerAttr(portType.getFieldID(targetIndex)));
          portAnno.push_back(anno.getDict());
          continue;
        }

        // Handle aggregate sub-fields, including `(r/w)data` and `(w)mask`.
        if (oldPortType.getElement(targetIndex).type.isa<BundleType>()) {
          // Check whether the annotation falls into the range of the current
          // field. Note that the `field` here is peeled from the `data`
          // sub-field of the memory port, thus we need to add the fieldID of
          // `data` or `mask` sub-field to get the "real" fieldID.
          auto fieldID = field.fieldID + oldPortType.getFieldID(targetIndex);
          if (annoFieldID >= fieldID &&
              annoFieldID <= fieldID + field.type.getMaxFieldID()) {
            // Set the field ID of the new annotation.
            auto newFieldID =
                annoFieldID - fieldID + portType.getFieldID(targetIndex);
            anno.setMember("circt.fieldID", b->getI32IntegerAttr(newFieldID));
            portAnno.push_back(anno.getDict());
          }
        }
      } else
        portAnno.push_back(attr);
    }
    newAnnotations.push_back(b->getArrayAttr(portAnno));
  }
  newMem.setAllPortAnnotations(newAnnotations);
  return newMem;
}

//===----------------------------------------------------------------------===//
// Module Type Lowering
//===----------------------------------------------------------------------===//
namespace {

struct AttrCache {
  AttrCache(MLIRContext *context) {
    i64ty = IntegerType::get(context, 64);
    innerSymAttr = StringAttr::get(context, "inner_sym");
    nameAttr = StringAttr::get(context, "name");
    nameKindAttr = StringAttr::get(context, "nameKind");
    sPortDirections = StringAttr::get(context, "portDirections");
    sPortNames = StringAttr::get(context, "portNames");
    sPortTypes = StringAttr::get(context, "portTypes");
    sPortSyms = StringAttr::get(context, "portSyms");
    sPortAnnotations = StringAttr::get(context, "portAnnotations");
    sEmpty = StringAttr::get(context, "");
  }
  AttrCache(const AttrCache &) = default;

  Type i64ty;
  StringAttr innerSymAttr, nameAttr, nameKindAttr, sPortDirections, sPortNames,
      sPortTypes, sPortSyms, sPortAnnotations, sEmpty;
};

// The visitors all return true if the operation should be deleted, false if
// not.
struct TypeLoweringVisitor : public FIRRTLVisitor<TypeLoweringVisitor, bool> {

  TypeLoweringVisitor(MLIRContext *context,
                      PreserveAggregate::PreserveMode preserveAggregate,
                      bool preservePublicTypes, SymbolTable &symTbl,
                      const AttrCache &cache, bool insertDebugInfo)
      : context(context), aggregatePreservationMode(preserveAggregate),
        preservePublicTypes(preservePublicTypes), symTbl(symTbl), cache(cache),
        insertDebugInfo(insertDebugInfo) {
  }
  using FIRRTLVisitor<TypeLoweringVisitor, bool>::visitDecl;
  using FIRRTLVisitor<TypeLoweringVisitor, bool>::visitExpr;
  using FIRRTLVisitor<TypeLoweringVisitor, bool>::visitStmt;

  /// If the referenced operation is a FModuleOp or an FExtModuleOp, perform
  /// type lowering on all operations.
  void lowerModule(FModuleLike op);

  bool lowerArg(FModuleLike module, size_t argIndex, size_t argsRemoved,
                SmallVectorImpl<PortInfo> &newArgs,
                SmallVectorImpl<Value> &lowering);
  std::pair<Value, PortInfo> addArg(Operation *module, unsigned insertPt,
                                    unsigned insertPtOffset, FIRRTLType srcType,
                                    FlatBundleFieldEntry field,
                                    PortInfo &oldArg);

  // Helpers to manage state.
  bool visitDecl(FExtModuleOp op);
  bool visitDecl(FModuleOp op);
  bool visitDecl(InstanceOp op);
  bool visitDecl(MemOp op);
  bool visitDecl(NodeOp op);
  bool visitDecl(RegOp op);
  bool visitDecl(WireOp op);
  bool visitDecl(RegResetOp op);
  bool visitExpr(InvalidValueOp op);
  bool visitExpr(SubaccessOp op);
  bool visitExpr(VectorCreateOp op);
  bool visitExpr(BundleCreateOp op);
  bool visitExpr(MultibitMuxOp op);
  bool visitExpr(MuxPrimOp op);
  bool visitExpr(mlir::UnrealizedConversionCastOp op);
  bool visitExpr(BitCastOp op);
  bool visitExpr(RefSendOp op);
  bool visitExpr(RefResolveOp op);
  bool visitStmt(ConnectOp op);
  bool visitStmt(StrictConnectOp op);
  bool visitStmt(WhenOp op);

  bool isFailed() const { return encounteredError; }

private:
  void processUsers(Value val, ArrayRef<Value> mapping);
  bool processSAPath(Operation *);
  void lowerBlock(Block *);
  void lowerSAWritePath(Operation *, ArrayRef<Operation *> writePath);
  bool lowerProducer(
      Operation *op,
      llvm::function_ref<Value(const FlatBundleFieldEntry &, ArrayAttr)> clone);
  /// Copy annotations from \p annotations to \p loweredAttrs, except
  /// annotations with "target" key, that do not match the field suffix.
  ArrayAttr filterAnnotations(MLIRContext *ctxt, ArrayAttr annotations,
                              FIRRTLType srcType, FlatBundleFieldEntry field);

  PreserveAggregate::PreserveMode
  getPreservatinoModeForModule(FModuleLike moduleLike);
  Value getSubWhatever(Value val, size_t index);

  size_t uniqueIdx = 0;
  std::string uniqueName() {
    auto myID = uniqueIdx++;
    return (Twine("__GEN_") + Twine(myID)).str();
  }

  MLIRContext *context;

  /// Aggregate preservation mode.
  PreserveAggregate::PreserveMode aggregatePreservationMode;

  /// Exteranal modules and toplevel modules should have lowered types if this
  /// flag is enabled.
  bool preservePublicTypes;

  /// The builder is set and maintained in the main loop.
  ImplicitLocOpBuilder *builder;

  // Keep a symbol table around for resolving symbols
  SymbolTable &symTbl;

  // Cache some attributes
  const AttrCache &cache;


  // Set true if the lowering failed.
  bool encounteredError = false;
  // Inserts debug information to keep track of name changes
  bool insertDebugInfo;
};
} // namespace

/// Return aggregate preservation mode for the module. If the module has a
/// public linkage, then it is not allowed to preserve aggregate values on ports
/// unless `preservePublicTypes` flag is disabled.
PreserveAggregate::PreserveMode
TypeLoweringVisitor::getPreservatinoModeForModule(FModuleLike module) {
  // We cannot preserve external module ports.
  if (!isa<FModuleOp>(module))
    return PreserveAggregate::None;
  if (aggregatePreservationMode != PreserveAggregate::None &&
      preservePublicTypes && cast<hw::HWModuleLike>(*module).isPublic())
    return PreserveAggregate::None;
  return aggregatePreservationMode;
}

Value TypeLoweringVisitor::getSubWhatever(Value val, size_t index) {
  if (BundleType bundle = val.getType().dyn_cast<BundleType>()) {
    return builder->create<SubfieldOp>(val, index);
  } else if (FVectorType fvector = val.getType().dyn_cast<FVectorType>()) {
    return builder->create<SubindexOp>(val, index);
  } else if (val.getType().isa<RefType>()) {
    return builder->create<RefSubOp>(val, index);
  }
  llvm_unreachable("Unknown aggregate type");
  return nullptr;
}

/// Conditionally expand a subaccessop write path
bool TypeLoweringVisitor::processSAPath(Operation *op) {
  // Does this LHS have a subaccessop?
  SmallVector<Operation *> writePath = getSAWritePath(op);
  if (writePath.empty())
    return false;

  lowerSAWritePath(op, writePath);
  // Unhook the writePath from the connect.  This isn't the right type, but we
  // are deleting the op anyway.
  op->eraseOperands(0, 2);
  // See how far up the tree we can delete things.
  for (size_t i = 0; i < writePath.size(); ++i) {
    if (writePath[i]->use_empty()) {
      writePath[i]->erase();
    } else {
      break;
    }
  }
  return true;
}

void TypeLoweringVisitor::lowerBlock(Block *block) {
  // Lower the operations bottom up.
  for (auto it = block->rbegin(), e = block->rend(); it != e;) {
    auto &iop = *it;
    builder->setInsertionPoint(&iop);
    builder->setLoc(iop.getLoc());
    bool removeOp = dispatchVisitor(&iop);
    ++it;
    // Erase old ops eagerly so we don't have dangling uses we've already
    // lowered.
    if (removeOp)
      iop.erase();
  }
}

ArrayAttr TypeLoweringVisitor::filterAnnotations(MLIRContext *ctxt,
                                                 ArrayAttr annotations,
                                                 FIRRTLType srcType,
                                                 FlatBundleFieldEntry field) {
  SmallVector<Attribute> retval;
  if (!annotations || annotations.empty())
    return ArrayAttr::get(ctxt, retval);
  for (auto opAttr : annotations) {
    Optional<int64_t> maybeFieldID = None;
    DictionaryAttr annotation;
    annotation = opAttr.dyn_cast<DictionaryAttr>();
    if (annotations)
      // Erase the circt.fieldID.  If this is needed later, it will be re-added.
      if (auto id = annotation.getAs<IntegerAttr>("circt.fieldID")) {
        maybeFieldID = id.getInt();
        Annotation anno(annotation);
        anno.removeMember("circt.fieldID");
        annotation = anno.getDict();
      }
    if (!maybeFieldID) {
      retval.push_back(
          updateAnnotationFieldID(ctxt, opAttr, field.fieldID, cache.i64ty));
      continue;
    }
    auto fieldID = maybeFieldID.value();
    // Check whether the annotation falls into the range of the current field.
    if (fieldID != 0 &&
        !(fieldID >= field.fieldID &&
          fieldID <= field.fieldID + field.type.getMaxFieldID()))
      continue;

    // Apply annotations to all elements if fieldID is equal to zero.
    if (fieldID == 0) {
      retval.push_back(annotation);
      continue;
    }

    if (auto newFieldID = fieldID - field.fieldID) {
      // If the target is a subfield/subindex of the current field, create a
      // new annotation with the correct circt.fieldID.
      Annotation newAnno(annotation);
      newAnno.setMember("circt.fieldID",
                        builder->getI32IntegerAttr(newFieldID));
      retval.push_back(newAnno.getDict());
      continue;
    }

    retval.push_back(annotation);
  }
  return ArrayAttr::get(ctxt, retval);
}

bool TypeLoweringVisitor::lowerProducer(
    Operation *op,
    llvm::function_ref<Value(const FlatBundleFieldEntry &, ArrayAttr)> clone) {
  // If this is not a bundle, there is nothing to do.
  auto srcType = op->getResult(0).getType().dyn_cast<FIRRTLType>();
  if (!srcType)
    return false;
  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;

  if (!peelType(srcType, fieldTypes, aggregatePreservationMode)) {
    if (insertDebugInfo && !op->hasAttr("hw.debug.name")) {
      // If it's not a temp nodes produced by Chisel. For now, we use a
      // naming heuristics: all temp nodes are prefixed with _. However, in
      // case where users create such node, this hack will fail
      if (auto nameAttr = op->getAttrOfType<StringAttr>("name")) {
        if (nameAttr.size() > 0 && nameAttr.data()[0] != '_')
          op->setAttr("hw.debug.name", nameAttr);
      }
    }
    return false;
  }

  // If an aggregate value has a symbol, emit errors.
  if (op->hasAttr(cache.innerSymAttr)) {
    op->emitError() << "has a symbol, but no symbols may exist on aggregates "
                       "passed through LowerTypes";
    encounteredError = true;
    return false;
  }

  SmallVector<Value> lowered;
  // Loop over the leaf aggregates.
  SmallString<16> loweredName;
  SmallString<16> loweredSymName;
  auto nameKindAttr = op->getAttrOfType<NameKindEnumAttr>(cache.nameKindAttr);

  if (auto nameAttr = op->getAttrOfType<StringAttr>(cache.nameAttr))
    loweredName = nameAttr.getValue();
  auto baseNameLen = loweredName.size();
  auto oldAnno = op->getAttr("annotations").dyn_cast_or_null<ArrayAttr>();

  bool isArray = srcType.isa<FVectorType>();

  auto baseName = loweredName;

  for (auto fieldIdx = 0u; fieldIdx < fieldTypes.size(); fieldIdx++) {
    auto field = fieldTypes[fieldIdx];
    // if it's an array, need to add array idx
    SmallString<16> targetName = baseName;
    if (isArray) {
      targetName.append(".");
      targetName.append(std::to_string(fieldIdx));
    }
    if (!loweredName.empty()) {
      loweredName.resize(baseNameLen);
      loweredName += field.suffix;
    }

    // For all annotations on the parent op, filter them based on the target
    // attribute.
    ArrayAttr loweredAttrs =
        filterAnnotations(context, oldAnno, srcType, field);
    auto newVal = clone(field, loweredAttrs);

    // Carry over the name, if present.
    if (auto *newOp = newVal.getDefiningOp()) {
      if (!loweredName.empty())
        newOp->setAttr(cache.nameAttr, StringAttr::get(context, loweredName));
      if (nameKindAttr)
        newOp->setAttr(cache.nameKindAttr, nameKindAttr);
      if (insertDebugInfo)
        newOp->setAttr("hw.debug.name", StringAttr::get(context, targetName));
    }
    lowered.push_back(newVal);
  }

  processUsers(op->getResult(0), lowered);
  return true;
}

void TypeLoweringVisitor::processUsers(Value val, ArrayRef<Value> mapping) {
  for (auto user : llvm::make_early_inc_range(val.getUsers())) {
    if (SubindexOp sio = dyn_cast<SubindexOp>(user)) {
      Value repl = mapping[sio.getIndex()];
      sio.replaceAllUsesWith(repl);
      sio.erase();
    } else if (SubfieldOp sfo = dyn_cast<SubfieldOp>(user)) {
      // Get the input bundle type.
      Value repl = mapping[sfo.getFieldIndex()];
      sfo.replaceAllUsesWith(repl);
      sfo.erase();
    } else if (auto refSub = dyn_cast<RefSubOp>(user)) {
      Value repl = mapping[refSub.getIndex()];
      refSub.replaceAllUsesWith(repl);
      refSub.erase();
    } else {
      // This means, we have already processed the user, and it didn't lower its
      // inputs. This is an opaque user, which will continue to have aggregate
      // type as input, even after LowerTypes. So, construct the vector/bundle
      // back from the lowered elements to ensure a valid input into the opaque
      // op. This only supports Bundle or vector of ground type elements.
      // Recursive aggregate types are not yet supported.

      // This builder ensures that the aggregate construction happens at the
      // user location, and the LowerTypes algorithm will not touch them any
      // more, because LowerTypes was reverse iterating on the block and the
      // user has already been processed.
      ImplicitLocOpBuilder b(user->getLoc(), user);
      // Cat all the field elements.
      Value accumulate;
      for (auto v : mapping) {
        if (!v.getType().cast<FIRRTLBaseType>().isGround()) {
          user->emitError("cannot handle an opaque user of aggregate types "
                          "with non-ground type elements");
          return;
        }
        if (val.getType().cast<FIRRTLType>().isa<FVectorType>())
          accumulate =
              (accumulate ? b.createOrFold<CatPrimOp>(v, accumulate) : v);
        else
          // Bundle subfields are filled from MSB to LSB.
          accumulate =
              (accumulate ? b.createOrFold<CatPrimOp>(accumulate, v) : v);
      }
      // Cast it back to the original aggregate type.
      auto input = b.createOrFold<BitCastOp>(val.getType(), accumulate);
      user->replaceUsesOfWith(val, input);
    }
  }
}

void TypeLoweringVisitor::lowerModule(FModuleLike op) {
  if (auto module = dyn_cast<FModuleOp>(*op))
    visitDecl(module);
  else if (auto extModule = dyn_cast<FExtModuleOp>(*op))
    visitDecl(extModule);
}

// Creates and returns a new block argument of the specified type to the
// module. This also maintains the name attribute for the new argument,
// possibly with a new suffix appended.
std::pair<Value, PortInfo>
TypeLoweringVisitor::addArg(Operation *module, unsigned insertPt,
                            unsigned insertPtOffset, FIRRTLType srcType,
                            FlatBundleFieldEntry field, PortInfo &oldArg) {
  Value newValue;
  FIRRTLType fieldType = srcType.isa<RefType>()
                             ? FIRRTLType(RefType::get(field.type))
                             : field.type;
  if (auto mod = dyn_cast<FModuleOp>(module)) {
    Block *body = mod.getBodyBlock();
    // Append the new argument.
    newValue = body->insertArgument(insertPt, fieldType, oldArg.loc);
  }

  // Save the name attribute for the new argument.
  auto name = builder->getStringAttr(oldArg.name.getValue() + field.suffix);

  if (oldArg.sym) {
    mlir::emitError(newValue ? newValue.getLoc() : module->getLoc())
        << "has a symbol, but no symbols may exist on aggregates "
           "passed through LowerTypes";
    encounteredError = true;
  }

  // Populate the new arg attributes.
  auto newAnnotations = filterAnnotations(
      context, oldArg.annotations.getArrayAttr(), srcType, field);
  // Flip the direction if the field is an output.
  auto direction = (Direction)((unsigned)oldArg.direction ^ field.isOutput);

  return std::make_pair(newValue, PortInfo{name,
                                           fieldType,
                                           direction,
                                           {},
                                           oldArg.loc,
                                           AnnotationSet(newAnnotations)});
}

// Lower arguments with bundle type by flattening them.
bool TypeLoweringVisitor::lowerArg(FModuleLike module, size_t argIndex,
                                   size_t argsRemoved,
                                   SmallVectorImpl<PortInfo> &newArgs,
                                   SmallVectorImpl<Value> &lowering) {

  // Flatten any bundle types.
  SmallVector<FlatBundleFieldEntry> fieldTypes;
  auto srcType = newArgs[argIndex].type.cast<FIRRTLType>();

  if (!peelType(srcType, fieldTypes, getPreservatinoModeForModule(module))) {
    // Use its default name instead
    if (insertDebugInfo) {
      auto attrs = mlir::DictionaryAttr::get(
          context, {{StringAttr::get(context, "hw.debug.name"),
                     newArgs[argIndex].name}});
      newArgs[argIndex].annotations.addAnnotations(attrs);
    }
    return false;
  }

  // Get original arg name
  auto originalName = newArgs[argIndex].name;

  for (const auto &field : llvm::enumerate(fieldTypes)) {
    auto newValue = addArg(module, 1 + argIndex + field.index(), argsRemoved,
                           srcType, field.value(), newArgs[argIndex]);
    // Insert renaming attributes to keep track of the naming changes
    // We store the original name as argName.subFieldName. subFieldName can
    // be either a name or a number
    if (insertDebugInfo) {
      auto attrs = mlir::DictionaryAttr::get(
          context,
          {{StringAttr::get(context, "hw.debug.name"),
            StringAttr::get(context, originalName.str() + "." +
                                         field.value().suffix.substr(1))}});

      newValue.second.annotations.addAnnotations(attrs);
    }
    newArgs.insert(newArgs.begin() + 1 + argIndex + field.index(),
                   newValue.second);
    // Lower any other arguments by copying them to keep the relative order.
    lowering.push_back(newValue.first);
  }
  return true;
}

static Value cloneAccess(ImplicitLocOpBuilder *builder, Operation *op,
                         Value rhs) {
  if (auto rop = dyn_cast<SubfieldOp>(op))
    return builder->create<SubfieldOp>(rhs, rop.getFieldIndex());
  if (auto rop = dyn_cast<SubindexOp>(op))
    return builder->create<SubindexOp>(rhs, rop.getIndex());
  if (auto rop = dyn_cast<SubaccessOp>(op))
    return builder->create<SubaccessOp>(rhs, rop.getIndex());
  op->emitError("Unknown accessor");
  return nullptr;
}

void TypeLoweringVisitor::lowerSAWritePath(Operation *op,
                                           ArrayRef<Operation *> writePath) {
  SubaccessOp sao = cast<SubaccessOp>(writePath.back());
  auto saoType = sao.getInput().getType().cast<FVectorType>();
  auto selectWidth = llvm::Log2_64_Ceil(saoType.getNumElements());

  for (size_t index = 0, e = saoType.getNumElements(); index < e; ++index) {
    auto cond = builder->create<EQPrimOp>(
        sao.getIndex(),
        builder->createOrFold<ConstantOp>(UIntType::get(context, selectWidth),
                                          APInt(selectWidth, index)));
    builder->create<WhenOp>(cond, false, [&]() {
      // Recreate the write Path
      Value leaf = builder->create<SubindexOp>(sao.getInput(), index);
      for (int i = writePath.size() - 2; i >= 0; --i)
        leaf = cloneAccess(builder, writePath[i], leaf);

      emitConnect(*builder, leaf, op->getOperand(1));
    });
  }
}

static bool
canLowerConnect(FConnectLike op,
                PreserveAggregate::PreserveMode aggregatePreservationMode) {
  auto destType = op.getDest().getType();
  return !(destType.isa<RefType>() &&
           isPreservableAggregateType(destType, aggregatePreservationMode));
}

// Expand connects of aggregates
bool TypeLoweringVisitor::visitStmt(ConnectOp op) {
  if (!canLowerConnect(op, aggregatePreservationMode))
    return false;
  if (processSAPath(op))
    return true;

  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;

  // We have to expand connections even if the aggregate preservation is true.
  if (!peelType(op.getDest().getType(), fields, PreserveAggregate::None)) {
    // Store the name as well if enabled
    if (!insertDebugInfo)
      return false;
    auto dest = op.getDest();
    if (auto *destOp = dest.getDefiningOp()) {
      if (auto nameAttr = destOp->getAttr("name")) {
        if (auto instOp = dest.getDefiningOp<InstanceOp>()) {
          // this is an instance connection, need to treat differently
          // since the defining OP would be the instance and therefore the
          // naming would be wrong
          auto const &results = instOp.getResults();
          for (auto const &res : results) {
            if (res == dest) {
              // Need to obtain the name. a little hacky,
              // but it's the best way I know of
              auto resultNo = res.getResultNumber();
              auto portName = instOp.getPortName(resultNo);
              auto nameStr = nameAttr.cast<mlir::StringAttr>().str() + "_" +
                             portName.str();
              op->setAttr("hw.debug.name",
                          mlir::StringAttr::get(context, nameStr));
              break;
            }
          }
        } else {
          op->setAttr("hw.debug.name", nameAttr);
        }
      }
    }
    return false;
  }

  // Loop over the leaf aggregates.
  for (const auto &field : llvm::enumerate(fields)) {
    Value src = getSubWhatever(op.getSrc(), field.index());
    Value dest = getSubWhatever(op.getDest(), field.index());
    if (field.value().isOutput)
      std::swap(src, dest);
    emitConnect(*builder, dest, src);
  }
  return true;
}

// Expand connects of aggregates
bool TypeLoweringVisitor::visitStmt(StrictConnectOp op) {
  if (!canLowerConnect(op, aggregatePreservationMode))
    return false;
  if (processSAPath(op))
    return true;

  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;

  // We have to expand connections even if the aggregate preservation is true.
  if (!peelType(op.getDest().getType(), fields, PreserveAggregate::None))
    return false;

  // Loop over the leaf aggregates.
  for (const auto &field : llvm::enumerate(fields)) {
    Value src = getSubWhatever(op.getSrc(), field.index());
    Value dest = getSubWhatever(op.getDest(), field.index());
    if (field.value().isOutput && !op.getDest().getType().isa<RefType>())
      std::swap(src, dest);
    builder->create<StrictConnectOp>(dest, src);
  }
  return true;
}

bool TypeLoweringVisitor::visitStmt(WhenOp op) {
  // The WhenOp itself does not require any lowering, the only value it uses
  // is a one-bit predicate.  Recursively visit all regions so internal
  // operations are lowered.

  // Visit operations in the then block.
  lowerBlock(&op.getThenBlock());

  // Visit operations in the else block.
  if (op.hasElseRegion())
    lowerBlock(&op.getElseBlock());
  return false; // don't delete the when!
}

/// Lower memory operations. A new memory is created for every leaf
/// element in a memory's data type.
bool TypeLoweringVisitor::visitDecl(MemOp op) {
  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;

  // MemOp should have ground types so we can't preserve aggregates.
  if (!peelType(op.getDataType(), fields, PreserveAggregate::None))
    return false;

  SmallVector<MemOp> newMemories;
  SmallVector<WireOp> oldPorts;

  // Wires for old ports
  for (unsigned int index = 0, end = op.getNumResults(); index < end; ++index) {
    auto result = op.getResult(index);
    if (op.getPortKind(index) == MemOp::PortKind::Debug) {
      op.emitOpError("cannot lower memory with debug port");
      return false;
    }
    auto wire = builder->create<WireOp>(
        result.getType(),
        (op.getName() + "_" + op.getPortName(index).getValue()).str());
    oldPorts.push_back(wire);
    result.replaceAllUsesWith(wire.getResult());
  }
  // If annotations targeting fields of an aggregate are present, we cannot
  // flatten the memory. It must be split into one memory per aggregate field.
  // Do not overwrite the pass flag!

  // Memory for each field
  for (const auto &field : fields)
    newMemories.push_back(cloneMemWithNewType(builder, op, field));
  // Hook up the new memories to the wires the old memory was replaced with.
  for (size_t index = 0, rend = op.getNumResults(); index < rend; ++index) {
    auto result = oldPorts[index];
    auto rType = result.getType().cast<BundleType>();
    for (size_t fieldIndex = 0, fend = rType.getNumElements();
         fieldIndex != fend; ++fieldIndex) {
      auto name = rType.getElement(fieldIndex).name.getValue();
      auto oldField = builder->create<SubfieldOp>(result, fieldIndex);
      // data and mask depend on the memory type which was split.  They can also
      // go both directions, depending on the port direction.
      if (name == "data" || name == "mask" || name == "wdata" ||
          name == "wmask" || name == "rdata") {
        for (const auto &field : fields) {
          auto realOldField = getSubWhatever(oldField, field.index);
          auto newField = getSubWhatever(
              newMemories[field.index].getResult(index), fieldIndex);
          if (rType.getElement(fieldIndex).isFlip)
            std::swap(realOldField, newField);
          emitConnect(*builder, newField, realOldField);
        }
      } else {
        for (auto mem : newMemories) {
          auto newField =
              builder->create<SubfieldOp>(mem.getResult(index), fieldIndex);
          emitConnect(*builder, newField, oldField);
        }
      }
    }
  }
  return true;
}

bool TypeLoweringVisitor::visitDecl(FExtModuleOp extModule) {
  ImplicitLocOpBuilder theBuilder(extModule.getLoc(), context);
  builder = &theBuilder;

  // Top level builder
  OpBuilder builder(context);

  // Lower the module block arguments.
  SmallVector<unsigned> argsToRemove;
  auto newArgs = extModule.getPorts();
  for (size_t argIndex = 0, argsRemoved = 0; argIndex < newArgs.size();
       ++argIndex) {
    SmallVector<Value> lowering;
    if (lowerArg(extModule, argIndex, argsRemoved, newArgs, lowering)) {
      argsToRemove.push_back(argIndex);
      ++argsRemoved;
    }
    // lowerArg might have invalidated any reference to newArgs, be careful
  }

  // Remove block args that have been lowered
  for (auto ii = argsToRemove.rbegin(), ee = argsToRemove.rend(); ii != ee;
       ++ii)
    newArgs.erase(newArgs.begin() + *ii);

  SmallVector<NamedAttribute, 8> newModuleAttrs;

  // Copy over any attributes that weren't original argument attributes.
  for (auto attr : extModule->getAttrDictionary())
    // Drop old "portNames", directions, and argument attributes.  These are
    // handled differently below.
    if (attr.getName() != "portDirections" && attr.getName() != "portNames" &&
        attr.getName() != "portTypes" && attr.getName() != "portAnnotations" &&
        attr.getName() != "portSyms")
      newModuleAttrs.push_back(attr);

  SmallVector<Direction> newArgDirections;
  SmallVector<Attribute> newArgNames;
  SmallVector<Attribute, 8> newPortTypes;
  SmallVector<Attribute, 8> newArgSyms;
  SmallVector<Attribute, 8> newArgAnnotations;

  for (auto &port : newArgs) {
    newArgDirections.push_back(port.direction);
    newArgNames.push_back(port.name);
    newPortTypes.push_back(TypeAttr::get(port.type));
    newArgSyms.push_back(port.sym);
    newArgAnnotations.push_back(port.annotations.getArrayAttr());
  }

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortDirections,
                     direction::packAttribute(context, newArgDirections)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortNames, builder.getArrayAttr(newArgNames)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortTypes, builder.getArrayAttr(newPortTypes)));

  newModuleAttrs.push_back(NamedAttribute(
      cache.sPortAnnotations, builder.getArrayAttr(newArgAnnotations)));

  // Update the module's attributes.
  extModule->setAttrs(newModuleAttrs);
  extModule.setPortSymbols(newArgSyms);
  return false;
}

bool TypeLoweringVisitor::visitDecl(FModuleOp module) {
  auto *body = module.getBodyBlock();

  ImplicitLocOpBuilder theBuilder(module.getLoc(), context);
  builder = &theBuilder;

  // Lower the operations.
  lowerBlock(body);

  // Lower the module block arguments.
  llvm::BitVector argsToRemove;
  auto newArgs = module.getPorts();
  for (size_t argIndex = 0, argsRemoved = 0; argIndex < newArgs.size();
       ++argIndex) {
    SmallVector<Value> lowerings;
    if (lowerArg(module, argIndex, argsRemoved, newArgs, lowerings)) {
      auto arg = module.getArgument(argIndex);
      processUsers(arg, lowerings);
      argsToRemove.push_back(true);
      ++argsRemoved;
    } else
      argsToRemove.push_back(false);
    // lowerArg might have invalidated any reference to newArgs, be careful
  }

  // Remove block args that have been lowered.
  body->eraseArguments(argsToRemove);
  for (auto deadArg = argsToRemove.find_last(); deadArg != -1;
       deadArg = argsToRemove.find_prev(deadArg))
    newArgs.erase(newArgs.begin() + deadArg);

  SmallVector<NamedAttribute, 8> newModuleAttrs;

  // Copy over any attributes that weren't original argument attributes.
  for (auto attr : module->getAttrDictionary())
    // Drop old "portNames", directions, and argument attributes.  These are
    // handled differently below.
    if (attr.getName() != "portNames" && attr.getName() != "portDirections" &&
        attr.getName() != "portTypes" && attr.getName() != "portAnnotations" &&
        attr.getName() != "portSyms")
      newModuleAttrs.push_back(attr);

  SmallVector<Direction> newArgDirections;
  SmallVector<Attribute> newArgNames;
  SmallVector<Attribute> newArgTypes;
  SmallVector<Attribute> newArgSyms;
  SmallVector<Attribute, 8> newArgAnnotations;
  for (auto &port : newArgs) {
    newArgDirections.push_back(port.direction);
    newArgNames.push_back(port.name);
    newArgTypes.push_back(TypeAttr::get(port.type));
    newArgSyms.push_back(port.sym);
    newArgAnnotations.push_back(port.annotations.getArrayAttr());
  }

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortDirections,
                     direction::packAttribute(context, newArgDirections)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortNames, builder->getArrayAttr(newArgNames)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortTypes, builder->getArrayAttr(newArgTypes)));
  newModuleAttrs.push_back(NamedAttribute(
      cache.sPortAnnotations, builder->getArrayAttr(newArgAnnotations)));

  // Update the module's attributes.
  module->setAttrs(newModuleAttrs);
  module.setPortSymbols(newArgSyms);
  return false;
}

/// Lower a wire op with a bundle to multiple non-bundled wires.
bool TypeLoweringVisitor::visitDecl(WireOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    return builder->create<WireOp>(field.type, "", NameKindEnum::DroppableName,
                                   attrs, StringAttr{});
  };
  auto handled = lowerProducer(op, clone);
  if (insertDebugInfo && !handled && !op->hasAttr("hw.debug.name")) {
    if (auto nameAttr = op->getAttr("name")) {
      op->setAttr("hw.debug.name", nameAttr);
    }
  }
  return handled;
}

/// Lower a reg op with a bundle to multiple non-bundled regs.
bool TypeLoweringVisitor::visitDecl(RegOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    return builder->create<RegOp>(field.type, op.getClockVal(), "",
                                  NameKindEnum::DroppableName, attrs,
                                  StringAttr{});
  };
  return lowerProducer(op, clone);
}

/// Lower a reg op with a bundle to multiple non-bundled regs.
bool TypeLoweringVisitor::visitDecl(RegResetOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    auto resetVal = getSubWhatever(op.getResetValue(), field.index);
    return builder->create<RegResetOp>(
        field.type, op.getClockVal(), op.getResetSignal(), resetVal, "",
        NameKindEnum::DroppableName, attrs, StringAttr{});
  };
  auto handled = lowerProducer(op, clone);
  if (insertDebugInfo && !handled) {
    // not a bundle type. op not changed
    if (auto name = op->getAttr("name")) {
      op->setAttr("hw.debug.name", name);
    }
  }
  return handled;
}

/// Lower a wire op with a bundle to multiple non-bundled wires.
bool TypeLoweringVisitor::visitDecl(NodeOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    auto input = getSubWhatever(op.getInput(), field.index);
    return builder->create<NodeOp>(field.type, input, "",
                                   NameKindEnum::DroppableName, attrs,
                                   StringAttr{});
  };
  return lowerProducer(op, clone);
}

/// Lower an InvalidValue op with a bundle to multiple non-bundled InvalidOps.
bool TypeLoweringVisitor::visitExpr(InvalidValueOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    return builder->create<InvalidValueOp>(field.type);
  };
  return lowerProducer(op, clone);
}

// Expand muxes of aggregates
bool TypeLoweringVisitor::visitExpr(MuxPrimOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    auto high = getSubWhatever(op.getHigh(), field.index);
    auto low = getSubWhatever(op.getLow(), field.index);
    return builder->create<MuxPrimOp>(op.getSel(), high, low);
  };
  return lowerProducer(op, clone);
}

// Expand UnrealizedConversionCastOp of aggregates
bool TypeLoweringVisitor::visitExpr(mlir::UnrealizedConversionCastOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    auto input = getSubWhatever(op.getOperand(0), field.index);
    return builder->create<mlir::UnrealizedConversionCastOp>(field.type, input)
        .getResult(0);
  };
  return lowerProducer(op, clone);
}

// Expand BitCastOp of aggregates
bool TypeLoweringVisitor::visitExpr(BitCastOp op) {
  Value srcLoweredVal = op.getInput();
  // If the input is of aggregate type, then cat all the leaf fields to form a
  // UInt type result. That is, first bitcast the aggregate type to a UInt.
  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;
  if (peelType(op.getInput().getType(), fields, PreserveAggregate::None)) {
    size_t uptoBits = 0;
    // Loop over the leaf aggregates and concat each of them to get a UInt.
    // Bitcast the fields to handle nested aggregate types.
    for (const auto &field : llvm::enumerate(fields)) {
      auto fieldBitwidth = getBitWidth(field.value().type).value();
      // Ignore zero width fields, like empty bundles.
      if (fieldBitwidth == 0)
        continue;
      Value src = getSubWhatever(op.getInput(), field.index());
      // The src could be an aggregate type, bitcast it to a UInt type.
      src = builder->createOrFold<BitCastOp>(
          UIntType::get(context, fieldBitwidth), src);
      // Take the first field, or else Cat the previous fields with this field.
      if (uptoBits == 0)
        srcLoweredVal = src;
      else
        srcLoweredVal = builder->create<CatPrimOp>(src, srcLoweredVal);
      // Record the total bits already accumulated.
      uptoBits += fieldBitwidth;
    }
  } else {
    srcLoweredVal = builder->createOrFold<AsUIntPrimOp>(srcLoweredVal);
  }
  // Now the input has been cast to srcLoweredVal, which is of UInt type.
  // If the result is an aggregate type, then use lowerProducer.
  if (op.getResult().getType().isa<BundleType, FVectorType>()) {
    // uptoBits is used to keep track of the bits that have been extracted.
    size_t uptoBits = 0;
    auto clone = [&](const FlatBundleFieldEntry &field,
                     ArrayAttr attrs) -> Value {
      // All the fields must have valid bitwidth, a requirement for BitCastOp.
      auto fieldBits = getBitWidth(field.type).value();
      // If empty field, then it doesnot have any use, so replace it with an
      // invalid op, which should be trivially removed.
      if (fieldBits == 0)
        return builder->create<InvalidValueOp>(field.type);

      // Assign the field to the corresponding bits from the input.
      // Bitcast the field, incase its an aggregate type.
      auto extractBits = builder->create<BitsPrimOp>(
          srcLoweredVal, uptoBits + fieldBits - 1, uptoBits);
      uptoBits += fieldBits;
      return builder->create<BitCastOp>(field.type, extractBits);
    };
    return lowerProducer(op, clone);
  }

  // If ground type, then replace the result.
  if (op.getType().dyn_cast<SIntType>())
    srcLoweredVal = builder->create<AsSIntPrimOp>(srcLoweredVal);
  op.getResult().replaceAllUsesWith(srcLoweredVal);
  return true;
}

bool TypeLoweringVisitor::visitExpr(RefSendOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    return builder->create<RefSendOp>(
        getSubWhatever(op.getBase(), field.index));
  };
  return lowerProducer(op, clone);
}

bool TypeLoweringVisitor::visitExpr(RefResolveOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    Value src = getSubWhatever(op.getRef(), field.index);
    return builder->create<RefResolveOp>(src);
  };
  return lowerProducer(op, clone);
}

bool TypeLoweringVisitor::visitDecl(InstanceOp op) {
  bool skip = true;
  SmallVector<Type, 8> resultTypes;
  SmallVector<int64_t, 8> endFields; // Compressed sparse row encoding
  auto oldPortAnno = op.getPortAnnotations();
  SmallVector<Direction> newDirs;
  SmallVector<Attribute> newNames;
  SmallVector<Attribute> newPortAnno;
  PreserveAggregate::PreserveMode mode =
      getPreservatinoModeForModule(op.getReferencedModule(symTbl));

  endFields.push_back(0);
  for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
    auto srcType = op.getType(i).cast<FIRRTLType>();

    // Flatten any nested bundle types the usual way.
    SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
    if (!peelType(srcType, fieldTypes, mode)) {
      newDirs.push_back(op.getPortDirection(i));
      newNames.push_back(op.getPortName(i));
      resultTypes.push_back(srcType);
      newPortAnno.push_back(oldPortAnno[i]);
    } else {
      skip = false;
      auto oldName = op.getPortNameStr(i);
      auto oldDir = op.getPortDirection(i);
      // Store the flat type for the new bundle type.
      for (const auto &field : fieldTypes) {
        newDirs.push_back(direction::get((unsigned)oldDir ^ field.isOutput));
        newNames.push_back(builder->getStringAttr(oldName + field.suffix));
        resultTypes.push_back(srcType.isa<RefType>()
                                  ? FIRRTLType(RefType::get(field.type))
                                  : FIRRTLType(field.type));
        auto annos = filterAnnotations(
            context, oldPortAnno[i].dyn_cast_or_null<ArrayAttr>(), srcType,
            field);
        newPortAnno.push_back(annos);
      }
    }
    endFields.push_back(resultTypes.size());
  }

  auto sym = getInnerSymName(op);

  if (skip) {
    return false;
  }

  // FIXME: annotation update
  auto newInstance = builder->create<InstanceOp>(
      resultTypes, op.getModuleNameAttr(), op.getNameAttr(),
      op.getNameKindAttr(), direction::packAttribute(context, newDirs),
      builder->getArrayAttr(newNames), op.getAnnotations(),
      builder->getArrayAttr(newPortAnno), op.getLowerToBindAttr(),
      sym ? hw::InnerSymAttr::get(sym) : hw::InnerSymAttr());

  SmallVector<Value> lowered;
  for (size_t aggIndex = 0, eAgg = op.getNumResults(); aggIndex != eAgg;
       ++aggIndex) {
    lowered.clear();
    for (size_t fieldIndex = endFields[aggIndex],
                eField = endFields[aggIndex + 1];
         fieldIndex < eField; ++fieldIndex)
      lowered.push_back(newInstance.getResult(fieldIndex));
    if (lowered.size() != 1 ||
        op.getType(aggIndex) != resultTypes[endFields[aggIndex]])
      processUsers(op.getResult(aggIndex), lowered);
    else
      op.getResult(aggIndex).replaceAllUsesWith(lowered[0]);
  }
  return true;
}

bool TypeLoweringVisitor::visitExpr(SubaccessOp op) {
  auto input = op.getInput();
  auto vType = input.getType().cast<FVectorType>();

  // Check for empty vectors
  if (vType.getNumElements() == 0) {
    Value inv = builder->create<InvalidValueOp>(vType.getElementType());
    op.replaceAllUsesWith(inv);
    return true;
  }

  // Check for constant instances
  if (ConstantOp arg =
          dyn_cast_or_null<ConstantOp>(op.getIndex().getDefiningOp())) {
    auto sio = builder->create<SubindexOp>(op.getInput(),
                                           arg.getValue().getExtValue());
    op.replaceAllUsesWith(sio.getResult());
    return true;
  }

  // Construct a multibit mux
  SmallVector<Value> inputs;
  inputs.reserve(vType.getNumElements());
  for (int index = vType.getNumElements() - 1; index >= 0; index--)
    inputs.push_back(builder->create<SubindexOp>(input, index));

  Value multibitMux = builder->create<MultibitMuxOp>(op.getIndex(), inputs);
  op.replaceAllUsesWith(multibitMux);
  return true;
}

bool TypeLoweringVisitor::visitExpr(VectorCreateOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    return op.getOperand(field.index);
  };
  return lowerProducer(op, clone);
}

bool TypeLoweringVisitor::visitExpr(BundleCreateOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    return op.getOperand(field.index);
  };
  return lowerProducer(op, clone);
}

bool TypeLoweringVisitor::visitExpr(MultibitMuxOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Value {
    SmallVector<Value> newInputs;
    newInputs.reserve(op.getInputs().size());
    for (auto input : op.getInputs()) {
      auto inputSub = getSubWhatever(input, field.index);
      newInputs.push_back(inputSub);
    }
    return builder->create<MultibitMuxOp>(op.getIndex(), newInputs);
  };
  return lowerProducer(op, clone);
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
struct LowerTypesPass : public LowerFIRRTLTypesBase<LowerTypesPass> {
  LowerTypesPass(
      circt::firrtl::PreserveAggregate::PreserveMode preserveAggregateFlag,
      bool preservePublicTypesFlag, bool insertDebugInfoFlag) {
    preserveAggregate = preserveAggregateFlag;
    preservePublicTypes = preservePublicTypesFlag;
    insertDebugInfo = insertDebugInfoFlag;
  }
  void runOnOperation() override;
};
} // end anonymous namespace

// This is the main entrypoint for the lowering pass.
void LowerTypesPass::runOnOperation() {
  LLVM_DEBUG(
      llvm::dbgs() << "===- Running LowerTypes Pass "
                      "------------------------------------------------===\n");
  std::vector<FModuleLike> ops;
  // Symbol Table
  SymbolTable symTbl(getOperation());
  // Cached attr
  AttrCache cache(&getContext());

  // Record all operations in the circuit.
  llvm::for_each(getOperation().getBodyBlock()->getOperations(),
                 [&](Operation &op) {
                   // Creating a map of all ops in the circt, but only modules
                   // are relevant.
                   if (auto module = dyn_cast<FModuleLike>(op))
                     ops.push_back(module);
                 });

  LLVM_DEBUG(llvm::dbgs() << "Recording Inner Symbol Renames:\n");

  // This lambda, executes in parallel for each Op within the circt.
  auto lowerModules = [&](FModuleLike op) -> LogicalResult {
    auto tl = TypeLoweringVisitor(&getContext(), preserveAggregate,
                                  preservePublicTypes, symTbl, cache,
                                  insertDebugInfo);
    tl.lowerModule(op);

    return LogicalResult::failure(tl.isFailed());
  };

  auto result = failableParallelForEach(&getContext(), ops.begin(), ops.end(),
                                        lowerModules);
  if (failed(result))
    signalPassFailure();
}

/// This is the pass constructor.
std::unique_ptr<mlir::Pass>
circt::firrtl::createLowerFIRRTLTypesPass(PreserveAggregate::PreserveMode mode,
                                          bool preservePublicTypes,
                                          bool insertDebugInfo) {

  return std::make_unique<LowerTypesPass>(mode, preservePublicTypes,
                                          insertDebugInfo);
}
