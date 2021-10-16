// Copyright (c) 2016 Google Inc.
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

#include "spirv-tools/optimizer.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "source/opt/build_module.h"
#include "source/opt/graphics_robust_access_pass.h"
#include "source/opt/log.h"
#include "source/opt/pass_manager.h"
#include "source/opt/passes.h"
#include "source/spirv_optimizer_options.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

namespace spvtools {

struct Optimizer::PassToken::Impl {
  Impl(std::unique_ptr<opt::Pass> p) : pass(std::move(p)) {}

  std::unique_ptr<opt::Pass> pass;  // Internal implementation pass.
};

Optimizer::PassToken::PassToken(
    std::unique_ptr<Optimizer::PassToken::Impl> impl)
    : impl_(std::move(impl)) {}

Optimizer::PassToken::PassToken(std::unique_ptr<opt::Pass>&& pass)
    : impl_(MakeUnique<Optimizer::PassToken::Impl>(std::move(pass))) {}

Optimizer::PassToken::PassToken(PassToken&& that)
    : impl_(std::move(that.impl_)) {}

Optimizer::PassToken& Optimizer::PassToken::operator=(PassToken&& that) {
  impl_ = std::move(that.impl_);
  return *this;
}

Optimizer::PassToken::~PassToken() {}

struct Optimizer::Impl {
  explicit Impl(spv_target_env env) : target_env(env), pass_manager() {}

  spv_target_env target_env;      // Target environment.
  opt::PassManager pass_manager;  // Internal implementation pass manager.
};

Optimizer::Optimizer(spv_target_env env) : impl_(new Impl(env)) {}

Optimizer::~Optimizer() {}

void Optimizer::SetMessageConsumer(MessageConsumer c) {
  // All passes' message consumer needs to be updated.
  for (uint32_t i = 0; i < impl_->pass_manager.NumPasses(); ++i) {
    impl_->pass_manager.GetPass(i)->SetMessageConsumer(c);
  }
  impl_->pass_manager.SetMessageConsumer(std::move(c));
}

const MessageConsumer& Optimizer::consumer() const {
  return impl_->pass_manager.consumer();
}

Optimizer& Optimizer::RegisterPass(PassToken&& p) {
  // Change to use the pass manager's consumer.
  p.impl_->pass->SetMessageConsumer(consumer());
  impl_->pass_manager.AddPass(std::move(p.impl_->pass));
  return *this;
}

// The legalization passes take a spir-v shader generated by an HLSL front-end
// and turn it into a valid vulkan spir-v shader.  There are two ways in which
// the code will be invalid at the start:
//
// 1) There will be opaque objects, like images, which will be passed around
//    in intermediate objects.  Valid spir-v will have to replace the use of
//    the opaque object with an intermediate object that is the result of the
//    load of the global opaque object.
//
// 2) There will be variables that contain pointers to structured or uniform
//    buffers.  It be legal, the variables must be eliminated, and the
//    references to the structured buffers must use the result of OpVariable
//    in the Uniform storage class.
//
// Optimization in this list must accept shaders with these relaxation of the
// rules.  There is not guarantee that this list of optimizations is able to
// legalize all inputs, but it is on a best effort basis.
//
// The legalization problem is essentially a very general copy propagation
// problem.  The optimization we use are all used to either do copy propagation
// or enable more copy propagation.
Optimizer& Optimizer::RegisterLegalizationPasses() {
  return
      // Wrap OpKill instructions so all other code can be inlined.
      RegisterPass(CreateWrapOpKillPass())
          // Remove unreachable block so that merge return works.
          .RegisterPass(CreateDeadBranchElimPass())
          // Merge the returns so we can inline.
          .RegisterPass(CreateMergeReturnPass())
          // Make sure uses and definitions are in the same function.
          .RegisterPass(CreateInlineExhaustivePass())
          // Make private variable function scope
          .RegisterPass(CreateEliminateDeadFunctionsPass())
          .RegisterPass(CreatePrivateToLocalPass())
          // Fix up the storage classes that DXC may have purposely generated
          // incorrectly.  All functions are inlined, and a lot of dead code has
          // been removed.
          .RegisterPass(CreateFixStorageClassPass())
          // Propagate the value stored to the loads in very simple cases.
          .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
          .RegisterPass(CreateLocalSingleStoreElimPass())
          .RegisterPass(CreateAggressiveDCEPass())
          // Split up aggregates so they are easier to deal with.
          .RegisterPass(CreateScalarReplacementPass(0))
          // Remove loads and stores so everything is in intermediate values.
          // Takes care of copy propagation of non-members.
          .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
          .RegisterPass(CreateLocalSingleStoreElimPass())
          .RegisterPass(CreateAggressiveDCEPass())
          .RegisterPass(CreateLocalMultiStoreElimPass())
          .RegisterPass(CreateAggressiveDCEPass())
          // Propagate constants to get as many constant conditions on branches
          // as possible.
          .RegisterPass(CreateCCPPass())
          .RegisterPass(CreateLoopUnrollPass(true))
          .RegisterPass(CreateDeadBranchElimPass())
          // Copy propagate members.  Cleans up code sequences generated by
          // scalar replacement.  Also important for removing OpPhi nodes.
          .RegisterPass(CreateSimplificationPass())
          .RegisterPass(CreateAggressiveDCEPass())
          .RegisterPass(CreateCopyPropagateArraysPass())
          // May need loop unrolling here see
          // https://github.com/Microsoft/DirectXShaderCompiler/pull/930
          // Get rid of unused code that contain traces of illegal code
          // or unused references to unbound external objects
          .RegisterPass(CreateVectorDCEPass())
          .RegisterPass(CreateDeadInsertElimPass())
          .RegisterPass(CreateReduceLoadSizePass())
          .RegisterPass(CreateAggressiveDCEPass());
}

Optimizer& Optimizer::RegisterPerformancePasses() {
  return RegisterPass(CreateWrapOpKillPass())
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateMergeReturnPass())
      .RegisterPass(CreateInlineExhaustivePass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreatePrivateToLocalPass())
      .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
      .RegisterPass(CreateLocalSingleStoreElimPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateScalarReplacementPass())
      .RegisterPass(CreateLocalAccessChainConvertPass())
      .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
      .RegisterPass(CreateLocalSingleStoreElimPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateLocalMultiStoreElimPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateCCPPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateLoopUnrollPass(true))
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateRedundancyEliminationPass())
      .RegisterPass(CreateCombineAccessChainsPass())
      .RegisterPass(CreateSimplificationPass())
      .RegisterPass(CreateScalarReplacementPass())
      .RegisterPass(CreateLocalAccessChainConvertPass())
      .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
      .RegisterPass(CreateLocalSingleStoreElimPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateSSARewritePass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateVectorDCEPass())
      .RegisterPass(CreateDeadInsertElimPass())
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateSimplificationPass())
      .RegisterPass(CreateIfConversionPass())
      .RegisterPass(CreateCopyPropagateArraysPass())
      .RegisterPass(CreateReduceLoadSizePass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateBlockMergePass())
      .RegisterPass(CreateRedundancyEliminationPass())
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateBlockMergePass())
      .RegisterPass(CreateSimplificationPass())
      // UE Change Begin
      .RegisterPass(CreateEliminateDeadMembersPass())
      .RegisterPass(CreateLocalSingleStoreElimPass())
      .RegisterPass(CreateBlockMergePass())
      .RegisterPass(CreateLocalMultiStoreElimPass())
      .RegisterPass(CreateRedundancyEliminationPass())
      .RegisterPass(CreateSimplificationPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateCFGCleanupPass());
      // UE Change End
      ;
}

Optimizer& Optimizer::RegisterSizePasses() {
  return RegisterPass(CreateWrapOpKillPass())
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateMergeReturnPass())
      .RegisterPass(CreateInlineExhaustivePass())
      .RegisterPass(CreateEliminateDeadFunctionsPass())
      .RegisterPass(CreatePrivateToLocalPass())
      .RegisterPass(CreateScalarReplacementPass(0))
      .RegisterPass(CreateLocalMultiStoreElimPass())
      .RegisterPass(CreateCCPPass())
      .RegisterPass(CreateLoopUnrollPass(true))
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateSimplificationPass())
      .RegisterPass(CreateScalarReplacementPass(0))
      .RegisterPass(CreateLocalSingleStoreElimPass())
      .RegisterPass(CreateIfConversionPass())
      .RegisterPass(CreateSimplificationPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateBlockMergePass())
      .RegisterPass(CreateLocalAccessChainConvertPass())
      .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateCopyPropagateArraysPass())
      .RegisterPass(CreateVectorDCEPass())
      .RegisterPass(CreateDeadInsertElimPass())
      .RegisterPass(CreateEliminateDeadMembersPass())
      .RegisterPass(CreateLocalSingleStoreElimPass())
      .RegisterPass(CreateBlockMergePass())
      .RegisterPass(CreateLocalMultiStoreElimPass())
      .RegisterPass(CreateRedundancyEliminationPass())
      .RegisterPass(CreateSimplificationPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateCFGCleanupPass());
}

Optimizer& Optimizer::RegisterVulkanToWebGPUPasses() {
  return RegisterPass(CreateStripAtomicCounterMemoryPass())
      .RegisterPass(CreateGenerateWebGPUInitializersPass())
      .RegisterPass(CreateLegalizeVectorShufflePass())
      .RegisterPass(CreateSplitInvalidUnreachablePass())
      .RegisterPass(CreateEliminateDeadConstantPass())
      .RegisterPass(CreateFlattenDecorationPass())
      .RegisterPass(CreateAggressiveDCEPass())
      .RegisterPass(CreateDeadBranchElimPass())
      .RegisterPass(CreateCompactIdsPass());
}

Optimizer& Optimizer::RegisterWebGPUToVulkanPasses() {
  return RegisterPass(CreateDecomposeInitializedVariablesPass())
      .RegisterPass(CreateCompactIdsPass());
}

bool Optimizer::RegisterPassesFromFlags(const std::vector<std::string>& flags) {
  for (const auto& flag : flags) {
    if (!RegisterPassFromFlag(flag)) {
      return false;
    }
  }

  return true;
}

bool Optimizer::FlagHasValidForm(const std::string& flag) const {
  if (flag == "-O" || flag == "-Os") {
    return true;
  } else if (flag.size() > 2 && flag.substr(0, 2) == "--") {
    return true;
  }

  Errorf(consumer(), nullptr, {},
         "%s is not a valid flag.  Flag passes should have the form "
         "'--pass_name[=pass_args]'. Special flag names also accepted: -O "
         "and -Os.",
         flag.c_str());
  return false;
}

bool Optimizer::RegisterPassFromFlag(const std::string& flag) {
  if (!FlagHasValidForm(flag)) {
    return false;
  }

  // Split flags of the form --pass_name=pass_args.
  auto p = utils::SplitFlagArgs(flag);
  std::string pass_name = p.first;
  std::string pass_args = p.second;

  // FIXME(dnovillo): This should be re-factored so that pass names can be
  // automatically checked against Pass::name() and PassToken instances created
  // via a template function.  Additionally, class Pass should have a desc()
  // method that describes the pass (so it can be used in --help).
  //
  // Both Pass::name() and Pass::desc() should be static class members so they
  // can be invoked without creating a pass instance.
  if (pass_name == "strip-atomic-counter-memory") {
    RegisterPass(CreateStripAtomicCounterMemoryPass());
  } else if (pass_name == "strip-debug") {
    RegisterPass(CreateStripDebugInfoPass());
  } else if (pass_name == "strip-reflect") {
    RegisterPass(CreateStripReflectInfoPass());
  } else if (pass_name == "set-spec-const-default-value") {
    if (pass_args.size() > 0) {
      auto spec_ids_vals =
          opt::SetSpecConstantDefaultValuePass::ParseDefaultValuesString(
              pass_args.c_str());
      if (!spec_ids_vals) {
        Errorf(consumer(), nullptr, {},
               "Invalid argument for --set-spec-const-default-value: %s",
               pass_args.c_str());
        return false;
      }
      RegisterPass(
          CreateSetSpecConstantDefaultValuePass(std::move(*spec_ids_vals)));
    } else {
      Errorf(consumer(), nullptr, {},
             "Invalid spec constant value string '%s'. Expected a string of "
             "<spec id>:<default value> pairs.",
             pass_args.c_str());
      return false;
    }
  } else if (pass_name == "if-conversion") {
    RegisterPass(CreateIfConversionPass());
  } else if (pass_name == "freeze-spec-const") {
    RegisterPass(CreateFreezeSpecConstantValuePass());
  } else if (pass_name == "inline-entry-points-exhaustive") {
    RegisterPass(CreateInlineExhaustivePass());
  } else if (pass_name == "inline-entry-points-opaque") {
    RegisterPass(CreateInlineOpaquePass());
  } else if (pass_name == "combine-access-chains") {
    RegisterPass(CreateCombineAccessChainsPass());
  } else if (pass_name == "convert-local-access-chains") {
    RegisterPass(CreateLocalAccessChainConvertPass());
  } else if (pass_name == "descriptor-scalar-replacement") {
    RegisterPass(CreateDescriptorScalarReplacementPass());
  } else if (pass_name == "eliminate-dead-code-aggressive") {
    RegisterPass(CreateAggressiveDCEPass());
  } else if (pass_name == "propagate-line-info") {
    RegisterPass(CreatePropagateLineInfoPass());
  } else if (pass_name == "eliminate-redundant-line-info") {
    RegisterPass(CreateRedundantLineInfoElimPass());
  } else if (pass_name == "eliminate-insert-extract") {
    RegisterPass(CreateInsertExtractElimPass());
  } else if (pass_name == "eliminate-local-single-block") {
    RegisterPass(CreateLocalSingleBlockLoadStoreElimPass());
  } else if (pass_name == "eliminate-local-single-store") {
    RegisterPass(CreateLocalSingleStoreElimPass());
  } else if (pass_name == "merge-blocks") {
    RegisterPass(CreateBlockMergePass());
  } else if (pass_name == "merge-return") {
    RegisterPass(CreateMergeReturnPass());
  } else if (pass_name == "eliminate-dead-branches") {
    RegisterPass(CreateDeadBranchElimPass());
  } else if (pass_name == "eliminate-dead-functions") {
    RegisterPass(CreateEliminateDeadFunctionsPass());
  } else if (pass_name == "eliminate-local-multi-store") {
    RegisterPass(CreateLocalMultiStoreElimPass());
  } else if (pass_name == "eliminate-dead-const") {
    RegisterPass(CreateEliminateDeadConstantPass());
  } else if (pass_name == "eliminate-dead-inserts") {
    RegisterPass(CreateDeadInsertElimPass());
  } else if (pass_name == "eliminate-dead-variables") {
    RegisterPass(CreateDeadVariableEliminationPass());
  } else if (pass_name == "eliminate-dead-members") {
    RegisterPass(CreateEliminateDeadMembersPass());
  } else if (pass_name == "fold-spec-const-op-composite") {
    RegisterPass(CreateFoldSpecConstantOpAndCompositePass());
  } else if (pass_name == "loop-unswitch") {
    RegisterPass(CreateLoopUnswitchPass());
  } else if (pass_name == "scalar-replacement") {
    if (pass_args.size() == 0) {
      RegisterPass(CreateScalarReplacementPass());
    } else {
      int limit = -1;
      if (pass_args.find_first_not_of("0123456789") == std::string::npos) {
        limit = atoi(pass_args.c_str());
      }

      if (limit >= 0) {
        RegisterPass(CreateScalarReplacementPass(limit));
      } else {
        Error(consumer(), nullptr, {},
              "--scalar-replacement must have no arguments or a non-negative "
              "integer argument");
        return false;
      }
    }
  } else if (pass_name == "strength-reduction") {
    RegisterPass(CreateStrengthReductionPass());
  } else if (pass_name == "unify-const") {
    RegisterPass(CreateUnifyConstantPass());
  } else if (pass_name == "flatten-decorations") {
    RegisterPass(CreateFlattenDecorationPass());
  } else if (pass_name == "compact-ids") {
    RegisterPass(CreateCompactIdsPass());
  } else if (pass_name == "cfg-cleanup") {
    RegisterPass(CreateCFGCleanupPass());
  } else if (pass_name == "local-redundancy-elimination") {
    RegisterPass(CreateLocalRedundancyEliminationPass());
  } else if (pass_name == "loop-invariant-code-motion") {
    RegisterPass(CreateLoopInvariantCodeMotionPass());
  } else if (pass_name == "reduce-load-size") {
    RegisterPass(CreateReduceLoadSizePass());
  } else if (pass_name == "redundancy-elimination") {
    RegisterPass(CreateRedundancyEliminationPass());
  } else if (pass_name == "private-to-local") {
    RegisterPass(CreatePrivateToLocalPass());
  } else if (pass_name == "remove-duplicates") {
    RegisterPass(CreateRemoveDuplicatesPass());
  } else if (pass_name == "workaround-1209") {
    RegisterPass(CreateWorkaround1209Pass());
  } else if (pass_name == "replace-invalid-opcode") {
    RegisterPass(CreateReplaceInvalidOpcodePass());
  } else if (pass_name == "inst-bindless-check") {
    RegisterPass(CreateInstBindlessCheckPass(7, 23, false, false, 2));
    RegisterPass(CreateSimplificationPass());
    RegisterPass(CreateDeadBranchElimPass());
    RegisterPass(CreateBlockMergePass());
    RegisterPass(CreateAggressiveDCEPass());
  } else if (pass_name == "inst-desc-idx-check") {
    RegisterPass(CreateInstBindlessCheckPass(7, 23, true, true, 2));
    RegisterPass(CreateSimplificationPass());
    RegisterPass(CreateDeadBranchElimPass());
    RegisterPass(CreateBlockMergePass());
    RegisterPass(CreateAggressiveDCEPass());
  } else if (pass_name == "inst-buff-addr-check") {
    RegisterPass(CreateInstBuffAddrCheckPass(7, 23, 2));
    RegisterPass(CreateAggressiveDCEPass());
  } else if (pass_name == "convert-relaxed-to-half") {
    RegisterPass(CreateConvertRelaxedToHalfPass());
  } else if (pass_name == "relax-float-ops") {
    RegisterPass(CreateRelaxFloatOpsPass());
  } else if (pass_name == "inst-debug-printf") {
    RegisterPass(CreateInstDebugPrintfPass(7, 23));
  } else if (pass_name == "simplify-instructions") {
    RegisterPass(CreateSimplificationPass());
  } else if (pass_name == "ssa-rewrite") {
    RegisterPass(CreateSSARewritePass());
  } else if (pass_name == "copy-propagate-arrays") {
    RegisterPass(CreateCopyPropagateArraysPass());
  } else if (pass_name == "loop-fission") {
    int register_threshold_to_split =
        (pass_args.size() > 0) ? atoi(pass_args.c_str()) : -1;
    if (register_threshold_to_split > 0) {
      RegisterPass(CreateLoopFissionPass(
          static_cast<size_t>(register_threshold_to_split)));
    } else {
      Error(consumer(), nullptr, {},
            "--loop-fission must have a positive integer argument");
      return false;
    }
  } else if (pass_name == "loop-fusion") {
    int max_registers_per_loop =
        (pass_args.size() > 0) ? atoi(pass_args.c_str()) : -1;
    if (max_registers_per_loop > 0) {
      RegisterPass(
          CreateLoopFusionPass(static_cast<size_t>(max_registers_per_loop)));
    } else {
      Error(consumer(), nullptr, {},
            "--loop-fusion must have a positive integer argument");
      return false;
    }
  } else if (pass_name == "loop-unroll") {
    RegisterPass(CreateLoopUnrollPass(true));
  } else if (pass_name == "upgrade-memory-model") {
    RegisterPass(CreateUpgradeMemoryModelPass());
  } else if (pass_name == "vector-dce") {
    RegisterPass(CreateVectorDCEPass());
  } else if (pass_name == "loop-unroll-partial") {
    int factor = (pass_args.size() > 0) ? atoi(pass_args.c_str()) : 0;
    if (factor > 0) {
      RegisterPass(CreateLoopUnrollPass(false, factor));
    } else {
      Error(consumer(), nullptr, {},
            "--loop-unroll-partial must have a positive integer argument");
      return false;
    }
  } else if (pass_name == "loop-peeling") {
    RegisterPass(CreateLoopPeelingPass());
  } else if (pass_name == "loop-peeling-threshold") {
    int factor = (pass_args.size() > 0) ? atoi(pass_args.c_str()) : 0;
    if (factor > 0) {
      opt::LoopPeelingPass::SetLoopPeelingThreshold(factor);
    } else {
      Error(consumer(), nullptr, {},
            "--loop-peeling-threshold must have a positive integer argument");
      return false;
    }
  } else if (pass_name == "ccp") {
    RegisterPass(CreateCCPPass());
  } else if (pass_name == "code-sink") {
    RegisterPass(CreateCodeSinkingPass());
  } else if (pass_name == "fix-storage-class") {
    RegisterPass(CreateFixStorageClassPass());
  } else if (pass_name == "O") {
    RegisterPerformancePasses();
  } else if (pass_name == "Os") {
    RegisterSizePasses();
  } else if (pass_name == "legalize-hlsl") {
    RegisterLegalizationPasses();
  } else if (pass_name == "generate-webgpu-initializers") {
    RegisterPass(CreateGenerateWebGPUInitializersPass());
  } else if (pass_name == "legalize-vector-shuffle") {
    RegisterPass(CreateLegalizeVectorShufflePass());
  /* UE Change Begin: Implement a fused-multiply-add pass to reduce the possibility of reassociation. */
  } else if (pass_name == "fused-multiply-add") {
    RegisterPass(CreateFusedMultiplyAddPass());
  /* UE Change End: Implement a fused-multiply-add pass to reduce the possibility of reassociation. */
  } else if (pass_name == "split-invalid-unreachable") {
    RegisterPass(CreateSplitInvalidUnreachablePass());
  } else if (pass_name == "decompose-initialized-variables") {
    RegisterPass(CreateDecomposeInitializedVariablesPass());
  } else if (pass_name == "graphics-robust-access") {
    RegisterPass(CreateGraphicsRobustAccessPass());
  } else if (pass_name == "wrap-opkill") {
    RegisterPass(CreateWrapOpKillPass());
  } else if (pass_name == "amd-ext-to-khr") {
    RegisterPass(CreateAmdExtToKhrPass());
  } else {
    Errorf(consumer(), nullptr, {},
           "Unknown flag '--%s'. Use --help for a list of valid flags",
           pass_name.c_str());
    return false;
  }

  return true;
}

void Optimizer::SetTargetEnv(const spv_target_env env) {
  impl_->target_env = env;
}

bool Optimizer::Run(const uint32_t* original_binary,
                    const size_t original_binary_size,
                    std::vector<uint32_t>* optimized_binary) const {
  return Run(original_binary, original_binary_size, optimized_binary,
             OptimizerOptions());
}

bool Optimizer::Run(const uint32_t* original_binary,
                    const size_t original_binary_size,
                    std::vector<uint32_t>* optimized_binary,
                    const ValidatorOptions& validator_options,
                    bool skip_validation) const {
  OptimizerOptions opt_options;
  opt_options.set_run_validator(!skip_validation);
  opt_options.set_validator_options(validator_options);
  return Run(original_binary, original_binary_size, optimized_binary,
             opt_options);
}

bool Optimizer::Run(const uint32_t* original_binary,
                    const size_t original_binary_size,
                    std::vector<uint32_t>* optimized_binary,
                    const spv_optimizer_options opt_options) const {
  spvtools::SpirvTools tools(impl_->target_env);
  tools.SetMessageConsumer(impl_->pass_manager.consumer());
  if (opt_options->run_validator_ &&
      !tools.Validate(original_binary, original_binary_size,
                      &opt_options->val_options_)) {
    return false;
  }

  std::unique_ptr<opt::IRContext> context = BuildModule(
      impl_->target_env, consumer(), original_binary, original_binary_size);
  if (context == nullptr) return false;

  context->set_max_id_bound(opt_options->max_id_bound_);
  context->set_preserve_bindings(opt_options->preserve_bindings_);
  context->set_preserve_spec_constants(opt_options->preserve_spec_constants_);

  impl_->pass_manager.SetValidatorOptions(&opt_options->val_options_);
  impl_->pass_manager.SetTargetEnv(impl_->target_env);
  auto status = impl_->pass_manager.Run(context.get());

  if (status == opt::Pass::Status::Failure) {
    return false;
  }

#ifndef NDEBUG
  // We do not keep the result id of DebugScope in struct DebugScope.
  // Instead, we assign random ids for them, which results in sanity
  // check failures. We want to skip the sanity check when the module
  // contains DebugScope instructions.
  if (status == opt::Pass::Status::SuccessWithoutChange &&
      !context->module()->ContainsDebugScope()) {
    std::vector<uint32_t> optimized_binary_with_nop;
    context->module()->ToBinary(&optimized_binary_with_nop,
                                /* skip_nop = */ false);
    assert(optimized_binary_with_nop.size() == original_binary_size &&
           "Binary size unexpectedly changed despite the optimizer saying "
           "there was no change");
    assert(memcmp(optimized_binary_with_nop.data(), original_binary,
                  original_binary_size) == 0 &&
           "Binary content unexpectedly changed despite the optimizer saying "
           "there was no change");
  }
#endif  // !NDEBUG

  // Note that |original_binary| and |optimized_binary| may share the same
  // buffer and the below will invalidate |original_binary|.
  optimized_binary->clear();
  context->module()->ToBinary(optimized_binary, /* skip_nop = */ true);

  return true;
}

Optimizer& Optimizer::SetPrintAll(std::ostream* out) {
  impl_->pass_manager.SetPrintAll(out);
  return *this;
}

Optimizer& Optimizer::SetTimeReport(std::ostream* out) {
  impl_->pass_manager.SetTimeReport(out);
  return *this;
}

Optimizer& Optimizer::SetValidateAfterAll(bool validate) {
  impl_->pass_manager.SetValidateAfterAll(validate);
  return *this;
}

Optimizer::PassToken CreateNullPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(MakeUnique<opt::NullPass>());
}

Optimizer::PassToken CreateStripAtomicCounterMemoryPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::StripAtomicCounterMemoryPass>());
}

Optimizer::PassToken CreateStripDebugInfoPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::StripDebugInfoPass>());
}

Optimizer::PassToken CreateStripReflectInfoPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::StripReflectInfoPass>());
}

Optimizer::PassToken CreateEliminateDeadFunctionsPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::EliminateDeadFunctionsPass>());
}

Optimizer::PassToken CreateEliminateDeadMembersPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::EliminateDeadMembersPass>());
}

Optimizer::PassToken CreateSetSpecConstantDefaultValuePass(
    const std::unordered_map<uint32_t, std::string>& id_value_map) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SetSpecConstantDefaultValuePass>(id_value_map));
}

Optimizer::PassToken CreateSetSpecConstantDefaultValuePass(
    const std::unordered_map<uint32_t, std::vector<uint32_t>>& id_value_map) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SetSpecConstantDefaultValuePass>(id_value_map));
}

Optimizer::PassToken CreateFlattenDecorationPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::FlattenDecorationPass>());
}

Optimizer::PassToken CreateFreezeSpecConstantValuePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::FreezeSpecConstantValuePass>());
}

Optimizer::PassToken CreateFoldSpecConstantOpAndCompositePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::FoldSpecConstantOpAndCompositePass>());
}

Optimizer::PassToken CreateUnifyConstantPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::UnifyConstantPass>());
}

Optimizer::PassToken CreateEliminateDeadConstantPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::EliminateDeadConstantPass>());
}

Optimizer::PassToken CreateDeadVariableEliminationPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::DeadVariableElimination>());
}

Optimizer::PassToken CreateStrengthReductionPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::StrengthReductionPass>());
}

Optimizer::PassToken CreateBlockMergePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::BlockMergePass>());
}

Optimizer::PassToken CreateInlineExhaustivePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::InlineExhaustivePass>());
}

Optimizer::PassToken CreateInlineOpaquePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::InlineOpaquePass>());
}

Optimizer::PassToken CreateLocalAccessChainConvertPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LocalAccessChainConvertPass>());
}

Optimizer::PassToken CreateLocalSingleBlockLoadStoreElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LocalSingleBlockLoadStoreElimPass>());
}

Optimizer::PassToken CreateLocalSingleStoreElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LocalSingleStoreElimPass>());
}

Optimizer::PassToken CreateInsertExtractElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SimplificationPass>());
}

Optimizer::PassToken CreateDeadInsertElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::DeadInsertElimPass>());
}

Optimizer::PassToken CreateDeadBranchElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::DeadBranchElimPass>());
}

Optimizer::PassToken CreateLocalMultiStoreElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SSARewritePass>());
}

Optimizer::PassToken CreateAggressiveDCEPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::AggressiveDCEPass>());
}

Optimizer::PassToken CreatePropagateLineInfoPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::ProcessLinesPass>(opt::kLinesPropagateLines));
}

Optimizer::PassToken CreateRedundantLineInfoElimPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::ProcessLinesPass>(opt::kLinesEliminateDeadLines));
}

Optimizer::PassToken CreateCompactIdsPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::CompactIdsPass>());
}

Optimizer::PassToken CreateMergeReturnPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::MergeReturnPass>());
}

std::vector<const char*> Optimizer::GetPassNames() const {
  std::vector<const char*> v;
  for (uint32_t i = 0; i < impl_->pass_manager.NumPasses(); i++) {
    v.push_back(impl_->pass_manager.GetPass(i)->name());
  }
  return v;
}

Optimizer::PassToken CreateCFGCleanupPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::CFGCleanupPass>());
}

Optimizer::PassToken CreateLocalRedundancyEliminationPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LocalRedundancyEliminationPass>());
}

Optimizer::PassToken CreateLoopFissionPass(size_t threshold) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LoopFissionPass>(threshold));
}

Optimizer::PassToken CreateLoopFusionPass(size_t max_registers_per_loop) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LoopFusionPass>(max_registers_per_loop));
}

Optimizer::PassToken CreateLoopInvariantCodeMotionPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(MakeUnique<opt::LICMPass>());
}

Optimizer::PassToken CreateLoopPeelingPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LoopPeelingPass>());
}

Optimizer::PassToken CreateLoopUnswitchPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LoopUnswitchPass>());
}

Optimizer::PassToken CreateRedundancyEliminationPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::RedundancyEliminationPass>());
}

Optimizer::PassToken CreateRemoveDuplicatesPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::RemoveDuplicatesPass>());
}

Optimizer::PassToken CreateScalarReplacementPass(uint32_t size_limit) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::ScalarReplacementPass>(size_limit));
}

Optimizer::PassToken CreatePrivateToLocalPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::PrivateToLocalPass>());
}

Optimizer::PassToken CreateCCPPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(MakeUnique<opt::CCPPass>());
}

Optimizer::PassToken CreateWorkaround1209Pass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::Workaround1209>());
}

Optimizer::PassToken CreateIfConversionPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::IfConversion>());
}

Optimizer::PassToken CreateReplaceInvalidOpcodePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::ReplaceInvalidOpcodePass>());
}

Optimizer::PassToken CreateSimplificationPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SimplificationPass>());
}

Optimizer::PassToken CreateLoopUnrollPass(bool fully_unroll, int factor) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LoopUnroller>(fully_unroll, factor));
}

Optimizer::PassToken CreateSSARewritePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SSARewritePass>());
}

Optimizer::PassToken CreateCopyPropagateArraysPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::CopyPropagateArrays>());
}

Optimizer::PassToken CreateVectorDCEPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(MakeUnique<opt::VectorDCE>());
}

Optimizer::PassToken CreateReduceLoadSizePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::ReduceLoadSize>());
}

Optimizer::PassToken CreateCombineAccessChainsPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::CombineAccessChains>());
}

Optimizer::PassToken CreateUpgradeMemoryModelPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::UpgradeMemoryModel>());
}

Optimizer::PassToken CreateInstBindlessCheckPass(uint32_t desc_set,
                                                 uint32_t shader_id,
                                                 bool input_length_enable,
                                                 bool input_init_enable,
                                                 uint32_t version) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::InstBindlessCheckPass>(desc_set, shader_id,
                                             input_length_enable,
                                             input_init_enable, version));
}

Optimizer::PassToken CreateInstDebugPrintfPass(uint32_t desc_set,
                                               uint32_t shader_id) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::InstDebugPrintfPass>(desc_set, shader_id));
}

Optimizer::PassToken CreateInstBuffAddrCheckPass(uint32_t desc_set,
                                                 uint32_t shader_id,
                                                 uint32_t version) {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::InstBuffAddrCheckPass>(desc_set, shader_id, version));
}

Optimizer::PassToken CreateConvertRelaxedToHalfPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::ConvertToHalfPass>());
}

Optimizer::PassToken CreateRelaxFloatOpsPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::RelaxFloatOpsPass>());
}

Optimizer::PassToken CreateCodeSinkingPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::CodeSinkingPass>());
}

Optimizer::PassToken CreateGenerateWebGPUInitializersPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::GenerateWebGPUInitializersPass>());
}

Optimizer::PassToken CreateFixStorageClassPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::FixStorageClass>());
}

Optimizer::PassToken CreateLegalizeVectorShufflePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::LegalizeVectorShufflePass>());
}

Optimizer::PassToken CreateDecomposeInitializedVariablesPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::DecomposeInitializedVariablesPass>());
}

Optimizer::PassToken CreateSplitInvalidUnreachablePass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::SplitInvalidUnreachablePass>());
}

Optimizer::PassToken CreateGraphicsRobustAccessPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::GraphicsRobustAccessPass>());
}

Optimizer::PassToken CreateDescriptorScalarReplacementPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::DescriptorScalarReplacement>());
}

Optimizer::PassToken CreateWrapOpKillPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(MakeUnique<opt::WrapOpKill>());
}

Optimizer::PassToken CreateAmdExtToKhrPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::AmdExtensionToKhrPass>());
}

/* UE Change Begin: Implement a fused-multiply-add pass to reduce the possibility of reassociation. */
Optimizer::PassToken CreateFusedMultiplyAddPass() {
  return MakeUnique<Optimizer::PassToken::Impl>(
      MakeUnique<opt::FusedMultiplyAddPass>());
}
/* UE Change End: Implement a fused-multiply-add pass to reduce the possibility of reassociation. */

}  // namespace spvtools
