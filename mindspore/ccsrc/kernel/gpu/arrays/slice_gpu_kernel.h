/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_CCSRC_KERNEL_GPU_SLICE_GPU_KERNEL_H
#define MINDSPORE_CCSRC_KERNEL_GPU_SLICE_GPU_KERNEL_H

#include <vector>
#include "kernel/gpu/gpu_kernel.h"
#include "kernel/gpu/gpu_kernel_factory.h"
#include "kernel/gpu/cuda_impl/slice_impl.cuh"

namespace mindspore {
namespace kernel {
template <typename T>
class SliceGpuFwdKernel : public GpuKernel {
 public:
  SliceGpuFwdKernel() : is_strided_slice_(false), input_size_(0), output_size_(0), workspace_size_(0) {}
  ~SliceGpuFwdKernel() override = default;
  const std::vector<size_t> &GetInputSizeList() const override { return input_size_list_; }
  const std::vector<size_t> &GetOutputSizeList() const override { return output_size_list_; }
  const std::vector<size_t> &GetWorkspaceSizeList() const override { return workspace_size_list_; }

  bool Launch(const std::vector<AddressPtr> &inputs, const std::vector<AddressPtr> &,
              const std::vector<AddressPtr> &outputs, uintptr_t stream_ptr) override {
    T *input = GetDeviceAddress<T>(inputs, 0);
    T *output = GetDeviceAddress<T>(outputs, 0);
    if (is_strided_slice_) {
      CalStridedSlice(output_size_ / sizeof(T), input, input_shape_, begin_, size_, strides_, output,
                      reinterpret_cast<cudaStream_t>(stream_ptr));
    } else {
      CalSlice(output_size_ / sizeof(T), input, input_shape_, begin_, size_, output,
               reinterpret_cast<cudaStream_t>(stream_ptr));
    }
    return true;
  }
  bool Init(const CNodePtr &kernel_node) override {
    if (!CheckParam(kernel_node)) {
      return false;
    }
    auto input_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 0);
    int shape_n = input_shape.size() < 4 ? 1 : SizeToInt(input_shape[input_shape.size() - 4]);
    int shape_c = input_shape.size() < 3 ? 1 : SizeToInt(input_shape[input_shape.size() - 3]);
    int shape_h = input_shape.size() < 2 ? 1 : SizeToInt(input_shape[input_shape.size() - 2]);
    int shape_w = SizeToInt(input_shape[input_shape.size() - 1]);
    input_shape_.push_back(shape_n);
    input_shape_.push_back(shape_c);
    input_shape_.push_back(shape_h);
    input_shape_.push_back(shape_w);

    auto strides = AnfAlgo::GetCNodePrimitive(kernel_node)->GetAttr("strides");
    if (strides) {
      strides_ = GetAttr<std::vector<int>>(kernel_node, "strides");
      for (auto i = strides_.size(); i < 4; i++) {
        (void)strides_.insert(strides_.begin(), 1);
      }
      size_ = GetAttr<std::vector<int>>(kernel_node, "end");
      is_strided_slice_ = true;
    } else {
      size_ = GetAttr<std::vector<int>>(kernel_node, "size");
    }
    for (auto i = begin_.size(); i < 4; i++) {
      (void)begin_.insert(begin_.begin(), 0);
    }
    for (size_t i = size_.size(); i < 4; i++) {
      (void)size_.insert(size_.begin(), 1);
    }
    for (size_t i = 0; i < begin_.size(); i++) {
      if (begin_[i] < 0) {
        begin_[i] = begin_[i] + input_shape_[i];
      }
    }
    for (size_t i = 0; i < size_.size(); i++) {
      if (size_[i] < 0) {
        size_[i] = (size_[i] + input_shape_[i]) > 0 ? (size_[i] + input_shape_[i]) : 0;
      }
    }

    input_size_ = IntToSize(shape_n * shape_c * shape_h * shape_w) * sizeof(T);
    auto out_shape = AnfAlgo::GetOutputInferShape(kernel_node, 0);

    output_size_ = sizeof(T);
    for (size_t x : out_shape) {
      output_size_ = output_size_ * x;
    }

    InitSizeLists();
    return true;
  }

 protected:
  void InitSizeLists() override {
    input_size_list_.push_back(input_size_);
    output_size_list_.push_back(output_size_);
  }

 private:
  bool CheckParam(const CNodePtr &kernel_node) {
    size_t input_num = AnfAlgo::GetInputTensorNum(kernel_node);
    if (input_num != 1) {
      MS_LOG(ERROR) << "Input number is " << input_num << ", but SliceGpuFwdKernel needs 1 inputs.";
      return false;
    }
    size_t output_num = AnfAlgo::GetOutputTensorNum(kernel_node);
    if (output_num != 1) {
      MS_LOG(ERROR) << "Output number is " << output_num << ", but SliceGpuFwdKernel needs 1 output.";
      return false;
    }
    auto input_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 0);
    if (input_shape.size() > 4) {
      MS_LOG(ERROR) << "Input dims is " << input_shape.size() << ", but SliceGpuFwdKernel olny support 4d or lower.";
      return false;
    }
    if (input_shape.size() == 0) {
      MS_LOG(ERROR) << "Input dims is " << input_shape.size() << ", scalar is not supported.";
      return false;
    }
    begin_ = GetAttr<std::vector<int>>(kernel_node, "begin");
    for (size_t i = 0; i < input_shape.size(); i++) {
      if ((begin_[i] > 0 && (begin_[i] > SizeToInt(input_shape[i]))) ||
          (begin_[i] < 0 && (std::abs(begin_[i]) > SizeToInt(input_shape[i])))) {
        MS_LOG(INFO) << "Input out of bounds " << input_shape[i] << " in axis " << i << ".";
        begin_[i] = 0;
      }
    }
    return true;
  }
  std::vector<int> begin_;
  std::vector<int> size_;
  std::vector<int> strides_;
  std::vector<int> input_shape_;

  std::vector<size_t> input_size_list_;
  std::vector<size_t> output_size_list_;
  std::vector<size_t> workspace_size_list_;

  bool is_strided_slice_;
  size_t input_size_;
  size_t output_size_;
  size_t workspace_size_;
};
}  // namespace kernel
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_KERNEL_GPU_SLICE_GPU_KERNEL_H
