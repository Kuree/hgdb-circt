set(LLVM_TARGET_DEFINITIONS DebugPasses.td)
mlir_tablegen(DebugPasses.h.inc -gen-pass-decls)
add_public_tablegen_target(CIRCTDebugIncGen)
add_circt_doc(DebugPasses -gen-pass-doc DebugPasses ./)
