add_circt_translation_library(CIRCTDebug
  HWDebug.cpp

  ADDITIONAL_HEADER_DIRS

  DEPENDS
  CIRCTConversionPassIncGen
  CIRCTDebugIncGen

  LINK_COMPONENTS
  Core

  LINK_LIBS PUBLIC
  CIRCTExportVerilog
  )