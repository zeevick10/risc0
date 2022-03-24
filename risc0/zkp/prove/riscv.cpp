// Copyright 2022 Risc0, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "risc0/zkp/prove/riscv.h"

#include "risc0/core/log.h"
#include "risc0/zkp/core/rou.h"
#include "risc0/zkp/prove/step/exec.h"
#include "risc0/zkp/verify/riscv.h"

namespace risc0 {

namespace {

class RiscVProveCircuit : public ProveCircuit {
public:
  RiscVProveCircuit(const std::string& elfFile, MemoryHandler& io);
  TapSetRef getTaps() const override { return getRiscVTaps(); }
  void execute(WriteIOP& iop) override;
  void accumulate(WriteIOP& iop) override;
  void evalCheck(           //
      AccelSlice<Fp> check, // Output: Check polynomial
      // Evaluations of each polynomial on an extended domain
      AccelConstSlice<Fp> codeEval,  //
      AccelConstSlice<Fp> dataEval,  //
      AccelConstSlice<Fp> accumEval, //
      // Mix factor for polynomial constraints
      Fp4 polyMix) const override;

  uint32_t getPo2() const override { return po2_; }
  const std::vector<Fp>& getCode() const override { return exec_.code; }
  const std::vector<Fp>& getData() const override { return exec_.data; }
  const std::vector<Fp>& getAccum() const override { return accum_; }

private:
  ExecState exec_;
  std::vector<Fp> accum_;
  MemoryHandler& io_;
  uint32_t po2_;
};

} // namespace

RiscVProveCircuit::RiscVProveCircuit(const std::string& elfFile, MemoryHandler& io)
    : exec_(elfFile), io_(io) {}

void RiscVProveCircuit::execute(WriteIOP& iop) {
  // Run actual RISC-V execution
  exec_.run(kMaxCycles, io_);

  // Get # of timesteps
  po2_ = log2Ceil(exec_.context.numSteps);
  size_t size = size_t(1) << po2_;
  LOG(1, "size = " << size);

  // Write final low register state
  for (size_t i = 0; i < kOutputRegs; i++) {
    const Fp* globals = exec_.context.globals;
    uint32_t regVal = globals[2 * i].asUInt32() | (globals[2 * i + 1].asUInt32() << 16);
    LOG(2, "x" << i + 1 << " = " << hex(regVal));
    iop.write(&regVal, 1);
  }

  // Write the po2 for size
  iop.write(&po2_, 1);

  // Now, do memory verification
  for (size_t i = 0; i < size - kZkCycles; i++) {
    exec_.context.curStep = i;
    dataStepCheck(exec_.context, exec_.code.data(), exec_.data.data());
  }

#ifdef CIRCUIT_DEBUG
  // Set any unset values to dead
  for (Fp& x : exec_.data) {
    if (x == Fp::invalid()) {
      x = 0xdead;
    }
  }
#endif

#ifndef CIRCUIT_DEBUG
  // Fill final cycles of data with noise to make ZK
  for (size_t i = 0; i < kDataSize; i++) {
    for (size_t j = size - kZkCycles; j < size; j++) {
      exec_.data[i * size + j] = Fp::random(CryptoRng::shared());
    }
  }
#endif
}

void RiscVProveCircuit::accumulate(WriteIOP& iop) {
  size_t size = size_t(1) << po2_;
  LOG(1, "size = " << size);
  // Fill in accum mix
  for (size_t i = 0; i < kAccumMixGlobalSize; i++) {
    exec_.context.globals[kAccumMixGlobalOffset + i] = Fp::random(iop);
  }

  // Generate accum
#ifdef CIRCUIT_DEBUG
  accum_.resize(kAccumSize * size, Fp::invalid());
#else
  accum_.resize(kAccumSize * size);
#endif
  LOG(1, "accum_.size() == " << accum_.size());
  for (size_t i = 0; i < size - kZkCycles; i++) {
    exec_.context.curStep = i;
    accumStep(exec_.context, exec_.code.data(), exec_.data.data(), accum_.data());
  }

#ifdef CIRCUIT_DEBUG
  // Set any unset values to dead
  for (Fp& x : accum_) {
    if (x == Fp::invalid()) {
      x = 0xdead;
    }
  }
#endif

#ifndef CIRCUIT_DEBUG
  // Fill final cycles of accum with noise to make ZK
  for (size_t i = 0; i < kAccumSize; i++) {
    for (size_t j = size - kZkCycles; j < size; j++) {
      accum_[i * size + j] = Fp::random(CryptoRng::shared());
    }
  }
#endif
}

void RiscVProveCircuit::evalCheck( //
    AccelSlice<Fp> check,          // Output: Check polynomial
    // Evaluations of each polynomial on an extended domain
    AccelConstSlice<Fp> codeEval,  //
    AccelConstSlice<Fp> dataEval,  //
    AccelConstSlice<Fp> accumEval, //
    // Mix factor for polynomial constraints
    Fp4 polyMix) const {
  size_t size = size_t(1) << po2_;
  size_t domain = size * kInvRate;
  evalCheckPolyAccel(check,
                     codeEval,
                     dataEval,
                     accumEval,
                     AccelSlice<Fp>::copy(exec_.context.globals, kGlobalSize),
                     AccelSlice<Fp4>::copy(&polyMix, 1),
                     AccelSlice<Fp>::copy(kRouFwd, kMaxRouPo2 + 1),
                     domain);
}

std::unique_ptr<ProveCircuit> getRiscVProveCircuit(const std::string& elfFile, MemoryHandler& io) {
  return std::make_unique<RiscVProveCircuit>(elfFile, io);
}

} // namespace risc0