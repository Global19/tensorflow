/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/delegates/gpu/cl/kernels/max_unpooling.h"

#include <string>

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"

namespace tflite {
namespace gpu {
namespace cl {

MaxUnpooling::MaxUnpooling(const OperationDef& definition,
                           const MaxUnpooling2DAttributes& attr,
                           const DeviceInfo& device_info)
    : GPUOperation(definition),
      stride_(attr.strides.w, attr.strides.h, 0, 0),
      padding_(attr.padding.appended.w, attr.padding.appended.h, 0, 0),
      kernel_size_(attr.kernel.w, attr.kernel.h, 0, 0) {
  code_ = GetMaxUnpoolingKernelCode(definition_, device_info);
}

MaxUnpooling::MaxUnpooling(const OperationDef& definition,
                           const MaxUnpooling3DAttributes& attr,
                           const DeviceInfo& device_info)
    : GPUOperation(definition),
      stride_(attr.strides.w, attr.strides.h, attr.strides.d, 0),
      padding_(attr.padding.appended.w, attr.padding.appended.h,
               attr.padding.appended.d, 0),
      kernel_size_(attr.kernel.w, attr.kernel.h, attr.kernel.d, 0) {
  code_ = GetMaxUnpoolingKernelCode(definition_, device_info);
}

MaxUnpooling::MaxUnpooling(MaxUnpooling&& kernel)
    : GPUOperation(std::move(kernel)),
      stride_(kernel.stride_),
      padding_(kernel.padding_),
      kernel_size_(kernel.kernel_size_) {}

MaxUnpooling& MaxUnpooling::operator=(MaxUnpooling&& kernel) {
  if (this != &kernel) {
    std::swap(stride_, kernel.stride_);
    std::swap(padding_, kernel.padding_);
    std::swap(kernel_size_, kernel.kernel_size_);
    GPUOperation::operator=(std::move(kernel));
  }
  return *this;
}

std::string MaxUnpooling::GetMaxUnpoolingKernelCode(
    const OperationDef& op_def, const DeviceInfo& device_info) {
  auto src_desc = op_def.src_tensors[0];
  src_desc.SetTextureAddressMode(GetFastestZeroMode(device_info));
  if (op_def.IsBatchSupported()) {
    src_desc.SetStateVar("BatchedWidth", "true");
  }
  AddSrcTensor("src_tensor", src_desc);
  auto src_ind_desc = op_def.src_tensors[1];
  src_ind_desc.SetTextureAddressMode(GetFastestZeroMode(device_info));
  if (op_def.IsBatchSupported()) {
    src_ind_desc.SetStateVar("BatchedWidth", "true");
  }
  AddSrcTensor("src_indices", src_ind_desc);
  auto dst_desc = op_def.dst_tensors[0];
  if (op_def.IsBatchSupported()) {
    dst_desc.SetStateVar("BatchedWidth", "true");
  }
  AddDstTensor("dst_tensor", dst_desc);
  if (op_def.dst_tensors[0].HasAxis(Axis::WIDTH)) {
    args_.AddInt("kernel_size_x");
    args_.AddInt("padding_x");
    args_.AddInt("stride_x");
  }
  if (op_def.dst_tensors[0].HasAxis(Axis::HEIGHT)) {
    args_.AddInt("kernel_size_y");
    args_.AddInt("padding_y");
    args_.AddInt("stride_y");
  }
  if (op_def.dst_tensors[0].HasAxis(Axis::DEPTH)) {
    args_.AddInt("kernel_size_z");
    args_.AddInt("padding_z");
    args_.AddInt("stride_z");
  }

  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  c += "  int X = get_global_id(0);\n";
  if (op_def.dst_tensors[0].HasAxis(Axis::DEPTH)) {
    c += "  int linear_id_1 = get_global_id(1);\n";
    c += "  int Y = linear_id_1 / args.dst_tensor.Depth();\n";
    c += "  int Z = linear_id_1 % args.dst_tensor.Depth();\n";
    c += "  int src_z = (Z + args.padding_z) / args.stride_z;\n";
  } else {
    c += "  int Y = get_global_id(1);\n";
  }
  c += "  int S = get_global_id(2);\n";
  c += "  if (X >= args.dst_tensor.Width() || Y >= args.dst_tensor.Height() || "
       "S >= args.dst_tensor.Slices()) { \n";
  c += "    return; \n";
  c += "  } \n";
  if (op_def.dst_tensors[0].HasAxis(Axis::BATCH)) {
    c += "  int linear_id_0 = get_global_id(0);\n";
    c += "  int X0 = linear_id_0 / args.dst_tensor.Batch();\n";
    c += "  int B = linear_id_0 % args.dst_tensor.Batch();\n";
    c += "  int src_x0 = (X0 + args.padding_x) / args.stride_x;\n";
    c += "  int src_x = src_x0 * args.dst_tensor.Batch() + B;\n";
  } else {
    c += "  int src_x = (X + args.padding_x) / args.stride_x;\n";
  }
  c += "  int src_y = (Y + args.padding_y) / args.stride_y;\n";
  std::string src_args = op_def.dst_tensors[0].HasAxis(Axis::DEPTH)
                             ? "src_x, src_y, src_z, S"
                             : "src_x, src_y, S";
  if (op_def.src_tensors[0].storage_type == TensorStorageType::BUFFER) {
    if (op_def.dst_tensors[0].HasAxis(Axis::DEPTH)) {
      c += "  bool outside = src_x < 0 || src_y < 0 || src_z < 0 || src_x >= "
           "args.src_tensor.Width() || src_y >= args.src_tensor.Height() || "
           "src_z >= args.src_tensor.Depth();\n";
    } else {
      c += "  bool outside = src_x < 0 || src_y < 0 || src_x >= "
           "args.src_tensor.Width() || src_y >= args.src_tensor.Height();\n";
    }
    c += "  FLT4 src = (FLT4)(0.0f);\n";
    c += "  int4 ind = (int4)(0);\n";
    c += "  if (!outside) {\n";
    c += "    src = args.src_tensor.Read(" + src_args + ");\n";
    c += "    ind = convert_int4(args.src_indices.Read(" + src_args + "));\n";
    c += "  }\n";
  } else {
    c += "  FLT4 src = args.src_tensor.Read(" + src_args + ");\n";
    c +=
        "  int4 ind = convert_int4(args.src_indices.Read(" + src_args + "));\n";
  }
  if (op_def.dst_tensors[0].HasAxis(Axis::BATCH)) {
    c += "  int t_x = X0 - (src_x0 * args.stride_x - args.padding_x);\n";
  } else {
    c += "  int t_x = X - (src_x * args.stride_x - args.padding_x);\n";
  }
  c += "  int t_y = Y - (src_y * args.stride_y - args.padding_y);\n";
  if (op_def.dst_tensors[0].HasAxis(Axis::DEPTH)) {
    c += "  int t_z = Z - (src_z * args.stride_z - args.padding_z);\n";
    c += "  int t_index = (t_y * args.kernel_size_x + t_x) * "
         "args.kernel_size_z + t_z;\n";
  } else {
    c += "  int t_index = t_y * args.kernel_size_x + t_x;\n";
  }
  c += "  FLT4 result;\n";
  const std::string channels[] = {".x", ".y", ".z", ".w"};
  for (int i = 0; i < 4; ++i) {
    const auto& s = channels[i];
    c += "  result" + s + "= t_index == ind" + s + "? src" + s + ": 0.0f;\n";
  }
  if (op_def.dst_tensors[0].HasAxis(Axis::DEPTH)) {
    c += "  args.dst_tensor.Write(result, X, Y, Z, S);\n";
  } else {
    c += "  args.dst_tensor.Write(result, X, Y, S);\n";
  }
  c += "}\n";

  return c;
}

absl::Status MaxUnpooling::BindArguments() {
  if (definition_.dst_tensors[0].HasAxis(Axis::WIDTH)) {
    RETURN_IF_ERROR(args_.SetInt("stride_x", stride_.x));
    RETURN_IF_ERROR(args_.SetInt("padding_x", padding_.x * src_[0]->Batch()));
    RETURN_IF_ERROR(args_.SetInt("kernel_size_x", kernel_size_.x));
  }
  if (definition_.dst_tensors[0].HasAxis(Axis::HEIGHT)) {
    RETURN_IF_ERROR(args_.SetInt("stride_y", stride_.y));
    RETURN_IF_ERROR(args_.SetInt("padding_y", padding_.y));
    RETURN_IF_ERROR(args_.SetInt("kernel_size_y", kernel_size_.y));
  }
  if (definition_.dst_tensors[0].HasAxis(Axis::DEPTH)) {
    RETURN_IF_ERROR(args_.SetInt("stride_z", stride_.z));
    RETURN_IF_ERROR(args_.SetInt("padding_z", padding_.z));
    RETURN_IF_ERROR(args_.SetInt("kernel_size_z", kernel_size_.z));
  }
  return absl::OkStatus();
}

int3 MaxUnpooling::GetGridSize() const {
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = dst_[0]->Height() * dst_[0]->Depth();
  const int grid_z = dst_[0]->Slices();
  return int3(grid_x, grid_y, grid_z);
}

MaxUnpooling CreateMaxUnpooling(const OperationDef& definition,
                                const MaxUnpooling2DAttributes& attr,
                                const DeviceInfo& device_info) {
  return MaxUnpooling(definition, attr, device_info);
}

MaxUnpooling CreateMaxUnpooling(const OperationDef& definition,
                                const MaxUnpooling3DAttributes& attr,
                                const DeviceInfo& device_info) {
  return MaxUnpooling(definition, attr, device_info);
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
