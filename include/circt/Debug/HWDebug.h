#ifndef CIRCT_DEBUG_HWDEBUG_H
#define CIRCT_DEBUG_HWDEBUG_H

#include "mlir/Pass/Pass.h"

namespace circt::debug {
std::unique_ptr<mlir::Pass>
createExportHGDBPass(Optional<std::string> filename = {});

#define GEN_PASS_REGISTRATION
#include "circt/Debug/DebugPasses.h.inc"

} // namespace circt::debug

#endif // CIRCT_DEBUG_HWDEBUG_H
