//===- InitAllPasses.h - CIRCT Global Pass Registration ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a helper to trigger the registration of all passes to the
// system.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_INITALLPASSES_H_
#define CIRCT_INITALLPASSES_H_

#include "circt/Conversion/ExportVerilog.h"
#include "circt/Conversion/Passes.h"
#include "circt/Dialect/Calyx/CalyxPasses.h"
#include "circt/Dialect/ESI/ESIDialect.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/FSM/FSMPasses.h"
#include "circt/Dialect/HW/HWPasses.h"
#include "circt/Dialect/Handshake/HandshakePasses.h"
#include "circt/Dialect/LLHD/Transforms/Passes.h"
#include "circt/Dialect/MSFT/MSFTPasses.h"
#include "circt/Dialect/Pipeline/PipelinePasses.h"
#include "circt/Dialect/SV/SVPasses.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "circt/Dialect/SystemC/SystemCPasses.h"
#include "circt/Transforms/Passes.h"
#include "circt/Debug/HWDebug.h"

namespace circt {

inline void registerAllPasses() {
  // Conversion Passes
  registerConversionPasses();

  // Transformation passes
  registerTransformsPasses();

  // Standard Passes
  calyx::registerPasses();
  esi::registerESIPasses();
  firrtl::registerPasses();
  fsm::registerPasses();
  llhd::initLLHDTransformationPasses();
  msft::registerPasses();
  seq::registerPasses();
  sv::registerPasses();
  handshake::registerPasses();
  hw::registerPasses();
  pipeline::registerPasses();
  systemc::registerPasses();

  // Debug pass
  debug::registerPasses();
}

} // namespace circt

#endif // CIRCT_INITALLPASSES_H_
