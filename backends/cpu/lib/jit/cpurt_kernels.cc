/*
 * Copyright 2021 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- cpurt_kernels.cc - CPURT kernels -----------------------------------===//
//
// This file defines the C++ kernels for the CPURT dialect.

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "tfrt/cpu/jit/cpurt.h"
#include "tfrt/dtype/dtype.h"
#include "tfrt/host_context/async_dispatch.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/attribute_utils.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/execution_context.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/host_context/kernel_registry.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/support/rc_array.h"
#include "tfrt/support/ref_count.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/tensor.h"
#include "tfrt/tensor/tensor_shape.h"

namespace tfrt {
namespace cpu {
namespace jit {

// -------------------------------------------------------------------------- //
// Compile compilation unit attribute to an executable result.
// -------------------------------------------------------------------------- //

static AsyncValueRef<JitExecutable> Compile(CompilationUnitAttribute kernel,
                                            const ExecutionContext& exec_ctx) {
  HostContext* host = exec_ctx.host();

  // We only support functions nested in top level compiled module.
  if (kernel.nested_symbols().size() != 1)
    return EmitErrorAsync(
        exec_ctx, "compiled kernel must be referenced by one nested symbol");

  ResourceContext* res_ctx = exec_ctx.resource_context();
  auto* jit_executable_cache =
      res_ctx->GetOrCreateResource<JitExecutableCache>("cpurt.cache");

  // TODO(ezhulenev): Compute cache key based on the content of MLIR module, or
  // better keep module fingerprint in the BEF file.
  intptr_t key = exec_ctx.location().data;

  // Maybe return JitExecutable from the cache.
  if (auto cached = jit_executable_cache->Find(key)) return cached.CopyRef();

  // Allocate a placeholder for the compiled JitExecutable.
  JitExecutableCache::Entry entry = jit_executable_cache->Allocate(key);

  // We lost the race; some other invocation will do the compilation.
  if (!entry.allocated) return entry.ptr.CopyRef();

  // Compile kernel asynchronously in the host context thread pool.
  EnqueueWork(exec_ctx, [kernel, host, ref = entry.ptr.CopyRef()]() {
    CompilationOptions opts;
    opts.num_worker_threads = host->GetNumWorkerThreads();

    string_view entrypoint = kernel.nested_symbols()[0];
    string_view module = kernel.serialized_operation();

    // Instantiate new JitExecutable from the MLIR source.
    Expected<JitExecutable> jit_executable =
        JitExecutable::Instantiate(module, entrypoint, opts);

    // Set the allocated async value state to error or concrete.
    if (auto err = jit_executable.takeError())
      ref.SetError(std::move(err));
    else
      ref.emplace(std::move(*jit_executable));
  });

  return entry.ptr.CopyRef();
}

// -------------------------------------------------------------------------- //
// Execute compiled CPURT kernels.
// -------------------------------------------------------------------------- //

namespace {
// We do not record any operands information for results conversion.
struct ConversionCtx {};
}  // namespace

static Error ConvertTensorOperandsToMemrefDesc(
    RepeatedArguments<Tensor> operands, SmallVectorImpl<MemrefDesc>* memrefs) {
  assert(memrefs->empty() && "memrefs must be empty");
  memrefs->reserve(operands.size());

  for (unsigned i = 0; i < operands.size(); ++i) {
    Expected<MemrefDesc> memref = ConvertTensorToMemrefDesc(operands[i]);
    if (auto err = memref.takeError()) return err;
    memrefs->push_back(std::move(*memref));
  }

  return Error::success();
}

static void ExecuteImpl(const Executable& executable,
                        const SmallVectorImpl<MemrefDesc>& memrefs,
                        RepeatedArguments<Tensor> operands,
                        RemainingResults results,
                        const ExecutionContext& exec_ctx) {
  // If execution failed errors will be automatically allocated for all results.
  ReturnValueConverter<ConversionCtx> converter(results);
  converter.AddConversion(ReturnAsyncToken<ConversionCtx>);
  converter.AddConversion(ReturnAsyncMemrefAsDenseHostTensor<ConversionCtx>);
  converter.AddConversion(ReturnMemrefAsDenseHostTensor<ConversionCtx>);

  if (auto err = executable.Execute(memrefs, converter, exec_ctx)) return;

  // Keep operands alive if we have unavailable results.
  RunWhenReady(results.values(),
               [operands = RCArray<AsyncValue>(operands.values())] {});
}

static void Execute(Argument<JitExecutable> jit_executable,
                    Argument<Chain> in_chain,
                    RepeatedArguments<Tensor> operands,
                    RemainingResults results,
                    const ExecutionContext& exec_ctx) {
  // Extract Memrefs from Tensor operands.
  SmallVector<MemrefDesc, 4> memrefs;
  if (auto err = ConvertTensorOperandsToMemrefDesc(operands, &memrefs))
    return EmitErrors(results, std::move(err), exec_ctx);

  // Get an executable that might be specialized to the operands.
  Expected<AsyncValuePtr<Executable>> executable =
      jit_executable->GetExecutable(memrefs, exec_ctx);
  if (auto err = executable.takeError())
    return EmitErrors(results, std::move(err), exec_ctx);

  // If specialization is available execute it inline.
  if (executable->IsAvailable()) {
    if (executable->IsError()) {
      EmitErrors(results, executable->GetError(), exec_ctx);
    } else {
      ExecuteImpl(executable->get(), memrefs, operands, results, exec_ctx);
    }
    return;
  }

  // Otherwise execute it when the executable will become available. This
  // requires careful lifetime extension of all async values passed as operands
  // to the kernel (and also results that will become available asynchronously).

  // Allocate indirect async values for all results, we'll forward them to the
  // actual async values computed by the executable later.
  for (unsigned i = 0; i < results.size(); ++i)
    results.AllocateIndirectResultAt(i);

  // Call executable when it's ready with the original operands.
  executable->AndThen([exec_ctx, executable = *executable,
                       memrefs = std::move(memrefs),
                       r = RCArray<AsyncValue>(results.values()),
                       o = RCArray<AsyncValue>(operands.values())] {
    // Allocate storage for the executable results.
    llvm::SmallVector<RCReference<AsyncValue>> results_storage;
    results_storage.resize(r.size());

    // Reconstruct arguments and results from captured async values.
    RepeatedArguments<Tensor> operands(o.values());
    RemainingResults results(results_storage);

    if (executable.IsError()) {
      EmitErrors(results, executable.GetError(), exec_ctx);
    } else {
      ExecuteImpl(*executable, memrefs, operands, results, exec_ctx);
    }

    // Forward previously allocated indirect results to the actual results.
    for (unsigned i = 0; i < r.size(); ++i)
      llvm::cast<IndirectAsyncValue>(*r[i]).ForwardTo(
          std::move(results_storage[i]));
  });
}

void RegisterCpuRuntimeKernels(KernelRegistry* registry) {
  registry->AddKernel("cpurt.compile", TFRT_KERNEL(Compile));
  registry->AddKernel("cpurt.execute", TFRT_KERNEL(Execute));
}

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt
