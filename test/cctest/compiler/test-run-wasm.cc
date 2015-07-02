// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>
#include <stdlib.h>

#include "src/compiler/graph-visualizer.h"
#include "src/compiler/js-graph.h"

#include "src/wasm/decoder.h"
#include "src/wasm/wasm-macro-gen.h"
#include "src/wasm/wasm-opcodes.h"

#include "test/cctest/cctest.h"
#include "test/cctest/compiler/graph-builder-tester.h"
#include "test/cctest/compiler/value-helper.h"

#if V8_TURBOFAN_TARGET

#if !V8_TARGET_ARCH_32_BIT || V8_TARGET_ARCH_X64
#define WASM_64 1
#else
#define WASM_64 0
#endif

using namespace v8::base;
using namespace v8::internal;
using namespace v8::internal::compiler;
using namespace v8::internal::wasm;

// Helpers for many common signatures that involve int32 types.
static LocalType kIntTypes5[] = {kAstInt32, kAstInt32, kAstInt32, kAstInt32,
                                 kAstInt32};
static LocalType kLongTypes5[] = {kAstInt64, kAstInt64, kAstInt64, kAstInt64,
                                  kAstInt64};

struct CommonSignatures {
  CommonSignatures()
      : sig_i_v(1, 0, kIntTypes5),
        sig_i_i(1, 1, kIntTypes5),
        sig_i_ii(1, 2, kIntTypes5),
        sig_i_iii(1, 3, kIntTypes5),
        sig_v_v(0, 0, nullptr),
        sig_l_ll(1, 2, kLongTypes5) {
    init_env(&env_i_v, &sig_i_v);
    init_env(&env_i_i, &sig_i_i);
    init_env(&env_i_ii, &sig_i_ii);
    init_env(&env_i_iii, &sig_i_iii);
    init_env(&env_v_v, &sig_v_v);
    init_env(&env_l_ll, &sig_l_ll);
  }

  FunctionSig sig_i_v;
  FunctionSig sig_i_i;
  FunctionSig sig_i_ii;
  FunctionSig sig_i_iii;
  FunctionSig sig_v_v;
  FunctionSig sig_l_ll;
  FunctionEnv env_i_v;
  FunctionEnv env_i_i;
  FunctionEnv env_i_ii;
  FunctionEnv env_i_iii;
  FunctionEnv env_v_v;
  FunctionEnv env_l_ll;

  static void init_env(FunctionEnv* env, FunctionSig* sig) {
    env->module = nullptr;
    env->sig = sig;
    env->local_int32_count = 0;
    env->local_int64_count = 0;
    env->local_float32_count = 0;
    env->local_float64_count = 0;
    env->total_locals = static_cast<unsigned>(sig->parameter_count());
  }
};


// A helper class to build graphs from Wasm bytecode, generate machine
// code, and run that code.
template <typename ReturnType>
class WasmRunner : public GraphBuilderTester<ReturnType> {
 public:
  WasmRunner(MachineType p0 = kMachNone, MachineType p1 = kMachNone,
             MachineType p2 = kMachNone, MachineType p3 = kMachNone,
             MachineType p4 = kMachNone)
      : GraphBuilderTester<ReturnType>(p0, p1, p2, p3, p4),
        jsgraph(this->isolate(), this->graph(), this->common(), nullptr,
                this->machine()) {
    if (p1 != kMachNone) {
      function_env = &sigs_.env_i_ii;
    } else if (p0 != kMachNone) {
      function_env = &sigs_.env_i_i;
    } else {
      function_env = &sigs_.env_i_v;
    }
  }

  JSGraph jsgraph;
  CommonSignatures sigs_;
  FunctionEnv* function_env;

  void Build(const byte* start, const byte* end) {
    Result result = BuildTFGraph(&jsgraph, function_env, start, end);
    if (result.error_code != kSuccess) {
      ptrdiff_t pc = result.error_pc - result.pc;
      ptrdiff_t pt = result.error_pt - result.pc;
      std::ostringstream str;
      str << "Verification failed: " << result.error_code << " pc = +" << pc
          << ", pt = +" << pt << ", msg = " << result.error_msg.get();
      FATAL(str.str().c_str());
    }
    if (FLAG_trace_turbo_graph) {
      OFStream os(stdout);
      os << AsRPO(*jsgraph.graph());
    }
  }

  byte AllocateLocal(LocalType type) {
    int result = static_cast<int>(function_env->sig->parameter_count());
    if (type == kAstInt32) result += function_env->local_int32_count++;
    if (type == kAstFloat32) result += function_env->local_float32_count++;
    if (type == kAstFloat64) result += function_env->local_float64_count++;
    function_env->total_locals++;
    byte b = static_cast<byte>(result);
    CHECK_EQ(result, b);
    return b;
  }
};


#define BUILD(r, ...)                      \
  do {                                     \
    byte code[] = {__VA_ARGS__};           \
    r.Build(code, code + arraysize(code)); \
  } while (false)


TEST(Run_WasmInt8Const) {
  WasmRunner<int8_t> r;
  const byte kExpectedValue = 121;
  // return(kExpectedValue)
  BUILD(r, WASM_RETURN(WASM_INT8(kExpectedValue)));
  CHECK_EQ(kExpectedValue, r.Call());
}


TEST(Run_WasmInt8Const_all) {
  for (int value = -128; value <= 127; value++) {
    WasmRunner<int8_t> r;
    // return(value)
    BUILD(r, WASM_RETURN(WASM_INT8(value)));
    int8_t result = r.Call();
    CHECK_EQ(value, result);
  }
}


TEST(Run_WasmInt32Const) {
  WasmRunner<int32_t> r;
  const int32_t kExpectedValue = 0x11223344;
  // return(kExpectedValue)
  BUILD(r, WASM_RETURN(WASM_INT32(kExpectedValue)));
  CHECK_EQ(kExpectedValue, r.Call());
}


TEST(Run_WasmInt32Const_many) {
  FOR_INT32_INPUTS(i) {
    WasmRunner<int32_t> r;
    const int32_t kExpectedValue = *i;
    // return(kExpectedValue)
    BUILD(r, WASM_RETURN(WASM_INT32(kExpectedValue)));
    CHECK_EQ(kExpectedValue, r.Call());
  }
}


#if WASM_64
TEST(Run_WasmInt64Const) {
  WasmRunner<int64_t> r;
  const int64_t kExpectedValue = 0x1122334455667788LL;
  // return(kExpectedValue)
  r.function_env = &r.sigs_.env_l_ll;
  BUILD(r, WASM_RETURN(WASM_INT64(kExpectedValue)));
  CHECK_EQ(kExpectedValue, r.Call());
}


TEST(Run_WasmInt64Const_many) {
  int cntr = 0;
  FOR_INT32_INPUTS(i) {
    WasmRunner<int64_t> r;
    r.function_env = &r.sigs_.env_l_ll;
    const int64_t kExpectedValue = (static_cast<int64_t>(*i) << 32) | cntr;
    // return(kExpectedValue)
    BUILD(r, WASM_RETURN(WASM_INT64(kExpectedValue)));
    CHECK_EQ(kExpectedValue, r.Call());
    cntr++;
  }
}
#endif


TEST(Run_WasmInt32Param0) {
  WasmRunner<int32_t> r(kMachInt32);
  // return(local[0])
  BUILD(r, WASM_RETURN(WASM_GET_LOCAL(0)));
  FOR_INT32_INPUTS(i) { CHECK_EQ(*i, r.Call(*i)); }
}


TEST(Run_WasmInt32Param1) {
  WasmRunner<int32_t> r(kMachInt32, kMachInt32);
  // return(local[1])
  BUILD(r, WASM_RETURN(WASM_GET_LOCAL(1)));
  FOR_INT32_INPUTS(i) { CHECK_EQ(*i, r.Call(-111, *i)); }
}


TEST(Run_WasmInt32Add) {
  WasmRunner<int32_t> r;
  // return 11 + 44
  BUILD(r, WASM_RETURN(WASM_INT32_ADD(WASM_INT8(11), WASM_INT8(44))));
  CHECK_EQ(55, r.Call());
}


TEST(Run_WasmInt32Add_P) {
  WasmRunner<int32_t> r(kMachInt32);
  // return p0 + 13
  BUILD(r, WASM_RETURN(WASM_INT32_ADD(WASM_INT8(13), WASM_GET_LOCAL(0))));
  FOR_INT32_INPUTS(i) { CHECK_EQ(*i + 13, r.Call(*i)); }
}


TEST(Run_WasmInt32Add_P2) {
  WasmRunner<int32_t> r(kMachInt32, kMachInt32);
  // return p0 + p1
  BUILD(r, WASM_RETURN(WASM_INT32_ADD(WASM_GET_LOCAL(0), WASM_GET_LOCAL(1))));
  FOR_INT32_INPUTS(i) {
    FOR_INT32_INPUTS(j) {
      int32_t expected = static_cast<int32_t>(static_cast<uint32_t>(*i) +
                                              static_cast<uint32_t>(*j));
      CHECK_EQ(expected, r.Call(*i, *j));
    }
  }
}


TEST(Run_WasmFloat32Add) {
  WasmRunner<int32_t> r;
  // return int(11.5f + 44.5f)
  BUILD(r, WASM_RETURN(WASM_INT32_SCONVERT_FLOAT32(
               WASM_FLOAT32_ADD(WASM_FLOAT32(11.5f), WASM_FLOAT32(44.5f)))));
  CHECK_EQ(56, r.Call());
}


TEST(Run_WasmFloat64Add) {
  WasmRunner<int32_t> r;
  // return int(13.5d + 43.5d)
  BUILD(r, WASM_RETURN(WASM_INT32_SCONVERT_FLOAT64(
               WASM_FLOAT64_ADD(WASM_FLOAT64(13.5), WASM_FLOAT64(43.5)))));
  CHECK_EQ(57, r.Call());
}


void TestInt32Binop(WasmOpcode opcode, int32_t expected, int32_t a, int32_t b) {
  {
    WasmRunner<int32_t> r;
    // return K op K
    BUILD(r, WASM_RETURN(WASM_BINOP(opcode, WASM_INT32(a), WASM_INT32(b))));
    CHECK_EQ(expected, r.Call());
  }
  {
    WasmRunner<int32_t> r(kMachInt32, kMachInt32);
    // return a op b
    BUILD(r, WASM_RETURN(
                 WASM_BINOP(opcode, WASM_GET_LOCAL(0), WASM_GET_LOCAL(1))));
    CHECK_EQ(expected, r.Call(a, b));
  }
}


TEST(Run_WasmInt32Binops) {
  TestInt32Binop(kExprInt32Add, 88888888, 33333333, 55555555);
  TestInt32Binop(kExprInt32Sub, -1111111, 7777777, 8888888);
  TestInt32Binop(kExprInt32Mul, 65130756, 88734, 734);
  TestInt32Binop(kExprInt32SDiv, -66, -4777344, 72384);
  TestInt32Binop(kExprInt32UDiv, 805306368, 0xF0000000, 5);
  TestInt32Binop(kExprInt32SRem, -3, -3003, 1000);
  TestInt32Binop(kExprInt32URem, 4, 4004, 1000);
  TestInt32Binop(kExprInt32And, 0xEE, 0xFFEE, 0xFF0000FF);
  TestInt32Binop(kExprInt32Ior, 0xF0FF00FF, 0xF0F000EE, 0x000F0011);
  TestInt32Binop(kExprInt32Xor, 0xABCDEF01, 0xABCDEFFF, 0xFE);
  TestInt32Binop(kExprInt32Shl, 0xA0000000, 0xA, 28);
  TestInt32Binop(kExprInt32Shr, 0x07000010, 0x70000100, 4);
  TestInt32Binop(kExprInt32Sar, 0xFF000000, 0x80000000, 7);
  TestInt32Binop(kExprInt32Eq, 1, -99, -99);
  TestInt32Binop(kExprInt32Slt, 1, -4, 4);
  TestInt32Binop(kExprInt32Sle, 0, -2, -3);
  TestInt32Binop(kExprInt32Ult, 1, 0, -6);
  TestInt32Binop(kExprInt32Ule, 1, 98978, 0xF0000000);
}


#if WASM_64
void TestInt64Binop(WasmOpcode opcode, int64_t expected, int64_t a, int64_t b) {
  if (!WasmOpcodes::IsSupported(opcode)) return;
  {
    WasmRunner<int64_t> r;
    r.function_env = &r.sigs_.env_l_ll;
    // return K op K
    BUILD(r, WASM_RETURN(WASM_BINOP(opcode, WASM_INT64(a), WASM_INT64(b))));
    CHECK_EQ(expected, r.Call());
  }
  {
    WasmRunner<int64_t> r(kMachInt64, kMachInt64);
    r.function_env = &r.sigs_.env_l_ll;
    // return a op b
    BUILD(r, WASM_RETURN(
                 WASM_BINOP(opcode, WASM_GET_LOCAL(0), WASM_GET_LOCAL(1))));
    CHECK_EQ(expected, r.Call(a, b));
  }
}


TEST(Run_WasmInt64Binops) {
  // TODO: real 64-bit numbers
  TestInt64Binop(kExprInt64Add, 8888888888888LL, 3333333333333LL,
                 5555555555555LL);
  TestInt64Binop(kExprInt64Sub, -111111111111LL, 777777777777LL,
                 888888888888LL);
  TestInt64Binop(kExprInt64Mul, 65130756, 88734, 734);
  TestInt64Binop(kExprInt64SDiv, -66, -4777344, 72384);
  TestInt64Binop(kExprInt64UDiv, 805306368, 0xF0000000, 5);
  TestInt64Binop(kExprInt64SRem, -3, -3003, 1000);
  TestInt64Binop(kExprInt64URem, 4, 4004, 1000);
  TestInt64Binop(kExprInt64And, 0xEE, 0xFFEE, 0xFF0000FF);
  TestInt64Binop(kExprInt64Ior, 0xF0FF00FF, 0xF0F000EE, 0x000F0011);
  TestInt64Binop(kExprInt64Xor, 0xABCDEF01, 0xABCDEFFF, 0xFE);
  TestInt64Binop(kExprInt64Shl, 0xA0000000, 0xA, 28);
  TestInt64Binop(kExprInt64Shr, 0x0700001000123456LL, 0x7000010001234567LL, 4);
  TestInt64Binop(kExprInt64Sar, 0xFF00000000000000LL, 0x8000000000000000LL, 7);
  TestInt64Binop(kExprInt64Eq, 1, -99, -99);
  TestInt64Binop(kExprInt64Slt, 1, -4, 4);
  TestInt64Binop(kExprInt64Sle, 0, -2, -3);
  TestInt64Binop(kExprInt64Ult, 1, 0, -6);
  TestInt64Binop(kExprInt64Ule, 1, 98978, 0xF0000000);
}
#endif


void TestFloat32Binop(WasmOpcode opcode, int32_t expected, float a, float b) {
  WasmRunner<int32_t> r;
  // return K op K
  BUILD(r, WASM_RETURN(WASM_BINOP(opcode, WASM_FLOAT32(a), WASM_FLOAT32(b))));
  CHECK_EQ(expected, r.Call());
  // TODO(titzer): test float parameters
}


void TestFloat32BinopWithConvert(WasmOpcode opcode, int32_t expected, float a,
                                 float b) {
  WasmRunner<int32_t> r;
  // return int(K op K)
  BUILD(r, WASM_RETURN(WASM_INT32_SCONVERT_FLOAT32(
               WASM_BINOP(opcode, WASM_FLOAT32(a), WASM_FLOAT32(b)))));
  CHECK_EQ(expected, r.Call());
  // TODO(titzer): test float parameters
}


void TestFloat32UnopWithConvert(WasmOpcode opcode, int32_t expected, float a) {
  WasmRunner<int32_t> r;
  // return int(K op K)
  BUILD(r, WASM_RETURN(WASM_INT32_SCONVERT_FLOAT32(
               WASM_UNOP(opcode, WASM_FLOAT32(a)))));
  CHECK_EQ(expected, r.Call());
  // TODO(titzer): test float parameters
}


void TestFloat64Binop(WasmOpcode opcode, int32_t expected, double a, double b) {
  WasmRunner<int32_t> r;
  // return K op K
  BUILD(r, WASM_RETURN(WASM_BINOP(opcode, WASM_FLOAT64(a), WASM_FLOAT64(b))));
  CHECK_EQ(expected, r.Call());
  // TODO(titzer): test double parameters
}


void TestFloat64BinopWithConvert(WasmOpcode opcode, int32_t expected, double a,
                                 double b) {
  WasmRunner<int32_t> r;
  // return int(K op K)
  BUILD(r, WASM_RETURN(WASM_INT32_SCONVERT_FLOAT64(
               WASM_BINOP(opcode, WASM_FLOAT64(a), WASM_FLOAT64(b)))));
  CHECK_EQ(expected, r.Call());
  // TODO(titzer): test double parameters
}


void TestFloat64UnopWithConvert(WasmOpcode opcode, int32_t expected, double a) {
  WasmRunner<int32_t> r;
  // return int(K op K)
  BUILD(r, WASM_RETURN(WASM_INT32_SCONVERT_FLOAT64(
               WASM_UNOP(opcode, WASM_FLOAT64(a)))));
  CHECK_EQ(expected, r.Call());
  // TODO(titzer): test float parameters
}


TEST(Run_WasmFloat32Binops) {
  TestFloat32Binop(kExprFloat32Eq, 1, 8.125, 8.125);
  TestFloat32Binop(kExprFloat32Lt, 1, -9.5, -9);
  TestFloat32Binop(kExprFloat32Le, 1, -1111, -1111);

  TestFloat32BinopWithConvert(kExprFloat32Add, 10, 3.5, 6.5);
  TestFloat32BinopWithConvert(kExprFloat32Sub, 2, 44.5, 42.5);
  TestFloat32BinopWithConvert(kExprFloat32Mul, -66, -132.1, 0.5);
  TestFloat32BinopWithConvert(kExprFloat32Div, 11, 22.1, 2);
}


TEST(Run_WasmFloat32Unops) {
  TestFloat32UnopWithConvert(kExprFloat32Abs, 8, 8.125);
  TestFloat32UnopWithConvert(kExprFloat32Abs, 9, -9.125);
}


TEST(Run_WasmFloat64Binops) {
  TestFloat64Binop(kExprFloat64Eq, 1, 16.25, 16.25);
  TestFloat64Binop(kExprFloat64Lt, 1, -32.4, 11.7);
  TestFloat64Binop(kExprFloat64Le, 1, -88.9, -88.9);

  TestFloat64BinopWithConvert(kExprFloat64Add, 100, 43.5, 56.5);
  TestFloat64BinopWithConvert(kExprFloat64Sub, 200, 12200.1, 12000.1);
  TestFloat64BinopWithConvert(kExprFloat64Mul, -33, 134, -0.25);
  TestFloat64BinopWithConvert(kExprFloat64Div, -1111, -2222.3, 2);
}


TEST(Run_WasmFloat64Unops) {
  TestFloat64UnopWithConvert(kExprFloat64Abs, 108, 108.125);
  TestFloat64UnopWithConvert(kExprFloat64Abs, 209, -209.125);
}


TEST(Run_Wasm_IfThen_P) {
  WasmRunner<int32_t> r(kMachInt32);
  // if (p0) return 11; else return 22;
  BUILD(r, WASM_IF_THEN(WASM_GET_LOCAL(0),             // --
                        WASM_RETURN(WASM_INT8(11)),    // --
                        WASM_RETURN(WASM_INT8(22))));  // --
  FOR_INT32_INPUTS(i) {
    int32_t expected = *i ? 11 : 22;
    CHECK_EQ(expected, r.Call(*i));
  }
}


TEST(Run_Wasm_VoidReturn) {
  WasmRunner<void> r;
  r.function_env = &r.sigs_.env_v_v;
  BUILD(r, WASM_RETURN0);
  // TODO(titzer): code generator fixes:  r.Call();
}


TEST(Run_Wasm_Block_If_P) {
  WasmRunner<int32_t> r(kMachInt32);
  // { if (p0) return 51; return 52; }
  BUILD(r, WASM_BLOCK(2,                                    // --
                      WASM_IF(WASM_GET_LOCAL(0),            // --
                              WASM_RETURN(WASM_INT8(51))),  // --
                      WASM_RETURN(WASM_INT8(52))));         // --
  FOR_INT32_INPUTS(i) {
    int32_t expected = *i ? 51 : 52;
    CHECK_EQ(expected, r.Call(*i));
  }
}


TEST(Run_Wasm_Block_IfThen_P_assign) {
  WasmRunner<int32_t> r(kMachInt32);
  // { if (p0) p0 = 71; else p0 = 72; return p0; }
  BUILD(r, WASM_BLOCK(2,                                               // --
                      WASM_IF_THEN(WASM_GET_LOCAL(0),                  // --
                                   WASM_SET_LOCAL(0, WASM_INT8(71)),   // --
                                   WASM_SET_LOCAL(0, WASM_INT8(72))),  // --
                      WASM_RETURN(WASM_GET_LOCAL(0))));
  FOR_INT32_INPUTS(i) {
    int32_t expected = *i ? 71 : 72;
    CHECK_EQ(expected, r.Call(*i));
  }
}


TEST(Run_Wasm_Block_If_P_assign) {
  WasmRunner<int32_t> r(kMachInt32);
  // { if (p0) p0 = 61; return p0; }
  BUILD(r, WASM_BLOCK(
               2, WASM_IF(WASM_GET_LOCAL(0), WASM_SET_LOCAL(0, WASM_INT8(61))),
               WASM_RETURN(WASM_GET_LOCAL(0))));
  FOR_INT32_INPUTS(i) {
    int32_t expected = *i ? 61 : *i;
    CHECK_EQ(expected, r.Call(*i));
  }
}


TEST(Run_Wasm_Ternary_P) {
  WasmRunner<int32_t> r(kMachInt32);
  // return p0 ? 11 : 22;
  BUILD(r, WASM_RETURN(WASM_TERNARY(WASM_GET_LOCAL(0),  // --
                                    WASM_INT8(11),      // --
                                    WASM_INT8(22))));   // --
  FOR_INT32_INPUTS(i) {
    int32_t expected = *i ? 11 : 22;
    CHECK_EQ(expected, r.Call(*i));
  }
}


TEST(Run_Wasm_Comma_P) {
  WasmRunner<int32_t> r(kMachInt32);
  // return p0, 17;
  BUILD(r, WASM_RETURN(WASM_COMMA(WASM_GET_LOCAL(0), WASM_INT8(17))));
  FOR_INT32_INPUTS(i) { CHECK_EQ(17, r.Call(*i)); }
}


TEST(Run_Wasm_CountDown) {
  WasmRunner<int32_t> r(kMachInt32);
  BUILD(r,
        WASM_BLOCK(
            2, WASM_LOOP(2, WASM_IF(WASM_NOT(WASM_GET_LOCAL(0)), WASM_BREAK(0)),
                         WASM_SET_LOCAL(0, WASM_INT32_SUB(WASM_GET_LOCAL(0),
                                                          WASM_INT8(1)))),
            WASM_RETURN(WASM_GET_LOCAL(0))));
  CHECK_EQ(0, r.Call(1));
  CHECK_EQ(0, r.Call(10));
  CHECK_EQ(0, r.Call(100));
}


TEST(Run_Wasm_WhileCountDown) {
  WasmRunner<int32_t> r(kMachInt32);
  BUILD(r, WASM_BLOCK(
               2, WASM_WHILE(WASM_GET_LOCAL(0),
                             WASM_SET_LOCAL(0, WASM_INT32_SUB(WASM_GET_LOCAL(0),
                                                              WASM_INT8(1)))),
               WASM_RETURN(WASM_GET_LOCAL(0))));
  CHECK_EQ(0, r.Call(1));
  CHECK_EQ(0, r.Call(10));
  CHECK_EQ(0, r.Call(100));
}


// A helper for allocating small module environments on the stack.
template <typename ElemType, size_t kSize>
class TestModule : public ModuleEnv {
 public:
  TestModule() : size(kSize) {
    STATIC_ASSERT(kSize * sizeof(ElemType) <= 1024);
    mem_start = reinterpret_cast<uintptr_t>(data);
    mem_end = reinterpret_cast<uintptr_t>(data + kSize);
    zero();
  }
  size_t size;
  ElemType data[kSize];
  // Zero-initialize the memory.
  void zero() { memset(data, 0, mem_end - mem_start); }
  // Pseudo-randomly intialize the memory.
  void randomize(unsigned seed = 88) {
    byte* raw = reinterpret_cast<byte*>(mem_start);
    byte* end = reinterpret_cast<byte*>(mem_end);
    while (raw < end) {
      *raw = static_cast<byte>(rand_r(&seed));
      raw++;
    }
  }
};


TEST(Run_Wasm_LoadMemInt32) {
  WasmRunner<int32_t> r(kMachInt32);
  TestModule<int32_t, 8> module;
  module.randomize(1111);
  r.function_env->module = &module;

  BUILD(r, WASM_RETURN(WASM_LOAD_MEM(kMemInt32, WASM_INT8(0))));

  module.data[0] = 99999999;
  CHECK_EQ(99999999, r.Call(0));

  module.data[0] = 88888888;
  CHECK_EQ(88888888, r.Call(0));

  module.data[0] = 77777777;
  CHECK_EQ(77777777, r.Call(0));
}


TEST(Run_Wasm_LoadMemInt32_P) {
  WasmRunner<int32_t> r(kMachInt32);
  TestModule<int32_t, 8> module;
  module.randomize(2222);
  r.function_env->module = &module;

  BUILD(r, WASM_RETURN(WASM_LOAD_MEM(kMemInt32, WASM_GET_LOCAL(0))));

  for (int i = 0; i < module.size; i++) {
    CHECK_EQ(module.data[i], r.Call(i * 4));
  }
}


TEST(Run_Wasm_MemInt32_Sum) {
  WasmRunner<uint32_t> r(kMachInt32);
  const byte kSum = r.AllocateLocal(kAstInt32);
  TestModule<uint32_t, 20> module;
  r.function_env->module = &module;

  BUILD(
      r,
      WASM_BLOCK(
          2, WASM_WHILE(
                 WASM_GET_LOCAL(0),
                 WASM_BLOCK(2, WASM_SET_LOCAL(
                                   kSum, WASM_INT32_ADD(
                                             WASM_GET_LOCAL(kSum),
                                             WASM_LOAD_MEM(kMemInt32,
                                                           WASM_GET_LOCAL(0)))),
                            WASM_SET_LOCAL(0, WASM_INT32_SUB(WASM_GET_LOCAL(0),
                                                             WASM_INT8(4))))),
          WASM_RETURN(WASM_GET_LOCAL(1))));

  // Run 4 trials.
  for (int i = 0; i < 3; i++) {
    module.randomize(i * 33);
    uint32_t expected = 0;
    for (size_t j = module.size - 1; j > 0; j--) {
      expected += module.data[j];
    }
    uint32_t result = r.Call(static_cast<int>(4 * (module.size - 1)));
    CHECK_EQ(expected, result);
  }
}


TEST(Run_Wasm_MemFloat32_Sum) {
  WasmRunner<int32_t> r(kMachInt32);
  const byte kSum = r.AllocateLocal(kAstFloat32);
  ModuleEnv module;
  const int kSize = 5;
  float buffer[kSize] = {-99.25, -888.25, -77.25, 66666.25, 5555.25};
  module.mem_start = reinterpret_cast<uintptr_t>(&buffer);
  module.mem_end = reinterpret_cast<uintptr_t>(&buffer[kSize]);
  r.function_env->module = &module;

  BUILD(
      r,
      WASM_BLOCK(
          3, WASM_WHILE(
                 WASM_GET_LOCAL(0),
                 WASM_BLOCK(2, WASM_SET_LOCAL(
                                   kSum, WASM_FLOAT32_ADD(
                                             WASM_GET_LOCAL(kSum),
                                             WASM_LOAD_MEM(kMemFloat32,
                                                           WASM_GET_LOCAL(0)))),
                            WASM_SET_LOCAL(0, WASM_INT32_SUB(WASM_GET_LOCAL(0),
                                                             WASM_INT8(4))))),
          WASM_STORE_MEM(kMemFloat32, WASM_ZERO, WASM_GET_LOCAL(kSum)),
          WASM_RETURN(WASM_GET_LOCAL(0))));

  CHECK_EQ(0, r.Call(4 * (kSize - 1)));
  CHECK_NE(-99.25, buffer[0]);
  CHECK_EQ(71256.0f, buffer[0]);
}


template <typename T>
void GenerateAndRunFold(WasmOpcode binop, T* buffer, size_t size,
                        LocalType astType, MemType memType) {
  WasmRunner<int32_t> r(kMachInt32);
  const byte kAccum = r.AllocateLocal(astType);
  ModuleEnv module;
  module.mem_start = reinterpret_cast<uintptr_t>(buffer);
  module.mem_end = reinterpret_cast<uintptr_t>(buffer + size);
  r.function_env->module = &module;

  BUILD(r,
        WASM_BLOCK(
            4, WASM_SET_LOCAL(kAccum, WASM_LOAD_MEM(memType, WASM_ZERO)),
            WASM_WHILE(
                WASM_GET_LOCAL(0),
                WASM_BLOCK(
                    2, WASM_SET_LOCAL(
                           kAccum, WASM_BINOP(binop, WASM_GET_LOCAL(kAccum),
                                              WASM_LOAD_MEM(
                                                  memType, WASM_GET_LOCAL(0)))),
                    WASM_SET_LOCAL(0, WASM_INT32_SUB(WASM_GET_LOCAL(0),
                                                     WASM_INT8(sizeof(T)))))),
            WASM_STORE_MEM(memType, WASM_ZERO, WASM_GET_LOCAL(kAccum)),
            WASM_RETURN(WASM_GET_LOCAL(0))));
  r.Call(static_cast<int>(sizeof(T) * (size - 1)));
}


TEST(Run_Wasm_MemFloat64_Mul) {
  const size_t kSize = 6;
  double buffer[kSize] = {1, 2, 2, 2, 2, 2};
  GenerateAndRunFold<double>(kExprFloat64Mul, buffer, kSize, kAstFloat64,
                             kMemFloat64);
  CHECK_EQ(32, buffer[0]);
}


TEST(Run_Wasm_Switch0) {
  WasmRunner<int32_t> r(kMachInt32);
  BUILD(r, WASM_BLOCK(2, WASM_ID(kStmtSwitch, 0, WASM_GET_LOCAL(0)),
                      WASM_RETURN(WASM_GET_LOCAL(0))));
  CHECK_EQ(0, r.Call(0));
  CHECK_EQ(1, r.Call(1));
  CHECK_EQ(2, r.Call(2));
  CHECK_EQ(32, r.Call(32));
}


TEST(Run_Wasm_Switch1) {
  WasmRunner<int32_t> r(kMachInt32);
  BUILD(r, WASM_BLOCK(2, WASM_SWITCH(1, WASM_GET_LOCAL(0),
                                     WASM_SET_LOCAL(0, WASM_INT8(44))),
                      WASM_RETURN(WASM_GET_LOCAL(0))));
  CHECK_EQ(44, r.Call(0));
  CHECK_EQ(1, r.Call(1));
  CHECK_EQ(2, r.Call(2));
  CHECK_EQ(-834, -834);
}


TEST(Run_Wasm_Switch4_fallthru) {
  WasmRunner<int32_t> r(kMachInt32);
  BUILD(r, WASM_BLOCK(2, WASM_SWITCH(4,                            // --
                                     WASM_GET_LOCAL(0),            // key
                                     WASM_NOP,                     // case 0
                                     WASM_RETURN(WASM_INT8(45)),   // case 1
                                     WASM_NOP,                     // case 2
                                     WASM_RETURN(WASM_INT8(47))),  // case 3
                      WASM_RETURN(WASM_GET_LOCAL(0))));

  CHECK_EQ(-1, r.Call(-1));
  CHECK_EQ(45, r.Call(0));
  CHECK_EQ(45, r.Call(1));
  CHECK_EQ(47, r.Call(2));
  CHECK_EQ(47, r.Call(3));
  CHECK_EQ(4, r.Call(4));
  CHECK_EQ(-834, -834);
}


#define APPEND(code, pos, fragment)                 \
  do {                                              \
    memcpy(code + pos, fragment, sizeof(fragment)); \
    pos += sizeof(fragment);                        \
  } while (false)


TEST(Run_Wasm_Switch_Ret_N) {
  Zone zone;
  for (int i = 3; i < 256; i += 28) {
    byte header[] = {kStmtBlock, 2, kStmtSwitch, static_cast<byte>(i),
                     kExprGetLocal, 0};
    byte ccase[] = {WASM_RETURN(WASM_INT32(i))};
    byte footer[] = {WASM_RETURN(WASM_GET_LOCAL(0))};
    byte* code = zone.NewArray<byte>(sizeof(header) + i * sizeof(ccase) +
                                     sizeof(footer));
    int pos = 0;
    // Add header code.
    APPEND(code, pos, header);
    for (int j = 0; j < i; j++) {
      // Add case code.
      byte ccase[] = {WASM_RETURN(WASM_INT32((10 + j)))};
      APPEND(code, pos, ccase);
    }
    // Add footer code.
    APPEND(code, pos, footer);
    // Build graph.
    WasmRunner<int32_t> r(kMachInt32);
    r.Build(code, code + pos);
    // Run.
    for (int j = -1; j < i + 5; j++) {
      int expected = j >= 0 && j < i ? (10 + j) : j;
      CHECK_EQ(expected, r.Call(j));
    }
  }
}


TEST(Run_Wasm_Switch_Nf_N) {
  Zone zone;
  for (int i = 3; i < 256; i += 28) {
    byte header[] = {kStmtBlock, 2, kStmtSwitchNf, static_cast<byte>(i),
                     kExprGetLocal, 0};
    byte ccase[] = {WASM_SET_LOCAL(0, WASM_INT32(i))};
    byte footer[] = {WASM_RETURN(WASM_GET_LOCAL(0))};
    byte* code = zone.NewArray<byte>(sizeof(header) + i * sizeof(ccase) +
                                     sizeof(footer));
    int pos = 0;
    // Add header code.
    APPEND(code, pos, header);
    for (int j = 0; j < i; j++) {
      // Add case code.
      byte ccase[] = {WASM_SET_LOCAL(0, WASM_INT32((10 + j)))};
      APPEND(code, pos, ccase);
    }
    // Add footer code.
    APPEND(code, pos, footer);
    // Build graph.
    WasmRunner<int32_t> r(kMachInt32);
    r.Build(code, code + pos);
    // Run.
    for (int j = -1; j < i + 5; j++) {
      int expected = j >= 0 && j < i ? (10 + j) : j;
      CHECK_EQ(expected, r.Call(j));
    }
  }
}


TEST(Build_Wasm_Infinite_Loop) {
  WasmRunner<int32_t> r(kMachInt32);
  // Only build the graph, don't run the code.
  BUILD(r, WASM_INFINITE_LOOP);
}


TEST(Run_Wasm_Infinite_Loop_not_taken) {
  WasmRunner<int32_t> r(kMachInt32);
  BUILD(r, WASM_IF_THEN(WASM_GET_LOCAL(0), WASM_INFINITE_LOOP,
                        WASM_RETURN(WASM_INT8(45))));
  // Run the code, but don't go into the infinite loop.
  CHECK_EQ(45, r.Call(0));
}


static void TestBuildGraphForUnop(WasmOpcode opcode, FunctionSig* sig) {
  WasmRunner<int32_t> r(kMachInt32);
  CommonSignatures::init_env(r.function_env, sig);
  BUILD(r, kStmtReturn, static_cast<byte>(opcode), kExprGetLocal, 0);
}


static void TestBuildGraphForBinop(WasmOpcode opcode, FunctionSig* sig) {
  WasmRunner<int32_t> r(kMachInt32, kMachInt32);
  CommonSignatures::init_env(r.function_env, sig);
  BUILD(r, kStmtReturn, static_cast<byte>(opcode), kExprGetLocal, 0,
        kExprGetLocal, 1);
}


TEST(Build_Wasm_SimpleExprs) {
// Test that the decoder can build a graph for all supported simple expressions.
#define GRAPH_BUILD_TEST(name, opcode, sig)                 \
  if (WasmOpcodes::IsSupported(kExpr##name)) {              \
    FunctionSig* sig = WasmOpcodes::Signature(kExpr##name); \
    if (sig->parameter_count() == 1) {                      \
      TestBuildGraphForUnop(kExpr##name, sig);              \
    } else {                                                \
      TestBuildGraphForBinop(kExpr##name, sig);             \
    }                                                       \
  }

  FOREACH_SIMPLE_EXPR_OPCODE(GRAPH_BUILD_TEST);

#undef GRAPH_BUILD_TEST
}


TEST(Run_Wasm_Int32LoadInt8_signext) {
  TestModule<int8_t, 16> module;
  module.randomize();
  module.data[0] = -1;
  WasmRunner<int32_t> r(kMachInt32);
  r.function_env->module = &module;
  BUILD(r, WASM_RETURN(WASM_LOAD_MEM(kMemInt8, WASM_GET_LOCAL(0))));

  for (size_t i = 0; i < module.size; i++) {
    CHECK_EQ(module.data[i], r.Call(static_cast<int>(i)));
  }
}


TEST(Run_Wasm_Int32LoadInt8_zeroext) {
  TestModule<uint8_t, 16> module;
  module.randomize(77);
  module.data[0] = 255;
  WasmRunner<int32_t> r(kMachInt32);
  r.function_env->module = &module;
  BUILD(r, WASM_RETURN(WASM_LOAD_MEM(kMemUint8, WASM_GET_LOCAL(0))));

  for (size_t i = 0; i < module.size; i++) {
    CHECK_EQ(module.data[i], r.Call(static_cast<int>(i)));
  }
}


TEST(Run_Wasm_Int32LoadInt16_signext) {
  TestModule<uint8_t, 16> module;
  module.randomize(888);
  module.data[1] = 200;
  WasmRunner<int32_t> r(kMachInt32);
  r.function_env->module = &module;
  BUILD(r, WASM_RETURN(WASM_LOAD_MEM(kMemInt16, WASM_GET_LOCAL(0))));

  for (size_t i = 0; i < module.size; i += 2) {
    int32_t expected =
        module.data[i] | (static_cast<int8_t>(module.data[i + 1]) << 8);
    CHECK_EQ(expected, r.Call(static_cast<int>(i)));
  }
}


TEST(Run_Wasm_Int32LoadInt16_zeroext) {
  TestModule<uint8_t, 16> module;
  module.randomize(9999);
  module.data[1] = 204;
  WasmRunner<int32_t> r(kMachInt32);
  r.function_env->module = &module;
  BUILD(r, WASM_RETURN(WASM_LOAD_MEM(kMemUint16, WASM_GET_LOCAL(0))));

  for (size_t i = 0; i < module.size; i += 2) {
    int32_t expected = module.data[i] | (module.data[i + 1] << 8);
    CHECK_EQ(expected, r.Call(static_cast<int>(i)));
  }
}


#endif  // V8_TURBOFAN_TARGET
