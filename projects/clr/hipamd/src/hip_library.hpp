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

#pragma once

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include <hip/hip_runtime.h>
#include "hip_platform.hpp"
#include "hip_code_object.hpp"
#include "hip_fatbin.hpp"

namespace hip {
// An abstract Library container
class LibraryKernel {
 public:
  LibraryKernel(const std::string&  name, int deviceId) : name_(name), deviceId_(deviceId) {
    deviceFuncs_.resize(g_devices.size());
  }
  ~LibraryKernel();

  const std::string& name() const { return name_; }
  int deviceId() const { return deviceId_; }
  std::vector<DeviceFunc*> deviceFuncs_;

 private:
  // name of kernel library
  const std::string name_;
  // device id this kernel is built for
  int deviceId_;
};

class LibraryContainer {
 public:
  // Create from pointer
  explicit LibraryContainer(const char* code_object);  // from pointer
  // Create from file
  explicit LibraryContainer(const std::string file_name);  // deep copy from file
  ~LibraryContainer();

  // Load and build the library
  hipError_t BuildIt();

  // Get the total Kernel count in Library
  hipError_t KernelCount(unsigned int* count) const {

    return hip::PlatformState::instance().getFuncCount(count, module_);
  }

  // Get the Kernel from name
  hipError_t Kernel(hipKernel_t* k, std::string name);

  // Register the kernel function, make an entry in global state
  void Register(std::string name, int device, hipKernel_t k);

 private:
  LibraryContainer() = delete;
  LibraryContainer(const LibraryContainer&) = delete;
  LibraryContainer(const LibraryContainer&&) = delete;
  LibraryContainer& operator=(const LibraryContainer&) = delete;
  LibraryContainer& operator=(const LibraryContainer&&) = delete;

  std::mutex lib_mutex_;
  std::atomic_bool built_ = false;
  hipModule_t module_;
  // Store already looked up kernels for certain devices
  std::map<std::pair<std::string /* name */, int /* device */>, hipKernel_t> kernels_;
  
  // Store original data to pass to DynCO
  std::string source_filename_;
  const void* source_image_;
};
}  // namespace hip
