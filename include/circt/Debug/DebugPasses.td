//===-- DebugPasses.td - Debug pass definition file --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions for passes that work with debugging
// information.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DEBUG_DEBUGPASSES
#define CIRCT_DEBUG_DEBUGPASSES

include "mlir/Pass/PassBase.td"

def HWExportHGDB: Pass<"hgdb", "mlir::ModuleOp"> {
  let summary = "Export hgdb symbol table";
  let description = [{
    This pass exports the symbol table for any constructs marked with
    hw.debug.name
  }];

  let constructor = "circt::debug::createExportHGDBPass()";
  let dependentDialects = ["circt::sv::SVDialect", "circt::comb::CombDialect",
                           "circt::hw::HWDialect"];

  let options = [
    Option<"filename", "filename", "std::string", "\"-\"",
           "Symbol table output">
  ];
}

#endif // CIRCT_DEBUG_DEBUGPASSES