//===- ESITypes.td - ESI type specifications ---------------*- tablegen -*-===//
//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ESI will have a rich, high level type system. Currently, it is much more
// minimal.
//
//===----------------------------------------------------------------------===//

class ESI_Type<string name> : TypeDef<ESI_Dialect, name> { }

def ESIAnyType : ESI_Type<"Any"> {
  let summary = "any type";
  let description = [{
    Used to state that any type is accepted. The specific type will be
    determined later in compilation.
  }];

  let mnemonic = "any";
  let assemblyFormat = "";

  let extraClassDeclaration = [{
    static AnyType get(MLIRContext *context);
  }];
}
