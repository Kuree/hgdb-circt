//===- LowerToHW.cpp - FIRRTL to HW/SV Lowering Pass ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the main FIRRTL to HW/SV Lowering Pass Implementation.
//
//===----------------------------------------------------------------------===//

#include "../PassDetail.h"
#include "circt/Conversion/FIRRTLToHW.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/NLATable.h"
#include "circt/Dialect/FIRRTL/Namespace.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/HW/Namespace.h"
#include "circt/Dialect/SV/SVOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Support/BackedgeBuilder.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Threading.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Parallel.h"

using namespace circt;
using namespace firrtl;
using circt::comb::ICmpPredicate;
using hw::PortDirection;

/// Attribute that indicates that the module hierarchy starting at the
/// annotated module should be dumped to a file.
static const char moduleHierarchyFileAttrName[] = "firrtl.moduleHierarchyFile";

/// Given a type, return the corresponding lowered type for the HW dialect.
/// Non-FIRRTL types are simply passed through. This returns a null type if it
/// cannot be lowered.
static Type lowerType(Type type) {
  auto firType = type.dyn_cast<FIRRTLBaseType>();
  if (!firType)
    return type;

  // Ignore flip types.
  firType = firType.getPassiveType();

  if (BundleType bundle = firType.dyn_cast<BundleType>()) {
    mlir::SmallVector<hw::StructType::FieldInfo, 8> hwfields;
    for (auto element : bundle) {
      Type etype = lowerType(element.type);
      if (!etype)
        return {};
      hwfields.push_back(hw::StructType::FieldInfo{element.name, etype});
    }
    return hw::StructType::get(type.getContext(), hwfields);
  }
  if (FVectorType vec = firType.dyn_cast<FVectorType>()) {
    auto elemTy = lowerType(vec.getElementType());
    if (!elemTy)
      return {};
    return hw::ArrayType::get(elemTy, vec.getNumElements());
  }

  auto width = firType.getBitWidthOrSentinel();
  if (width >= 0) // IntType, analog with known width, clock, etc.
    return IntegerType::get(type.getContext(), width);

  return {};
}

/// Return true if the specified type is a sized FIRRTL type (Int or Analog)
/// with zero bits.
static bool isZeroBitFIRRTLType(Type type) {
  auto ftype = type.dyn_cast<FIRRTLBaseType>();
  return ftype && ftype.getPassiveType().getBitWidthOrSentinel() == 0;
}

// Return a single source value in the operands of the given attach op if
// exists.
static Value getSingleNonInstanceOperand(AttachOp op) {
  Value singleSource;
  for (auto operand : op.getAttached()) {
    if (isZeroBitFIRRTLType(operand.getType()) ||
        operand.getDefiningOp<InstanceOp>())
      continue;
    // If it is used by other than attach op or there is already a source
    // value, bail out.
    if (!operand.hasOneUse() || singleSource)
      return {};
    singleSource = operand;
  }
  return singleSource;
}

/// This verifies that the target operation has been lowered to a legal
/// operation.  This checks that the operation recursively has no FIRRTL
/// operations or types.
static LogicalResult verifyOpLegality(Operation *op) {
  auto checkTypes = [](Operation *op) -> WalkResult {
    // Check that this operation is not a FIRRTL op.
    if (isa_and_nonnull<FIRRTLDialect>(op->getDialect()))
      return op->emitError("Found unhandled FIRRTL operation '")
             << op->getName() << "'";

    // Helper to check a TypeRange for any FIRRTL types.
    auto checkTypeRange = [&](TypeRange types) -> LogicalResult {
      if (llvm::any_of(types, [](Type type) {
            return isa<FIRRTLDialect>(type.getDialect());
          }))
        return op->emitOpError("found unhandled FIRRTL type");
      return success();
    };

    // Check operand and result types.
    if (failed(checkTypeRange(op->getOperandTypes())) ||
        failed(checkTypeRange(op->getResultTypes())))
      return WalkResult::interrupt();

    // Check the block argument types.
    for (auto &region : op->getRegions())
      for (auto &block : region)
        if (failed(checkTypeRange(block.getArgumentTypes())))
          return WalkResult::interrupt();

    // Continue to the next operation.
    return WalkResult::advance();
  };

  if (checkTypes(op).wasInterrupted() || op->walk(checkTypes).wasInterrupted())
    return failure();
  return success();
}

/// Given two FIRRTL integer types, return the widest one.
static IntType getWidestIntType(Type t1, Type t2) {
  auto t1c = t1.cast<IntType>(), t2c = t2.cast<IntType>();
  return t2c.getWidth() > t1c.getWidth() ? t2c : t1c;
}

/// Cast a value to a desired target type. This will insert struct casts and
/// unrealized conversion casts as necessary.
static Value castToFIRRTLType(Value val, Type type,
                              ImplicitLocOpBuilder &builder) {
  // Use HWStructCastOp for a bundle type.
  if (BundleType bundle = type.dyn_cast<BundleType>())
    val = builder.createOrFold<HWStructCastOp>(bundle.getPassiveType(), val);

  if (type != val.getType())
    val = builder.create<mlir::UnrealizedConversionCastOp>(type, val).getResult(
        0);

  return val;
}

/// Cast from a FIRRTL type (potentially with a flip) to a standard type.
static Value castFromFIRRTLType(Value val, Type type,
                                ImplicitLocOpBuilder &builder) {

  if (hw::StructType structTy = type.dyn_cast<hw::StructType>()) {
    // Strip off Flip type if needed.
    val = builder
              .create<mlir::UnrealizedConversionCastOp>(
                  val.getType().cast<FIRRTLBaseType>().getPassiveType(), val)
              .getResult(0);
    val = builder.createOrFold<HWStructCastOp>(type, val);
    return val;
  }

  val =
      builder.create<mlir::UnrealizedConversionCastOp>(type, val).getResult(0);

  return val;
}

/// Move a ExtractTestCode related annotation from annotations to an attribute.
static void moveVerifAnno(ModuleOp top, AnnotationSet &annos,
                          StringRef annoClass, StringRef attrBase) {
  auto anno = annos.getAnnotation(annoClass);
  auto ctx = top.getContext();
  if (!anno)
    return;
  if (auto dir = anno.getMember<StringAttr>("directory")) {
    SmallVector<NamedAttribute> old;
    for (auto i : top->getAttrs())
      old.push_back(i);
    old.emplace_back(
        StringAttr::get(ctx, attrBase),
        hw::OutputFileAttr::getAsDirectory(ctx, dir.getValue(), true, true));
    top->setAttrs(old);
  }
  if (auto file = anno.getMember<StringAttr>("filename")) {
    SmallVector<NamedAttribute> old;
    for (auto i : top->getAttrs())
      old.push_back(i);
    old.emplace_back(StringAttr::get(ctx, attrBase + ".bindfile"),
                     hw::OutputFileAttr::getFromFilename(
                         ctx, file.getValue(), /*excludeFromFileList=*/true));
    top->setAttrs(old);
  }
}

static SmallVector<FirMemory>
mergeFIRRTLMemories(const SmallVector<FirMemory> &lhs,
                    SmallVector<FirMemory> rhs) {
  if (rhs.empty())
    return lhs;
  // lhs is always sorted and uniqued
  llvm::sort(rhs);
  rhs.erase(std::unique(rhs.begin(), rhs.end()), rhs.end());
  SmallVector<FirMemory> retval;
  std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                 std::back_inserter(retval));
  return retval;
}

static unsigned getBitWidthFromVectorSize(unsigned size) {
  return size == 1 ? 1 : llvm::Log2_64_Ceil(size);
}

// Try moving a name from an firrtl expression to a hw expression as a name
// hint.  Dont' overwrite an existing name.
static void tryCopyName(Operation *dst, Operation *src) {
  if (auto attr = src->getAttrOfType<StringAttr>("name"))
    if (!dst->hasAttr("sv.namehint") && !dst->hasAttr("name"))
      dst->setAttr("sv.namehint", attr);
}

//===----------------------------------------------------------------------===//
// firrtl.module Lowering Pass
//===----------------------------------------------------------------------===//
namespace {

struct FIRRTLModuleLowering;

/// This is state shared across the parallel module lowering logic.
struct CircuitLoweringState {
  std::atomic<bool> used_PRINTF_COND{false};
  std::atomic<bool> used_ASSERT_VERBOSE_COND{false};
  std::atomic<bool> used_STOP_COND{false};

  std::atomic<bool> used_RANDOMIZE_REG_INIT{false},
      used_RANDOMIZE_MEM_INIT{false};
  std::atomic<bool> used_RANDOMIZE_GARBAGE_ASSIGN{false};

  CircuitLoweringState(CircuitOp circuitOp, bool enableAnnotationWarning,
                       bool emitChiselAssertsAsSVA, bool stripMuxPragmas,
                       InstanceGraph *instanceGraph, NLATable *nlaTable)
      : circuitOp(circuitOp), instanceGraph(instanceGraph),
        enableAnnotationWarning(enableAnnotationWarning),
        emitChiselAssertsAsSVA(emitChiselAssertsAsSVA),
        stripMuxPragmas(stripMuxPragmas), nlaTable(nlaTable) {
    auto *context = circuitOp.getContext();

    // Get the testbench output directory.
    if (auto tbAnno =
            AnnotationSet(circuitOp).getAnnotation(testBenchDirAnnoClass)) {
      auto dirName = tbAnno.getMember<StringAttr>("dirname");
      testBenchDirectory = hw::OutputFileAttr::getAsDirectory(
          context, dirName.getValue(), false, true);
    }

    for (auto &op : *circuitOp.getBodyBlock()) {
      if (auto module = dyn_cast<FModuleLike>(op))
        if (AnnotationSet::removeAnnotations(module, dutAnnoClass))
          dut = module;
    }

    // Figure out which module is the DUT and TestHarness.  If there is no
    // module marked as the DUT, the top module is the DUT. If the DUT and the
    // test harness are the same, then there is no test harness.
    testHarness = instanceGraph->getTopLevelModule();
    if (!dut) {
      dut = testHarness;
      testHarness = nullptr;
    } else if (dut == testHarness) {
      testHarness = nullptr;
    }
  }

  Operation *getNewModule(Operation *oldModule) {
    auto it = oldToNewModuleMap.find(oldModule);
    return it != oldToNewModuleMap.end() ? it->second : nullptr;
  }

  // Process remaining annotations and emit warnings on unprocessed annotations
  // still remaining in the annoSet.
  void processRemainingAnnotations(Operation *op, const AnnotationSet &annoSet);

  CircuitOp circuitOp;

  // Safely add a BindOp to global mutable state.  This will acquire a lock to
  // do this safely.
  void addBind(sv::BindOp op) {
    std::lock_guard<std::mutex> lock(bindsMutex);
    binds.push_back(op);
  }

  FModuleLike getDut() { return dut; }
  FModuleLike getTestHarness() { return testHarness; }

  // Return true if this module is the DUT or is instantiated by the DUT.
  // Returns false if the module is not instantiated by the DUT.
  bool isInDUT(hw::HWModuleLike child) {
    if (auto parent = dyn_cast<hw::HWModuleLike>(*dut))
      return getInstanceGraph()->isAncestor(child, parent);
    return dut == child;
  }

  hw::OutputFileAttr getTestBenchDirectory() { return testBenchDirectory; }

  // Return true if this module is instantiated by the Test Harness.  Returns
  // false if the module is not instantiated by the Test Harness or if the Test
  // Harness is not known.
  bool isInTestHarness(hw::HWModuleLike mod) { return !isInDUT(mod); }

  InstanceGraph *getInstanceGraph() { return instanceGraph; }

  // Given the FirMemory name, return the generated op module name. This map
  // maintains the appropriate names for the deduped memories.
  StringAttr getGenOpMemName(StringAttr oldName) const {
    auto iter = memoryNameMap.find(oldName);
    assert(iter != memoryNameMap.end() && "Memory name map not found");
    return iter->getSecond();
  }

private:
  friend struct FIRRTLModuleLowering;
  friend struct FIRRTLLowering;
  CircuitLoweringState(const CircuitLoweringState &) = delete;
  void operator=(const CircuitLoweringState &) = delete;

  DenseMap<Operation *, Operation *> oldToNewModuleMap;

  /// Cache of module symbols.  We need to test hirarchy-based properties to
  /// lower annotaitons.
  InstanceGraph *instanceGraph;

  // Record the set of remaining annotation classes. This is used to warn only
  // once about any annotation class.
  StringSet<> pendingAnnotations;
  const bool enableAnnotationWarning;
  std::mutex annotationPrintingMtx;

  const bool emitChiselAssertsAsSVA;
  const bool stripMuxPragmas;

  // Records any sv::BindOps that are found during the course of execution.
  // This is unsafe to access directly and should only be used through addBind.
  SmallVector<sv::BindOp> binds;

  // Control access to binds.
  std::mutex bindsMutex;

  // The design-under-test (DUT), if it is found.  This will be set if a
  // "sifive.enterprise.firrtl.MarkDUTAnnotation" exists.
  FModuleLike dut;

  // If there is a module marked as the DUT and it is not the top level module,
  // this will be set.
  FModuleLike testHarness;

  // If there is a testbench output directory, this will be set.
  hw::OutputFileAttr testBenchDirectory;

  /// A mapping of instances to their forced instantiation names (if
  /// applicable).
  DenseMap<std::pair<Attribute, Attribute>, Attribute> instanceForceNames;

  /// Map of original FirMemory name to the Generated Op name. All the deduped
  /// memories have the same FirMemory name (because it is derived from the
  /// memory parameters), but the generated op name depends on the MemOp name,
  /// which is selected after dedup.
  DenseMap<StringAttr, StringAttr> memoryNameMap;

  /// Cached nla table analysis.
  NLATable *nlaTable = nullptr;
};

void CircuitLoweringState::processRemainingAnnotations(
    Operation *op, const AnnotationSet &annoSet) {
  if (!enableAnnotationWarning || annoSet.empty())
    return;
  std::lock_guard<std::mutex> lock(annotationPrintingMtx);

  for (auto a : annoSet) {
    auto inserted = pendingAnnotations.insert(a.getClass());
    if (!inserted.second)
      continue;

    // The following annotations are okay to be silently dropped at this point.
    // This can occur for example if an annotation marks something in the IR as
    // not to be processed by a pass, but that pass hasn't run anyway.
    if (a.isClass(
            // If the class is `circt.nonlocal`, it's not really an annotation,
            // but part of a path specifier for another annotation which is
            // non-local.  We can ignore these path specifiers since there will
            // be a warning produced for the real annotation.
            "circt.nonlocal",
            // The following are either consumed by a pass running before
            // LowerToHW, or they have no effect if the pass doesn't run at all.
            // If the accompanying pass runs on the HW dialect, then LowerToHW
            // should have consumed and processed these into an attribute on the
            // output.
            dontObfuscateModuleAnnoClass, noDedupAnnoClass,
            // The following are inspected (but not consumed) by FIRRTL/GCT
            // passes that have all run by now. Since no one is responsible for
            // consuming these, they will linger around and can be ignored.
            scalaClassAnnoClass, dutAnnoClass, metadataDirectoryAttrName,
            elaborationArtefactsDirectoryAnnoClass, testBenchDirAnnoClass,
            subCircuitsTargetDirectoryAnnoClass,
            // This annotation is used to mark which external modules are
            // imported blackboxes from the BlackBoxReader pass.
            blackBoxAnnoClass,
            // This annotation is used by several GrandCentral passes.
            extractGrandCentralClass,
            // The following will be handled while lowering the verification
            // ops.
            extractAssertAnnoClass, extractAssumeAnnoClass,
            extractCoverageAnnoClass,
            // The following will be handled after lowering FModule ops, since
            // they are still needed on the circuit until after lowering
            // FModules.
            moduleHierAnnoClass, testHarnessHierAnnoClass,
            blackBoxTargetDirAnnoClass))
      continue;

    mlir::emitWarning(op->getLoc(), "unprocessed annotation:'" + a.getClass() +
                                        "' still remaining after LowerToHW");
  }
}
} // end anonymous namespace

namespace {
struct FIRRTLModuleLowering : public LowerFIRRTLToHWBase<FIRRTLModuleLowering> {

  void runOnOperation() override;
  void setDisableMemRandomization() { disableMemRandomization = true; }
  void setDisableRegRandomization() { disableRegRandomization = true; }
  void setEnableAnnotationWarning() { enableAnnotationWarning = true; }
  void setEmitChiselAssertAsSVA() { emitChiselAssertsAsSVA = true; }
  void setStripMuxPragmas() { stripMuxPragmas = true; }

private:
  void lowerFileHeader(CircuitOp op, CircuitLoweringState &loweringState);
  LogicalResult lowerPorts(ArrayRef<PortInfo> firrtlPorts,
                           SmallVectorImpl<hw::PortInfo> &ports,
                           Operation *moduleOp,
                           CircuitLoweringState &loweringState);
  bool handleForceNameAnnos(FModuleLike oldModule, AnnotationSet &annos,
                            CircuitLoweringState &loweringState);
  hw::HWModuleOp lowerModule(FModuleOp oldModule, Block *topLevelModule,
                             CircuitLoweringState &loweringState);
  hw::HWModuleExternOp lowerExtModule(FExtModuleOp oldModule,
                                      Block *topLevelModule,
                                      CircuitLoweringState &loweringState);
  hw::HWModuleExternOp lowerMemModule(FMemModuleOp oldModule,
                                      Block *topLevelModule,
                                      CircuitLoweringState &loweringState);

  LogicalResult lowerModuleBody(FModuleOp oldModule,
                                CircuitLoweringState &loweringState);
  LogicalResult lowerModuleOperations(hw::HWModuleOp module,
                                      CircuitLoweringState &loweringState);

  void lowerMemoryDecls(ArrayRef<FirMemory> mems,
                        CircuitLoweringState &loweringState);
};

} // end anonymous namespace

/// This is the pass constructor.
std::unique_ptr<mlir::Pass> circt::createLowerFIRRTLToHWPass(
    bool enableAnnotationWarning, bool emitChiselAssertsAsSVA,
    bool stripMuxPragmas, bool disableMemRandomization,
    bool disableRegRandomization) {
  auto pass = std::make_unique<FIRRTLModuleLowering>();
  if (enableAnnotationWarning)
    pass->setEnableAnnotationWarning();
  if (emitChiselAssertsAsSVA)
    pass->setEmitChiselAssertAsSVA();
  if (stripMuxPragmas)
    pass->setStripMuxPragmas();
  if (disableMemRandomization)
    pass->setDisableMemRandomization();
  if (disableRegRandomization)
    pass->setDisableRegRandomization();
  return pass;
}

/// Run on the firrtl.circuit operation, lowering any firrtl.module operations
/// it contains.
void FIRRTLModuleLowering::runOnOperation() {

  // We run on the top level modules in the IR blob.  Start by finding the
  // firrtl.circuit within it.  If there is none, then there is nothing to do.
  auto *topLevelModule = getOperation().getBody();

  // Find the single firrtl.circuit in the module.
  CircuitOp circuit;
  for (auto &op : *topLevelModule) {
    if ((circuit = dyn_cast<CircuitOp>(&op)))
      break;
  }

  if (!circuit)
    return;

  auto *circuitBody = circuit.getBodyBlock();

  // Keep track of the mapping from old to new modules.  The result may be null
  // if lowering failed.
  CircuitLoweringState state(
      circuit, enableAnnotationWarning, emitChiselAssertsAsSVA, stripMuxPragmas,
      &getAnalysis<InstanceGraph>(), &getAnalysis<NLATable>());

  SmallVector<FModuleOp, 32> modulesToProcess;

  AnnotationSet circuitAnno(circuit);
  moveVerifAnno(getOperation(), circuitAnno, extractAssertAnnoClass,
                "firrtl.extract.assert");
  moveVerifAnno(getOperation(), circuitAnno, extractAssumeAnnoClass,
                "firrtl.extract.assume");
  moveVerifAnno(getOperation(), circuitAnno, extractCoverageAnnoClass,
                "firrtl.extract.cover");
  circuitAnno.removeAnnotationsWithClass(
      extractAssertAnnoClass, extractAssumeAnnoClass, extractCoverageAnnoClass);

  // Pass along the testbench directory for ExtractTestCode to use later.
  if (auto tbAnno = circuitAnno.getAnnotation(testBenchDirAnnoClass)) {
    auto dirName = tbAnno.getMember<StringAttr>("dirname");
    auto testBenchDir = hw::OutputFileAttr::getAsDirectory(
        &getContext(), dirName.getValue(), /*excludeFromFileList=*/true,
        /*includeReplicatedOps=*/true);
    getOperation()->setAttr("firrtl.extract.testbench", testBenchDir);
  }

  state.processRemainingAnnotations(circuit, circuitAnno);
  // Iterate through each operation in the circuit body, transforming any
  // FModule's we come across. If any module fails to lower, return early.
  for (auto &op : make_early_inc_range(circuitBody->getOperations())) {
    TypeSwitch<Operation *>(&op)
        .Case<FModuleOp>([&](auto module) {
          auto loweredMod = lowerModule(module, topLevelModule, state);
          if (!loweredMod)
            return signalPassFailure();

          state.oldToNewModuleMap[&op] = loweredMod;
          modulesToProcess.push_back(module);
        })
        .Case<FExtModuleOp>([&](auto extModule) {
          auto loweredMod = lowerExtModule(extModule, topLevelModule, state);
          if (!loweredMod)
            return signalPassFailure();
          state.oldToNewModuleMap[&op] = loweredMod;
        })
        .Case<FMemModuleOp>([&](auto memModule) {
          auto loweredMod = lowerMemModule(memModule, topLevelModule, state);
          if (!loweredMod)
            return signalPassFailure();
          state.oldToNewModuleMap[&op] = loweredMod;
        })
        .Case<HierPathOp>([&](auto nla) {
          // Just drop it.
        })
        .Default([&](Operation *op) {
          // We don't know what this op is.  If it has no illegal FIRRTL types,
          // we can forward the operation.  Otherwise, we emit an error and drop
          // the operation from the circuit.
          if (succeeded(verifyOpLegality(op)))
            op->moveBefore(topLevelModule, topLevelModule->end());
          else
            return signalPassFailure();
        });
  }
  // Handle the creation of the module hierarchy metadata.

  // Collect the two sets of hierarchy files from the circuit. Some of them will
  // be rooted at the test harness, the others will be rooted at the DUT.
  SmallVector<Attribute> dutHierarchyFiles;
  SmallVector<Attribute> testHarnessHierarchyFiles;
  circuitAnno.removeAnnotations([&](Annotation annotation) {
    if (annotation.isClass(moduleHierAnnoClass)) {
      auto file = hw::OutputFileAttr::getFromFilename(
          &getContext(),
          annotation.getMember<StringAttr>("filename").getValue(),
          /*excludeFromFileList=*/true);
      dutHierarchyFiles.push_back(file);
      return true;
    }
    if (annotation.isClass(testHarnessHierAnnoClass)) {
      auto file = hw::OutputFileAttr::getFromFilename(
          &getContext(),
          annotation.getMember<StringAttr>("filename").getValue(),
          /*excludeFromFileList=*/true);
      // If there is no testHarness, we print the hiearchy for this file
      // starting at the DUT.
      if (state.getTestHarness())
        testHarnessHierarchyFiles.push_back(file);
      else
        dutHierarchyFiles.push_back(file);
      return true;
    }
    return false;
  });
  // Attach the lowered form of these annotations.
  if (!dutHierarchyFiles.empty())
    state.oldToNewModuleMap[state.getDut()]->setAttr(
        moduleHierarchyFileAttrName,
        ArrayAttr::get(&getContext(), dutHierarchyFiles));
  if (!testHarnessHierarchyFiles.empty())
    state.oldToNewModuleMap[state.getTestHarness()]->setAttr(
        moduleHierarchyFileAttrName,
        ArrayAttr::get(&getContext(), testHarnessHierarchyFiles));

  // Collect all the memories in the module. Construct the FirMemory, set the
  // DUT flag and also record the Memop.
  auto collectFIRRTLMemories =
      [&state](FModuleOp module) -> SmallVector<FirMemory> {
    SmallVector<FirMemory> retval;
    // Check if this module is in the DUT hierarchy.
    bool isInDut = state.isInDUT(module);
    for (auto op : module.getBodyBlock()->getOps<MemOp>()) {
      auto sum = op.getSummary();
      sum.isInDut = isInDut;
      sum.op = op;
      retval.push_back(sum);
    }
    return retval;
  };

  // 1. First collect the memories as FirMemory, set the DUT flag and also
  // record the MemOp.
  // 2. Then Dedup the memories using the mergeFIRRTLMemories routine. This uses
  // the memory parameters to merge memories with the exactly same parameters.
  // 3. Then For each of the deduped memories, create the HWModuleGeneratedOp,
  // and assign a unique name to the wrapper module based on the MemOp name. It
  // is important to use the MemOp name because appropriate prefixes have to be
  // respected, also the correct output directory needs to be set based on the
  // DUT or testbench hierarchy.
  SmallVector<FirMemory> memories;
  if (getContext().isMultithreadingEnabled()) {
    memories = llvm::parallelTransformReduce(
        modulesToProcess.begin(), modulesToProcess.end(),
        SmallVector<FirMemory>(), mergeFIRRTLMemories, collectFIRRTLMemories);
  } else {
    for (auto m : modulesToProcess)
      memories = mergeFIRRTLMemories(memories, collectFIRRTLMemories(m));
  }
  if (!memories.empty())
    lowerMemoryDecls(memories, state);

  // Now that we've lowered all of the modules, move the bodies over and
  // update any instances that refer to the old modules.
  auto result = mlir::failableParallelForEachN(
      &getContext(), 0, modulesToProcess.size(), [&](auto index) {
        return lowerModuleBody(modulesToProcess[index], state);
      });

  // If any module bodies failed to lower, return early.
  if (failed(result))
    return signalPassFailure();

  // Move binds from inside modules to outside modules.
  for (auto bind : state.binds) {
    bind->moveBefore(bind->getParentOfType<hw::HWModuleOp>());
  }

  // Finally delete all the old modules.
  for (auto oldNew : state.oldToNewModuleMap)
    oldNew.first->erase();

  // Emit all the macros and preprocessor gunk at the start of the file.
  lowerFileHeader(circuit, state);

  // Now that the modules are moved over, remove the Circuit.
  circuit.erase();
}

void FIRRTLModuleLowering::lowerMemoryDecls(ArrayRef<FirMemory> mems,
                                            CircuitLoweringState &state) {
  assert(!mems.empty());
  auto namesp = CircuitNamespace(state.circuitOp);
  state.used_RANDOMIZE_MEM_INIT = 1;
  // Insert memories at the bottom of the file.
  OpBuilder b(state.circuitOp);
  b.setInsertionPointAfter(state.circuitOp);
  std::array<StringRef, 11> schemaFields = {
      "depth",          "numReadPorts",    "numWritePorts", "numReadWritePorts",
      "readLatency",    "writeLatency",    "width",         "maskGran",
      "readUnderWrite", "writeUnderWrite", "writeClockIDs"};
  auto schemaFieldsAttr = b.getStrArrayAttr(schemaFields);
  auto schema = b.create<hw::HWGeneratorSchemaOp>(
      mems.front().loc, "FIRRTLMem", "FIRRTL_Memory", schemaFieldsAttr);
  auto memorySchema = SymbolRefAttr::get(schema);

  Type b1Type = IntegerType::get(&getContext(), 1);

  for (auto &mem : mems) {
    SmallVector<hw::PortInfo> ports;
    size_t inputPin = 0;
    size_t outputPin = 0;
    // We don't need a single bit mask, it can be combined with enable. Create
    // an unmasked memory if maskBits = 1.

    auto makePortCommon = [&](StringRef prefix, size_t idx, Type bAddrType) {
      ports.push_back({b.getStringAttr(prefix + Twine(idx) + "_addr"),
                       PortDirection::INPUT, bAddrType, inputPin++});
      ports.push_back({b.getStringAttr(prefix + Twine(idx) + "_en"),
                       PortDirection::INPUT, b1Type, inputPin++});
      ports.push_back({b.getStringAttr(prefix + Twine(idx) + "_clk"),
                       PortDirection::INPUT, b1Type, inputPin++});
    };

    Type bDataType =
        IntegerType::get(&getContext(), std::max((size_t)1, mem.dataWidth));
    Type maskType = IntegerType::get(&getContext(), mem.maskBits);

    Type bAddrType = IntegerType::get(
        &getContext(), std::max(1U, llvm::Log2_64_Ceil(mem.depth)));

    for (size_t i = 0, e = mem.numReadPorts; i != e; ++i) {
      makePortCommon("R", i, bAddrType);
      ports.push_back({b.getStringAttr("R" + Twine(i) + "_data"),
                       PortDirection::OUTPUT, bDataType, outputPin++});
    }
    for (size_t i = 0, e = mem.numReadWritePorts; i != e; ++i) {
      makePortCommon("RW", i, bAddrType);
      ports.push_back({b.getStringAttr("RW" + Twine(i) + "_wmode"),
                       PortDirection::INPUT, b1Type, inputPin++});
      ports.push_back({b.getStringAttr("RW" + Twine(i) + "_wdata"),
                       PortDirection::INPUT, bDataType, inputPin++});
      ports.push_back({b.getStringAttr("RW" + Twine(i) + "_rdata"),
                       PortDirection::OUTPUT, bDataType, outputPin++});
      // Ignore mask port, if maskBits =1
      if (mem.isMasked)
        ports.push_back({b.getStringAttr("RW" + Twine(i) + "_wmask"),
                         PortDirection::INPUT, maskType, inputPin++});
    }

    for (size_t i = 0, e = mem.numWritePorts; i != e; ++i) {
      makePortCommon("W", i, bAddrType);
      ports.push_back({b.getStringAttr("W" + Twine(i) + "_data"),
                       PortDirection::INPUT, bDataType, inputPin++});
      // Ignore mask port, if maskBits =1
      if (mem.isMasked)
        ports.push_back({b.getStringAttr("W" + Twine(i) + "_mask"),
                         PortDirection::INPUT, maskType, inputPin++});
    }

    // Mask granularity is the number of data bits that each mask bit can
    // guard. By default it is equal to the data bitwidth.
    auto maskGran = mem.isMasked ? mem.dataWidth / mem.maskBits : mem.dataWidth;
    NamedAttribute genAttrs[] = {
        b.getNamedAttr("depth", b.getI64IntegerAttr(mem.depth)),
        b.getNamedAttr("numReadPorts", b.getUI32IntegerAttr(mem.numReadPorts)),
        b.getNamedAttr("numWritePorts",
                       b.getUI32IntegerAttr(mem.numWritePorts)),
        b.getNamedAttr("numReadWritePorts",
                       b.getUI32IntegerAttr(mem.numReadWritePorts)),
        b.getNamedAttr("readLatency", b.getUI32IntegerAttr(mem.readLatency)),
        b.getNamedAttr("writeLatency", b.getUI32IntegerAttr(mem.writeLatency)),
        b.getNamedAttr("width", b.getUI32IntegerAttr(mem.dataWidth)),
        b.getNamedAttr("maskGran", b.getUI32IntegerAttr(maskGran)),
        b.getNamedAttr("readUnderWrite",
                       b.getUI32IntegerAttr(mem.readUnderWrite)),
        b.getNamedAttr("writeUnderWrite",
                       hw::WUWAttr::get(b.getContext(), mem.writeUnderWrite)),
        b.getNamedAttr("writeClockIDs", b.getI32ArrayAttr(mem.writeClockIDs))};

    // Make the global module for the memory
    // Set a name for the memory wrapper module, the combMem is an arbitrary
    // suffix. It is important to derive the name from the original MemOp name,
    // to respect the corresponding prefixes..
    auto memoryName = b.getStringAttr(
        namesp.newName(static_cast<MemOp>(mem.op).getName() + "_combMem"));
    // Now record this generated name for the corresponding FirMemory name,
    // because all the memories that have the same FirMemory name, will now use
    // this new generated name at the Instance Op. Basically this is the name
    // used for all the deduped memories.
    state.memoryNameMap[mem.getFirMemoryName()] = memoryName;
    auto genOp = b.create<hw::HWModuleGeneratedOp>(
        mem.loc, memorySchema, memoryName, ports, StringRef(), ArrayAttr(),
        genAttrs);
    // Also set the appropriate directory.
    if (!mem.isInDut)
      if (auto testBenchDir = state.getTestBenchDirectory())
        genOp->setAttr("output_file", testBenchDir);
  }
}

/// Emit the file header that defines a bunch of macros.
void FIRRTLModuleLowering::lowerFileHeader(CircuitOp op,
                                           CircuitLoweringState &state) {
  // Intentionally pass an UnknownLoc here so we don't get line number
  // comments on the output of this boilerplate in generated Verilog.
  ImplicitLocOpBuilder b(UnknownLoc::get(&getContext()), op);

  // TODO: We could have an operation for macros and uses of them, and
  // even turn them into symbols so we can DCE unused macro definitions.
  auto emitString = [&](StringRef verilogString) {
    b.create<sv::VerbatimOp>(verilogString);
  };

  // Helper function to emit a "#ifdef guard" with a `define in the then and
  // optionally in the else branch.
  auto emitGuardedDefine = [&](const char *guard, const char *defineTrue,
                               const char *defineFalse = nullptr) {
    std::string define = "`define ";
    if (!defineFalse) {
      assert(defineTrue && "didn't define anything");
      b.create<sv::IfDefOp>(guard, [&]() { emitString(define + defineTrue); });
    } else {
      b.create<sv::IfDefOp>(
          guard,
          [&]() {
            if (defineTrue)
              emitString(define + defineTrue);
          },
          [&]() { emitString(define + defineFalse); });
    }
  };

  bool needsRandomizeRegInit =
      state.used_RANDOMIZE_REG_INIT && !disableRegRandomization;
  bool needsRandomizeMemInit =
      state.used_RANDOMIZE_MEM_INIT && !disableMemRandomization;

  // If none of the macros are needed, then don't emit any header at all, not
  // even the header comment.
  if (!state.used_RANDOMIZE_GARBAGE_ASSIGN && !needsRandomizeRegInit &&
      !needsRandomizeMemInit && !state.used_PRINTF_COND &&
      !state.used_ASSERT_VERBOSE_COND && !state.used_STOP_COND)
    return;

  emitString("// Standard header to adapt well known macros to our needs.");

  bool needRandom = false;
  if (state.used_RANDOMIZE_GARBAGE_ASSIGN) {
    emitGuardedDefine("RANDOMIZE_GARBAGE_ASSIGN", "RANDOMIZE");
    needRandom = true;
  }
  if (needsRandomizeRegInit) {
    emitGuardedDefine("RANDOMIZE_REG_INIT", "RANDOMIZE");
    needRandom = true;
  }
  if (needsRandomizeMemInit) {
    emitGuardedDefine("RANDOMIZE_MEM_INIT", "RANDOMIZE");
    needRandom = true;
  }

  if (needRandom) {
    emitString("\n// RANDOM may be set to an expression that produces a 32-bit "
               "random unsigned value.");
    emitGuardedDefine("RANDOM", nullptr, "RANDOM $random");
  }

  if (state.used_PRINTF_COND) {
    emitString("\n// Users can define 'PRINTF_COND' to add an extra gate to "
               "prints.");
    emitGuardedDefine("PRINTF_COND", "PRINTF_COND_ (`PRINTF_COND)",
                      "PRINTF_COND_ 1");
  }

  if (state.used_ASSERT_VERBOSE_COND) {
    emitString("\n// Users can define 'ASSERT_VERBOSE_COND' to add an extra "
               "gate to assert error printing.");
    emitGuardedDefine("ASSERT_VERBOSE_COND",
                      "ASSERT_VERBOSE_COND_ (`ASSERT_VERBOSE_COND)",
                      "ASSERT_VERBOSE_COND_ 1");
  }

  if (state.used_STOP_COND) {
    emitString("\n// Users can define 'STOP_COND' to add an extra gate "
               "to stop conditions.");
    emitGuardedDefine("STOP_COND", "STOP_COND_ (`STOP_COND)", "STOP_COND_ 1");
  }

  if (needRandom) {
    emitString("\n// Users can define INIT_RANDOM as general code that gets "
               "injected "
               "into the\n// initializer block for modules with registers.");
    emitGuardedDefine("INIT_RANDOM", nullptr, "INIT_RANDOM");

    emitString(
        "\n// If using random initialization, you can also define "
        "RANDOMIZE_DELAY to\n// customize the delay used, otherwise 0.002 "
        "is used.");
    emitGuardedDefine("RANDOMIZE_DELAY", nullptr, "RANDOMIZE_DELAY 0.002");

    emitString("\n// Define INIT_RANDOM_PROLOG_ for use in our modules below.");
    b.create<sv::IfDefOp>(
        "RANDOMIZE",
        [&]() {
          emitGuardedDefine(
              "VERILATOR", "INIT_RANDOM_PROLOG_ `INIT_RANDOM",
              "INIT_RANDOM_PROLOG_ `INIT_RANDOM #`RANDOMIZE_DELAY begin end");
        },
        [&]() { emitString("`define INIT_RANDOM_PROLOG_"); });
  }

  if (state.used_RANDOMIZE_GARBAGE_ASSIGN) {
    emitString("\n// RANDOMIZE_GARBAGE_ASSIGN enable range checks for mem "
               "assignments.");
    b.create<sv::IfDefOp>(
        "RANDOMIZE_GARBAGE_ASSIGN",
        [&]() {
          emitString(
              "`define RANDOMIZE_GARBAGE_ASSIGN_BOUND_CHECK(INDEX, VALUE, "
              "SIZE) \\");
          emitString("  ((INDEX) < (SIZE) ? (VALUE) : {`RANDOM})");
        },
        [&]() {
          emitString("`define RANDOMIZE_GARBAGE_ASSIGN_BOUND_CHECK(INDEX, "
                     "VALUE, SIZE) (VALUE)");
        });
  }

  // Blank line to separate the header from the modules.
  emitString("");
}

LogicalResult FIRRTLModuleLowering::lowerPorts(
    ArrayRef<PortInfo> firrtlPorts, SmallVectorImpl<hw::PortInfo> &ports,
    Operation *moduleOp, CircuitLoweringState &loweringState) {
  ports.reserve(firrtlPorts.size());
  size_t numArgs = 0;
  size_t numResults = 0;
  for (auto firrtlPort : firrtlPorts) {
    hw::PortInfo hwPort;
    hwPort.name = firrtlPort.name;
    hwPort.type = lowerType(firrtlPort.type);
    if (firrtlPort.sym)
      if (firrtlPort.sym.size() > 1 ||
          (firrtlPort.sym.size() == 1 && !firrtlPort.sym.getSymName())) {
        moduleOp->emitError("cannot lower aggregate port `" +
                            firrtlPort.name.getValue() +
                            "` with field sensitive symbols, HW dialect does  "
                            "not support per field symbols yet.");
        return failure();
      }
    hwPort.sym = firrtlPort.sym ? firrtlPort.sym.getSymName()
                                : StringAttr::get(moduleOp->getContext(), "");

    // Pass down the debug attr if any
    // Notice that hasAnnotation does not work since the debug attribute
    // is not inserted as a class
    for (auto anno : firrtlPort.annotations) {
      auto annoDict = anno.getDict();
      if (auto debugAttr = annoDict.get("hw.debug.name")) {
        hwPort.debugAttr = debugAttr.cast<StringAttr>();
        firrtlPort.annotations.removeAnnotation(anno);
        break;
      }
    }
    // We can't lower all types, so make sure to cleanly reject them.
    if (!hwPort.type) {
      moduleOp->emitError("cannot lower this port type to HW");
      return failure();
    }

    // If this is a zero bit port, just drop it.  It doesn't matter if it is
    // input, output, or inout.  We don't want these at the HW level.
    if (hwPort.type.isInteger(0)) {
      if (hwPort.sym && hwPort.sym.size()) {
        moduleOp->emitError("zero width port ")
            << hwPort.name << " is referenced by name [" << hwPort.sym
            << "] (e.g. in an XMR).";
        return failure();
      }
      continue;
    }

    // Figure out the direction of the port.
    if (firrtlPort.isOutput()) {
      hwPort.direction = hw::PortDirection::OUTPUT;
      hwPort.argNum = numResults++;
    } else if (firrtlPort.isInput()) {
      hwPort.direction = hw::PortDirection::INPUT;
      hwPort.argNum = numArgs++;
    } else {
      // If the port is an inout bundle or contains an analog type, then it is
      // implicitly inout.
      hwPort.type = hw::InOutType::get(hwPort.type);
      hwPort.direction = hw::PortDirection::INOUT;
      hwPort.argNum = numArgs++;
    }
    ports.push_back(hwPort);
    loweringState.processRemainingAnnotations(moduleOp, firrtlPort.annotations);
  }
  return success();
}

/// Map the parameter specifier on the specified extmodule into the HWModule
/// representation for parameters.  If `ignoreValues` is true, all the values
/// are dropped.
static ArrayAttr getHWParameters(FExtModuleOp module, bool ignoreValues) {
  auto params = llvm::map_range(module.getParameters(), [](Attribute a) {
    return a.cast<ParamDeclAttr>();
  });
  if (params.empty())
    return {};

  Builder builder(module);

  // Map the attributes over from firrtl attributes to HW attributes
  // directly.  MLIR's DictionaryAttr always stores keys in the dictionary
  // in sorted order which is nicely stable.
  SmallVector<Attribute> newParams;
  for (const ParamDeclAttr &entry : params) {
    auto name = entry.getName();
    auto type = entry.getType();
    auto value = ignoreValues ? Attribute() : entry.getValue();
    auto paramAttr =
        hw::ParamDeclAttr::get(builder.getContext(), name, type, value);
    newParams.push_back(paramAttr);
  }
  return builder.getArrayAttr(newParams);
}

bool FIRRTLModuleLowering::handleForceNameAnnos(
    FModuleLike oldModule, AnnotationSet &annos,
    CircuitLoweringState &loweringState) {
  bool failed = false;
  // Remove ForceNameAnnotations by generating verilogNames on instances.
  annos.removeAnnotations([&](Annotation anno) {
    if (!anno.isClass(forceNameAnnoClass))
      return false;

    auto sym = anno.getMember<FlatSymbolRefAttr>("circt.nonlocal");
    // This must be a non-local annotation due to how the Chisel API is
    // implemented.
    //
    // TODO: handle this in some sensible way based on what the SFC does with
    // a local annotation.
    if (!sym) {
      auto diag = oldModule.emitOpError()
                  << "contains a '" << forceNameAnnoClass
                  << "' that is not a non-local annotation";
      diag.attachNote() << "the erroneous annotation is '" << anno.getDict()
                        << "'\n";
      failed = true;
      return false;
    }

    auto nla = loweringState.nlaTable->getNLA(sym.getAttr());
    // The non-local anchor must exist.
    //
    // TODO: handle this with annotation verification.
    if (!nla) {
      auto diag = oldModule.emitOpError()
                  << "contains a '" << forceNameAnnoClass
                  << "' whose non-local symbol, '" << sym
                  << "' does not exist in the circuit";
      diag.attachNote() << "the erroneous annotation is '" << anno.getDict();
      failed = true;
      return false;
    }

    // Add the forced name to global state (keyed by a pseudo-inner name ref).
    // Error out if this key is alredy in use.
    //
    // TODO: this error behavior can be relaxed to always overwrite with the
    // new forced name (the bug-compatible behavior of the Chisel
    // implementation) or fixed to duplicate modules such that the naming can
    // be applied.
    auto inst =
        nla.getNamepath().getValue().take_back(2)[0].cast<hw::InnerRefAttr>();
    auto inserted = loweringState.instanceForceNames.insert(
        {{inst.getModule(), inst.getName()}, anno.getMember("name")});
    if (!inserted.second &&
        (anno.getMember("name") != (inserted.first->second))) {
      auto diag = oldModule.emitError()
                  << "contained multiple '" << forceNameAnnoClass
                  << "' with different names: " << inserted.first->second
                  << " was not " << anno.getMember("name");
      diag.attachNote() << "the erroneous annotation is '" << anno.getDict()
                        << "'";
      failed = true;
      return false;
    }
    return true;
  });
  return failed;
}

hw::HWModuleExternOp
FIRRTLModuleLowering::lowerExtModule(FExtModuleOp oldModule,
                                     Block *topLevelModule,
                                     CircuitLoweringState &loweringState) {
  // Map the ports over, lowering their types as we go.
  SmallVector<PortInfo> firrtlPorts = oldModule.getPorts();
  SmallVector<hw::PortInfo, 8> ports;
  if (failed(lowerPorts(firrtlPorts, ports, oldModule, loweringState)))
    return {};

  StringRef verilogName;
  if (auto defName = oldModule.getDefname())
    verilogName = defName.value();

  // Build the new hw.module op.
  auto builder = OpBuilder::atBlockEnd(topLevelModule);
  auto nameAttr = builder.getStringAttr(oldModule.getName());
  // Map over parameters if present.  Drop all values as we do so, so there are
  // no known default values in the extmodule.  This ensures that the
  // hw.instance will print all the parameters when generating verilog.
  auto parameters = getHWParameters(oldModule, /*ignoreValues=*/true);
  auto newModule = builder.create<hw::HWModuleExternOp>(
      oldModule.getLoc(), nameAttr, ports, verilogName, parameters);
  SymbolTable::setSymbolVisibility(newModule,
                                   SymbolTable::getSymbolVisibility(oldModule));

  bool hasOutputPort =
      llvm::any_of(firrtlPorts, [&](auto p) { return p.isOutput(); });
  if (!hasOutputPort &&
      AnnotationSet::removeAnnotations(oldModule, verifBlackBoxAnnoClass) &&
      loweringState.isInDUT(oldModule))
    newModule->setAttr("firrtl.extract.cover.extra", builder.getUnitAttr());

  AnnotationSet annos(oldModule);
  if (handleForceNameAnnos(oldModule, annos, loweringState))
    return {};

  loweringState.processRemainingAnnotations(oldModule, annos);
  return newModule;
}

hw::HWModuleExternOp
FIRRTLModuleLowering::lowerMemModule(FMemModuleOp oldModule,
                                     Block *topLevelModule,
                                     CircuitLoweringState &loweringState) {
  // Map the ports over, lowering their types as we go.
  SmallVector<PortInfo> firrtlPorts = oldModule.getPorts();
  SmallVector<hw::PortInfo, 8> ports;
  if (failed(lowerPorts(firrtlPorts, ports, oldModule, loweringState)))
    return {};

  // Build the new hw.module op.
  auto builder = OpBuilder::atBlockEnd(topLevelModule);
  auto newModule = builder.create<hw::HWModuleExternOp>(
      oldModule.getLoc(), oldModule.moduleNameAttr(), ports,
      oldModule.moduleNameAttr());
  loweringState.processRemainingAnnotations(oldModule,
                                            AnnotationSet(oldModule));
  return newModule;
}

/// Run on each firrtl.module, transforming it from an firrtl.module into an
/// hw.module, then deleting the old one.
hw::HWModuleOp
FIRRTLModuleLowering::lowerModule(FModuleOp oldModule, Block *topLevelModule,
                                  CircuitLoweringState &loweringState) {
  // Map the ports over, lowering their types as we go.
  SmallVector<PortInfo> firrtlPorts = oldModule.getPorts();
  SmallVector<hw::PortInfo, 8> ports;
  if (failed(lowerPorts(firrtlPorts, ports, oldModule, loweringState)))
    return {};

  // Build the new hw.module op.
  auto builder = OpBuilder::atBlockEnd(topLevelModule);
  auto nameAttr = builder.getStringAttr(oldModule.getName());
  auto newModule =
      builder.create<hw::HWModuleOp>(oldModule.getLoc(), nameAttr, ports);
  if (auto outputFile = oldModule->getAttr("output_file"))
    newModule->setAttr("output_file", outputFile);
  if (auto comment = oldModule->getAttrOfType<StringAttr>("comment"))
    newModule.setCommentAttr(comment);

  // Pass along the number of random initialization bits needed for this module.
  if (auto randomWidth = oldModule->getAttr("firrtl.random_init_width"))
    newModule->setAttr("firrtl.random_init_width", randomWidth);

  // If the circuit has an entry point, set all other modules private.
  // Otherwise, mark all modules as public.
  SymbolTable::setSymbolVisibility(newModule,
                                   SymbolTable::getSymbolVisibility(oldModule));

  // Transform module annotations
  AnnotationSet annos(oldModule);

  if (annos.removeAnnotation(verifBlackBoxAnnoClass))
    newModule->setAttr("firrtl.extract.cover.extra", builder.getUnitAttr());

  // If this is in the test harness, make sure it goes to the test directory.
  if (auto testBenchDir = loweringState.getTestBenchDirectory())
    if (loweringState.isInTestHarness(oldModule)) {
      newModule->setAttr("output_file", testBenchDir);
      newModule->setAttr("firrtl.extract.do_not_extract",
                         builder.getUnitAttr());
      newModule.setCommentAttr(
          builder.getStringAttr("VCS coverage exclude_file"));
    }

  if (handleForceNameAnnos(oldModule, annos, loweringState))
    return {};
  loweringState.processRemainingAnnotations(oldModule, annos);
  return newModule;
}

/// Given a value of analog type, check to see the only use of it is an
/// attach. If so, remove the attach and return the value being attached to
/// it, converted to an HW inout type.  If this isn't a situation we can
/// handle, just return null.
static Value tryEliminatingAttachesToAnalogValue(Value value,
                                                 Operation *insertPoint) {
  if (!value.hasOneUse())
    return {};

  auto attach = dyn_cast<AttachOp>(*value.user_begin());
  if (!attach || attach.getNumOperands() != 2)
    return {};

  // Don't optimize zero bit analogs.
  auto loweredType = lowerType(value.getType());
  if (loweredType.isInteger(0))
    return {};

  // Check to see if the attached value dominates the insertion point.  If
  // not, just fail.
  auto attachedValue = attach.getOperand(attach.getOperand(0) == value);
  auto *op = attachedValue.getDefiningOp();
  if (op && op->getBlock() == insertPoint->getBlock() &&
      !op->isBeforeInBlock(insertPoint))
    return {};

  attach.erase();

  ImplicitLocOpBuilder builder(insertPoint->getLoc(), insertPoint);
  return castFromFIRRTLType(attachedValue, hw::InOutType::get(loweredType),
                            builder);
}

/// Given a value of flip type, check to see if all of the uses of it are
/// connects.  If so, remove the connects and return the value being connected
/// to it, converted to an HW type.  If this isn't a situation we can handle,
/// just return null.
///
/// This can happen when there are no connects to the value.  The 'mergePoint'
/// location is where a 'hw.merge' operation should be inserted if needed.
static Value tryEliminatingConnectsToValue(Value flipValue,
                                           Operation *insertPoint) {
  // Handle analog's separately.
  if (flipValue.getType().isa<AnalogType>())
    return tryEliminatingAttachesToAnalogValue(flipValue, insertPoint);

  Operation *connectOp = nullptr;
  for (auto &use : flipValue.getUses()) {
    // We only know how to deal with connects where this value is the
    // destination.
    if (use.getOperandNumber() != 0)
      return {};
    if (!isa<ConnectOp, StrictConnectOp>(use.getOwner()))
      return {};

    // We only support things with a single connect.
    if (connectOp)
      return {};
    connectOp = use.getOwner();
  }

  // We don't have an HW equivalent of "poison" so just don't special case
  // the case where there are no connects other uses of an output.
  if (!connectOp)
    return {}; // TODO: Emit an sv.constant here since it is unconnected.

  // Don't special case zero-bit results.
  auto loweredType = lowerType(flipValue.getType());
  if (loweredType.isInteger(0))
    return {};

  // Convert each connect into an extended version of its operand being
  // output.
  ImplicitLocOpBuilder builder(insertPoint->getLoc(), insertPoint);

  auto connectSrc = connectOp->getOperand(1);

  // Directly forward foreign types.
  if (!connectSrc.getType().isa<FIRRTLType>()) {
    connectOp->erase();
    return connectSrc;
  }

  if (auto *srcOp = connectSrc.getDefiningOp()) {
    if (srcOp->hasAttr("hw.debug.name")) {
      // If the source has debug attribute, we don't do any optimization
      // because otherwise we will lose this symbol
      auto str = srcOp->getAttr("hw.debug.name").cast<StringAttr>().str();
      return {};
    }
  }

  // Convert fliped sources to passive sources.
  if (!connectSrc.getType().cast<FIRRTLBaseType>().isPassive())
    connectSrc =
        builder
            .create<mlir::UnrealizedConversionCastOp>(
                connectSrc.getType().cast<FIRRTLBaseType>().getPassiveType(),
                connectSrc)
            .getResult(0);

  // We know it must be the destination operand due to the types, but the
  // source may not match the destination width.
  auto destTy = flipValue.getType().cast<FIRRTLBaseType>().getPassiveType();

  if (!destTy.isGround()) {
    // If types are not ground type and they don't match, we give up.
    if (destTy != connectSrc.getType().cast<FIRRTLType>())
      return {};
  } else if (destTy.getBitWidthOrSentinel() != connectSrc.getType()
                                                   .cast<FIRRTLBaseType>()
                                                   .getBitWidthOrSentinel()) {
    // The only type mismatchs we care about is due to integer width
    // differences.
    auto destWidth = destTy.getBitWidthOrSentinel();
    assert(destWidth != -1 && "must know integer widths");
    connectSrc = builder.createOrFold<PadPrimOp>(destTy, connectSrc, destWidth);
  }

  // Remove the connect and use its source as the value for the output.
  connectOp->erase();

  // Convert from FIRRTL type to builtin type.
  return castFromFIRRTLType(connectSrc, loweredType, builder);
}

static SmallVector<SubfieldOp> getAllFieldAccesses(Value structValue,
                                                   StringRef field) {
  SmallVector<SubfieldOp> accesses;
  for (auto op : structValue.getUsers()) {
    assert(isa<SubfieldOp>(op));
    auto fieldAccess = cast<SubfieldOp>(op);
    auto elemIndex =
        fieldAccess.getInput().getType().cast<BundleType>().getElementIndex(
            field);
    if (elemIndex && *elemIndex == fieldAccess.getFieldIndex())
      accesses.push_back(fieldAccess);
  }
  return accesses;
}

/// Now that we have the operations for the hw.module's corresponding to the
/// firrtl.module's, we can go through and move the bodies over, updating the
/// ports and instances.
LogicalResult
FIRRTLModuleLowering::lowerModuleBody(FModuleOp oldModule,
                                      CircuitLoweringState &loweringState) {
  auto newModule =
      dyn_cast_or_null<hw::HWModuleOp>(loweringState.getNewModule(oldModule));
  // Don't touch modules if we failed to lower ports.
  if (!newModule)
    return success();

  ImplicitLocOpBuilder bodyBuilder(oldModule.getLoc(), newModule.getBody());

  // Use a placeholder instruction be a cursor that indicates where we want to
  // move the new function body to.  This is important because we insert some
  // ops at the start of the function and some at the end, and the body is
  // currently empty to avoid iterator invalidation.
  auto cursor = bodyBuilder.create<hw::ConstantOp>(APInt(1, 1));
  bodyBuilder.setInsertionPoint(cursor);

  // Insert argument casts, and re-vector users in the old body to use them.
  SmallVector<PortInfo> ports = oldModule.getPorts();
  assert(oldModule.getBody().getNumArguments() == ports.size() &&
         "port count mismatch");

  size_t nextNewArg = 0;
  size_t firrtlArg = 0;
  SmallVector<Value, 4> outputs;

  // This is the terminator in the new module.
  auto outputOp = newModule.getBodyBlock()->getTerminator();
  ImplicitLocOpBuilder outputBuilder(oldModule.getLoc(), outputOp);

  for (auto &port : ports) {
    // Inputs and outputs are both modeled as arguments in the FIRRTL level.
    auto oldArg = oldModule.getBody().getArgument(firrtlArg++);

    bool isZeroWidth =
        port.type.isa<FIRRTLBaseType>() &&
        port.type.cast<FIRRTLBaseType>().getBitWidthOrSentinel() == 0;

    if (!port.isOutput() && !isZeroWidth) {
      // Inputs and InOuts are modeled as arguments in the result, so we can
      // just map them over.  We model zero bit outputs as inouts.
      Value newArg = newModule.getBody().getArgument(nextNewArg++);

      // Cast the argument to the old type, reintroducing sign information in
      // the hw.module body.
      newArg = castToFIRRTLType(newArg, oldArg.getType(), bodyBuilder);
      // Switch all uses of the old operands to the new ones.
      oldArg.replaceAllUsesWith(newArg);
      continue;
    }

    // We lower zero width inout and outputs to a wire that isn't connected to
    // anything outside the module.  Inputs are lowered to zero.
    if (isZeroWidth && port.isInput()) {
      Value newArg = bodyBuilder.create<WireOp>(
          port.type, "." + port.getName().str() + ".0width_input");
      oldArg.replaceAllUsesWith(newArg);
      continue;
    }

    if (auto value = tryEliminatingConnectsToValue(oldArg, outputOp)) {
      // If we were able to find the value being connected to the output,
      // directly use it!
      outputs.push_back(value);
      assert(oldArg.use_empty() && "should have removed all uses of oldArg");
      continue;
    }

    // Outputs need a temporary wire so they can be connect'd to, which we
    // then return.
    Value newArg = bodyBuilder.create<WireOp>(
        port.type, "." + port.getName().str() + ".output");
    // Switch all uses of the old operands to the new ones.
    oldArg.replaceAllUsesWith(newArg);

    // Don't output zero bit results or inouts.
    auto resultHWType = lowerType(port.type);
    if (!resultHWType.isInteger(0)) {
      auto output = castFromFIRRTLType(newArg, resultHWType, outputBuilder);
      outputs.push_back(output);
    }
  }

  // Update the hw.output terminator with the list of outputs we have.
  outputOp->setOperands(outputs);

  // Finally splice the body over, don't move the old terminator over though.
  auto &oldBlockInstList = oldModule.getBodyBlock()->getOperations();
  auto &newBlockInstList = newModule.getBodyBlock()->getOperations();
  newBlockInstList.splice(Block::iterator(cursor), oldBlockInstList,
                          oldBlockInstList.begin(), oldBlockInstList.end());

  // We are done with our cursor op.
  cursor.erase();

  // Lower all of the other operations.
  return lowerModuleOperations(newModule, loweringState);
}

//===----------------------------------------------------------------------===//
// Module Body Lowering Pass
//===----------------------------------------------------------------------===//

namespace {

struct FIRRTLLowering : public FIRRTLVisitor<FIRRTLLowering, LogicalResult> {

  FIRRTLLowering(hw::HWModuleOp module, CircuitLoweringState &circuitState)
      : theModule(module), circuitState(circuitState),
        builder(module.getLoc(), module.getContext()),
        moduleNamespace(hw::ModuleNamespace(module)),
        backedgeBuilder(builder, module.getLoc()) {}

  LogicalResult run();

  void optimizeTemporaryWire(sv::WireOp wire);

  // Helpers.
  Value getOrCreateIntConstant(const APInt &value);
  Value getOrCreateIntConstant(unsigned numBits, uint64_t val,
                               bool isSigned = false) {
    return getOrCreateIntConstant(APInt(numBits, val, isSigned));
  }
  Value getOrCreateXConstant(unsigned numBits);
  Value getPossiblyInoutLoweredValue(Value value);
  Value getLoweredValue(Value value);
  Value getLoweredAndExtendedValue(Value value, Type destType);
  Value getLoweredAndExtOrTruncValue(Value value, Type destType);
  LogicalResult setLowering(Value orig, Value result);
  LogicalResult setPossiblyFoldedLowering(Value orig, Value result);
  template <typename ResultOpType, typename... CtorArgTypes>
  LogicalResult setLoweringTo(Operation *orig, CtorArgTypes... args);
  Backedge createBackedge(Value orig, Type type);
  bool updateIfBackedge(Value dest, Value src);

  void runWithInsertionPointAtEndOfBlock(std::function<void(void)> fn,
                                         Region &region);

  /// Return a read value for the specified inout value, auto-uniquing them.
  Value getReadValue(Value v);

  void addToAlwaysBlock(sv::EventControl clockEdge, Value clock,
                        ::ResetType resetStyle, sv::EventControl resetEdge,
                        Value reset, std::function<void(void)> body = {},
                        std::function<void(void)> resetBody = {});
  void addToAlwaysBlock(Value clock, std::function<void(void)> body = {}) {
    addToAlwaysBlock(sv::EventControl::AtPosEdge, clock, ::ResetType(),
                     sv::EventControl(), Value(), body,
                     std::function<void(void)>());
  }

  void addToIfDefBlock(StringRef cond, std::function<void(void)> thenCtor,
                       std::function<void(void)> elseCtor = {});
  void addToInitialBlock(std::function<void(void)> body);
  void addIfProceduralBlock(Value cond, std::function<void(void)> thenCtor,
                            std::function<void(void)> elseCtor = {});
  Value getExtOrTruncAggregateValue(Value array, FIRRTLBaseType sourceType,
                                    FIRRTLBaseType destType,
                                    bool allowTruncate);
  Value createArrayIndexing(Value array, Value index);

  // Create a temporary wire at the current insertion point, and try to
  // eliminate it later as part of lowering post processing.
  sv::WireOp createTmpWireOp(Type type, StringRef name) {
    // This is a locally visible, private wire created by the compiler, so do
    // not attach a symbol name.
    auto result = builder.create<sv::WireOp>(type, name);
    tmpWiresToOptimize.push_back(result);
    return result;
  }

  using FIRRTLVisitor<FIRRTLLowering, LogicalResult>::visitExpr;
  using FIRRTLVisitor<FIRRTLLowering, LogicalResult>::visitDecl;
  using FIRRTLVisitor<FIRRTLLowering, LogicalResult>::visitStmt;

  // Lowering hooks.
  enum UnloweredOpResult { AlreadyLowered, NowLowered, LoweringFailure };
  UnloweredOpResult handleUnloweredOp(Operation *op);
  LogicalResult visitExpr(ConstantOp op);
  LogicalResult visitExpr(SpecialConstantOp op);
  LogicalResult visitExpr(SubindexOp op);
  LogicalResult visitExpr(SubaccessOp op);
  LogicalResult visitExpr(SubfieldOp op);
  LogicalResult visitExpr(VectorCreateOp op);
  LogicalResult visitExpr(BundleCreateOp op);
  LogicalResult visitExpr(AggregateConstantOp op);
  LogicalResult visitUnhandledOp(Operation *op) { return failure(); }
  LogicalResult visitInvalidOp(Operation *op) { return failure(); }

  // Declarations.
  LogicalResult visitDecl(WireOp op);
  LogicalResult visitDecl(NodeOp op);
  LogicalResult visitDecl(RegOp op);
  LogicalResult visitDecl(RegResetOp op);
  LogicalResult visitDecl(MemOp op);
  LogicalResult visitDecl(InstanceOp op);
  LogicalResult visitDecl(VerbatimWireOp op);

  // Unary Ops.
  LogicalResult lowerNoopCast(Operation *op);
  LogicalResult visitExpr(AsSIntPrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsUIntPrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsClockPrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsAsyncResetPrimOp op) { return lowerNoopCast(op); }

  LogicalResult visitExpr(HWStructCastOp op);
  LogicalResult visitExpr(BitCastOp op);
  LogicalResult visitExpr(mlir::UnrealizedConversionCastOp op);
  LogicalResult visitExpr(CvtPrimOp op);
  LogicalResult visitExpr(NotPrimOp op);
  LogicalResult visitExpr(NegPrimOp op);
  LogicalResult visitExpr(PadPrimOp op);
  LogicalResult visitExpr(XorRPrimOp op);
  LogicalResult visitExpr(AndRPrimOp op);
  LogicalResult visitExpr(OrRPrimOp op);

  // Binary Ops.
  template <typename ResultUnsignedOpType,
            typename ResultSignedOpType = ResultUnsignedOpType>
  LogicalResult lowerBinOp(Operation *op);
  template <typename ResultOpType>
  LogicalResult lowerBinOpToVariadic(Operation *op);
  LogicalResult lowerCmpOp(Operation *op, ICmpPredicate signedOp,
                           ICmpPredicate unsignedOp);
  template <typename SignedOp, typename UnsignedOp>
  LogicalResult lowerDivLikeOp(Operation *op);

  LogicalResult visitExpr(CatPrimOp op);

  LogicalResult visitExpr(AndPrimOp op) {
    return lowerBinOpToVariadic<comb::AndOp>(op);
  }
  LogicalResult visitExpr(OrPrimOp op) {
    return lowerBinOpToVariadic<comb::OrOp>(op);
  }
  LogicalResult visitExpr(XorPrimOp op) {
    return lowerBinOpToVariadic<comb::XorOp>(op);
  }
  LogicalResult visitExpr(AddPrimOp op) {
    return lowerBinOpToVariadic<comb::AddOp>(op);
  }
  LogicalResult visitExpr(EQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::eq, ICmpPredicate::eq);
  }
  LogicalResult visitExpr(NEQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::ne, ICmpPredicate::ne);
  }
  LogicalResult visitExpr(LTPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::slt, ICmpPredicate::ult);
  }
  LogicalResult visitExpr(LEQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::sle, ICmpPredicate::ule);
  }
  LogicalResult visitExpr(GTPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::sgt, ICmpPredicate::ugt);
  }
  LogicalResult visitExpr(GEQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::sge, ICmpPredicate::uge);
  }

  LogicalResult visitExpr(SubPrimOp op) { return lowerBinOp<comb::SubOp>(op); }
  LogicalResult visitExpr(MulPrimOp op) {
    return lowerBinOpToVariadic<comb::MulOp>(op);
  }
  LogicalResult visitExpr(DivPrimOp op) {
    return lowerDivLikeOp<comb::DivSOp, comb::DivUOp>(op);
  }
  LogicalResult visitExpr(RemPrimOp op) {
    return lowerDivLikeOp<comb::ModSOp, comb::ModUOp>(op);
  }

  // Verif Operations
  LogicalResult visitExpr(IsXVerifOp op);

  // Other Operations
  LogicalResult visitExpr(BitsPrimOp op);
  LogicalResult visitExpr(InvalidValueOp op);
  LogicalResult visitExpr(HeadPrimOp op);
  LogicalResult visitExpr(ShlPrimOp op);
  LogicalResult visitExpr(ShrPrimOp op);
  LogicalResult visitExpr(DShlPrimOp op) {
    return lowerDivLikeOp<comb::ShlOp, comb::ShlOp>(op);
  }
  LogicalResult visitExpr(DShrPrimOp op) {
    return lowerDivLikeOp<comb::ShrSOp, comb::ShrUOp>(op);
  }
  LogicalResult visitExpr(DShlwPrimOp op) {
    return lowerDivLikeOp<comb::ShlOp, comb::ShlOp>(op);
  }
  LogicalResult visitExpr(TailPrimOp op);
  LogicalResult visitExpr(MuxPrimOp op);
  LogicalResult visitExpr(MultibitMuxOp op);
  LogicalResult visitExpr(VerbatimExprOp op);

  // Statements
  LogicalResult lowerVerificationStatement(
      Operation *op, StringRef labelPrefix, Value clock, Value predicate,
      Value enable, StringAttr messageAttr, ValueRange operands,
      StringAttr nameAttr, bool isConcurrent, EventControl eventControl);

  LogicalResult visitStmt(SkipOp op);

  void lowerRegConnect(const FieldRef &fieldRef, Value dest, Value srcVal);
  LogicalResult visitStmt(ConnectOp op);
  LogicalResult visitStmt(StrictConnectOp op);
  LogicalResult visitStmt(ForceOp op);
  LogicalResult visitStmt(PrintFOp op);
  LogicalResult visitStmt(StopOp op);
  LogicalResult visitStmt(AssertOp op);
  LogicalResult visitStmt(AssumeOp op);
  LogicalResult visitStmt(CoverOp op);
  LogicalResult visitStmt(AttachOp op);
  LogicalResult visitStmt(ProbeOp op);

private:
  /// The module we're lowering into.
  hw::HWModuleOp theModule;

  /// Global state.
  CircuitLoweringState &circuitState;

  /// This builder is set to the right location for each visit call.
  ImplicitLocOpBuilder builder;

  /// Each value lowered (e.g. operation result) is kept track in this map.
  /// The key should have a FIRRTL type, the result will have an HW dialect
  /// type.
  DenseMap<Value, Value> valueMapping;

  /// This keeps track of constants that we have created so we can reuse them.
  /// This is populated by the getOrCreateIntConstant method.
  DenseMap<Attribute, Value> hwConstantMap;

  /// This keeps track of constant X that we have created so we can reuse them.
  /// This is populated by the getOrCreateXConstant method.
  DenseMap<unsigned, Value> hwConstantXMap;

  /// We auto-unique "ReadInOut" ops from wires and regs, enabling
  /// optimizations and CSEs of the read values to be more obvious.  This
  /// caches a known ReadInOutOp for the given value and is managed by
  /// `getReadValue(v)`.
  DenseMap<Value, Value> readInOutCreated;

  // We auto-unique graph-level blocks to reduce the amount of generated
  // code and ensure that side effects are properly ordered in FIRRTL.
  using AlwaysKeyType = std::tuple<Block *, sv::EventControl, Value,
                                   ::ResetType, sv::EventControl, Value>;
  llvm::SmallDenseMap<AlwaysKeyType, std::pair<sv::AlwaysOp, sv::IfOp>>
      alwaysBlocks;
  llvm::SmallDenseMap<std::pair<Block *, Attribute>, sv::IfDefOp> ifdefBlocks;
  llvm::SmallDenseMap<Block *, sv::InitialOp> initialBlocks;

  /// This is a set of wires that get inserted as an artifact of the
  /// lowering process.  LowerToHW should attempt to clean these up after
  /// lowering.
  SmallVector<sv::WireOp> tmpWiresToOptimize;

  /// A namespace that can be used to generte new symbol names that are unique
  /// within this module.
  hw::ModuleNamespace moduleNamespace;

  /// A backedge builder to directly materialize values during the lowering
  /// without requiring temporary wires.
  BackedgeBuilder backedgeBuilder;
  /// Currently unresolved backedges. More precisely, a mapping from the
  /// backedge value to the value it will be replaced with. We use a MapVector
  /// so that a combinational cycles of backedges, the one backedge that gets
  /// replaced with an undriven wire is consistent.
  llvm::MapVector<Value, Value> backedges;

  /// Back-edges for register.  A mapping from the operation defining the
  /// FIRRTL register to the backedge which represents the next value, along
  /// with the value to which the register was lowered.
  DenseMap<Operation *, seq::FirRegOp> regMapping;
};
} // end anonymous namespace

LogicalResult FIRRTLModuleLowering::lowerModuleOperations(
    hw::HWModuleOp module, CircuitLoweringState &loweringState) {
  return FIRRTLLowering(module, loweringState).run();
}

// This is the main entrypoint for the lowering pass.
LogicalResult FIRRTLLowering::run() {
  // FIRRTL FModule is a single block because FIRRTL ops are a DAG.  Walk
  // through each operation, lowering each in turn if we can, introducing
  // casts if we cannot.
  auto &body = theModule.getBody();

  SmallVector<Operation *, 16> opsToRemove;

  // Iterate through each operation in the module body, attempting to lower
  // each of them.  We maintain 'builder' for each invocation.
  for (auto &op : body.front().getOperations()) {
    builder.setInsertionPoint(&op);
    builder.setLoc(op.getLoc());
    auto done = succeeded(dispatchVisitor(&op));
    circuitState.processRemainingAnnotations(&op, AnnotationSet(&op));
    if (done)
      opsToRemove.push_back(&op);
    else {
      switch (handleUnloweredOp(&op)) {
      case AlreadyLowered:
        break;         // Something like hw.output, which is already lowered.
      case NowLowered: // Something handleUnloweredOp removed.
        opsToRemove.push_back(&op);
        break;
      case LoweringFailure:
        backedgeBuilder.abandon();
        return failure();
      }
    }
  }

  // Replace all backedges with uses of their regular values.  We process them
  // after the module body since the lowering table is too hard to keep up to
  // date.  Multiple operations may be lowered to the same backedge when values
  // are folded, which means we would have to scan the entire lowering table to
  // safely replace a backedge.
  for (auto &[backedge, value] : backedges) {
    // In the case where we have backedges connected to other backedges, we have
    // to find the value that actually drives the group.
    while (true) {
      // If the we find the original backedge we have some undriven logic.
      if (backedge == value) {
        // Create a wire with no driver and use that as the backedge value.
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointAfterValue(backedge);
        value = builder.create<sv::WireOp>(backedge.getLoc(),
                                           backedge.getType(), "undriven");
        value = builder.createOrFold<sv::ReadInOutOp>(value);
        break;
      }
      // If the value is not another backedge, we have found the driver.
      auto it = backedges.find(value);
      if (it == backedges.end())
        break;
      // Find what is driving the next backedge.
      value = it->second;
    }
    backedge.replaceAllUsesWith(value);
  }

  // Now that all of the operations that can be lowered are, remove th
  // original values.  We know that any lowered operations will be dead (if
  // removed in reverse order) at this point - any users of them from
  // unremapped operations will be changed to use the newly lowered ops.
  while (!opsToRemove.empty()) {
    assert(opsToRemove.back()->use_empty() &&
           "Should remove ops in reverse order of visitation");
    opsToRemove.pop_back_val()->erase();
  }

  // Now that the IR is in a stable form, try to eliminate temporary wires
  // inserted by MemOp insertions.
  for (auto wire : tmpWiresToOptimize)
    optimizeTemporaryWire(wire);

  return backedgeBuilder.clearOrEmitError();
}

// Try to optimize out temporary wires introduced during lowering.
void FIRRTLLowering::optimizeTemporaryWire(sv::WireOp wire) {
  // Wires have inout type, so they'll have connects and read_inout operations
  // that work on them.  If anything unexpected is found then leave it alone.
  SmallVector<sv::ReadInOutOp> reads;
  sv::AssignOp write;

  // if the wire has debug attributes, skip it
  if (wire->hasAttr("hw.debug.name")) {
    return;
  }

  for (auto *user : wire->getUsers()) {
    if (auto read = dyn_cast<sv::ReadInOutOp>(user)) {
      reads.push_back(read);
      continue;
    }

    // Otherwise must be a connect, and we must not have seen a write yet.
    auto assign = dyn_cast<sv::AssignOp>(user);
    if (!assign || write)
      return;
    write = assign;
  }

  // Must have found the write!
  if (!write)
    return;

  // If the write is happening at the module level then we don't have any
  // use-before-def checking to do, so we only handle that for now.
  if (!isa<hw::HWModuleOp>(write->getParentOp()))
    return;

  auto connected = write.getSrc();

  // Ok, we can do this.  Replace all the reads with the connected value.
  for (auto read : reads) {
    read.replaceAllUsesWith(connected);
    read.erase();
  }
  // And remove the write and wire itself.
  write.erase();
  wire.erase();
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Check to see if we've already lowered the specified constant.  If so,
/// return it.  Otherwise create it and put it in the entry block for reuse.
Value FIRRTLLowering::getOrCreateIntConstant(const APInt &value) {
  auto attr = builder.getIntegerAttr(
      builder.getIntegerType(value.getBitWidth()), value);

  auto &entry = hwConstantMap[attr];
  if (entry)
    return entry;

  OpBuilder entryBuilder(&theModule.getBodyBlock()->front());
  entry = entryBuilder.create<hw::ConstantOp>(builder.getLoc(), attr);
  return entry;
}

/// Zero bit operands end up looking like failures from getLoweredValue.  This
/// helper function invokes the closure specified if the operand was actually
/// zero bit, or returns failure() if it was some other kind of failure.
static LogicalResult handleZeroBit(Value failedOperand,
                                   std::function<LogicalResult()> fn) {
  assert(failedOperand && "Should be called on the failed operand");
  if (!isZeroBitFIRRTLType(failedOperand.getType()))
    return failure();
  return fn();
}

/// Check to see if we've already lowered the specified constant.  If so,
/// return it.  Otherwise create it and put it in the entry block for reuse.
Value FIRRTLLowering::getOrCreateXConstant(unsigned numBits) {

  auto &entry = hwConstantXMap[numBits];
  if (entry)
    return entry;

  OpBuilder entryBuilder(&theModule.getBodyBlock()->front());
  entry = entryBuilder.create<sv::ConstantXOp>(
      builder.getLoc(), entryBuilder.getIntegerType(numBits));
  return entry;
}

/// Return the lowered HW value corresponding to the specified original value.
/// This returns a null value for FIRRTL values that haven't be lowered, e.g.
/// unknown width integers.  This returns hw::inout type values if present, it
/// does not implicitly read from them.
Value FIRRTLLowering::getPossiblyInoutLoweredValue(Value value) {
  // Block arguments are considered lowered.
  if (value.isa<BlockArgument>())
    return value;

  // If we lowered this value, then return the lowered value, otherwise fail.
  if (auto lowering = valueMapping.lookup(value)) {
    assert(!lowering.getType().isa<FIRRTLType>() &&
           "Lowered value should be a non-FIRRTL value");
    return lowering;
  }
  return Value();
}

/// Return the lowered value corresponding to the specified original value.
/// This returns a null value for FIRRTL values that cannot be lowered, e.g.
/// unknown width integers.
Value FIRRTLLowering::getLoweredValue(Value value) {
  auto result = getPossiblyInoutLoweredValue(value);
  if (!result)
    return result;

  // If we got an inout value, implicitly read it.  FIRRTL allows direct use
  // of wires and other things that lower to inout type.
  if (result.getType().isa<hw::InOutType>())
    return getReadValue(result);

  return result;
}

/// Return the lowered aggregate value whose type is converted into
/// `destType`. We have to care about the extension/truncation/signedness of
/// each element.
Value FIRRTLLowering::getExtOrTruncAggregateValue(Value array,
                                                  FIRRTLBaseType sourceType,
                                                  FIRRTLBaseType destType,
                                                  bool allowTruncate) {
  SmallVector<Value> resultBuffer;

  // Helper function to cast each element of array to dest type.
  auto cast = [&](Value value, FIRRTLBaseType sourceType,
                  FIRRTLBaseType destType) {
    auto srcWidth = sourceType.cast<IntType>().getWidthOrSentinel();
    auto destWidth = destType.cast<IntType>().getWidthOrSentinel();
    auto resultType = builder.getIntegerType(destWidth);

    if (srcWidth == destWidth)
      return value;

    if (srcWidth > destWidth) {
      if (allowTruncate)
        return builder.createOrFold<comb::ExtractOp>(resultType, value, 0);

      builder.emitError("operand should not be a truncation");
      return Value();
    }

    if (sourceType.cast<IntType>().isSigned())
      return comb::createOrFoldSExt(value, resultType, builder);
    auto zero = getOrCreateIntConstant(destWidth - srcWidth, 0);
    return builder.createOrFold<comb::ConcatOp>(zero, value);
  };

  // This recursive function constructs the output array.
  std::function<LogicalResult(Value, FIRRTLBaseType, FIRRTLBaseType)> recurse =
      [&](Value src, FIRRTLBaseType srcType,
          FIRRTLBaseType destType) -> LogicalResult {
    return TypeSwitch<FIRRTLBaseType, LogicalResult>(srcType)
        .Case<FVectorType>([&](auto srcVectorType) {
          auto destVectorType = destType.cast<FVectorType>();
          unsigned size = resultBuffer.size();
          unsigned indexWidth =
              getBitWidthFromVectorSize(srcVectorType.getNumElements());
          for (size_t i = 0, e = std::min(srcVectorType.getNumElements(),
                                          destVectorType.getNumElements());
               i != e; ++i) {
            auto iIdx = getOrCreateIntConstant(indexWidth, i);
            auto arrayIndex = builder.create<hw::ArrayGetOp>(src, iIdx);
            if (failed(recurse(arrayIndex, srcVectorType.getElementType(),
                               destVectorType.getElementType())))
              return failure();
          }
          SmallVector<Value> temp(resultBuffer.begin() + size,
                                  resultBuffer.end());
          auto array = builder.createOrFold<hw::ArrayCreateOp>(temp);
          resultBuffer.resize(size);
          resultBuffer.push_back(array);
          return success();
        })
        .Case<BundleType>([&](BundleType srcStructType) {
          auto destStructType = destType.cast<BundleType>();
          unsigned size = resultBuffer.size();

          // TODO: We don't support partial connects for bundles for now.
          if (destStructType.getNumElements() != srcStructType.getNumElements())
            return failure();

          for (auto elem : llvm::enumerate(destStructType)) {
            auto structExtract =
                builder.create<hw::StructExtractOp>(src, elem.value().name);
            if (failed(recurse(structExtract,
                               srcStructType.getElementType(elem.index()),
                               destStructType.getElementType(elem.index()))))
              return failure();
          }
          SmallVector<Value> temp(resultBuffer.begin() + size,
                                  resultBuffer.end());
          auto newStruct = builder.createOrFold<hw::StructCreateOp>(
              lowerType(destStructType), temp);
          resultBuffer.resize(size);
          resultBuffer.push_back(newStruct);
          return success();
        })
        .Case<IntType>([&](auto) {
          if (auto result = cast(src, srcType, destType)) {
            resultBuffer.push_back(result);
            return success();
          }
          return failure();
        })
        .Default([&](auto) { return failure(); });
  };

  if (failed(recurse(array, sourceType, destType)))
    return Value();

  assert(resultBuffer.size() == 1 &&
         "resultBuffer must only contain a result array if `success` is true");
  return resultBuffer[0];
}

/// Return the lowered value corresponding to the specified original value and
/// then extend it to match the width of destType if needed.
///
/// This returns a null value for FIRRTL values that cannot be lowered, e.g.
/// unknown width integers.
Value FIRRTLLowering::getLoweredAndExtendedValue(Value value, Type destType) {
  assert(value.getType().isa<FIRRTLBaseType>() &&
         destType.isa<FIRRTLBaseType>() &&
         "input/output value should be FIRRTL");

  // We only know how to extend integer types with known width.
  auto destWidth = destType.cast<FIRRTLBaseType>().getBitWidthOrSentinel();
  if (destWidth == -1)
    return {};

  auto result = getLoweredValue(value);
  if (!result) {
    // If this was a zero bit operand being extended, then produce a zero of
    // the right result type.  If it is just a failure, fail.
    if (!isZeroBitFIRRTLType(value.getType()))
      return {};
    // Zero bit results have to be returned as null.  The caller can handle
    // this if they want to.
    if (destWidth == 0)
      return {};
    // Otherwise, FIRRTL semantics is that an extension from a zero bit value
    // always produces a zero value in the destination width.
    return getOrCreateIntConstant(destWidth, 0);
  }

  // Aggregates values
  if (result.getType().isa<hw::ArrayType, hw::StructType>()) {
    // Types already match.
    if (destType == value.getType())
      return result;

    return getExtOrTruncAggregateValue(result,
                                       value.getType().cast<FIRRTLBaseType>(),
                                       destType.cast<FIRRTLBaseType>(),
                                       /* allowTruncate */ false);
  }

  auto srcWidth = result.getType().cast<IntegerType>().getWidth();
  if (srcWidth == unsigned(destWidth))
    return result;

  if (srcWidth > unsigned(destWidth)) {
    builder.emitError("operand should not be a truncation");
    return {};
  }

  auto resultType = builder.getIntegerType(destWidth);

  // Extension follows the sign of the source value, not the destination.
  auto valueFIRType = value.getType().cast<FIRRTLBaseType>().getPassiveType();
  if (valueFIRType.cast<IntType>().isSigned())
    return comb::createOrFoldSExt(result, resultType, builder);

  auto zero = getOrCreateIntConstant(destWidth - srcWidth, 0);
  return builder.createOrFold<comb::ConcatOp>(zero, result);
}

/// Return the lowered value corresponding to the specified original value and
/// then extended or truncated to match the width of destType if needed.
///
/// This returns a null value for FIRRTL values that cannot be lowered, e.g.
/// unknown width integers.
Value FIRRTLLowering::getLoweredAndExtOrTruncValue(Value value, Type destType) {
  assert(value.getType().isa<FIRRTLBaseType>() &&
         destType.isa<FIRRTLBaseType>() &&
         "input/output value should be FIRRTL");

  // We only know how to adjust integer types with known width.
  auto destWidth = destType.cast<FIRRTLBaseType>().getBitWidthOrSentinel();
  if (destWidth == -1)
    return {};

  auto result = getLoweredValue(value);
  if (!result) {
    // If this was a zero bit operand being extended, then produce a zero of
    // the right result type.  If it is just a failure, fail.
    if (!isZeroBitFIRRTLType(value.getType()))
      return {};
    // Zero bit results have to be returned as null.  The caller can handle
    // this if they want to.
    if (destWidth == 0)
      return {};
    // Otherwise, FIRRTL semantics is that an extension from a zero bit value
    // always produces a zero value in the destination width.
    return getOrCreateIntConstant(destWidth, 0);
  }

  // Aggregates values
  if (result.getType().isa<hw::ArrayType, hw::StructType>()) {
    // Types already match.
    if (destType == value.getType())
      return result;

    return getExtOrTruncAggregateValue(result,
                                       value.getType().cast<FIRRTLBaseType>(),
                                       destType.cast<FIRRTLBaseType>(),
                                       /* allowTruncate */ true);
  }

  auto srcWidth = result.getType().cast<IntegerType>().getWidth();
  if (srcWidth == unsigned(destWidth))
    return result;

  if (destWidth == 0)
    return {};

  if (srcWidth > unsigned(destWidth)) {
    auto resultType = builder.getIntegerType(destWidth);
    return builder.createOrFold<comb::ExtractOp>(resultType, result, 0);
  }

  auto resultType = builder.getIntegerType(destWidth);

  // Extension follows the sign of the source value, not the destination.
  auto valueFIRType = value.getType().cast<FIRRTLBaseType>().getPassiveType();
  if (valueFIRType.cast<IntType>().isSigned())
    return comb::createOrFoldSExt(result, resultType, builder);

  auto zero = getOrCreateIntConstant(destWidth - srcWidth, 0);
  return builder.createOrFold<comb::ConcatOp>(zero, result);
}

/// Set the lowered value of 'orig' to 'result', remembering this in a map.
/// This always returns success() to make it more convenient in lowering code.
///
/// Note that result may be null here if we're lowering orig to a zero-bit
/// value.
///
LogicalResult FIRRTLLowering::setLowering(Value orig, Value result) {
  if (auto origType = orig.getType().dyn_cast<FIRRTLType>()) {
    assert((!result || !result.getType().isa<FIRRTLType>()) &&
           "Lowering didn't turn a FIRRTL value into a non-FIRRTL value");

#ifndef NDEBUG
    // Only base types should reach this point.
    auto baseType = origType.cast<FIRRTLBaseType>();
    auto srcWidth = baseType.getPassiveType().getBitWidthOrSentinel();

    // Caller should pass null value iff this was a zero bit value.
    if (srcWidth != -1) {
      if (result)
        assert((srcWidth != 0) &&
               "Lowering produced value for zero width source");
      else
        assert((srcWidth == 0) &&
               "Lowering produced null value but source wasn't zero width");
    }
#endif
  } else {
    assert(result && "Lowering of foreign type produced null value");
  }

  auto &slot = valueMapping[orig];
  assert(!slot && "value lowered multiple times");
  slot = result;
  return success();
}

/// Set the lowering for a value to the specified result.  This came from a
/// possible folding, so check to see if we need to handle a constant.
LogicalResult FIRRTLLowering::setPossiblyFoldedLowering(Value orig,
                                                        Value result) {
  // If this is a constant, check to see if we have it in our unique mapping:
  // it could have come from folding an operation.
  if (auto cst = dyn_cast_or_null<hw::ConstantOp>(result.getDefiningOp())) {
    auto &entry = hwConstantMap[cst.getValueAttr()];
    if (entry == cst) {
      // We're already using an entry in the constant map, nothing to do.
    } else if (entry) {
      // We already had this constant, reuse the one we have instead of the
      // one we just folded.
      result = entry;
      cst->erase();
    } else {
      // This is a new constant.  Remember it!
      entry = cst;
      cst->moveBefore(&theModule.getBodyBlock()->front());
    }
  }

  return setLowering(orig, result);
}

/// Create a new operation with type ResultOpType and arguments CtorArgTypes,
/// then call setLowering with its result.
template <typename ResultOpType, typename... CtorArgTypes>
LogicalResult FIRRTLLowering::setLoweringTo(Operation *orig,
                                            CtorArgTypes... args) {
  auto result = builder.createOrFold<ResultOpType>(args...);
  if (auto debugAttr = orig->getAttr("hw.debug.name")) {
    auto *op = result.getDefiningOp();
    op->setAttr("hw.debug.name", debugAttr);
  }
  if (auto *op = result.getDefiningOp())
    tryCopyName(op, orig);
  return setPossiblyFoldedLowering(orig->getResult(0), result);
}

/// Sets the lowering for a value to a backedge of the specified result type.
/// This is useful for lowering types which cannot pass through a wire, or to
/// directly materialize values in operations that violate the SSA dominance
/// constraint.
Backedge FIRRTLLowering::createBackedge(Value orig, Type type) {
  auto backedge = backedgeBuilder.get(type, orig.getLoc());
  backedges.insert({backedge, backedge});
  (void)setLowering(orig, backedge);
  return backedge;
}

/// If the `from` value is in fact a backedge, record that the backedge will
/// be replaced by the value.  Return true if the destination is a backedge.
bool FIRRTLLowering::updateIfBackedge(Value dest, Value src) {
  auto backedgeIt = backedges.find(dest);
  if (backedgeIt == backedges.end())
    return false;
  assert(backedgeIt->first == backedgeIt->second && "backedge lowered twice");
  backedgeIt->second = src;
  return true;
}

/// Switch the insertion point of the current builder to the end of the
/// specified block and run the closure.  This correctly handles the case
/// where the closure is null, but the caller needs to make sure the block
/// exists.
void FIRRTLLowering::runWithInsertionPointAtEndOfBlock(
    std::function<void(void)> fn, Region &region) {
  if (!fn)
    return;

  auto oldIP = builder.saveInsertionPoint();

  builder.setInsertionPointToEnd(&region.front());
  fn();
  builder.restoreInsertionPoint(oldIP);
}

/// Return a read value for the specified inout operation, auto-uniquing them.
Value FIRRTLLowering::getReadValue(Value v) {
  Value result = readInOutCreated.lookup(v);
  if (result)
    return result;

  // Make sure to put the read value at the correct scope so it dominates all
  // future uses.
  auto oldIP = builder.saveInsertionPoint();
  if (auto *vOp = v.getDefiningOp()) {
    builder.setInsertionPointAfter(vOp);
  } else {
    // For reads of ports, just set the insertion point at the top of the
    // module.
    builder.setInsertionPoint(&theModule.getBodyBlock()->front());
  }

  // Instead of creating `ReadInOutOp` for `ArrayIndexInOutOp`, create
  // `ArrayGetOp` for root arrays.
  if (auto arrayIndexInout = v.getDefiningOp<sv::ArrayIndexInOutOp>()) {
    result = getReadValue(arrayIndexInout.getInput());
    result = builder.createOrFold<hw::ArrayGetOp>(result,
                                                  arrayIndexInout.getIndex());
  } else {
    // Otherwise, create a read inout operation.
    result = builder.createOrFold<sv::ReadInOutOp>(v);
  }
  if (auto *vOp = v.getDefiningOp()) {
    if (auto debugAttr = vOp->getAttr("hw.debug.name")) {
      result.getDefiningOp()->setAttr("hw.debug.name", debugAttr);
    }
  }
  builder.restoreInsertionPoint(oldIP);
  readInOutCreated.insert({v, result});
  return result;
}

void FIRRTLLowering::addToAlwaysBlock(sv::EventControl clockEdge, Value clock,
                                      ::ResetType resetStyle,
                                      sv::EventControl resetEdge, Value reset,
                                      std::function<void(void)> body,
                                      std::function<void(void)> resetBody) {
  AlwaysKeyType key{builder.getBlock(), clockEdge, clock,
                    resetStyle,         resetEdge, reset};
  sv::AlwaysOp alwaysOp;
  sv::IfOp insideIfOp;
  std::tie(alwaysOp, insideIfOp) = alwaysBlocks.lookup(key);

  if (!alwaysOp) {
    if (reset) {
      assert(resetStyle != ::ResetType::NoReset);
      // Here, we want to create the folloing structure with sv.always and
      // sv.if. If `reset` is async, we need to add `reset` to a sensitivity
      // list.
      //
      // sv.always @(clockEdge or reset) {
      //   sv.if (reset) {
      //     resetBody
      //   } else {
      //     body
      //   }
      // }

      auto createIfOp = [&]() {
        // It is weird but intended. Here we want to create an empty sv.if
        // with an else block.
        insideIfOp = builder.create<sv::IfOp>(
            reset, []() {}, []() {});
      };
      if (resetStyle == ::ResetType::AsyncReset) {
        sv::EventControl events[] = {clockEdge, resetEdge};
        Value clocks[] = {clock, reset};

        alwaysOp = builder.create<sv::AlwaysOp>(events, clocks, [&]() {
          if (resetEdge == sv::EventControl::AtNegEdge)
            llvm_unreachable("negative edge for reset is not expected");
          createIfOp();
        });
      } else {
        alwaysOp = builder.create<sv::AlwaysOp>(clockEdge, clock, createIfOp);
      }
    } else {
      assert(!resetBody);
      alwaysOp = builder.create<sv::AlwaysOp>(clockEdge, clock);
      insideIfOp = nullptr;
    }
    alwaysBlocks[key] = {alwaysOp, insideIfOp};
  }

  if (reset) {
    assert(insideIfOp && "reset body must be initialized before");
    runWithInsertionPointAtEndOfBlock(resetBody, insideIfOp.getThenRegion());
    runWithInsertionPointAtEndOfBlock(body, insideIfOp.getElseRegion());
  } else {
    runWithInsertionPointAtEndOfBlock(body, alwaysOp.getBody());
  }

  // Move the earlier always block(s) down to where the last would have been
  // inserted.  This ensures that any values used by the always blocks are
  // defined ahead of the uses, which leads to better generated Verilog.
  alwaysOp->moveBefore(builder.getInsertionBlock(),
                       builder.getInsertionPoint());
}

void FIRRTLLowering::addToIfDefBlock(StringRef cond,
                                     std::function<void(void)> thenCtor,
                                     std::function<void(void)> elseCtor) {
  auto condAttr = builder.getStringAttr(cond);
  auto op = ifdefBlocks.lookup({builder.getBlock(), condAttr});
  if (op) {
    runWithInsertionPointAtEndOfBlock(thenCtor, op.getThenRegion());
    runWithInsertionPointAtEndOfBlock(elseCtor, op.getElseRegion());

    // Move the earlier #ifdef block(s) down to where the last would have been
    // inserted.  This ensures that any values used by the #ifdef blocks are
    // defined ahead of the uses, which leads to better generated Verilog.
    op->moveBefore(builder.getInsertionBlock(), builder.getInsertionPoint());
  } else {
    ifdefBlocks[{builder.getBlock(), condAttr}] =
        builder.create<sv::IfDefOp>(condAttr, thenCtor, elseCtor);
  }
}

void FIRRTLLowering::addToInitialBlock(std::function<void(void)> body) {
  auto op = initialBlocks.lookup(builder.getBlock());
  if (op) {
    runWithInsertionPointAtEndOfBlock(body, op.getBody());

    // Move the earlier initial block(s) down to where the last would have
    // been inserted.  This ensures that any values used by the initial blocks
    // are defined ahead of the uses, which leads to better generated Verilog.
    op->moveBefore(builder.getInsertionBlock(), builder.getInsertionPoint());
  } else {
    initialBlocks[builder.getBlock()] = builder.create<sv::InitialOp>(body);
  }
}

void FIRRTLLowering::addIfProceduralBlock(Value cond,
                                          std::function<void(void)> thenCtor,
                                          std::function<void(void)> elseCtor) {
  // Check to see if we already have an if on this condition immediately
  // before the insertion point.  If so, extend it.
  auto insertIt = builder.getInsertionPoint();
  if (insertIt != builder.getBlock()->begin())
    if (auto ifOp = dyn_cast<sv::IfOp>(*--insertIt)) {
      if (ifOp.getCond() == cond) {
        runWithInsertionPointAtEndOfBlock(thenCtor, ifOp.getThenRegion());
        runWithInsertionPointAtEndOfBlock(elseCtor, ifOp.getElseRegion());
        return;
      }
    }

  builder.create<sv::IfOp>(cond, thenCtor, elseCtor);
}

//===----------------------------------------------------------------------===//
// Special Operations
//===----------------------------------------------------------------------===//

/// Handle the case where an operation wasn't lowered.  When this happens, the
/// operands should just be unlowered non-FIRRTL values.  If the operand was
/// not lowered then leave it alone, otherwise we have a problem with
/// lowering.
///
FIRRTLLowering::UnloweredOpResult
FIRRTLLowering::handleUnloweredOp(Operation *op) {
  // Simply pass through non-FIRRTL operations and consider them already
  // lowered. This allows us to handled partially lowered inputs, and also allow
  // other FIRRTL operations to spawn additional already-lowered operations,
  // like `hw.output`.
  if (!isa<FIRRTLDialect>(op->getDialect())) {
    for (auto &operand : op->getOpOperands())
      if (auto lowered = getPossiblyInoutLoweredValue(operand.get()))
        operand.set(lowered);
    for (auto result : op->getResults())
      (void)setLowering(result, result);
    return AlreadyLowered;
  }

  // Ok, at least one operand got lowered, so this operation is using a FIRRTL
  // value, but wasn't itself lowered.  This is because the lowering is
  // incomplete. This is either a bug or incomplete implementation.
  //
  // There is one aspect of incompleteness we intentionally expect: we allow
  // primitive operations that produce a zero bit result to be ignored by the
  // lowering logic.  They don't have side effects, and handling this corner
  // case just complicates each of the lowering hooks. Instead, we just handle
  // them all right here.
  if (op->getNumResults() == 1) {
    auto resultType = op->getResult(0).getType();
    if (resultType.isa<FIRRTLBaseType>() && isZeroBitFIRRTLType(resultType) &&
        (isExpression(op) || isa<mlir::UnrealizedConversionCastOp>(op))) {
      // Zero bit values lower to the null Value.
      (void)setLowering(op->getResult(0), Value());
      return NowLowered;
    }
  }
  op->emitOpError("LowerToHW couldn't handle this operation");
  return LoweringFailure;
}

LogicalResult FIRRTLLowering::visitExpr(ConstantOp op) {
  // Zero width values must be lowered to nothing.
  if (isZeroBitFIRRTLType(op.getType()))
    return setLowering(op, Value());

  return setLowering(op, getOrCreateIntConstant(op.getValue()));
}

LogicalResult FIRRTLLowering::visitExpr(SpecialConstantOp op) {
  return setLowering(
      op, getOrCreateIntConstant(APInt(/*bitWidth*/ 1, op.getValue())));
}

LogicalResult FIRRTLLowering::visitExpr(SubindexOp op) {
  if (isZeroBitFIRRTLType(op->getResult(0).getType()))
    return setLowering(op, Value());

  auto resultType = lowerType(op->getResult(0).getType());
  Value value = getPossiblyInoutLoweredValue(op.getInput());
  if (!resultType || !value) {
    op.emitError() << "input lowering failed";
    return failure();
  }

  auto iIdx = getOrCreateIntConstant(
      getBitWidthFromVectorSize(
          op.getInput().getType().cast<FVectorType>().getNumElements()),
      op.getIndex());

  // If the value has an inout type, we need to lower to ArrayIndexInOutOp.
  if (value.getType().isa<sv::InOutType>())
    return setLoweringTo<sv::ArrayIndexInOutOp>(op, value, iIdx);
  // Otherwise, hw::ArrayGetOp
  return setLoweringTo<hw::ArrayGetOp>(op, value, iIdx);
}

LogicalResult FIRRTLLowering::visitExpr(SubaccessOp op) {
  if (isZeroBitFIRRTLType(op->getResult(0).getType()))
    return setLowering(op, Value());

  auto resultType = lowerType(op->getResult(0).getType());
  Value value = getPossiblyInoutLoweredValue(op.getInput());
  Value valueIdx = getLoweredAndExtOrTruncValue(
      op.getIndex(),
      UIntType::get(
          op.getContext(),
          getBitWidthFromVectorSize(
              op.getInput().getType().cast<FVectorType>().getNumElements())));

  if (!resultType || !value || !valueIdx) {
    op.emitError() << "input lowering failed";
    return failure();
  }

  // If the value has an inout type, we need to lower to ArrayIndexInOutOp.
  if (value.getType().isa<sv::InOutType>())
    return setLoweringTo<sv::ArrayIndexInOutOp>(op, value, valueIdx);
  // Otherwise, lower the op to array indexing.
  return setLowering(op, createArrayIndexing(value, valueIdx));
}

LogicalResult FIRRTLLowering::visitExpr(SubfieldOp op) {
  // firrtl.mem lowering lowers some SubfieldOps.  Zero-width can leave
  // invalid subfield accesses
  if (getLoweredValue(op) || !op.getInput())
    return success();

  // Extracting a zero bit value from a struct is defined but doesn't do
  // anything.
  if (isZeroBitFIRRTLType(op->getResult(0).getType()))
    return setLowering(op, Value());

  auto resultType = lowerType(op->getResult(0).getType());
  Value value = getPossiblyInoutLoweredValue(op.getInput());
  assert(resultType && value && "subfield type lowering failed");

  // If the value has an inout type, we need to lower to StructFieldInOutOp.
  if (value.getType().isa<sv::InOutType>()) {
    auto field = op.getInput().getType().cast<BundleType>().getElementName(
        op.getFieldIndex());
    return setLoweringTo<sv::StructFieldInOutOp>(op, value, field);
  }

  return setLoweringTo<hw::StructExtractOp>(
      op, resultType, value,
      op.getInput().getType().cast<BundleType>().getElementName(
          op.getFieldIndex()));
}

LogicalResult FIRRTLLowering::visitExpr(VectorCreateOp op) {
  auto resultType = lowerType(op.getResult().getType());
  SmallVector<Value> operands;
  // NOTE: The operand order must be inverted.
  for (auto oper : llvm::reverse(op.getOperands())) {
    auto val = getLoweredValue(oper);
    if (!val)
      return failure();
    operands.push_back(val);
  }
  return setLoweringTo<hw::ArrayCreateOp>(op, resultType, operands);
}

LogicalResult FIRRTLLowering::visitExpr(BundleCreateOp op) {
  auto resultType = lowerType(op.getResult().getType());
  SmallVector<Value> operands;
  for (auto oper : op.getOperands()) {
    auto val = getLoweredValue(oper);
    if (!val)
      return failure();
    operands.push_back(val);
  }
  return setLoweringTo<hw::StructCreateOp>(op, resultType, operands);
}

LogicalResult FIRRTLLowering::visitExpr(AggregateConstantOp op) {
  auto resultType = lowerType(op.getResult().getType());
  auto vec = op.getType().dyn_cast<FVectorType>();
  // Currently we only support 1d vector types.
  if (!vec || !vec.getElementType().isa<IntType>()) {
    op.emitError()
        << "has an unsupported type; currently we only support 1d vectors";
    return failure();
  }

  // TODO: Use hw aggregate constant
  SmallVector<Value> operands;
  // Make sure to reverse the operands.
  for (auto elem : llvm::reverse(op.getFields()))
    operands.push_back(
        getOrCreateIntConstant(elem.cast<IntegerAttr>().getValue()));

  return setLoweringTo<hw::ArrayCreateOp>(op, resultType, operands);
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitDecl(WireOp op) {
  // Foreign types lower to a backedge that needs to be resolved by a later
  // connect op.
  if (!op.getType().isa<FIRRTLType>()) {
    createBackedge(op, op.getType());
    return success();
  }

  auto resultType = lowerType(op.getResult().getType());
  if (!resultType)
    return failure();

  if (resultType.isInteger(0))
    return setLowering(op, Value());

  // Name attr is required on sv.wire but optional on firrtl.wire.
  StringAttr symName = getInnerSymName(op);
  auto name = op.getNameAttr();
  if (AnnotationSet::removeAnnotations(op, dontTouchAnnoClass) && !symName) {
    auto moduleName = cast<hw::HWModuleOp>(op->getParentOp()).getName();
    // Prepend the name of the module to make the symbol name unique in the
    // symbol table, it is already unique in the module. Checking if the name
    // is unique in the SymbolTable is non-trivial.
    symName = builder.getStringAttr(moduleNamespace.newName(
        Twine("__") + moduleName + Twine("__") + name.getValue()));
  }
  if (!symName && !op.hasDroppableName()) {
    auto moduleName = cast<hw::HWModuleOp>(op->getParentOp()).getName();
    symName = builder.getStringAttr(moduleNamespace.newName(
        Twine("__") + moduleName + Twine("__") + name.getValue()));
  }
  // This is not a temporary wire created by the compiler, so attach a symbol
  // name.
  return setLoweringTo<sv::WireOp>(op, resultType, name, symName);
}

LogicalResult FIRRTLLowering::visitDecl(VerbatimWireOp op) {
  auto resultTy = lowerType(op.getType());
  if (!resultTy)
    return failure();
  resultTy = sv::InOutType::get(op.getContext(), resultTy);

  SmallVector<Value, 4> operands;
  operands.reserve(op.getSubstitutions().size());
  for (auto operand : op.getSubstitutions()) {
    auto lowered = getLoweredValue(operand);
    if (!lowered)
      return failure();
    operands.push_back(lowered);
  }

  ArrayAttr symbols = op.getSymbolsAttr();
  if (!symbols)
    symbols = ArrayAttr::get(op.getContext(), {});

  return setLoweringTo<sv::VerbatimExprSEOp>(op, resultTy, op.getTextAttr(),
                                             operands, symbols);
}

LogicalResult FIRRTLLowering::visitDecl(NodeOp op) {
  auto operand = getLoweredValue(op.getInput());
  if (!operand)
    return handleZeroBit(op.getInput(),
                         [&]() { return setLowering(op, Value()); });

  // Node operations are logical noops, but may carry annotations or be
  // referred to through an inner name. If a don't touch is present, ensure
  // that we have a symbol name so we can keep the node as a wire.
  auto symName = getInnerSymName(op);
  auto name = op.getNameAttr();
  // Or we have debug attribute attached
  if ((AnnotationSet::removeAnnotations(
          op, "firrtl.transforms.DontTouchAnnotation") ||
       op->hasAttr("hw.debug.name")) &&
      !symName) {
    // name may be empty
    auto moduleName = cast<hw::HWModuleOp>(op->getParentOp()).getName();
    symName = builder.getStringAttr(Twine("__") + moduleName + Twine("__") +
                                    name.getValue());
  }
  if (!symName && !hasDroppableName(op)) {
    auto moduleName = cast<hw::HWModuleOp>(op->getParentOp()).getName();
    symName = builder.getStringAttr(Twine("__") + moduleName + Twine("__") +
                                    name.getValue());
  }

  if (symName) {
    auto wire = builder.create<sv::WireOp>(operand.getType(), name, symName);
    auto assign = builder.create<sv::AssignOp>(wire, operand);
    operand = builder.create<sv::ReadInOutOp>(wire);
    if (auto debugAttr = op->getAttr("hw.debug.name")) {
      assign->setAttr("hw.debug.name", debugAttr);
      wire->setAttr("hw.debug.name", debugAttr);
    }
  }

  return setLowering(op, operand);
}

LogicalResult FIRRTLLowering::visitDecl(RegOp op) {
  auto resultType = lowerType(op.getResult().getType());
  if (!resultType)
    return failure();
  if (resultType.isInteger(0))
    return setLowering(op, Value());

  Value clockVal = getLoweredValue(op.getClockVal());
  if (!clockVal)
    return failure();

  // Add symbol if DontTouch annotation present.
  auto symName = getInnerSymName(op);
  if ((AnnotationSet::removeAnnotations(op, dontTouchAnnoClass) ||
       op.getNameKind() == NameKindEnum::InterestingName) &&
      !symName)
    symName = op.getNameAttr();

  // Create a reg op, wiring itself to its input.
  Backedge inputEdge = backedgeBuilder.get(resultType);
  auto reg = builder.create<seq::FirRegOp>(inputEdge, clockVal,
                                           op.getNameAttr(), symName);

  // Pass along the start and end random initialization bits for this register.
  if (auto randomRegister = op->getAttr("firrtl.random_init_register"))
    reg->setAttr("firrtl.random_init_register", randomRegister);
  if (auto randomStart = op->getAttr("firrtl.random_init_start"))
    reg->setAttr("firrtl.random_init_start", randomStart);
  if (auto randomEnd = op->getAttr("firrtl.random_init_end"))
    reg->setAttr("firrtl.random_init_end", randomEnd);

  inputEdge.setValue(reg);
  circuitState.used_RANDOMIZE_REG_INIT = 1;
  regMapping.try_emplace(op, reg);
  (void)setLowering(op, reg);
  return success();
}

LogicalResult FIRRTLLowering::visitDecl(RegResetOp op) {
  auto resultType = lowerType(op.getResult().getType());
  if (!resultType)
    return failure();
  if (resultType.isInteger(0))
    return setLowering(op, Value());

  Value clockVal = getLoweredValue(op.getClockVal());
  Value resetSignal = getLoweredValue(op.getResetSignal());
  // Reset values may be narrower than the register.  Extend appropriately.
  Value resetValue = getLoweredAndExtOrTruncValue(
      op.getResetValue(), op.getType().cast<FIRRTLBaseType>());

  if (!clockVal || !resetSignal || !resetValue)
    return failure();

  auto symName = getInnerSymName(op);
  if ((AnnotationSet::removeAnnotations(op, dontTouchAnnoClass) ||
       op.getNameKind() == NameKindEnum::InterestingName) &&
      !symName)
    symName = op.getNameAttr();

  // Create a reg op, wiring itself to its input.
  bool isAsync = op.getResetSignal().getType().isa<AsyncResetType>();
  Backedge inputEdge = backedgeBuilder.get(resultType);
  auto reg =
      builder.create<seq::FirRegOp>(inputEdge, clockVal, op.getNameAttr(),
                                    resetSignal, resetValue, symName, isAsync);

  // Pass along the start and end random initialization bits for this register.
  if (auto randomRegister = op->getAttr("firrtl.random_init_register"))
    reg->setAttr("firrtl.random_init_register", randomRegister);
  if (auto randomStart = op->getAttr("firrtl.random_init_start"))
    reg->setAttr("firrtl.random_init_start", randomStart);
  if (auto randomEnd = op->getAttr("firrtl.random_init_end"))
    reg->setAttr("firrtl.random_init_end", randomEnd);
  if (auto debugAttr = op->getAttr("hw.debug.name"))
    reg->setAttr("hw.debug.name", debugAttr);

  inputEdge.setValue(reg);
  circuitState.used_RANDOMIZE_REG_INIT = 1;
  regMapping.try_emplace(op, reg);
  (void)setLowering(op, reg);

  return success();
}

LogicalResult FIRRTLLowering::visitDecl(MemOp op) {
  auto memName = op.getName();
  if (memName.empty())
    memName = "mem";

  // TODO: Remove this restriction and preserve aggregates in
  // memories.
  if (op.getDataType()
          .cast<FIRRTLBaseType>()
          .getPassiveType()
          .isa<BundleType>())
    return op.emitOpError(
        "should have already been lowered from a ground type to an aggregate "
        "type using the LowerTypes pass. Use "
        "'firtool --lower-types' or 'circt-opt "
        "--pass-pipeline='firrtl.circuit(firrtl-lower-types)' "
        "to run this.");

  FirMemory memSummary = op.getSummary();

  // Process each port in turn.
  SmallVector<Type, 8> resultTypes;
  SmallVector<Value, 8> operands;
  DenseMap<Operation *, size_t> returnHolder;
  SmallVector<Attribute> argNames, resultNames;

  // The result values of the memory are not necessarily in the same order as
  // the memory module that we're lowering to.  We need to lower the read
  // ports before the read/write ports, before the write ports.
  for (unsigned memportKindIdx = 0; memportKindIdx != 3; ++memportKindIdx) {
    MemOp::PortKind memportKind;
    switch (memportKindIdx) {
    default:
      assert(0 && "invalid idx");
      break; // Silence warning
    case 0:
      memportKind = MemOp::PortKind::Read;
      break;
    case 1:
      memportKind = MemOp::PortKind::ReadWrite;
      break;
    case 2:
      memportKind = MemOp::PortKind::Write;
      break;
    }

    // This is set to the count of the kind of memport we're emitting, for
    // label names.
    unsigned portNumber = 0;

    // Memories return multiple structs, one for each port, which means we
    // have two layers of type to split apart.
    for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
      // Process all of one kind before the next.
      if (memportKind != op.getPortKind(i))
        continue;

      auto portName = op.getPortName(i).getValue();

      auto addInput = [&](StringRef portLabel, StringRef portLabel2,
                          StringRef field, size_t width,
                          StringRef field2 = "") {
        auto portType =
            IntegerType::get(op.getContext(), std::max((size_t)1, width));
        auto accesses = getAllFieldAccesses(op.getResult(i), field);

        Value wire = createTmpWireOp(
            portType, ("." + portName + "." + field + ".wire").str());

        for (auto a : accesses) {
          if (a.getType()
                  .cast<FIRRTLBaseType>()
                  .getPassiveType()
                  .getBitWidthOrSentinel() > 0)
            (void)setLowering(a, wire);
          else
            a->eraseOperand(0);
        }
        wire = getReadValue(wire);
        if (!field2.empty()) {
          // This handles the case, when the single bit mask field is removed,
          // and the enable is updated after 'And' with mask bit.
          Value wire2 = createTmpWireOp(
              portType, ("." + portName + "." + field2 + ".wire").str());
          auto accesses2 = getAllFieldAccesses(op.getResult(i), field2);

          for (auto a : accesses2) {
            if (a.getType()
                    .cast<FIRRTLBaseType>()
                    .getPassiveType()
                    .getBitWidthOrSentinel() > 0)
              (void)setLowering(a, wire2);
            else
              a->eraseOperand(0);
          }
          wire = builder.createOrFold<comb::AndOp>(wire, getReadValue(wire2),
                                                   true);
        }

        operands.push_back(wire);
        argNames.push_back(
            builder.getStringAttr(portLabel + Twine(portNumber) + portLabel2));
      };
      auto addOutput = [&](StringRef portLabel, StringRef portLabel2,
                           StringRef field, size_t width) {
        auto portType =
            IntegerType::get(op.getContext(), std::max((size_t)1, width));
        resultTypes.push_back(portType);

        // Now collect the data for the instance.  A op produces multiple
        // structures, so we need to look through SubfieldOps to see the
        // true inputs and outputs.
        auto accesses = getAllFieldAccesses(op.getResult(i), field);
        // Read data ports are tracked to be updated later
        for (auto &a : accesses) {
          if (width)
            returnHolder[a] = resultTypes.size() - 1;
          else
            a->eraseOperand(0);
        }
        resultNames.push_back(
            builder.getStringAttr(portLabel + Twine(portNumber) + portLabel2));
      };

      if (memportKind == MemOp::PortKind::Read) {
        addInput("R", "_addr", "addr", llvm::Log2_64_Ceil(memSummary.depth));
        addInput("R", "_en", "en", 1);
        addInput("R", "_clk", "clk", 1);
        addOutput("R", "_data", "data", memSummary.dataWidth);
      } else if (memportKind == MemOp::PortKind::ReadWrite) {
        addInput("RW", "_addr", "addr", llvm::Log2_64_Ceil(memSummary.depth));
        addInput("RW", "_en", "en", 1);
        addInput("RW", "_clk", "clk", 1);
        // If maskBits =1, then And the mask field with enable, and update the
        // enable. Else keep mask port.
        if (memSummary.isMasked)
          addInput("RW", "_wmode", "wmode", 1);
        else
          addInput("RW", "_wmode", "wmode", 1, "wmask");
        addInput("RW", "_wdata", "wdata", memSummary.dataWidth);
        addOutput("RW", "_rdata", "rdata", memSummary.dataWidth);
        // Ignore mask port, if maskBits =1
        if (memSummary.isMasked)
          addInput("RW", "_wmask", "wmask", memSummary.maskBits);
      } else {
        addInput("W", "_addr", "addr", llvm::Log2_64_Ceil(memSummary.depth));
        // If maskBits =1, then And the mask field with enable, and update the
        // enable. Else keep mask port.
        if (memSummary.isMasked)
          addInput("W", "_en", "en", 1);
        else
          addInput("W", "_en", "en", 1, "mask");
        addInput("W", "_clk", "clk", 1);
        addInput("W", "_data", "data", memSummary.dataWidth);
        // Ignore mask port, if maskBits =1
        if (memSummary.isMasked)
          addInput("W", "_mask", "mask", memSummary.maskBits);
      }

      ++portNumber;
    }
  }

  auto memModuleAttr =
      circuitState.getGenOpMemName(memSummary.getFirMemoryName());

  // Create the instance to replace the memop.
  auto inst = builder.create<hw::InstanceOp>(
      resultTypes, builder.getStringAttr(memName + "_ext"), memModuleAttr,
      operands, builder.getArrayAttr(argNames),
      builder.getArrayAttr(resultNames),
      /*parameters=*/builder.getArrayAttr({}),
      /*sym_name=*/getInnerSymName(op));
  // Update all users of the result of read ports
  for (auto &ret : returnHolder)
    (void)setLowering(ret.first->getResult(0), inst.getResult(ret.second));
  return success();
}

LogicalResult FIRRTLLowering::visitDecl(InstanceOp oldInstance) {
  Operation *oldModule =
      circuitState.getInstanceGraph()->getReferencedModule(oldInstance);
  auto newModule = circuitState.getNewModule(oldModule);
  if (!newModule) {
    oldInstance->emitOpError("could not find module referenced by instance");
    return failure();
  }

  // If this is a referenced to a parameterized extmodule, then bring the
  // parameters over to this instance.
  ArrayAttr parameters;
  if (auto oldExtModule = dyn_cast<FExtModuleOp>(oldModule))
    parameters = getHWParameters(oldExtModule, /*ignoreValues=*/false);

  // Decode information about the input and output ports on the referenced
  // module.
  SmallVector<PortInfo, 8> portInfo = cast<FModuleLike>(oldModule).getPorts();

  // Build an index from the name attribute to an index into portInfo, so we
  // can do efficient lookups.
  llvm::SmallDenseMap<Attribute, unsigned> portIndicesByName;
  for (unsigned portIdx = 0, e = portInfo.size(); portIdx != e; ++portIdx)
    portIndicesByName[portInfo[portIdx].name] = portIdx;

  // Ok, get ready to create the new instance operation.  We need to prepare
  // input operands.
  SmallVector<Value, 8> operands;
  for (size_t portIndex = 0, e = portInfo.size(); portIndex != e; ++portIndex) {
    auto &port = portInfo[portIndex];
    auto portType = lowerType(port.type);
    if (!portType) {
      oldInstance->emitOpError("could not lower type of port ") << port.name;
      return failure();
    }

    // Drop zero bit input/inout ports.
    if (portType.isInteger(0))
      continue;

    // We wire outputs up after creating the instance.
    if (port.isOutput())
      continue;

    auto portResult = oldInstance.getResult(portIndex);
    assert(portResult && "invalid IR, couldn't find port");

    // Directly materialize inputs which are trivially assigned once through a
    // `StrictConnectOp`.
    if (port.isInput() && getSingleConnectUserOf(portResult)) {
      operands.push_back(createBackedge(portResult, portType));
      continue;
    }

    // If the result has an analog type and is used only by attach op,
    // try eliminating a temporary wire by directly using an attached value.
    if (portResult.getType().isa<AnalogType>() && portResult.hasOneUse()) {
      if (auto attach = dyn_cast<AttachOp>(*portResult.getUsers().begin())) {
        if (auto source = getSingleNonInstanceOperand(attach)) {
          auto loweredResult = getPossiblyInoutLoweredValue(source);
          operands.push_back(loweredResult);
          (void)setLowering(portResult, loweredResult);
          continue;
        }
      }
    }

    // Directly materialize foreign types.
    if (!port.type.isa<FIRRTLType>()) {
      operands.push_back(createBackedge(portResult, portType));
      continue;
    }

    // Create a wire for each input/inout operand, so there is
    // something to connect to.
    auto w = createTmpWireOp(portType, "." + port.getName().str() + ".wire");
    Value wire = w;

    // migrate the renaming process
    auto *op = wire.getDefiningOp();
    auto &annos = port.annotations;
    if (op) {
      for (auto const &attr : annos) {
        auto const &dict = attr.getDict();
        if (auto name = dict.get("hw.debug.name")) {
          w->setAttr("hw.debug.name", name);
        }
      }
    }

    // Know that the argument FIRRTL value is equal to this wire, allowing
    // connects to it to be lowered.
    (void)setLowering(portResult, wire);

    // inout ports directly use the wire, but normal inputs read it.
    if (!port.isInOut())
      wire = getReadValue(wire);

    operands.push_back(wire);
  }

  // If this instance is destined to be lowered to a bind, generate a symbol
  // for it and generate a bind op.  Enter the bind into global
  // CircuitLoweringState so that this can be moved outside of module once
  // we're guaranteed to not be a parallel context.
  StringAttr symbol = getInnerSymName(oldInstance);
  if (oldInstance.getLowerToBind()) {
    if (!symbol)
      symbol = builder.getStringAttr("__" + oldInstance.getName() + "__");
    auto bindOp = builder.create<sv::BindOp>(theModule.getNameAttr(), symbol);
    // If the lowered op already had output file information, then use that.
    // Otherwise, generate some default bind information.
    if (auto outputFile = oldInstance->getAttr("output_file"))
      bindOp->setAttr("output_file", outputFile);
    // Add the bind to the circuit state.  This will be moved outside of the
    // encapsulating module after all modules have been processed in parallel.
    circuitState.addBind(bindOp);
  }

  // Create the new hw.instance operation.
  auto newInstance = builder.create<hw::InstanceOp>(
      newModule, oldInstance.getNameAttr(), operands, parameters, symbol);

  if (oldInstance.getLowerToBind())
    newInstance->setAttr("doNotPrint", builder.getBoolAttr(true));

  if (newInstance.getInnerSymAttr())
    if (auto forceName = circuitState.instanceForceNames.lookup(
            {cast<hw::HWModuleOp>(newInstance->getParentOp()).getNameAttr(),
             newInstance.getInnerSymAttr()}))
      newInstance->setAttr("hw.verilogName", forceName);

  // Now that we have the new hw.instance, we need to remap all of the users
  // of the outputs/results to the values returned by the instance.
  unsigned resultNo = 0;
  for (size_t portIndex = 0, e = portInfo.size(); portIndex != e; ++portIndex) {
    auto &port = portInfo[portIndex];
    if (!port.isOutput() || isZeroBitFIRRTLType(port.type))
      continue;

    Value resultVal = newInstance.getResult(resultNo);

    auto oldPortResult = oldInstance.getResult(portIndex);
    (void)setLowering(oldPortResult, resultVal);
    ++resultNo;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Unary Operations
//===----------------------------------------------------------------------===//

// Lower a cast that is a noop at the HW level.
LogicalResult FIRRTLLowering::lowerNoopCast(Operation *op) {
  auto operand = getPossiblyInoutLoweredValue(op->getOperand(0));
  if (!operand)
    return failure();

  // Noop cast.
  return setLowering(op->getResult(0), operand);
}

LogicalResult FIRRTLLowering::visitExpr(mlir::UnrealizedConversionCastOp op) {
  // General lowering for non-unary casts.
  if (op.getNumOperands() != 1 || op.getNumResults() != 1)
    return failure();

  auto operand = op.getOperand(0);
  auto result = op.getResult(0);

  // FIRRTL -> FIRRTL
  if (operand.getType().isa<FIRRTLType>() && result.getType().isa<FIRRTLType>())
    return lowerNoopCast(op);

  // other -> FIRRTL
  // other -> other
  if (!operand.getType().isa<FIRRTLType>()) {
    if (result.getType().isa<FIRRTLType>())
      return setLowering(result, getPossiblyInoutLoweredValue(operand));
    return failure(); // general foreign op lowering for other -> other
  }

  // FIRRTL -> other
  // Otherwise must be a conversion from FIRRTL type to standard type.
  auto lowered_result = getLoweredValue(operand);
  if (!lowered_result) {
    // If this is a conversion from a zero bit HW type to firrtl value, then
    // we want to successfully lower this to a null Value.
    if (operand.getType().isSignlessInteger(0)) {
      return setLowering(result, Value());
    }
    return failure();
  }

  // We lower builtin.unrealized_conversion_cast converting from a firrtl type
  // to a standard type into the lowered operand.
  result.replaceAllUsesWith(lowered_result);
  return success();
}

LogicalResult FIRRTLLowering::visitExpr(HWStructCastOp op) {
  // Conversions from hw struct types to FIRRTL types are lowered as the
  // input operand.
  if (auto opStructType = op.getOperand().getType().dyn_cast<hw::StructType>())
    return setLowering(op, op.getOperand());

  // Otherwise must be a conversion from FIRRTL bundle type to hw struct
  // type.
  auto result = getLoweredValue(op.getOperand());
  if (!result)
    return failure();

  // We lower firrtl.stdStructCast converting from a firrtl bundle to an hw
  // struct type into the lowered operand.
  op.replaceAllUsesWith(result);
  return success();
}

LogicalResult FIRRTLLowering::visitExpr(BitCastOp op) {
  auto operand = getLoweredValue(op.getOperand());
  if (!operand)
    return failure();
  auto resultType = lowerType(op.getType());
  if (!resultType)
    return failure();

  return setLoweringTo<hw::BitcastOp>(op, resultType, operand);
}

LogicalResult FIRRTLLowering::visitExpr(CvtPrimOp op) {
  auto operand = getLoweredValue(op.getOperand());
  if (!operand) {
    return handleZeroBit(op.getOperand(), [&]() {
      // Unsigned zero bit to Signed is 1b0.
      if (op.getOperand().getType().cast<IntType>().isUnsigned())
        return setLowering(op, getOrCreateIntConstant(1, 0));
      // Signed->Signed is a zero bit value.
      return setLowering(op, Value());
    });
  }

  // Signed to signed is a noop.
  if (op.getOperand().getType().cast<IntType>().isSigned())
    return setLowering(op, operand);

  // Otherwise prepend a zero bit.
  auto zero = getOrCreateIntConstant(1, 0);
  return setLoweringTo<comb::ConcatOp>(op, zero, operand);
}

LogicalResult FIRRTLLowering::visitExpr(NotPrimOp op) {
  auto operand = getLoweredValue(op.getInput());
  if (!operand)
    return failure();
  // ~x  ---> x ^ 0xFF
  auto allOnes = getOrCreateIntConstant(
      APInt::getAllOnes(operand.getType().getIntOrFloatBitWidth()));
  return setLoweringTo<comb::XorOp>(op, operand, allOnes, true);
}

LogicalResult FIRRTLLowering::visitExpr(NegPrimOp op) {
  // FIRRTL negate always adds a bit.
  // -x ---> 0-sext(x) or 0-zext(x)
  auto operand = getLoweredAndExtendedValue(op.getInput(), op.getType());
  if (!operand)
    return failure();

  auto resultType = lowerType(op.getType());

  auto zero = getOrCreateIntConstant(resultType.getIntOrFloatBitWidth(), 0);
  return setLoweringTo<comb::SubOp>(op, zero, operand, true);
}

// Pad is a noop or extension operation.
LogicalResult FIRRTLLowering::visitExpr(PadPrimOp op) {
  auto operand = getLoweredAndExtendedValue(op.getInput(), op.getType());
  if (!operand)
    return failure();
  return setLowering(op, operand);
}

LogicalResult FIRRTLLowering::visitExpr(XorRPrimOp op) {
  auto operand = getLoweredValue(op.getInput());
  if (!operand) {
    return handleZeroBit(op.getInput(), [&]() {
      return setLowering(op, getOrCreateIntConstant(1, 0));
    });
    return failure();
  }

  return setLoweringTo<comb::ParityOp>(op, builder.getIntegerType(1), operand);
}

LogicalResult FIRRTLLowering::visitExpr(AndRPrimOp op) {
  auto operand = getLoweredValue(op.getInput());
  if (!operand) {
    return handleZeroBit(op.getInput(), [&]() {
      return setLowering(op, getOrCreateIntConstant(1, 1));
    });
  }

  // Lower AndR to == -1
  return setLoweringTo<comb::ICmpOp>(
      op, ICmpPredicate::eq, operand,
      getOrCreateIntConstant(
          APInt::getAllOnes(operand.getType().getIntOrFloatBitWidth())),
      true);
}

LogicalResult FIRRTLLowering::visitExpr(OrRPrimOp op) {
  auto operand = getLoweredValue(op.getInput());
  if (!operand) {
    return handleZeroBit(op.getInput(), [&]() {
      return setLowering(op, getOrCreateIntConstant(1, 0));
    });
    return failure();
  }

  // Lower OrR to != 0
  return setLoweringTo<comb::ICmpOp>(
      op, ICmpPredicate::ne, operand,
      getOrCreateIntConstant(operand.getType().getIntOrFloatBitWidth(), 0),
      true);
}

//===----------------------------------------------------------------------===//
// Binary Operations
//===----------------------------------------------------------------------===//

template <typename ResultOpType>
LogicalResult FIRRTLLowering::lowerBinOpToVariadic(Operation *op) {
  auto resultType = op->getResult(0).getType();
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), resultType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), resultType);
  if (!lhs || !rhs)
    return failure();

  return setLoweringTo<ResultOpType>(op, lhs, rhs, true);
}

/// lowerBinOp extends each operand to the destination type, then performs the
/// specified binary operator.
template <typename ResultUnsignedOpType, typename ResultSignedOpType>
LogicalResult FIRRTLLowering::lowerBinOp(Operation *op) {
  // Extend the two operands to match the destination type.
  auto resultType = op->getResult(0).getType();
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), resultType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), resultType);
  if (!lhs || !rhs)
    return failure();

  // Emit the result operation.
  if (resultType.cast<IntType>().isSigned())
    return setLoweringTo<ResultSignedOpType>(op, lhs, rhs, true);
  return setLoweringTo<ResultUnsignedOpType>(op, lhs, rhs, true);
}

/// lowerCmpOp extends each operand to the longest type, then performs the
/// specified binary operator.
LogicalResult FIRRTLLowering::lowerCmpOp(Operation *op, ICmpPredicate signedOp,
                                         ICmpPredicate unsignedOp) {
  // Extend the two operands to match the longest type.
  auto lhsIntType = op->getOperand(0).getType().cast<IntType>();
  auto rhsIntType = op->getOperand(1).getType().cast<IntType>();
  if (!lhsIntType.hasWidth() || !rhsIntType.hasWidth())
    return failure();

  auto cmpType = getWidestIntType(lhsIntType, rhsIntType);
  if (cmpType.getWidth() == 0) // Handle 0-width inputs by promoting to 1 bit.
    cmpType = UIntType::get(builder.getContext(), 1);
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), cmpType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), cmpType);
  if (!lhs || !rhs)
    return failure();

  // Emit the result operation.
  Type resultType = builder.getIntegerType(1);
  return setLoweringTo<comb::ICmpOp>(
      op, resultType, lhsIntType.isSigned() ? signedOp : unsignedOp, lhs, rhs,
      true);
}

/// Lower a divide or dynamic shift, where the operation has to be performed
/// in the widest type of the result and two inputs then truncated down.
template <typename SignedOp, typename UnsignedOp>
LogicalResult FIRRTLLowering::lowerDivLikeOp(Operation *op) {
  // hw has equal types for these, firrtl doesn't.  The type of the firrtl
  // RHS may be wider than the LHS, and we cannot truncate off the high bits
  // (because an overlarge amount is supposed to shift in sign or zero bits).
  auto opType = op->getResult(0).getType().cast<IntType>();
  if (opType.getWidth() == 0)
    return setLowering(op->getResult(0), Value());

  auto resultType = getWidestIntType(opType, op->getOperand(1).getType());
  resultType = getWidestIntType(resultType, op->getOperand(0).getType());
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), resultType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), resultType);
  if (!lhs || !rhs)
    return failure();

  Value result;
  if (opType.isSigned())
    result = builder.createOrFold<SignedOp>(lhs, rhs, true);
  else
    result = builder.createOrFold<UnsignedOp>(lhs, rhs, true);

  tryCopyName(result.getDefiningOp(), op);

  if (resultType == opType)
    return setLowering(op->getResult(0), result);
  return setLoweringTo<comb::ExtractOp>(op, lowerType(opType), result, 0);
}

LogicalResult FIRRTLLowering::visitExpr(CatPrimOp op) {
  auto lhs = getLoweredValue(op.getLhs());
  auto rhs = getLoweredValue(op.getRhs());
  if (!lhs) {
    return handleZeroBit(op.getLhs(), [&]() {
      if (rhs) // cat(0bit, x) --> x
        return setLowering(op, rhs);
      // cat(0bit, 0bit) --> 0bit
      return handleZeroBit(op.getRhs(),
                           [&]() { return setLowering(op, Value()); });
    });
  }

  if (!rhs) // cat(x, 0bit) --> x
    return handleZeroBit(op.getRhs(), [&]() { return setLowering(op, lhs); });

  return setLoweringTo<comb::ConcatOp>(op, lhs, rhs);
}

//===----------------------------------------------------------------------===//
// Verif Operations
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitExpr(IsXVerifOp op) {
  auto input = getLoweredValue(op.getArg());
  if (!input)
    return failure();

  return setLoweringTo<comb::ICmpOp>(
      op, ICmpPredicate::ceq, input,
      getOrCreateXConstant(input.getType().getIntOrFloatBitWidth()), true);
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitExpr(BitsPrimOp op) {
  auto input = getLoweredValue(op.getInput());
  if (!input)
    return failure();

  Type resultType = builder.getIntegerType(op.getHi() - op.getLo() + 1);
  return setLoweringTo<comb::ExtractOp>(op, resultType, input, op.getLo());
}

LogicalResult FIRRTLLowering::visitExpr(InvalidValueOp op) {
  auto resultTy = lowerType(op.getType());
  if (!resultTy)
    return failure();

  // Values of analog type always need to be lowered to something with inout
  // type.  We do that by lowering to a wire and return that.  As with the
  // SFC, we do not connect anything to this, because it is bidirectional.
  if (op.getType().isa<AnalogType>())
    // This is a locally visible, private wire created by the compiler, so do
    // not attach a symbol name.
    return setLoweringTo<sv::WireOp>(op, resultTy, ".invalid_analog");

  // We don't allow aggregate values which contain values of analog types.
  if (op.getType().cast<FIRRTLBaseType>().containsAnalog())
    return failure();

  // We lower invalid to 0.  TODO: the FIRRTL spec mentions something about
  // lowering it to a random value, we should see if this is what we need to
  // do.
  if (auto bitwidth =
          firrtl::getBitWidth(op.getType().cast<FIRRTLBaseType>())) {
    if (bitwidth.value() == 0) // Let the caller handle zero width values.
      return failure();

    auto constant = getOrCreateIntConstant(bitwidth.value(), 0);
    // If the result is an aggregate value, we have to bitcast the constant.
    if (!resultTy.isa<IntegerType>())
      constant = builder.create<hw::BitcastOp>(resultTy, constant);
    return setLowering(op, constant);
  }

  // Invalid for bundles isn't supported.
  op.emitOpError("unsupported type");
  return failure();
}

LogicalResult FIRRTLLowering::visitExpr(HeadPrimOp op) {
  auto input = getLoweredValue(op.getInput());
  if (!input)
    return failure();
  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  if (op.getAmount() == 0)
    return setLowering(op, Value());
  Type resultType = builder.getIntegerType(op.getAmount());
  return setLoweringTo<comb::ExtractOp>(op, resultType, input,
                                        inWidth - op.getAmount());
}

LogicalResult FIRRTLLowering::visitExpr(ShlPrimOp op) {
  auto input = getLoweredValue(op.getInput());
  if (!input) {
    return handleZeroBit(op.getInput(), [&]() {
      if (op.getAmount() == 0)
        return failure();
      return setLowering(op, getOrCreateIntConstant(op.getAmount(), 0));
    });
  }

  // Handle the degenerate case.
  if (op.getAmount() == 0)
    return setLowering(op, input);

  auto zero = getOrCreateIntConstant(op.getAmount(), 0);
  return setLoweringTo<comb::ConcatOp>(op, input, zero);
}

LogicalResult FIRRTLLowering::visitExpr(ShrPrimOp op) {
  auto input = getLoweredValue(op.getInput());
  if (!input)
    return failure();

  // Handle the special degenerate cases.
  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  auto shiftAmount = op.getAmount();
  if (shiftAmount >= inWidth) {
    // Unsigned shift by full width returns a single-bit zero.
    if (op.getInput().getType().cast<IntType>().isUnsigned())
      return setLowering(op, getOrCreateIntConstant(1, 0));

    // Signed shift by full width is equivalent to extracting the sign bit.
    shiftAmount = inWidth - 1;
  }

  Type resultType = builder.getIntegerType(inWidth - shiftAmount);
  return setLoweringTo<comb::ExtractOp>(op, resultType, input, shiftAmount);
}

LogicalResult FIRRTLLowering::visitExpr(TailPrimOp op) {
  auto input = getLoweredValue(op.getInput());
  if (!input)
    return failure();

  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  if (inWidth == op.getAmount())
    return setLowering(op, Value());
  Type resultType = builder.getIntegerType(inWidth - op.getAmount());
  return setLoweringTo<comb::ExtractOp>(op, resultType, input, 0);
}

LogicalResult FIRRTLLowering::visitExpr(MuxPrimOp op) {
  auto cond = getLoweredValue(op.getSel());
  auto ifTrue = getLoweredAndExtendedValue(op.getHigh(), op.getType());
  auto ifFalse = getLoweredAndExtendedValue(op.getLow(), op.getType());
  if (!cond || !ifTrue || !ifFalse)
    return failure();

  return setLoweringTo<comb::MuxOp>(op, ifTrue.getType(), cond, ifTrue, ifFalse,
                                    true);
}

// Construct array indexing annotated with vendor pragmas to get
// better synthesis results. Specifically we annotate pragmas in the following
// form.
// ```
//   wire GEN;
//   /* synopsys infer_mux_override */
//   assign GEN = array[index] /* cadence map_to_mux */;
// ```
Value FIRRTLLowering::createArrayIndexing(Value array, Value index) {

  auto size = hw::type_cast<hw::ArrayType>(array.getType()).getSize();
  // Extend to power of 2.  FIRRTL semantics say out-of-bounds access result in
  // an indeterminate value.  Existing chisel code depends on this behavior
  // being "return index 0".  Ideally, we would tail extend the array to improve
  // optimization.
  if (!llvm::isPowerOf2_64(size)) {
    auto extElem = getOrCreateIntConstant(APInt(llvm::Log2_64_Ceil(size), 0));
    auto extValue = builder.create<hw::ArrayGetOp>(array, extElem);
    SmallVector<Value> temp(llvm::NextPowerOf2(size) - size, extValue);
    auto ext = builder.create<hw::ArrayCreateOp>(temp);
    Value temp2[] = {ext.getResult(), array};
    array = builder.create<hw::ArrayConcatOp>(temp2);
  }

  Value inBoundsRead;
  // If `stripMuxPragmas` is specified, just lower to a vanilla array indexing.
  // Also remove mux pragmas if the array size is 1 since it causes a
  // complication failure.
  if (circuitState.stripMuxPragmas || size <= 1) {
    inBoundsRead = builder.create<hw::ArrayGetOp>(array, index);
  } else {
    auto arrayGet = builder.create<hw::ArrayGetOp>(array, index);
    auto valWire = builder.create<sv::WireOp>(
        hw::type_cast<hw::ArrayType>(array.getType()).getElementType());
    // Use SV attributes to annotate pragmas.
    circt::sv::setSVAttributes(
        arrayGet,
        sv::SVAttributesAttr::get(builder.getContext(), {"cadence map_to_mux"},
                                  /*emitAsComments=*/true));

    auto assignOp = builder.create<sv::AssignOp>(valWire, arrayGet);
    sv::setSVAttributes(
        assignOp, sv::SVAttributesAttr::get(builder.getContext(),
                                            {"synopsys infer_mux_override"},
                                            /*emitAsComments=*/true));
    inBoundsRead = builder.create<sv::ReadInOutOp>(valWire);
  }

  return inBoundsRead;
}

LogicalResult FIRRTLLowering::visitExpr(MultibitMuxOp op) {
  // Lower and resize to the index width.
  auto index = getLoweredAndExtOrTruncValue(
      op.getIndex(),
      UIntType::get(op.getContext(),
                    getBitWidthFromVectorSize(op.getInputs().size())));

  if (!index)
    return failure();
  SmallVector<Value> loweredInputs;
  loweredInputs.reserve(op.getInputs().size());
  for (auto input : op.getInputs()) {
    auto lowered = getLoweredAndExtendedValue(input, op.getType());
    if (!lowered)
      return failure();
    loweredInputs.push_back(lowered);
  }

  Value array = builder.create<hw::ArrayCreateOp>(loweredInputs);
  return setLowering(op, createArrayIndexing(array, index));
}

LogicalResult FIRRTLLowering::visitExpr(VerbatimExprOp op) {
  auto resultTy = lowerType(op.getType());
  if (!resultTy)
    return failure();

  SmallVector<Value, 4> operands;
  operands.reserve(op.getSubstitutions().size());
  for (auto operand : op.getSubstitutions()) {
    auto lowered = getLoweredValue(operand);
    if (!lowered)
      return failure();
    operands.push_back(lowered);
  }

  ArrayAttr symbols = op.getSymbolsAttr();
  if (!symbols)
    symbols = ArrayAttr::get(op.getContext(), {});

  return setLoweringTo<sv::VerbatimExprOp>(op, resultTy, op.getTextAttr(),
                                           operands, symbols);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitStmt(SkipOp op) {
  // Nothing!  We could emit an comment as a verbatim op if there were a
  // reason to.
  return success();
}

void FIRRTLLowering::lowerRegConnect(const FieldRef &fieldRef, Value dest,
                                     Value srcVal) {
  auto *regOp = fieldRef.getValue().getDefiningOp();
  // Construct the updated value.  For each access, starting with the
  // outermost ones, the new value is computed by updating one of its
  // fields. The value of the field itself is determined by updating the
  // previous value according to the subsequent access operation.
  std::function<Value(Value, Type, unsigned)> inject =
      [&](Value base, Type type, unsigned fieldID) -> Value {
    if (auto bundle = type.dyn_cast<BundleType>()) {
      auto index = bundle.getIndexForFieldID(fieldID);
      fieldID -= bundle.getFieldID(index);

      auto ty = hw::type_cast<hw::StructType>(base.getType());
      auto field = ty.getElements()[index];
      auto name = field.name;

      // Determine the value of the new field.
      Value value;
      if (fieldID == 0) {
        value = srcVal;
      } else {
        auto oldField =
            builder.create<hw::StructExtractOp>(field.type, base, field.name);
        value = inject(oldField, bundle.getElementType(index), fieldID);
      }

      return builder.create<hw::StructInjectOp>(ty, base, name, value)
          .getResult();

    } else {
      auto vector = type.cast<FVectorType>();
      auto index = vector.getIndexForFieldID(fieldID);
      fieldID -= vector.getFieldID(index);

      auto ty = hw::type_cast<hw::ArrayType>(base.getType());
      auto elemTy = ty.getElementType();

      auto size = ty.getSize();
      auto indexWidth = getBitWidthFromVectorSize(size);

      // Determine the value of the new field.
      Value value;
      if (fieldID == 0) {
        value = srcVal;
      } else {
        auto oldField = builder.create<hw::ArrayGetOp>(
            elemTy, base,
            builder.create<hw::ConstantOp>(APInt(indexWidth, index)));
        value = inject(oldField, vector.getElementType(), fieldID);
      }

      // Build a new array by concatenating predecessors, the new
      // value and any successors from the old array.
      if (index == 0 && size == 1)
        return builder.create<hw::ArrayCreateOp>(value).getResult();

      // Slice around the updated element and concatenate the results.
      // Higher indices are passed to the concat op first.
      SmallVector<Value> elements;
      if (index + 1 != size) {
        elements.push_back(builder.create<hw::ArraySliceOp>(
            hw::ArrayType::get(elemTy, size - index - 1), base,
            getOrCreateIntConstant(indexWidth, index + 1)));
      }
      elements.push_back(builder.create<hw::ArrayCreateOp>(value));
      if (index != 0) {
        elements.push_back(builder.create<hw::ArraySliceOp>(
            hw::ArrayType::get(elemTy, index), base,
            getOrCreateIntConstant(indexWidth, 0)));
      }

      return builder.create<hw::ArrayConcatOp>(elements).getResult();
    }
  };

  // Update the last value mapped for the register.
  auto it = regMapping.find(regOp);
  assert(it != regMapping.end() && "register not defined");
  auto seqReg = it->second;

  auto fieldID = fieldRef.getFieldID();
  Value nextValue = srcVal;
  if (fieldID != 0) {
    nextValue =
        inject(seqReg.getNext(), regOp->getResult(0).getType(), fieldID);
  }
  seqReg.getNextMutable().assign(nextValue);
}

LogicalResult FIRRTLLowering::visitStmt(ConnectOp op) {
  auto dest = op.getDest();
  // The source can be a smaller integer, extend it as appropriate if so.
  auto destType = dest.getType().cast<FIRRTLBaseType>().getPassiveType();
  auto srcVal = getLoweredAndExtendedValue(op.getSrc(), destType);
  if (!srcVal)
    return handleZeroBit(op.getSrc(), []() { return success(); });

  auto destVal = getPossiblyInoutLoweredValue(dest);
  if (!destVal)
    return failure();

  auto fieldRef = getFieldRefFromValue(dest);
  auto definingOp = fieldRef.getValue().getDefiningOp();
  if (isa<RegOp>(definingOp) || isa<RegResetOp>(definingOp)) {
    lowerRegConnect(fieldRef, dest, srcVal);
    return success();
  }

  if (!destVal.getType().isa<hw::InOutType>())
    return op.emitError("destination isn't an inout type");

  auto assignOp = builder.create<sv::AssignOp>(destVal, srcVal);
  if (auto debugAttr = op->getAttr("hw.debug.name")) {
    assignOp->setAttr("hw.debug.name", debugAttr);
  }
  return success();
}

LogicalResult FIRRTLLowering::visitStmt(StrictConnectOp op) {
  auto dest = op.getDest();
  auto srcVal = getLoweredValue(op.getSrc());
  if (!srcVal)
    return handleZeroBit(op.getSrc(), []() { return success(); });

  auto destVal = getPossiblyInoutLoweredValue(dest);
  if (!destVal)
    return failure();

  auto fieldRef = getFieldRefFromValue(dest);
  auto definingOp = fieldRef.getValue().getDefiningOp();
  if (isa<RegOp>(definingOp) || isa<RegResetOp>(definingOp)) {
    lowerRegConnect(fieldRef, dest, srcVal);
    return success();
  }

  // If this connect is driving a value that is currently a backedge, record
  // that the source is the value of the backedge.
  if (updateIfBackedge(destVal, srcVal))
    return success();

  if (!destVal.getType().isa<hw::InOutType>())
    return op.emitError("destination isn't an inout type");

  builder.create<sv::AssignOp>(destVal, srcVal);
  return success();
}

LogicalResult FIRRTLLowering::visitStmt(ForceOp op) {
  auto srcVal = getLoweredValue(op.getSrc());
  if (!srcVal)
    return failure();

  auto destVal = getPossiblyInoutLoweredValue(op.getDest());
  if (!destVal)
    return failure();

  if (!destVal.getType().isa<hw::InOutType>())
    return op.emitError("destination isn't an inout type");

  addToIfDefBlock("VERILATOR", std::function<void()>(), [&]() {
    addToInitialBlock([&]() { builder.create<sv::ForceOp>(destVal, srcVal); });
  });
  return success();
}

// Printf is a macro op that lowers to an sv.ifdef.procedural, an sv.if,
// and an sv.fwrite all nested together.
LogicalResult FIRRTLLowering::visitStmt(PrintFOp op) {
  auto clock = getLoweredValue(op.getClock());
  auto cond = getLoweredValue(op.getCond());
  if (!clock || !cond)
    return failure();

  SmallVector<Value, 4> operands;
  operands.reserve(op.getSubstitutions().size());
  for (auto operand : op.getSubstitutions()) {
    operands.push_back(getLoweredValue(operand));
    if (!operands.back()) {
      // If this is a zero bit operand, just pass a one bit zero.
      if (!isZeroBitFIRRTLType(operand.getType()))
        return failure();
      operands.back() = getOrCreateIntConstant(1, 0);
    }
  }

  // Emit an "#ifndef SYNTHESIS" guard into the always block.
  addToIfDefBlock("SYNTHESIS", std::function<void()>(), [&]() {
    addToAlwaysBlock(clock, [&]() {
      circuitState.used_PRINTF_COND = true;

      // Emit an "sv.if '`PRINTF_COND_ & cond' into the #ifndef.
      Value ifCond =
          builder.create<sv::MacroRefExprOp>(cond.getType(), "PRINTF_COND_");
      ifCond = builder.createOrFold<comb::AndOp>(ifCond, cond, true);

      addIfProceduralBlock(ifCond, [&]() {
        // Emit the sv.fwrite, writing to stderr by default.
        Value fdStderr = builder.create<hw::ConstantOp>(APInt(32, 0x80000002));
        builder.create<sv::FWriteOp>(fdStderr, op.getFormatString(), operands);
      });
    });
  });

  return success();
}

// Stop lowers into a nested series of behavioral statements plus $fatal
// or $finish.
LogicalResult FIRRTLLowering::visitStmt(StopOp op) {
  auto clock = getLoweredValue(op.getClock());
  auto cond = getLoweredValue(op.getCond());
  if (!clock || !cond)
    return failure();

  // Emit an "#ifndef SYNTHESIS" guard into the always block.
  addToIfDefBlock("SYNTHESIS", std::function<void()>(), [&]() {
    // Emit this into an "sv.always posedge" body.
    addToAlwaysBlock(clock, [&]() {
      circuitState.used_STOP_COND = true;

      // Emit an "sv.if '`STOP_COND_ & cond' into the #ifndef.
      Value ifCond =
          builder.create<sv::MacroRefExprOp>(cond.getType(), "STOP_COND_");
      ifCond = builder.createOrFold<comb::AndOp>(ifCond, cond, true);
      addIfProceduralBlock(ifCond, [&]() {
        // Emit the sv.fatal or sv.finish.
        if (op.getExitCode())
          builder.create<sv::FatalOp>();
        else
          builder.create<sv::FinishOp>();
      });
    });
  });

  return success();
}

/// Helper function to build an immediate assert operation based on the
/// original FIRRTL operation name. This reduces code duplication in
/// `lowerVerificationStatement`.
template <typename... Args>
static Operation *buildImmediateVerifOp(ImplicitLocOpBuilder &builder,
                                        StringRef opName, Args &&...args) {
  if (opName == "assert")
    return builder.create<sv::AssertOp>(std::forward<Args>(args)...);
  if (opName == "assume")
    return builder.create<sv::AssumeOp>(std::forward<Args>(args)...);
  if (opName == "cover")
    return builder.create<sv::CoverOp>(std::forward<Args>(args)...);
  llvm_unreachable("unknown verification op");
}

/// Helper function to build a concurrent assert operation based on the
/// original FIRRTL operation name. This reduces code duplication in
/// `lowerVerificationStatement`.
template <typename... Args>
static Operation *buildConcurrentVerifOp(ImplicitLocOpBuilder &builder,
                                         StringRef opName, Args &&...args) {
  if (opName == "assert")
    return builder.create<sv::AssertConcurrentOp>(std::forward<Args>(args)...);
  if (opName == "assume")
    return builder.create<sv::AssumeConcurrentOp>(std::forward<Args>(args)...);
  if (opName == "cover")
    return builder.create<sv::CoverConcurrentOp>(std::forward<Args>(args)...);
  llvm_unreachable("unknown verification op");
}

/// Template for lowering verification statements from type A to
/// type B.
///
/// For example, lowering the "foo" op to the "bar" op would start
/// with:
///
///     foo(clock, condition, enable, "message")
///
/// This becomes a Verilog clocking block with the "bar" op guarded
/// by an if enable:
///
///     always @(posedge clock) begin
///       if (enable) begin
///         bar(condition);
///       end
///     end
/// The above can also be reduced into a concurrent verification statement
/// sv.assert.concurrent posedge %clock (condition && enable)
LogicalResult FIRRTLLowering::lowerVerificationStatement(
    Operation *op, StringRef labelPrefix, Value opClock, Value opPredicate,
    Value opEnable, StringAttr opMessageAttr, ValueRange opOperands,
    StringAttr opNameAttr, bool isConcurrent, EventControl opEventControl) {
  StringRef opName = op->getName().stripDialect();
  auto isAssert = opName == "assert";
  auto isCover = opName == "cover";

  auto clock = getLoweredValue(opClock);
  auto enable = getLoweredValue(opEnable);
  auto predicate = getLoweredValue(opPredicate);
  if (!clock || !enable || !predicate)
    return failure();

  StringAttr label;
  if (opNameAttr && !opNameAttr.getValue().empty())
    label = opNameAttr;
  StringAttr prefixedLabel;
  if (label)
    prefixedLabel =
        StringAttr::get(builder.getContext(), labelPrefix + label.getValue());

  StringAttr message;
  SmallVector<Value> messageOps;
  if (!isCover && opMessageAttr && !opMessageAttr.getValue().empty()) {
    message = opMessageAttr;
    for (auto operand : opOperands) {
      auto loweredValue = getLoweredValue(operand);
      if (!loweredValue) {
        // If this is a zero bit operand, just pass a one bit zero.
        if (!isZeroBitFIRRTLType(operand.getType()))
          return failure();
        loweredValue = getOrCreateIntConstant(1, 0);
      }
      // Wrap any message ops in $sampled() to guarantee that these will print
      // with the same value as when the assertion triggers.  (See SystemVerilog
      // 2017 spec section 16.9.3 for more information.)  The custom
      // "ifElseFatal" variant is special cased because this isn't actually a
      // concurrent assertion.
      auto format = op->getAttrOfType<StringAttr>("format");
      if (isConcurrent && (!format || format.getValue() != "ifElseFatal" ||
                           circuitState.emitChiselAssertsAsSVA))
        loweredValue = builder.create<sv::SampledOp>(loweredValue);
      messageOps.push_back(loweredValue);
    }
  }

  auto emit = [&]() {
    // Handle the purely procedural flavor of the operation.
    if (!isConcurrent) {
      auto deferImmediate = circt::sv::DeferAssertAttr::get(
          builder.getContext(), circt::sv::DeferAssert::Immediate);
      addToAlwaysBlock(clock, [&]() {
        addIfProceduralBlock(enable, [&]() {
          buildImmediateVerifOp(builder, opName, predicate, deferImmediate,
                                prefixedLabel, message, messageOps);
        });
      });
      return;
    }

    auto boolType = IntegerType::get(builder.getContext(), 1);

    // Handle the `ifElseFatal` format, which does not emit an SVA but
    // rather a process that uses $error and $fatal to perform the checks.
    // TODO: This should *not* be part of the op, but rather a lowering
    // option that the user of this pass can choose.
    auto format = op->template getAttrOfType<StringAttr>("format");
    if (format && (format.getValue() == "ifElseFatal" &&
                   !circuitState.emitChiselAssertsAsSVA)) {
      predicate = comb::createOrFoldNot(predicate, builder);
      predicate = builder.createOrFold<comb::AndOp>(enable, predicate, true);
      addToIfDefBlock("SYNTHESIS", {}, [&]() {
        addToAlwaysBlock(clock, [&]() {
          addIfProceduralBlock(predicate, [&]() {
            circuitState.used_ASSERT_VERBOSE_COND = true;
            circuitState.used_STOP_COND = true;
            addIfProceduralBlock(
                builder.create<sv::MacroRefExprOp>(boolType,
                                                   "ASSERT_VERBOSE_COND_"),
                [&]() { builder.create<sv::ErrorOp>(message, messageOps); });
            addIfProceduralBlock(
                builder.create<sv::MacroRefExprOp>(boolType, "STOP_COND_"),
                [&]() { builder.create<sv::FatalOp>(); });
          });
        });
      });
      return;
    }

    // Formulate the `enable -> predicate` as `!enable | predicate`.
    // Except for covers, combine them: enable & predicate
    if (!isCover) {
      auto notEnable = comb::createOrFoldNot(enable, builder);
      predicate = builder.createOrFold<comb::OrOp>(notEnable, predicate, true);
    } else {
      predicate = builder.createOrFold<comb::AndOp>(enable, predicate, true);
    }

    // Handle the regular SVA case.
    sv::EventControl event;
    switch (opEventControl) {
    case EventControl::AtPosEdge:
      event = circt::sv::EventControl::AtPosEdge;
      break;
    case EventControl::AtEdge:
      event = circt::sv::EventControl::AtEdge;
      break;
    case EventControl::AtNegEdge:
      event = circt::sv::EventControl::AtNegEdge;
      break;
    }

    buildConcurrentVerifOp(
        builder, opName,
        circt::sv::EventControlAttr::get(builder.getContext(), event), clock,
        predicate, prefixedLabel, message, messageOps);

    // Assertions gain a companion `assume` behind a
    // `USE_PROPERTY_AS_CONSTRAINT` guard.
    if (isAssert) {
      StringAttr assumeLabel;
      if (label)
        assumeLabel = StringAttr::get(builder.getContext(),
                                      "assume__" + label.getValue());
      addToIfDefBlock("USE_PROPERTY_AS_CONSTRAINT", [&]() {
        builder.create<sv::AssumeConcurrentOp>(
            circt::sv::EventControlAttr::get(builder.getContext(), event),
            clock, predicate, assumeLabel);
      });
    }
  };

  // Wrap the verification statement up in the optional preprocessor
  // guards. This is a bit awkward since we want to translate an array of
  // guards into a recursive call to `addToIfDefBlock`.
  ArrayRef<Attribute> guards{};
  if (auto guardsAttr = op->template getAttrOfType<ArrayAttr>("guards"))
    guards = guardsAttr.getValue();
  bool anyFailed = false;
  std::function<void()> emitWrapped = [&]() {
    if (guards.empty()) {
      emit();
      return;
    }
    auto guard = guards[0].dyn_cast<StringAttr>();
    if (!guard) {
      op->emitOpError("elements in `guards` array must be `StringAttr`");
      anyFailed = true;
      return;
    }
    guards = guards.drop_front();
    addToIfDefBlock(guard.getValue(), emitWrapped);
  };
  emitWrapped();
  if (anyFailed)
    return failure();

  return success();
}

// Lower an assert to SystemVerilog.
LogicalResult FIRRTLLowering::visitStmt(AssertOp op) {
  return lowerVerificationStatement(
      op, "assert__", op.getClock(), op.getPredicate(), op.getEnable(),
      op.getMessageAttr(), op.getSubstitutions(), op.getNameAttr(),
      op.getIsConcurrent(), op.getEventControl());
}

// Lower an assume to SystemVerilog.
LogicalResult FIRRTLLowering::visitStmt(AssumeOp op) {
  return lowerVerificationStatement(
      op, "assume__", op.getClock(), op.getPredicate(), op.getEnable(),
      op.getMessageAttr(), op.getSubstitutions(), op.getNameAttr(),
      op.getIsConcurrent(), op.getEventControl());
}

// Lower a cover to SystemVerilog.
LogicalResult FIRRTLLowering::visitStmt(CoverOp op) {
  return lowerVerificationStatement(
      op, "cover__", op.getClock(), op.getPredicate(), op.getEnable(),
      op.getMessageAttr(), op.getSubstitutions(), op.getNameAttr(),
      op.getIsConcurrent(), op.getEventControl());
}

LogicalResult FIRRTLLowering::visitStmt(AttachOp op) {
  // Don't emit anything for a zero or one operand attach.
  if (op.getAttached().size() < 2)
    return success();

  SmallVector<Value, 4> inoutValues;
  for (auto v : op.getAttached()) {
    inoutValues.push_back(getPossiblyInoutLoweredValue(v));
    if (!inoutValues.back()) {
      // Ignore zero bit values.
      if (!isZeroBitFIRRTLType(v.getType()))
        return failure();
      inoutValues.pop_back();
      continue;
    }

    if (!inoutValues.back().getType().isa<hw::InOutType>())
      return op.emitError("operand isn't an inout type");
  }

  if (inoutValues.size() < 2)
    return success();

  // If the op has a single source value, the value is used as a lowering result
  // of other values. Therefore we can delete the attach op here.
  if (getSingleNonInstanceOperand(op))
    return success();

  addToIfDefBlock(
      "SYNTHESIS",
      // If we're doing synthesis, we emit an all-pairs assign complex.
      [&]() {
        SmallVector<Value, 4> values;
        for (size_t i = 0, e = inoutValues.size(); i != e; ++i)
          values.push_back(getReadValue(inoutValues[i]));

        for (size_t i1 = 0, e = inoutValues.size(); i1 != e; ++i1) {
          for (size_t i2 = 0; i2 != e; ++i2)
            if (i1 != i2)
              builder.create<sv::AssignOp>(inoutValues[i1], values[i2]);
        }
      },
      // In the non-synthesis case, we emit a SystemVerilog alias
      // statement.
      [&]() {
        builder.create<sv::IfDefOp>(
            "verilator",
            [&]() {
              builder.create<sv::VerbatimOp>(
                  "`error \"Verilator does not support alias and thus "
                  "cannot "
                  "arbitrarily connect bidirectional wires and ports\"");
            },
            [&]() { builder.create<sv::AliasOp>(inoutValues); });
      });

  return success();
}

LogicalResult FIRRTLLowering::visitStmt(ProbeOp op) {
  SmallVector<Value, 4> operands;
  operands.reserve(op.getCaptured().size());
  for (auto operand : op.getCaptured()) {
    operands.push_back(getLoweredValue(operand));
    if (!operands.back()) {
      // If this is a zero bit operand, just pass a one bit zero.
      if (!isZeroBitFIRRTLType(operand.getType()))
        return failure();
      operands.back() = getOrCreateIntConstant(1, 0);
    }
  }

  builder.create<hw::ProbeOp>(op.getInnerSym(), operands);

  return success();
}
