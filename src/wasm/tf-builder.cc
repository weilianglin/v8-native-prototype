// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/common-operator.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/simplified-operator.h"
#include "src/compiler/simplified-lowering.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/diamond.h"

#include "src/compiler/access-builder.h"

#include "src/code-stubs.h"
#include "src/code-factory.h"

#include "src/compiler/linkage.h"

#include "src/wasm/tf-builder.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-opcodes.h"

// TODO(titzer): pull WASM_64 up to a common header.
#if !V8_TARGET_ARCH_32_BIT || V8_TARGET_ARCH_X64
#define WASM_64 1
#else
#define WASM_64 0
#endif

namespace v8 {
namespace internal {
namespace wasm {

namespace {
void MergeControlToEnd(TFGraph* graph, TFNode* node) {
  compiler::Graph* g = graph->graph();
  if (g->end()) {
    compiler::NodeProperties::MergeControlToEnd(g, graph->common(), node);
  } else {
    g->SetEnd(g->NewNode(graph->common()->End(1), node));
  }
}

enum TrapReason {
  kTrapUnreachable,
  kTrapMemOutOfBounds,
  kTrapDivByZero,
  kTrapDivUnrepresentable,
  kTrapRemByZero,
  kTrapFloatUnrepresentable,
  kTrapFuncInvalid,
  kTrapFuncSigMismatch,
  kTrapCount
};

static const char* kTrapMessages[] = {
  "unreachable",
  "memory access out of bounds",
  "divide by zero",
  "divide result unrepresentable", 
  "remainder by zero",
  "integer result unrepresentable",
  "invalid function",
  "function signature mismatch"
};

compiler::MachineType MachineTypeFor(LocalType type) {
  switch (type) {
    case kAstI32:
      return compiler::kMachInt32;
    case kAstI64:
      return compiler::kMachInt64;
    case kAstF64:
      return compiler::kMachFloat64;
    case kAstF32:
      return compiler::kMachFloat32;
    default:
      UNREACHABLE();
      return compiler::kMachAnyTagged;
  }
}

compiler::MachineType MachineTypeFor(MemType type) {
  switch (type) {
    case kMemI8:
      return compiler::kMachInt8;
    case kMemI16:
      return compiler::kMachInt16;
    case kMemI32:
      return compiler::kMachInt32;
    case kMemI64:
      return compiler::kMachInt64;
    case kMemU8:
      return compiler::kMachUint8;
    case kMemU16:
      return compiler::kMachUint16;
    case kMemU32:
      return compiler::kMachUint32;
    case kMemU64:
      return compiler::kMachUint64;
    case kMemF64:
      return compiler::kMachFloat64;
    case kMemF32:
      return compiler::kMachFloat32;
    default:
      UNREACHABLE();
      return compiler::kMachAnyTagged;
  }
}
}  // namespace


// A helper that handles building graph fragments for trapping.
// To avoid generating a ton of redundant code that just calls the runtime
// to trap, we generate a per-trap-reason block of code that all trap sites
// in this function will branch to.
class TFTrapHelper : public ZoneObject{
 public:
  explicit TFTrapHelper(TFBuilder* b) :
    builder(b),
    graph(b->graph),
    g(b->graph ? b->graph->graph() : nullptr) {

    for (int i = 0; i < kTrapCount; i++) traps[i] = nullptr;
  }

  // Make the current control path trap to unreachable.
  void Unreachable() {
    ConnectTrap(kTrapUnreachable);
  }
  // Add a check that traps if {node} is equal to {val}.
  TFNode* TrapIfEq32(TrapReason reason, TFNode* node, int32_t val) {
    compiler::Int32Matcher m(node);
    if (m.HasValue() && !m.Is(val)) return g->start();
    if (val == 0) {
      AddTrapIfFalse(reason, node);
    } else {
      AddTrapIfTrue(reason, g->NewNode(graph->machine()->Word32Equal(),
                                       node, graph->Int32Constant(val)));
    }
    return *(builder->control);
  }
  // Add a check that traps if {node} is zero.
  TFNode* ZeroCheck32(TrapReason reason, TFNode* node) {
    return TrapIfEq32(reason, node, 0);
  }
  // Add a check that traps if {node} is equal to {val}.
  TFNode* TrapIfEq64(TrapReason reason, TFNode* node, int64_t val) {
    compiler::Int64Matcher m(node);
    if (m.HasValue() && !m.Is(val)) return g->start();
    AddTrapIfTrue(reason, g->NewNode(graph->machine()->Word64Equal(),
                                     node, graph->Int64Constant(val)));
    return *(builder->control);
  }
  // Add a check that traps if {node} is zero.
  TFNode* ZeroCheck64(TrapReason reason, TFNode* node) {
    return TrapIfEq64(reason, node, 0);
  }
  // Add a trap if {cond} is true.
  void AddTrapIfTrue(TrapReason reason, TFNode* cond) {
    AddTrapIf(reason, cond, true);
  }
  // Add a trap if {cond} is false.
  void AddTrapIfFalse(TrapReason reason, TFNode* cond) {
    AddTrapIf(reason, cond, false);
  }
  // Add a trap if {cond} is true or false according to {iftrue}.
  void AddTrapIf(TrapReason reason, TFNode* cond, bool iftrue) {
    DCHECK_NOT_NULL(graph);
    TFNode** effect = builder->effect;
    TFNode** control = builder->control;
    TFNode* before = *effect;
    compiler::BranchHint hint = iftrue ? compiler::BranchHint::kFalse :
      compiler::BranchHint::kTrue;
    TFNode* branch = g->NewNode(graph->common()->Branch(hint), cond,
                                *(builder->control));
    TFNode* if_true = g->NewNode(graph->common()->IfTrue(), branch);
    TFNode* if_false = g->NewNode(graph->common()->IfFalse(), branch);
    
    *control = iftrue ? if_true : if_false;
    ConnectTrap(reason);
    *control = iftrue ? if_false : if_true;
    *effect = before;
  }

 private:
  TFBuilder* builder;
  TFGraph* graph;
  compiler::Graph* g;
  TFNode* traps[kTrapCount];
  TFNode* effects[kTrapCount];

  void ConnectTrap(TrapReason reason) {
    if (traps[reason] == nullptr) {
      // Create trap code for the first time this trap is used.
      return BuildTrapCode(reason);
    }
    // Connect the current control and effect to the existing trap code.
    builder->AppendToMerge(traps[reason], *(builder->control));
    builder->AppendToPhi(traps[reason], effects[reason], *(builder->effect));
  }

  void BuildTrapCode(TrapReason reason) {
    TFNode* exception = builder->String(kTrapMessages[reason]);
    TFNode* end;
    TFNode** control = builder->control;
    TFNode** effect = builder->effect;
    ModuleEnv* module = builder->module;
    *control = traps[reason] = g->NewNode(graph->common()->Merge(1), *control);
    *effect = effects[reason] = g->NewNode(graph->common()->EffectPhi(1),
                                           *effect, *control);

    if (module && !module->context.is_null()) {
      // Use the module context to call the runtime to throw an exception.
      Runtime::FunctionId f = Runtime::kThrow;
      const Runtime::Function* fun = Runtime::FunctionForId(f);
      compiler::CallDescriptor* desc =
        compiler::Linkage::GetRuntimeCallDescriptor(graph->zone(), f, fun->nargs,
                                                    compiler::Operator::kNoProperties);
      TFNode* inputs[] = {
        graph->CEntryStubConstant(fun->result_size),                     // C entry
        exception,                                                       // exception
        graph->ExternalConstant(ExternalReference(f, graph->isolate())), // ref
        graph->Int32Constant(fun->nargs),                                // arity
        graph->Constant(module->context),                                // context
        graph->EmptyFrameState(),
        *effect,
        *control
      };
      
      TFNode* node = g->NewNode(graph->common()->Call(desc),
                                static_cast<int>(arraysize(inputs)), inputs);
      *control = node;
      *effect = node;
      
    }
    if (false) {
      // End the control flow with a throw
      TFNode* thrw = g->NewNode(graph->common()->Throw(),
                                graph->ZeroConstant(), *effect, *control);
      end = thrw;
    } else {
      // End the control flow with returning 0xdeadbeef
      TFNode* ret_dead =
        g->NewNode(graph->common()->Return(), graph->Int32Constant(0xdeadbeef),
                   *effect, *control);
      end = ret_dead;
    }
    
    MergeControlToEnd(graph, end);
  }
};




TFBuilder::TFBuilder(Zone* z, TFGraph* g)
    : zone(z),
      graph(g),
      module(nullptr),
      mem_buffer(nullptr),
      mem_size(nullptr),
      function_table(nullptr),
      control(nullptr),
      effect(nullptr),
      cur_buffer(def_buffer),
      cur_bufsize(kDefaultBufferSize),
      trap(new (z) TFTrapHelper(this)) {
}

TFNode* TFBuilder::Error() {
  DCHECK_NOT_NULL(graph);
  return graph->Dead();
}

TFNode* TFBuilder::Start(unsigned params) {
  DCHECK_NOT_NULL(graph);
  compiler::Graph* g = graph->graph();
  TFNode* start = g->NewNode(graph->common()->Start(params));
  g->SetStart(start);
  return start;
}

TFNode* TFBuilder::Param(unsigned index, LocalType type) {
  DCHECK_NOT_NULL(graph);
  compiler::Graph* g = graph->graph();
  // TODO(titzer): use LocalType for parameters
  return g->NewNode(graph->common()->Parameter(index), g->start());
}

TFNode* TFBuilder::Loop(TFNode* entry) {
  DCHECK_NOT_NULL(graph);
  return graph->graph()->NewNode(graph->common()->Loop(1), entry);
}

TFNode* TFBuilder::Terminate(TFNode* effect, TFNode* control) {
  DCHECK_NOT_NULL(graph);
  TFNode* terminate =
      graph->graph()->NewNode(graph->common()->Terminate(), effect, control);
  MergeControlToEnd(graph, terminate);
  return terminate;
}

unsigned TFBuilder::InputCount(TFNode* node) {
  return static_cast<unsigned>(node->InputCount());
}

bool TFBuilder::IsPhiWithMerge(TFNode* phi, TFNode* merge) {
  return phi && compiler::IrOpcode::IsPhiOpcode(phi->opcode()) &&
         compiler::NodeProperties::GetControlInput(phi) == merge;
}

void TFBuilder::AppendToMerge(TFNode* merge, TFNode* from) {
  DCHECK_NOT_NULL(graph);
  DCHECK(compiler::IrOpcode::IsMergeOpcode(merge->opcode()));
  merge->AppendInput(graph->zone(), from);
  int new_size = merge->InputCount();
  compiler::NodeProperties::ChangeOp(
      merge, graph->common()->ResizeMergeOrPhi(merge->op(), new_size));
}

void TFBuilder::AppendToPhi(TFNode* merge, TFNode* phi, TFNode* from) {
  DCHECK_NOT_NULL(graph);
  DCHECK(compiler::IrOpcode::IsPhiOpcode(phi->opcode()));
  DCHECK(compiler::IrOpcode::IsMergeOpcode(merge->opcode()));
  int new_size = phi->InputCount();
  phi->InsertInput(graph->zone(), phi->InputCount() - 1, from);
  compiler::NodeProperties::ChangeOp(
      phi, graph->common()->ResizeMergeOrPhi(phi->op(), new_size));
}

TFNode* TFBuilder::Merge(unsigned count, TFNode** controls) {
  DCHECK_NOT_NULL(graph);
  return graph->graph()->NewNode(graph->common()->Merge(count), count,
                                 controls);
}

TFNode* TFBuilder::Phi(LocalType type,
                       unsigned count,
                       TFNode** vals,
                       TFNode* control) {
  DCHECK(compiler::IrOpcode::IsMergeOpcode(control->opcode()));
  TFNode** buf = Realloc(vals, count + 1);
  buf[count] = control;
  compiler::MachineType machine_type = MachineTypeFor(type);
  return graph->graph()->NewNode(graph->common()->Phi(machine_type, count),
                                 count + 1, buf);
}

TFNode* TFBuilder::EffectPhi(unsigned count,
                             TFNode** effects,
                             TFNode* control) {
  DCHECK_NOT_NULL(graph);
  DCHECK(compiler::IrOpcode::IsMergeOpcode(control->opcode()));
  TFNode** buf = Realloc(effects, count + 1);
  buf[count] = control;
  return graph->graph()->NewNode(graph->common()->EffectPhi(count), count + 1,
                                 buf);
}

TFNode* TFBuilder::Int32Constant(int32_t value) {
  DCHECK_NOT_NULL(graph);
  return graph->Int32Constant(value);
}

TFNode* TFBuilder::Int64Constant(int64_t value) {
  DCHECK_NOT_NULL(graph);
  return graph->Int64Constant(value);
}

static const compiler::Operator* UnsupportedOpcode(WasmOpcode opcode) {
  if (WasmOpcodes::IsSupported(opcode)) {
    V8_Fatal(__FILE__, __LINE__,
             "Unsupported opcode #%d:%s reported as supported", opcode,
             WasmOpcodes::OpcodeName(opcode));
  }
  V8_Fatal(__FILE__, __LINE__, "Unsupported opcode #%d:%s", opcode,
           WasmOpcodes::OpcodeName(opcode));
  return nullptr;
}

TFNode* TFBuilder::Binop(WasmOpcode opcode, TFNode* left, TFNode* right) {
  // TODO(titzer): insert manual divide-by-zero checks.
  DCHECK_NOT_NULL(graph);
  const compiler::Operator* op;
  compiler::MachineOperatorBuilder* m = graph->machine();
  switch (opcode) {
    case kExprI32Add:
      op = m->Int32Add();
      break;
    case kExprI32Sub:
      op = m->Int32Sub();
      break;
    case kExprI32Mul:
      op = m->Int32Mul();
      break;
    case kExprI32DivS: {
      trap->ZeroCheck32(kTrapDivByZero, right);
      TFNode* before = *control;
      TFNode* denom_is_m1;
      TFNode* denom_is_not_m1;
      Branch(graph->graph()->NewNode(graph->machine()->Word32Equal(),
                                     right, graph->Int32Constant(-1)),
             &denom_is_m1, &denom_is_not_m1);
      *control = denom_is_m1;
      trap->TrapIfEq32(kTrapDivUnrepresentable, left, kMinInt);
      if (*control != denom_is_m1) {
        *control = graph->graph()->NewNode(graph->common()->Merge(2),
                                           denom_is_not_m1, *control);
      } else {
        *control = before;
      }
      return graph->graph()->NewNode(m->Int32Div(), left, right, *control);
    }
    case kExprI32DivU:
      op = m->Uint32Div();
      return graph->graph()->NewNode(op, left, right,
                                     trap->ZeroCheck32(kTrapDivByZero, right));
    case kExprI32RemS: {
      trap->ZeroCheck32(kTrapRemByZero, right);
      compiler::Diamond d(graph->graph(), graph->common(),
                          graph->graph()->NewNode(graph->machine()->Word32Equal(),
                                                  right, graph->Int32Constant(-1)));

      TFNode* rem = graph->graph()->NewNode(m->Int32Mod(), left, right,
                                            d.if_false);

      return d.Phi(compiler::kMachInt32, graph->Int32Constant(0), rem);
    }
    case kExprI32RemU:
      op = m->Uint32Mod();
      return graph->graph()->NewNode(op, left, right,
                                     trap->ZeroCheck32(kTrapRemByZero, right));
    case kExprI32And:
      op = m->Word32And();
      break;
    case kExprI32Ior:
      op = m->Word32Or();
      break;
    case kExprI32Xor:
      op = m->Word32Xor();
      break;
    case kExprI32Shl:
      op = m->Word32Shl();
      break;
    case kExprI32ShrU:
      op = m->Word32Shr();
      break;
    case kExprI32ShrS:
      op = m->Word32Sar();
      break;
    case kExprI32Eq:
      op = m->Word32Equal();
      break;
    case kExprI32Ne:
      return Invert(Binop(kExprI32Eq, left, right));
    case kExprI32LtS:
      op = m->Int32LessThan();
      break;
    case kExprI32LeS:
      op = m->Int32LessThanOrEqual();
      break;
    case kExprI32LtU:
      op = m->Uint32LessThan();
      break;
    case kExprI32LeU:
      op = m->Uint32LessThanOrEqual();
      break;
    case kExprI32GtS:
      op = m->Int32LessThan();
      std::swap(left, right);
      break;
    case kExprI32GeS:
      op = m->Int32LessThanOrEqual();
      std::swap(left, right);
      break;
    case kExprI32GtU:
      op = m->Uint32LessThan();
      std::swap(left, right);
      break;
    case kExprI32GeU:
      op = m->Uint32LessThanOrEqual();
      std::swap(left, right);
      break;
#if WASM_64
    // Opcodes only supported on 64-bit platforms.
    // TODO(titzer): query the machine operator builder here instead of #ifdef.
    case kExprI64Add:
      op = m->Int64Add();
      break;
    case kExprI64Sub:
      op = m->Int64Sub();
      break;
    case kExprI64Mul:
      op = m->Int64Mul();
      break;
    case kExprI64DivS: {
      trap->ZeroCheck64(kTrapDivByZero, right);
      TFNode* before = *control;
      TFNode* denom_is_m1;
      TFNode* denom_is_not_m1;
      Branch(graph->graph()->NewNode(graph->machine()->Word64Equal(),
                                     right, graph->Int64Constant(-1)),
             &denom_is_m1, &denom_is_not_m1);
      *control = denom_is_m1;
      trap->TrapIfEq64(kTrapDivUnrepresentable, left,
                       std::numeric_limits<int64_t>::min());
      if (*control != denom_is_m1) {
        *control = graph->graph()->NewNode(graph->common()->Merge(2),
                                           denom_is_not_m1, *control);
      } else {
        *control = before;
      }
      return graph->graph()->NewNode(m->Int64Div(), left, right, *control);
    }
    case kExprI64DivU:
      op = m->Uint64Div();
      return graph->graph()->NewNode(op, left, right,
                                     trap->ZeroCheck64(kTrapDivByZero, right));
    case kExprI64RemS: {
      trap->ZeroCheck64(kTrapRemByZero, right);
      compiler::Diamond d(graph->graph(), graph->common(),
                          graph->graph()->NewNode(graph->machine()->Word64Equal(),
                                                  right, graph->Int64Constant(-1)));

      TFNode* rem = graph->graph()->NewNode(m->Int64Mod(), left, right,
                                            d.if_false);

      return d.Phi(compiler::kMachInt64, graph->Int64Constant(0), rem);
    }
    case kExprI64RemU:
      op = m->Uint64Mod();
      return graph->graph()->NewNode(op, left, right, 
                                     trap->ZeroCheck64(kTrapRemByZero, right));
    case kExprI64And:
      op = m->Word64And();
      break;
    case kExprI64Ior:
      op = m->Word64Or();
      break;
    case kExprI64Xor:
      op = m->Word64Xor();
      break;
    case kExprI64Shl:
      op = m->Word64Shl();
      break;
    case kExprI64ShrU:
      op = m->Word64Shr();
      break;
    case kExprI64ShrS:
      op = m->Word64Sar();
      break;
    case kExprI64Eq:
      op = m->Word64Equal();
      break;
    case kExprI64Ne:
      return Invert(Binop(kExprI64Eq, left, right));
    case kExprI64LtS:
      op = m->Int64LessThan();
      break;
    case kExprI64LeS:
      op = m->Int64LessThanOrEqual();
      break;
    case kExprI64LtU:
      op = m->Uint64LessThan();
      break;
    case kExprI64LeU:
      op = m->Uint64LessThanOrEqual();
      break;
    case kExprI64GtS:
      op = m->Int64LessThan();
      std::swap(left, right);
      break;
    case kExprI64GeS:
      op = m->Int64LessThanOrEqual();
      std::swap(left, right);
      break;
    case kExprI64GtU:
      op = m->Uint64LessThan();
      std::swap(left, right);
      break;
    case kExprI64GeU:
      op = m->Uint64LessThanOrEqual();
      std::swap(left, right);
      break;
#endif

    case kExprF32CopySign:
      return MakeF32CopySign(left, right);
    case kExprF64CopySign:
      return MakeF64CopySign(left, right);
    case kExprF32Add:
      op = m->Float32Add();
      break;
    case kExprF32Sub:
      op = m->Float32Sub();
      break;
    case kExprF32Mul:
      op = m->Float32Mul();
      break;
    case kExprF32Div:
      op = m->Float32Div();
      break;
    case kExprF32Eq:
      op = m->Float32Equal();
      break;
    case kExprF32Ne:
      return Invert(Binop(kExprF32Eq, left, right));
    case kExprF32Lt:
      op = m->Float32LessThan();
      break;
    case kExprF32Ge:
      op = m->Float32LessThanOrEqual();
      std::swap(left, right);
      break;
    case kExprF32Gt:
      op = m->Float32LessThan();
      std::swap(left, right);
      break;
    case kExprF32Le:
      op = m->Float32LessThanOrEqual();
      break;
    case kExprF64Add:
      op = m->Float64Add();
      break;
    case kExprF64Sub:
      op = m->Float64Sub();
      break;
    case kExprF64Mul:
      op = m->Float64Mul();
      break;
    case kExprF64Div:
      op = m->Float64Div();
      break;
    case kExprF64Eq:
      op = m->Float64Equal();
      break;
    case kExprF64Ne:
      return Invert(Binop(kExprF64Eq, left, right));
    case kExprF64Lt:
      op = m->Float64LessThan();
      break;
    case kExprF64Le:
      op = m->Float64LessThanOrEqual();
      break;
    case kExprF64Gt:
      op = m->Float64LessThan();
      std::swap(left, right);
      break;
    case kExprF64Ge:
      op = m->Float64LessThanOrEqual();
      std::swap(left, right);
      break;
    case kExprF32Min: {
      if (m->Float32Min().IsSupported()) {
        op = m->Float32Min().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF64Min: {
      if (m->Float64Min().IsSupported()) {
        op = m->Float64Min().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF32Max: {
      if (m->Float32Max().IsSupported()) {
        op = m->Float32Max().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF64Max: {
      if (m->Float64Max().IsSupported()) {
        op = m->Float64Max().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    default:
      op = UnsupportedOpcode(opcode);
  }
  return graph->graph()->NewNode(op, left, right);
}

TFNode* TFBuilder::Unop(WasmOpcode opcode, TFNode* input) {
  DCHECK_NOT_NULL(graph);
  const compiler::Operator* op;
  compiler::MachineOperatorBuilder* m = graph->machine();
  switch (opcode) {
    case kExprBoolNot:
      op = m->Word32Equal();
      return graph->graph()->NewNode(op, input, graph->Int32Constant(0));
    case kExprF32Abs:
      op = m->Float32Abs();
      break;
    case kExprF32Neg:
      op = m->Float32Sub();
      return graph->graph()->NewNode(op, graph->Float32Constant(0), input);
    case kExprF32Sqrt:
      op = m->Float32Sqrt();
      break;
    case kExprF64Abs:
      op = m->Float64Abs();
      break;
    case kExprF64Neg:
      op = m->Float64Sub();
      return graph->graph()->NewNode(op, graph->Float64Constant(0), input);
    case kExprF64Sqrt:
      op = m->Float64Sqrt();
      break;
    case kExprI32SConvertF64:
      op = m->ChangeFloat64ToInt32();
      break;
    case kExprI32UConvertF64:
      op = m->ChangeFloat64ToUint32();
      break;
    case kExprF32ConvertF64:
      op = m->TruncateFloat64ToFloat32();
      break;
    case kExprF64SConvertI32:
      op = m->ChangeInt32ToFloat64();
      break;
    case kExprF64UConvertI32:
      op = m->ChangeUint32ToFloat64();
      break;
    case kExprF32SConvertI32:
      op = m->ChangeInt32ToFloat64();  // TODO(titzer): two conversions
      input = graph->graph()->NewNode(op, input);
      op = m->TruncateFloat64ToFloat32();
      break;
    case kExprF32UConvertI32:
      op = m->ChangeUint32ToFloat64();  // TODO(titzer): two conversions
      input = graph->graph()->NewNode(op, input);
      op = m->TruncateFloat64ToFloat32();
      break;
    case kExprI32SConvertF32:
      op = m->ChangeFloat32ToFloat64();  // TODO(titzer): two conversions
      input = graph->graph()->NewNode(op, input);
      op = m->ChangeFloat64ToInt32();
      break;
    case kExprI32UConvertF32:
      op = m->ChangeFloat32ToFloat64();  // TODO(titzer): two conversions
      input = graph->graph()->NewNode(op, input);
      op = m->ChangeFloat64ToUint32();
      break;
    case kExprF64ConvertF32:
      op = m->ChangeFloat32ToFloat64();
      break;
    case kExprF32ReinterpretI32:
      op = m->BitcastInt32ToFloat32();
      break;
    case kExprI32ReinterpretF32:
      op = m->BitcastFloat32ToInt32();
      break;
    case kExprI32Clz:
      op = m->Word32Clz();
      break;
    case kExprI32Ctz: {
      if (m->Word32Ctz().IsSupported()) {
        op = m->Word32Ctz().op();
        break;
      } else {
        return MakeI32Ctz(input);
      }
    }
    case kExprI32Popcnt: {
      if (m->Word32Popcnt().IsSupported()) {
        op = m->Word32Popcnt().op();
        break;
      } else {
        return MakeI32Popcnt(input);
      }
    }
    case kExprF32Floor: {
      if (m->Float32RoundDown().IsSupported()) {
        op = m->Float32RoundDown().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF32Ceil: {
      if (m->Float32RoundUp().IsSupported()) {
        op = m->Float32RoundUp().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF32Trunc: {
      if (m->Float32RoundTruncate().IsSupported()) {
        op = m->Float32RoundTruncate().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF32NearestInt: {
      if (m->Float32RoundTiesEven().IsSupported()) {
        op = m->Float32RoundTiesEven().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF64Floor: {
      if (m->Float64RoundDown().IsSupported()) {
        op = m->Float64RoundDown().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF64Ceil: {
      if (m->Float64RoundUp().IsSupported()) {
        op = m->Float64RoundUp().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF64Trunc: {
      if (m->Float64RoundTruncate().IsSupported()) {
        op = m->Float64RoundTruncate().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }
    case kExprF64NearestInt: {
      if (m->Float64RoundTiesEven().IsSupported()) {
        op = m->Float64RoundTiesEven().op();
        break;
      } else {
        op = UnsupportedOpcode(opcode);
        break;
      }
    }

#if WASM_64
    // Opcodes only supported on 64-bit platforms.
    // TODO(titzer): query the machine operator builder here instead of #ifdef.
    case kExprI32ConvertI64:
      op = m->TruncateInt64ToInt32();
      break;
    case kExprI64SConvertI32:
      op = m->ChangeInt32ToInt64();
      break;
    case kExprI64UConvertI32:
      op = m->ChangeUint32ToUint64();
      break;
    case kExprF32SConvertI64:
      op = m->RoundInt64ToFloat32();
      break;
    case kExprF32UConvertI64:
      op = m->RoundUint64ToFloat32();
      break;
    case kExprF64SConvertI64:
      op = m->RoundInt64ToFloat64();
      break;
    case kExprF64UConvertI64:
      op = m->RoundUint64ToFloat64();
      break;
    case kExprF64ReinterpretI64:
      op = m->BitcastInt64ToFloat64();
      break;
    case kExprI64ReinterpretF64:
      op = m->BitcastFloat64ToInt64();
      break;
    case kExprI64Clz:
      op = m->Word64Clz();
      break;
    case kExprI64Ctz: {
      if (m->Word64Ctz().IsSupported()) {
        op = m->Word64Ctz().op();
        break;
      } else {
        return MakeI64Ctz(input);
      }
    }
    case kExprI64Popcnt: {
      if (m->Word64Popcnt().IsSupported()) {
        op = m->Word64Popcnt().op();
        break;
      } else {
        return MakeI64Popcnt(input);
      }
    }
#endif
    default:
      op = UnsupportedOpcode(opcode);
  }
  return graph->graph()->NewNode(op, input);
}

TFNode* TFBuilder::Float32Constant(float value) {
  DCHECK_NOT_NULL(graph);
  return graph->Float32Constant(value);
}

TFNode* TFBuilder::Float64Constant(double value) {
  DCHECK_NOT_NULL(graph);
  return graph->Float64Constant(value);
}

TFNode* TFBuilder::Constant(Handle<Object> value) {
  DCHECK_NOT_NULL(graph);
  return graph->Constant(value);
}

TFNode* TFBuilder::Branch(TFNode* cond, TFNode** true_node, TFNode** false_node) {
  DCHECK_NOT_NULL(graph);
  DCHECK_NOT_NULL(cond);
  DCHECK_NOT_NULL(*control);
  TFNode* branch =
      graph->graph()->NewNode(graph->common()->Branch(), cond, *control);
  *true_node = graph->graph()->NewNode(graph->common()->IfTrue(), branch);
  *false_node = graph->graph()->NewNode(graph->common()->IfFalse(), branch);
  return branch;
}


TFNode* TFBuilder::Switch(unsigned count, TFNode* key) {
  DCHECK_NOT_NULL(graph);
  return graph->graph()->NewNode(graph->common()->Switch(count), key, *control);
}


TFNode* TFBuilder::IfValue(int32_t value, TFNode* sw) {
  DCHECK_NOT_NULL(graph);
  DCHECK_EQ(compiler::IrOpcode::kSwitch, sw->opcode());
  return graph->graph()->NewNode(graph->common()->IfValue(value), sw);
}


TFNode* TFBuilder::IfDefault(TFNode* sw) {
  DCHECK_NOT_NULL(graph);
  DCHECK_EQ(compiler::IrOpcode::kSwitch, sw->opcode());
  return graph->graph()->NewNode(graph->common()->IfDefault(), sw);
}


TFNode* TFBuilder::Return(unsigned count, TFNode** vals) {
  DCHECK_NOT_NULL(graph);
  DCHECK_NOT_NULL(*control);
  DCHECK_NOT_NULL(*effect);

  if (count == 0) {
    // Handle a return of void.
    vals[0] = graph->Int32Constant(0);
    count = 1;
  }

  compiler::Graph* g = graph->graph();
  TFNode** buf = Realloc(vals, count + 2);
  buf[count] = *effect;
  buf[count + 1] = *control;
  TFNode* ret = g->NewNode(graph->common()->Return(), count + 2, vals);

  MergeControlToEnd(graph, ret);
  return ret;
}


TFNode* TFBuilder::ReturnVoid() {
  return Return(0, Buffer(0));
}

TFNode* TFBuilder::Unreachable() {
  DCHECK_NOT_NULL(graph);
  trap->Unreachable();
  return nullptr;
}


TFNode* TFBuilder::MakeF32CopySign(TFNode* left, TFNode* right) {
  TFNode* result = Unop(kExprF32ReinterpretI32,
                     Binop(kExprI32Ior,
                           Binop(kExprI32And,
                                 Unop(kExprI32ReinterpretF32, left),
                                 graph->Int32Constant(0x7fffffff)),
                           Binop(kExprI32And,
                                 Unop(kExprI32ReinterpretF32, right),
                                 graph->Int32Constant(0x80000000))));

  return result;
}


TFNode* TFBuilder::MakeF64CopySign(TFNode* left, TFNode* right) {

#if WASM_64
  TFNode* result = Unop(kExprF64ReinterpretI64,
                     Binop(kExprI64Ior,
                           Binop(kExprI64And,
                                 Unop(kExprI64ReinterpretF64, left),
                                 graph->Int64Constant(0x7fffffffffffffff)),
                           Binop(kExprI64And,
                                 Unop(kExprI64ReinterpretF64, right),
                                 graph->Int64Constant(0x8000000000000000))));

  return result;
#else
  compiler::MachineOperatorBuilder* m = graph->machine();

  TFNode* high_word_left = graph->graph()->NewNode(m->Float64ExtractHighWord32(), left);
  TFNode* high_word_right = graph->graph()->NewNode(m->Float64ExtractHighWord32(), right);

  TFNode* new_high_word = Binop(kExprI32Ior,
                                Binop(kExprI32And,
                                      high_word_left,
                                      graph->Int32Constant(0x7fffffff)),
                                Binop(kExprI32And,
                                      high_word_right,
                                      graph->Int32Constant(0x80000000)));
  
  return graph->graph()->NewNode(m->Float64InsertHighWord32(), left, new_high_word);
#endif
}


TFNode* TFBuilder::MakeI32Ctz(TFNode* input) {
  DCHECK_NOT_NULL(graph);
  //// Implement the following code as TF graph.
  // value = value | (value << 1);
  // value = value | (value << 2);
  // value = value | (value << 4);
  // value = value | (value << 8);
  // value = value | (value << 16);
  // return CountPopulation32(0xffffffff XOR value);

  TFNode* result = Binop(kExprI32Ior, input,
                         Binop(kExprI32Shl, input, graph->Int32Constant(1)));

  result = Binop(kExprI32Ior, result,
                 Binop(kExprI32Shl, result, graph->Int32Constant(2)));

  result = Binop(kExprI32Ior, result,
                 Binop(kExprI32Shl, result, graph->Int32Constant(4)));

  result = Binop(kExprI32Ior, result,
                 Binop(kExprI32Shl, result, graph->Int32Constant(8)));

  result = Binop(kExprI32Ior, result,
                 Binop(kExprI32Shl, result, graph->Int32Constant(16)));

  result = MakeI32Popcnt(
      Binop(kExprI32Xor, graph->Int32Constant(0xffffffff), result));

  return result;
}


TFNode* TFBuilder::MakeI64Ctz(TFNode* input) {
  //// Implement the following code as TF graph.
  // value = value | (value << 1);
  // value = value | (value << 2);
  // value = value | (value << 4);
  // value = value | (value << 8);
  // value = value | (value << 16);
  // value = value | (value << 32);
  // return CountPopulation64(0xffffffffffffffff XOR value);

  TFNode* result = Binop(kExprI64Ior, input,
                         Binop(kExprI64Shl, input, graph->Int64Constant(1)));

  result = Binop(kExprI64Ior, result,
                 Binop(kExprI64Shl, result, graph->Int64Constant(2)));

  result = Binop(kExprI64Ior, result,
                 Binop(kExprI64Shl, result, graph->Int64Constant(4)));

  result = Binop(kExprI64Ior, result,
                 Binop(kExprI64Shl, result, graph->Int64Constant(8)));

  result = Binop(kExprI64Ior, result,
                 Binop(kExprI64Shl, result, graph->Int64Constant(16)));

  result = Binop(kExprI64Ior, result,
                 Binop(kExprI64Shl, result, graph->Int64Constant(32)));

  result = MakeI64Popcnt(
      Binop(kExprI64Xor, graph->Int64Constant(0xffffffffffffffff), result));

  return result;
}

TFNode* TFBuilder::MakeI32Popcnt(TFNode* input) {
  DCHECK_NOT_NULL(graph);
  //// Implement the following code as a TF graph.
  // value = ((value >> 1) & 0x55555555) + (value & 0x55555555);
  // value = ((value >> 2) & 0x33333333) + (value & 0x33333333);
  // value = ((value >> 4) & 0x0f0f0f0f) + (value & 0x0f0f0f0f);
  // value = ((value >> 8) & 0x00ff00ff) + (value & 0x00ff00ff);
  // value = ((value >> 16) & 0x0000ffff) + (value & 0x0000ffff);

  TFNode* result = Binop(
      kExprI32Add,
      Binop(kExprI32And, Binop(kExprI32ShrU, input, graph->Int32Constant(1)),
            graph->Int32Constant(0x55555555)),
      Binop(kExprI32And, input, graph->Int32Constant(0x55555555)));

  result = Binop(kExprI32Add, Binop(kExprI32And, Binop(kExprI32ShrU, result,
                                                       graph->Int32Constant(2)),
                                    graph->Int32Constant(0x33333333)),
                 Binop(kExprI32And, result, graph->Int32Constant(0x33333333)));

  result = Binop(kExprI32Add, Binop(kExprI32And, Binop(kExprI32ShrU, result,
                                                       graph->Int32Constant(4)),
                                    graph->Int32Constant(0x0f0f0f0f)),
                 Binop(kExprI32And, result, graph->Int32Constant(0x0f0f0f0f)));

  result = Binop(kExprI32Add, Binop(kExprI32And, Binop(kExprI32ShrU, result,
                                                       graph->Int32Constant(8)),
                                    graph->Int32Constant(0x00ff00ff)),
                 Binop(kExprI32And, result, graph->Int32Constant(0x00ff00ff)));

  result = Binop(
      kExprI32Add,
      Binop(kExprI32And, Binop(kExprI32ShrU, result, graph->Int32Constant(16)),
            graph->Int32Constant(0x0000ffff)),
      Binop(kExprI32And, result, graph->Int32Constant(0x0000ffff)));

  return result;
}


TFNode* TFBuilder::MakeI64Popcnt(TFNode* input) {
  DCHECK_NOT_NULL(graph);
  //// Implement the following code as a TF graph.
  // value = ((value >> 1) & 0x5555555555555555) + (value & 0x5555555555555555);
  // value = ((value >> 2) & 0x3333333333333333) + (value & 0x3333333333333333);
  // value = ((value >> 4) & 0x0f0f0f0f0f0f0f0f) + (value & 0x0f0f0f0f0f0f0f0f);
  // value = ((value >> 8) & 0x00ff00ff00ff00ff) + (value & 0x00ff00ff00ff00ff);
  // value = ((value >> 16) & 0x0000ffff0000ffff) + (value & 0x0000ffff0000ffff);
  // value = ((value >> 32) & 0x00000000ffffffff) + (value & 0x00000000ffffffff);

  TFNode* result = Binop(
      kExprI64Add,
      Binop(kExprI64And, Binop(kExprI64ShrU, input, graph->Int64Constant(1)),
            graph->Int64Constant(0x5555555555555555)),
      Binop(kExprI64And, input, graph->Int64Constant(0x5555555555555555)));

  result = Binop(kExprI64Add, Binop(kExprI64And, Binop(kExprI64ShrU, result,
                                                       graph->Int64Constant(2)),
                                    graph->Int64Constant(0x3333333333333333)),
                 Binop(kExprI64And, result, graph->Int64Constant(0x3333333333333333)));

  result = Binop(kExprI64Add, Binop(kExprI64And, Binop(kExprI64ShrU, result,
                                                       graph->Int64Constant(4)),
                                    graph->Int64Constant(0x0f0f0f0f0f0f0f0f)),
                 Binop(kExprI64And, result, graph->Int64Constant(0x0f0f0f0f0f0f0f0f)));

  result = Binop(kExprI64Add, Binop(kExprI64And, Binop(kExprI64ShrU, result,
                                                       graph->Int64Constant(8)),
                                    graph->Int64Constant(0x00ff00ff00ff00ff)),
                 Binop(kExprI64And, result, graph->Int64Constant(0x00ff00ff00ff00ff)));

  result = Binop(
      kExprI64Add,
      Binop(kExprI64And, Binop(kExprI64ShrU, result, graph->Int64Constant(16)),
            graph->Int64Constant(0x0000ffff0000ffff)),
      Binop(kExprI64And, result, graph->Int64Constant(0x0000ffff0000ffff)));

  result = Binop(
      kExprI64Add,
      Binop(kExprI64And, Binop(kExprI64ShrU, result, graph->Int64Constant(32)),
            graph->Int64Constant(0x00000000ffffffff)),
      Binop(kExprI64And, result, graph->Int64Constant(0x00000000ffffffff)));

  return result;
}


TFNode* TFBuilder::MakeWasmCall(FunctionSig* sig, TFNode** args) {
  const size_t params = sig->parameter_count();
  const size_t extra = 2;  // effect and control inputs.
  const size_t count = 1 + params + extra;

  // Reallocate the buffer to make space for extra inputs.
  args = Realloc(args, count);

  // Add effect and control inputs.
  args[params + 1] = *effect;
  args[params + 2] = *control;

  const compiler::Operator* op =
      graph->common()->Call(module->GetWasmCallDescriptor(graph->zone(), sig));
  TFNode* call = graph->graph()->NewNode(op, static_cast<int>(count), args);

  *effect = call;
  return call;
}

TFNode* TFBuilder::CallDirect(uint32_t index, TFNode** args) {
  DCHECK_NOT_NULL(graph);
  DCHECK_NULL(args[0]);

  // Add code object as constant.
  args[0] = Constant(module->GetFunctionCode(index));
  FunctionSig* sig = module->GetFunctionSignature(index);

  return MakeWasmCall(sig, args);
}

TFNode* TFBuilder::CallIndirect(uint32_t index, TFNode** args) {
  DCHECK_NOT_NULL(graph);
  DCHECK_NOT_NULL(args[0]);

  compiler::Graph* g = graph->graph();
  compiler::MachineOperatorBuilder* machine = graph->machine();

  // Compute the code object by loading it from the function table.
  TFNode* key = args[0];
  TFNode* table = FunctionTable();

  // Bounds check the index.
  int table_size = static_cast<int>(module->FunctionTableSize());
  {
    TFNode* size = Int32Constant(static_cast<int>(table_size));
    TFNode* in_bounds = g->NewNode(machine->Uint32LessThan(), key, size);
    trap->AddTrapIfFalse(kTrapFuncInvalid, in_bounds);
  }

  // Load signature from the table and check.
  // The table is a FixedArray; signatures are encoded as SMIs.
  // [sig1, sig2, sig3, ...., code1, code2, code3 ...]
  compiler::ElementAccess access =
      compiler::AccessBuilder::ForFixedArrayElement();
  const int fixed_offset = access.header_size - access.tag();
  {
    TFNode* load_sig =
        g->NewNode(machine->Load(compiler::kMachAnyTagged), table,
                   g->NewNode(machine->Int32Add(),
                              g->NewNode(machine->Word32Shl(), key,
                                         Int32Constant(kPointerSizeLog2)),
                              Int32Constant(fixed_offset)),
                   *effect, *control);
    TFNode* sig_match =
        g->NewNode(machine->WordEqual(), load_sig, graph->SmiConstant(index));
    trap->AddTrapIfFalse(kTrapFuncSigMismatch, sig_match);
  }

  // Load code object from the table.
  int offset = fixed_offset + kPointerSize * table_size;
  TFNode* load_code =
      g->NewNode(machine->Load(compiler::kMachAnyTagged), table,
                 g->NewNode(machine->Int32Add(),
                            g->NewNode(machine->Word32Shl(), key,
                                       Int32Constant(kPointerSizeLog2)),
                            Int32Constant(offset)),
                 *effect, *control);

  args[0] = load_code;
  FunctionSig* sig = module->GetSignature(index);
  return MakeWasmCall(sig, args);
}

TFNode* TFBuilder::ToJS(TFNode* node, TFNode* context, LocalType type) {
  DCHECK_NOT_NULL(graph);
  compiler::Graph* g = graph->graph();
  compiler::SimplifiedOperatorBuilder simplified(graph->zone());
  switch (type) {
    case kAstI32:
      return g->NewNode(simplified.ChangeInt32ToTagged(), node);
    case kAstI64:
      // TODO(titzer): i64->JS has no good solution right now. Using lower 32
      // bits.
      node = g->NewNode(graph->machine()->TruncateInt64ToInt32(), node);
      return g->NewNode(simplified.ChangeInt32ToTagged(), node);
    case kAstF32:
      node = g->NewNode(graph->machine()->ChangeFloat32ToFloat64(), node);
      return g->NewNode(simplified.ChangeFloat64ToTagged(), node);
    case kAstF64:
      return g->NewNode(simplified.ChangeFloat64ToTagged(), node);
    case kAstStmt:
      return graph->UndefinedConstant();
    case kAstEnd:
      UNREACHABLE();
      return nullptr;
  }
}

TFNode* TFBuilder::FromJS(TFNode* node, TFNode* context, LocalType type) {
  DCHECK_NOT_NULL(graph);
  compiler::Graph* g = graph->graph();
  // Do a JavaScript ToNumber.
  TFNode* num = g->NewNode(graph->javascript()->ToNumber(), node, context,
                           graph->EmptyFrameState(), *effect, *control);
  *control = num;
  *effect = num;

  // Change representation.
  compiler::SimplifiedOperatorBuilder simplified(graph->zone());
  num = g->NewNode(simplified.ChangeTaggedToFloat64(), num);

  switch (type) {
    case kAstI32: {
      num = g->NewNode(graph->machine()->TruncateFloat64ToInt32(
                           compiler::TruncationMode::kJavaScript),
                       num);
      break;
    }
    case kAstI64:
      // TODO(titzer): JS->i64 has no good solution right now. Using 32 bits.
      num = g->NewNode(graph->machine()->TruncateFloat64ToInt32(
                           compiler::TruncationMode::kJavaScript),
                       num);
      num = g->NewNode(graph->machine()->ChangeInt32ToInt64(), num);
      break;
    case kAstF32:
      num = g->NewNode(graph->machine()->TruncateFloat64ToFloat32(), num);
      break;
    case kAstF64:
      break;
    case kAstStmt:
      num = graph->Int32Constant(0);
      break;
    case kAstEnd:
      UNREACHABLE();
      return nullptr;
  }
  return num;
}

TFNode* TFBuilder::Invert(TFNode* node) {
  DCHECK_NOT_NULL(graph);
  return Unop(kExprBoolNot, node);
}

void TFBuilder::BuildJSToWasmWrapper(Handle<Code> wasm_code, FunctionSig* sig) {
  DCHECK_NOT_NULL(graph);

  int params = static_cast<int>(sig->parameter_count());
  compiler::Graph* g = graph->graph();
  int count = params + 3;
  TFNode** args = Buffer(count);

  // Build the start and the JS parameter nodes.
  TFNode* start = Start(params + 3);
  *control = start;
  *effect = start;
  // JS context is the last parameter.
  TFNode* context =
      g->NewNode(graph->common()->Parameter(params + 1, "context"), start);

  int pos = 0;
  args[pos++] = Constant(wasm_code);

  // Convert JS parameters to WASM numbers.
  for (int i = 0; i < params; i++) {
    TFNode* param = g->NewNode(graph->common()->Parameter(i), start);
    args[pos++] = FromJS(param, context, sig->GetParam(i));
  }

  args[pos++] = *effect;
  args[pos++] = *control;

  // Call the WASM code.
  compiler::CallDescriptor* desc =
      module->GetWasmCallDescriptor(graph->zone(), sig);
  TFNode* call = g->NewNode(graph->common()->Call(desc), count, args);
  TFNode* jsval = ToJS(call, context,
                       sig->return_count() == 0 ? kAstStmt : sig->GetReturn());
  TFNode* ret = g->NewNode(graph->common()->Return(), jsval, call, start);

  MergeControlToEnd(graph, ret);
}

void TFBuilder::BuildWasmToJSWrapper(Handle<JSFunction> function,
                                     FunctionSig* sig) {
  DCHECK_NOT_NULL(graph);
  CHECK_NOT_NULL(graph);
  int js_count = function->shared()->internal_formal_parameter_count();
  int wasm_count = static_cast<int>(sig->parameter_count());

  // Build the start and the parameter nodes.
  Isolate* isolate = graph->isolate();
  compiler::Graph* g = graph->graph();
  compiler::CallDescriptor* desc;
  TFNode* start = Start(wasm_count + 3);
  *effect = start;
  *control = start;
  // JS context is the last parameter.
  TFNode* context = Constant(Handle<Context>(function->context(), isolate));
  TFNode** args = Buffer(wasm_count + 7);

  bool arg_count_before_args = false;
  bool add_new_target_undefined = false;

  int pos = 0;
  if (js_count == wasm_count) {
    // exact arity match, just call the function directly.
    desc = compiler::Linkage::GetJSCallDescriptor(
        g->zone(), false, wasm_count + 1, compiler::CallDescriptor::kNoFlags);
    arg_count_before_args = false;
    add_new_target_undefined = true;
  } else {
    // Use the Call builtin.
    Callable callable = CodeFactory::Call(isolate);
    args[pos++] = graph->HeapConstant(callable.code());
    desc = compiler::Linkage::GetStubCallDescriptor(
        isolate, g->zone(), callable.descriptor(), wasm_count + 1,
        compiler::CallDescriptor::kNoFlags);
    arg_count_before_args = true;
  }

  args[pos++] = graph->Constant(function);   // JS function.
  if (arg_count_before_args) {
    args[pos++] = graph->Int32Constant(wasm_count);  // argument count
  }
  args[pos++] = graph->UndefinedConstant();  // JS receiver.

  // Convert WASM numbers to JS values.
  for (int i = 0; i < wasm_count; i++) {
    TFNode* param = g->NewNode(graph->common()->Parameter(i), start);
    args[pos++] = ToJS(param, context, sig->GetParam(i));
  }

  if (add_new_target_undefined) {
    args[pos++] = graph->UndefinedConstant();  // new target
  }

  if (!arg_count_before_args) {
    args[pos++] = graph->Int32Constant(wasm_count);  // argument count
  }
  args[pos++] = context;
  args[pos++] = *effect;
  args[pos++] = *control;

  TFNode* call = g->NewNode(graph->common()->Call(desc), pos, args);

  // Convert the return value back.
  TFNode* val = FromJS(call, context,
                       sig->return_count() == 0 ? kAstStmt : sig->GetReturn());
  TFNode* ret = g->NewNode(graph->common()->Return(), val, call, start);

  MergeControlToEnd(graph, ret);
}

TFNode* TFBuilder::MemBuffer(uint32_t offset) {
  if (!graph) return nullptr;
  if (offset == 0) {
    if (!mem_buffer) mem_buffer = graph->IntPtrConstant(module->mem_start);
    return mem_buffer;
  } else {
    return graph->IntPtrConstant(module->mem_start + offset);
  }
}

TFNode* TFBuilder::MemSize(uint32_t offset) {
  if (!graph) return nullptr;
  int32_t size = static_cast<int>(module->mem_end - module->mem_start);
  if (offset == 0) {
    if (!mem_size) mem_size = graph->Int32Constant(size);
    return mem_size;
  } else {
    return graph->Int32Constant(size + offset);
  }
}


TFNode* TFBuilder::FunctionTable() {
  if (!graph)
    return nullptr;
  if (!function_table) {
    DCHECK(!module->function_table.is_null());
    function_table = graph->Constant(module->function_table);
  }
  return function_table;
}


TFNode* TFBuilder::LoadGlobal(uint32_t index) {
  DCHECK_NOT_NULL(graph);
  MemType mem_type = module->GetGlobalType(index);
  TFNode* addr = graph->IntPtrConstant(
      module->globals_area + module->module->globals->at(index).offset);
  const compiler::Operator* op =
      graph->machine()->Load(MachineTypeFor(mem_type));
  TFNode* node = graph->graph()->NewNode(op, addr, graph->Int32Constant(0),
                                         *effect, *control);
  *effect = node;
  return node;
}


TFNode* TFBuilder::StoreGlobal(uint32_t index, TFNode* val) {
  DCHECK_NOT_NULL(graph);
  MemType mem_type = module->GetGlobalType(index);
  TFNode* addr = graph->IntPtrConstant(
      module->globals_area + module->module->globals->at(index).offset);
  const compiler::Operator* op =
      graph->machine()->Store(compiler::StoreRepresentation(
          MachineTypeFor(mem_type), compiler::kNoWriteBarrier));
  TFNode* node = graph->graph()->NewNode(op, addr, graph->Int32Constant(0), val,
                                         *effect, *control);
  *effect = node;
  return node;
}

void TFBuilder::BoundsCheckMem(MemType memtype, TFNode* index, uint32_t offset) {
  // TODO(turbofan): fold bounds checks for constant indexes.
  compiler::Graph* g = graph->graph();
  CHECK_GE(module->mem_end, module->mem_start);
  ptrdiff_t size = module->mem_end - module->mem_start;
  byte memsize = WasmOpcodes::MemSize(memtype);
  TFNode* cond;
  if (offset >= size || (offset + memsize) >= size) {
    // The access will always throw.
    cond = graph->Int32Constant(0);
  } else {
    // Check against the limit.
    size_t limit = size - offset - memsize;
    CHECK(limit <= kMaxUInt32);
    cond = g->NewNode(graph->machine()->Uint32LessThanOrEqual(), index,
                      graph->Int32Constant(static_cast<uint32_t>(limit)));
  }

  trap->AddTrapIfFalse(kTrapMemOutOfBounds, cond);
}


TFNode* TFBuilder::LoadMem(LocalType type, MemType memtype, TFNode* index,
			     uint32_t offset) {
  if (!graph)
    return nullptr;

  compiler::Graph* g = graph->graph();
  TFNode* load;

  if (module && module->asm_js) {
    // asm.js semantics use CheckedLoad (i.e. OOB reads return 0ish).
    DCHECK_EQ(0, offset);
    const compiler::Operator* op =
      graph->machine()->CheckedLoad(MachineTypeFor(memtype));
    load = g->NewNode(op, MemBuffer(0), index, MemSize(0),
                                   *effect, *control);
  } else {
    // WASM semantics throw on OOB. Introduce explicit bounds check.
    BoundsCheckMem(memtype, index, offset);
    load = g->NewNode(graph->machine()->Load(MachineTypeFor(memtype)),
                      MemBuffer(offset), index, *effect, *control);
  }

  *effect = load;

  if (type == kAstI64 && WasmOpcodes::MemSize(memtype) < 8) {
    // TODO(titzer): TF zeroes the upper bits of 64-bit loads for subword sizes.
    bool sign_extend =
        memtype == kMemI8 || memtype == kMemI16 || memtype == kMemI32;
    if (sign_extend) {
      load = g->NewNode(graph->machine()->ChangeInt32ToInt64(), load);
    } else {
      load = g->NewNode(graph->machine()->ChangeUint32ToUint64(), load);
    }
  }

  return load;
}


TFNode* TFBuilder::StoreMem(MemType memtype, TFNode* index, uint32_t offset,
                            TFNode* val) {
  if (!graph)
    return nullptr;

  TFNode* store;
  if (module && module->asm_js) {
    // asm.js semantics use CheckedStore (i.e. ignore OOB writes).
    DCHECK_EQ(0, offset);
    const compiler::Operator* op =
      graph->machine()->CheckedStore(MachineTypeFor(memtype));
    store = graph->graph()->NewNode(op, MemBuffer(0), index, MemSize(0), val,
                                           *effect, *control);
  } else {
    // WASM semantics throw on OOB. Introduce explicit bounds check.
    BoundsCheckMem(memtype, index, offset);
    compiler::StoreRepresentation rep(MachineTypeFor(memtype),
                                      compiler::kNoWriteBarrier);
    store = graph->graph()->NewNode(graph->machine()->Store(rep),
                                    MemBuffer(offset),
                                    index, val, *effect, *control);
  }
  *effect = store;
  return store;
}


void TFBuilder::PrintDebugName(TFNode* node) {
  PrintF("#%d:%s", node->id(), node->op()->mnemonic());
}


TFNode* TFBuilder::String(const char* string) {
  DCHECK_NOT_NULL(graph);
  return graph->Constant(graph->isolate()->factory()->NewStringFromAsciiChecked(string));
}

}
}
}
