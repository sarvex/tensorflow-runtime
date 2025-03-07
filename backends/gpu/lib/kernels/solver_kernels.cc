// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file implements the tfrt_gpu.solver kernels, at the moment for CUDA
// only. Support for ROCm still needs to be implemented.
#include <cstdint>

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "tfrt/gpu/gpu_types.h"
#include "tfrt/gpu/kernels/kernels_detail.h"
#include "tfrt/gpu/wrapper/blas_wrapper.h"
#include "tfrt/gpu/wrapper/cublas_wrapper.h"
#include "tfrt/gpu/wrapper/cusolver_wrapper.h"
#include "tfrt/gpu/wrapper/solver_wrapper.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/kernel_registry.h"

namespace tfrt {
namespace gpu {

static llvm::Expected<GpuSolverHandle> SolverCreate(
    Argument<GpuStream> stream) {
  auto current = wrapper::CtxSetCurrent(stream->context()->get());
  if (!current) return current.takeError();
  auto handle = wrapper::SolverCreate(current->platform());
  if (!handle) return handle.takeError();
  if (auto error = wrapper::SolverSetStream(handle->get(), stream->get()))
    return std::move(error);
  return GpuSolverHandle(stream.ValueRef(), std::move(*handle));
}

static llvm::Expected<int64_t> SolverPotrfBufferSize(
    const GpuSolverHandle& handle, int32_t n, int32_t stride,
    Attribute<int32_t> dataType, Attribute<int32_t> fillMode) {
  auto platform = handle->platform();
  if (platform != wrapper::Platform::CUDA)
    return MakeStringError("Unsupported platform ", platform);

  auto current = wrapper::CtxSetCurrent(handle.context()->get());
  if (!current) return current.takeError();

  cudaDataType data_type = wrapper::BlasDataType::FromOpaqueValue(*dataType);
  auto fill_mode = wrapper::BlasFillMode::FromOpaqueValue(*fillMode);

  auto call = [&](auto dummy) {
    using Pointer = wrapper::Pointer<decltype(dummy)>;
    return wrapper::CusolverDnPotrfBufferSize(
        current.get(), handle.get(), fill_mode, n, Pointer(nullptr, platform),
        stride);
  };

  switch (data_type) {
    case CUDA_R_32F:
      return call(float{});
    case CUDA_R_64F:
      return call(double{});
    case CUDA_C_32F:
      return call(cuComplex{});
    case CUDA_C_64F:
      return call(cuDoubleComplex{});
    default:
      return MakeStringError("Unsupported data type ", data_type);
  }
}

static Error SolverPotrf(const GpuSolverHandle& handle, int32_t n,
                         const GpuBuffer& buffer, int32_t stride,
                         const GpuBuffer& workspace, const GpuBuffer& devInfo,
                         Attribute<int32_t> dataType,
                         Attribute<int32_t> fillMode) {
  // These functions eventually need to make two separate calls to
  // CusolverDn<t>potrf and corresponding ROCm function, as wrappers
  // SolverPotrf for CUDA/ROCm is not feasible due to mismatch in APIs
  // (Cusolver requires use of CusolverDn<t>potrf_bufferSize). Right now only
  // CusolverDnPotrf calls are supported.
  auto platform = handle->platform();
  if (platform != wrapper::Platform::CUDA)
    return MakeStringError("Unsupported platform ", platform);

  auto current = wrapper::CtxSetCurrent(handle.context()->get());
  if (!current) return current.takeError();

  cudaDataType data_type = wrapper::BlasDataType::FromOpaqueValue(*dataType);
  auto fill_mode = wrapper::BlasFillMode::FromOpaqueValue(*fillMode);

  auto call = [&](auto dummy) {
    using Pointer = wrapper::Pointer<decltype(dummy)>;
    return wrapper::CusolverDnPotrf(
        current.get(), handle.get(), fill_mode, n,
        static_cast<Pointer>(buffer.pointer()), stride,
        static_cast<Pointer>(workspace.pointer()), workspace.size(),
        static_cast<wrapper::Pointer<int>>(devInfo.pointer()));
  };

  switch (data_type) {
    case CUDA_R_32F:
      return call(float{});
    case CUDA_R_64F:
      return call(double{});
    case CUDA_C_32F:
      return call(cuComplex{});
    case CUDA_C_64F:
      return call(cuDoubleComplex{});
    default:
      return MakeStringError("Unsupported data type ", data_type);
  }
}

static Error SolverPotrfBatch(const GpuSolverHandle& handle, int32_t n,
                              const GpuBuffer& buffer, int32_t stride,
                              const GpuBuffer& devInfo, int32_t batch_size,
                              Attribute<int32_t> dataType,
                              Attribute<int32_t> fillMode) {
  // These functions eventually need to make two separate calls to
  // CusolverDn<t>potrfBatched and corresponding ROCm function, as wrappers
  // SolverPotrf for CUDA/ROCm is not feasible due to mismatch in APIs.
  auto platform = handle->platform();
  if (platform != wrapper::Platform::CUDA)
    return MakeStringError("Unsupported platform ", platform);

  cudaDataType data_type = wrapper::BlasDataType::FromOpaqueValue(*dataType);
  auto fill_mode = wrapper::BlasFillMode::FromOpaqueValue(*fillMode);

  auto call = [&](auto dummy) {
    auto current = wrapper::CtxSetCurrent(handle.context()->get());
    if (!current) return current.takeError();

    using T = decltype(dummy);
    auto pointer_array =
        handle.context()->AllocateHostPoolMemory<T*>(*current, batch_size);
    if (!pointer_array) return pointer_array.takeError();
    T** buffer_array = pointer_array->get().raw(platform);

    T* buffer_ptr = static_cast<T*>(buffer.pointer().raw(platform));
    ptrdiff_t batch_stride = n * n;
    for (int32_t i = 0; i < batch_size; ++i) {
      buffer_array[i] = buffer_ptr + i * batch_stride;
    }

    wrapper::Pointer<T*> buffer_array_ptr(buffer_array, platform);
    auto devInfo_ptr = static_cast<wrapper::Pointer<int>>(devInfo.pointer());
    return wrapper::CusolverDnPotrfBatched(*current, handle.get(), fill_mode, n,
                                           buffer_array_ptr, stride,
                                           devInfo_ptr, batch_size);
  };

  switch (data_type) {
    case CUDA_R_32F:
      return call(float{});
    case CUDA_R_64F:
      return call(double{});
    case CUDA_C_32F:
      return call(cuComplex{});
    case CUDA_C_64F:
      return call(cuDoubleComplex{});
    default:
      return MakeStringError("Unsupported data type ", data_type);
  }
}

void RegisterGpuSolverKernels(KernelRegistry* kernel_reg) {
  kernel_reg->AddKernel("tfrt_gpu.solver.create", TFRT_KERNEL(SolverCreate));
  kernel_reg->AddKernel("tfrt_gpu.solver.potrf.buffer_size",
                        TFRT_KERNEL(SolverPotrfBufferSize));
  kernel_reg->AddKernel("tfrt_gpu.solver.potrf",
                        TFRT_KERNEL_WITH_CHAIN_RESULT(SolverPotrf));
  kernel_reg->AddKernel("tfrt_gpu.solver.potrf.batch",
                        TFRT_KERNEL_WITH_CHAIN_RESULT(SolverPotrfBatch));
}

}  // namespace gpu
}  // namespace tfrt
