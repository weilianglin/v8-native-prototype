// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_ASM_WASM_BUILDER_H_
#define V8_WASM_ASM_WASM_BUILDER_H_

#include "src/allocation.h"
#include "src/zone.h"
#include "src/wasm/encoder.h"

namespace v8 {
namespace internal {

class FunctionLiteral;

namespace wasm {

class AsmWasmBuilder {
 public:
  explicit AsmWasmBuilder(Isolate* isolate, Zone* zone, FunctionLiteral* root);
  WasmModuleIndex* Run();

 private:
  Isolate* isolate_;
  Zone* zone_;
  FunctionLiteral* literal_;
};
}
}
}

#endif  // V8_WASM_ASM_WASM_BUILDER_H_
