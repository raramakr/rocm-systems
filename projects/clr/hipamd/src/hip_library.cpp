/*
Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANNTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER INN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR INN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "hip/hip_runtime.h"
#include "hip_code_object.hpp"
#include "hip_library.hpp"
#include "hip_platform.hpp"
#include "utils/debug.hpp"

namespace hip {
void LibraryContainer::Register(std::string name, int device, hipKernel_t k) {
  std::scoped_lock<std::mutex> lock(lib_mutex_);
  auto key = std::make_pair(name, device);
  if (kernels_.find(key) == kernels_.end()) {
    kernels_.insert(std::make_pair(std::make_pair(name, device), k));
    if (!hip::PlatformState::instance().RegisterLibraryFunction(k)) {
      LogPrintfInfo("Already registered: %p", k);
    }
  }
}

hipError_t LibraryContainer::Kernel(hipKernel_t* k, std::string name) {
  auto device_id = hip::ihipGetDevice();
  if (auto ki = kernels_.find(std::make_pair(name, device_id)); ki != kernels_.end()) {
    *k = ki->second;
    return hipSuccess;
  }

  hipFunction_t f;
  auto ret = hip::PlatformState::instance().getDynFunc(&f, module_, name.c_str());
  if (ret != hipSuccess) {
    return hipErrorNotFound;
  }       
  LibraryKernel* kernel = new LibraryKernel(name, device_id);
  kernel->deviceFuncs_[device_id] = hip::DeviceFunc::asFunction(reinterpret_cast<hipFunction_t>(f));
  // Register it, basically make it available for query though the hip context.
  *k = reinterpret_cast<hipKernel_t>(kernel);
  Register(name, device_id, reinterpret_cast<hipKernel_t>(kernel));
  return hipSuccess;  
}

LibraryContainer::LibraryContainer(const char* code_object) : source_image_(code_object) {}

LibraryContainer::LibraryContainer(const std::string file_name) :
                                   source_filename_(file_name), source_image_(nullptr) {}

LibraryContainer::~LibraryContainer() {
  for (const auto& k : kernels_) {
    (void)hip::PlatformState::instance().UnregisterLibraryFunction(k.second);
  }
  kernels_.clear();
}

// BuildIt builds and loads the Library, default behavior is lazy load.
// This function needs to be called before any query on library.
hipError_t LibraryContainer::BuildIt() {
  std::scoped_lock<std::mutex> lock(lib_mutex_);
  if (built_) {
    return hipSuccess;
  }

  hipModule_t* module;
  const char* fname = source_filename_.empty() ? nullptr : source_filename_.c_str();
  const void* image = source_image_;

  HIP_RETURN(hip::PlatformState::instance().loadModule(&module_, fname, image));

  built_ = true;
  return hipSuccess;
}

hipError_t hipLibraryLoadData(hipLibrary_t* library, const void* image, hipJitOption* jitOptions,
                              void** jitOptionsValues, unsigned int numJitOptions,
                              hipLibraryOption* libraryOptions, void** libraryOptionValues,
                              unsigned int numLibraryOptions) {
  HIP_INIT_API(hipLibraryLoadData, library, image, jitOptions, jitOptionsValues, numJitOptions,
               libraryOptions, libraryOptionValues, numLibraryOptions);
  if (library == nullptr || image == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  // We do not support JIT options
  if (numJitOptions > 0) {
    HIP_RETURN(hipErrorInvalidValue);
  }

  auto* l = new hip::LibraryContainer((const char*)image);
  *library = reinterpret_cast<hipLibrary_t>(l);
  HIP_RETURN(hipSuccess);
}

hipError_t hipLibraryLoadFromFile(hipLibrary_t* library, const char* fname,
                                  hipJitOption* jitOptions, void** jitOptionsValues,
                                  unsigned int numJitOptions, hipLibraryOption* libraryOptions,
                                  void** libraryOptionValues, unsigned int numLibraryOptions) {
  HIP_INIT_API(hipLibraryLoadFromFile, library, fname, jitOptions, jitOptionsValues, numJitOptions,
               libraryOptions, libraryOptionValues, numLibraryOptions);
  if (library == nullptr || !std::filesystem::exists(fname) || numJitOptions > 0) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  auto* l = new hip::LibraryContainer(std::string(fname));
  *library = reinterpret_cast<hipLibrary_t>(l);
  HIP_RETURN(hipSuccess);
}

hipError_t hipLibraryUnload(hipLibrary_t library) {
  HIP_INIT_API(hipLibraryUnload, library);
  if (library == nullptr) {
    HIP_RETURN(hipErrorInvalidResourceHandle);
  }
  auto l = reinterpret_cast<hip::LibraryContainer*>(library);
  delete l;
  HIP_RETURN(hipSuccess);
}

hipError_t hipLibraryGetKernelCount(unsigned int* count, hipLibrary_t library) {
  HIP_INIT_API(hipLibraryGetKernelCount, count, library);
  if (library == nullptr || count == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  auto l = reinterpret_cast<hip::LibraryContainer*>(library);
  auto ret = l->BuildIt();
  if (ret != hipSuccess) {
    HIP_RETURN(ret);
  }
  
  HIP_RETURN(l->KernelCount(count));
}

hipError_t hipLibraryGetKernel(hipKernel_t* kernel, hipLibrary_t library, const char* kname) {
  HIP_INIT_API(hipLibraryGetKernel, kernel, library, kname);
  if (library == nullptr || kname == nullptr || kernel == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  auto l = reinterpret_cast<hip::LibraryContainer*>(library);
  auto ret = l->BuildIt();
  if (ret != hipSuccess) {
    HIP_RETURN(ret);
  }
  ret = l->Kernel(kernel, kname);
  HIP_RETURN(ret);
}

hipError_t hipKernelSetAttribute(hipFunction_attribute attrib, int value, hipKernel_t kernel, hipDevice_t dev) {
  HIP_INIT_API(hipKernelSetAttribute, attrib, value, kernel, dev);

  if (kernel == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  if (!hip::PlatformState::instance().IsLibraryFunctionRegistered(kernel)) {
    HIP_RETURN(hipErrorNotFound);
  }
  hip::LibraryKernel* libraryKernel = reinterpret_cast<hip::LibraryKernel*>(kernel);

  int deviceId;
  hipError_t error = hipGetDevice(&deviceId);

  if(deviceId != dev) {
    HIP_RETURN(hipErrorInvalidDevice);
  }
  amd::Kernel* kernelFunc = libraryKernel->deviceFuncs_[deviceId]->kernel();
  if (kernelFunc == nullptr) {
    HIP_RETURN(hipErrorInvalidDeviceFunction);
  }

  device::Kernel* d_kernel =
      const_cast<device::Kernel*>(kernelFunc->getDeviceKernel(*(hip::getCurrentDevice()->devices()[0])));

  device::Kernel::WorkGroupInfo* wrkGrpInfo = d_kernel->workGroupInfo();

  if (wrkGrpInfo == nullptr) {
    HIP_RETURN(hipErrorMissingConfiguration);
  }

  switch (attrib) {
    case HIP_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES:
      if (value > (hip::getCurrentDevice()->devices()[0]->info().localMemSize_)) {
        HIP_RETURN(hipErrorInvalidValue);
      }
      wrkGrpInfo->localMemSize_ = static_cast<size_t>(value);
      break;
    /* Read Only*/
    case HIP_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK:
    case HIP_FUNC_ATTRIBUTE_CONST_SIZE_BYTES:
    case HIP_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES:
    case HIP_FUNC_ATTRIBUTE_NUM_REGS:
    case HIP_FUNC_ATTRIBUTE_CACHE_MODE_CA:
    case HIP_FUNC_ATTRIBUTE_PTX_VERSION:
    case HIP_FUNC_ATTRIBUTE_BINARY_VERSION:
      HIP_RETURN(hipErrorInvalidValue);
      break;
    case HIP_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES:
      if ((value < 0) || (value > (wrkGrpInfo->availableLDSSize_ - wrkGrpInfo->localMemSize_))) {
        HIP_RETURN(hipErrorInvalidValue);
      }
      if(!d_kernel->isAttrSet(HIP_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES)) {
        wrkGrpInfo->maxDynamicSharedSizeBytes_ = value;
      }
      d_kernel->workGroupInfoKernelAttribute()->maxDynamicSharedSizeBytes_ = value;
      break;
    case HIP_FUNC_ATTRIBUTE_PREFERRED_SHARED_MEMORY_CARVEOUT:
      break;
    default:
      HIP_RETURN(hipErrorInvalidValue);
  }

  HIP_RETURN(hipSuccess);
}

hipError_t hipKernelGetFunction(hipFunction_t* pFunc, hipKernel_t kernel) {
  HIP_INIT_API(hipKernelGetFunction, pFunc, kernel);

  if (pFunc == nullptr || kernel == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  if (!hip::PlatformState::instance().IsLibraryFunctionRegistered(kernel)) {
    HIP_RETURN(hipErrorNotFound);
  }
  hip::LibraryKernel* libraryKernel = reinterpret_cast<hip::LibraryKernel*>(kernel);
  *pFunc = libraryKernel->deviceFuncs_[hip::getCurrentDevice()->deviceId()]->asHipFunction();
  if (*pFunc == nullptr) {
    HIP_RETURN(hipErrorInvalidDeviceFunction);
  }
  HIP_RETURN(hipSuccess);
}
}  // namespace hip
