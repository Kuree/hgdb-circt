//===-- SeqPasses.td - Seq pass definition file ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions for passes that work on the Seq dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_SEQ_SEQPASSES
#define CIRCT_DIALECT_SEQ_SEQPASSES

include "mlir/Pass/PassBase.td"

def LowerSeqToSV: Pass<"lower-seq-to-sv", "mlir::ModuleOp"> {
  let summary = "Lower sequential ops to SV.";
  let constructor = "circt::seq::createSeqLowerToSVPass()";
  let dependentDialects = ["circt::sv::SVDialect"];
}

def LowerSeqFIRRTLToSV: Pass<"lower-seq-firrtl-to-sv", "hw::HWModuleOp"> {
  let summary = "Lower sequential firrtl ops to SV.";
  let constructor = "circt::seq::createSeqFIRRTLLowerToSVPass()";
  let dependentDialects = ["circt::sv::SVDialect"];
  let options = [
    Option<"disableRegRandomization", "disable-reg-randomization", "bool", "false",
            "Disable emission of register randomization code">,
    Option<"addVivadoRAMAddressConflictSynthesisBugWorkaround",
           "add-vivado-ram-address-conflict-synthesis-bug-workaround", "bool", "false",
            "Add a vivado attribute to specify a ram style of array registers">
  ];
}

def LowerSeqHLMem: Pass<"lower-seq-hlmem", "hw::HWModuleOp"> {
  let summary = "Lowers seq.hlmem operations.";
  let constructor = "circt::seq::createLowerSeqHLMemPass()";
  let dependentDialects = ["circt::sv::SVDialect"];
}

#endif // CIRCT_DIALECT_SEQ_SEQPASSES
