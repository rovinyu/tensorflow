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

#include "tensorflow/lite/delegates/gpu/cl/inference_context.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/cl_device.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/model_hints.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"
#include "tensorflow/lite/delegates/gpu/cl/selectors/operation_selector.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/memory_management.h"
#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/model_transformer.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/transformations/add_bias.h"
#include "tensorflow/lite/delegates/gpu/common/transformations/merge_padding_with.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

namespace {
bool IsReady(const std::unordered_set<ValueId>& ready_tensors,
             const CLNode& node) {
  for (const ValueId in_id : node.inputs) {
    if (ready_tensors.find(in_id) == ready_tensors.end()) {
      return false;
    }
  }
  return true;
}

std::vector<std::pair<ValueId, TensorDescriptor>> GetCLNodeTensors(
    const CLNode& node) {
  std::vector<std::pair<ValueId, TensorDescriptor>> result;
  const OperationDef main_def = node.operations[0]->GetDefinition();
  const auto& first_range = node.ranges[0];
  for (int k = first_range.x; k < first_range.y; ++k) {
    result.push_back({node.inputs[k], main_def.src_tensors[k - first_range.x]});
  }
  for (int j = 1; j < node.ranges.size(); ++j) {
    const auto& range = node.ranges[j];
    const OperationDef op_def = node.operations[j]->GetDefinition();
    for (int k = range.x; k < range.y; ++k) {
      result.push_back({node.inputs[k], op_def.src_tensors[k - range.x + 1]});
    }
  }
  for (int j = 0; j < node.outputs.size(); ++j) {
    result.push_back({node.outputs[j], main_def.dst_tensors[j]});
  }

  return result;
}

void MergeCLNodes(CLNode* src, CLNode* dst) {
  int offset = dst->inputs.size();
  for (int j = 1; j < src->inputs.size(); ++j) {
    dst->inputs.push_back(src->inputs[j]);
  }
  auto first_range = src->ranges[0];
  dst->ranges.push_back(
      int2(first_range.x + offset, first_range.y - 1 + offset));
  for (int i = 1; i < src->ranges.size(); ++i) {
    auto range = src->ranges[i];
    dst->ranges.push_back(int2(range.x + offset, range.y + offset));
  }
  dst->outputs[0] = src->outputs[0];
  for (int i = 0; i < src->operations.size(); ++i) {
    dst->operations.push_back(std::move(src->operations[i]));
  }
  dst->name += " linked : " + src->name;
}

void AddUsage(ValueId id, int task_index,
              std::map<ValueId, int2>* usage_records) {
  auto it = usage_records->find(id);
  if (it == usage_records->end()) {
    (*usage_records)[id].x = task_index;
    (*usage_records)[id].y = task_index;
  } else {
    (*usage_records)[id].y = task_index;
  }
}

TensorStorageType SelectBestStorageType(const CLContext& context,
                                        const CLDevice& device,
                                        const BHWC& shape,
                                        const TensorStorageType& desired,
                                        const DataType& data_type) {
  if (CanCreateTensorWithShape(context, device, shape,
                               TensorDescriptor{data_type, desired})) {
    return desired;
  }
  if (desired == TensorStorageType::TEXTURE_2D ||
      desired == TensorStorageType::SINGLE_TEXTURE_2D) {
    if (device.SupportsTextureArray() &&
        CanCreateTensorWithShape(
            context, device, shape,
            TensorDescriptor{data_type, TensorStorageType::TEXTURE_ARRAY})) {
      return TensorStorageType::TEXTURE_ARRAY;
    } else {
      return TensorStorageType::BUFFER;
    }
  } else {
    return TensorStorageType::BUFFER;
  }
}

void GetTensorDescriptors(
    const InferenceContext::CreateInferenceInfo& create_info,
    const CreationContext& creation_context, const GraphFloat32& graph,
    std::unordered_map<ValueId, TensorDescriptor>* tensor_descriptors) {
  const auto inputs = graph.inputs();
  const auto outputs = graph.outputs();
  auto tensors = graph.values();
  auto data_type = DeduceDataTypeFromPrecision(create_info.precision);
  for (auto& t : tensors) {
    TensorStorageType storage_type = create_info.storage_type;
    const auto shape = graph.GetValue(t->id)->tensor.shape;
    if (graph.IsGraphInput(t->id) || graph.IsGraphOutput(t->id)) {
      if (shape.c < 4 &&
          CanCreateTensorWithShape(
              *creation_context.context, *creation_context.device, shape,
              TensorDescriptor{data_type,
                               TensorStorageType::SINGLE_TEXTURE_2D})) {
        storage_type = TensorStorageType::SINGLE_TEXTURE_2D;
      }
    }
    storage_type = SelectBestStorageType(*creation_context.context,
                                         *creation_context.device, shape,
                                         storage_type, data_type);
    (*tensor_descriptors)[t->id] = TensorDescriptor{data_type, storage_type};
  }
}

}  // namespace

CLNode::CLNode(CLNode&& node)
    : operations(std::move(node.operations)),
      inputs(std::move(node.inputs)),
      outputs(std::move(node.outputs)),
      ranges(std::move(node.ranges)),
      name(std::move(node.name)) {}

CLNode& CLNode::operator=(CLNode&& node) {
  if (this != &node) {
    operations = std::move(node.operations);
    inputs = std::move(node.inputs);
    outputs = std::move(node.outputs);
    ranges = std::move(node.ranges);
    name = std::move(node.name);
  }
  return *this;
}

Status InferenceContext::InitFromGraph(const CreateInferenceInfo& create_info,
                                       const GraphFloat32& graph,
                                       Environment* env) {
  CreationContext creation_context;
  creation_context.device = env->GetDevicePtr();
  creation_context.context = &env->context();
  creation_context.queue = env->queue();
  creation_context.cache = env->program_cache();

  std::unordered_map<ValueId, TensorDescriptor> tensor_descriptors;
  GetTensorDescriptors(create_info, creation_context, graph,
                       &tensor_descriptors);
  precision_ = create_info.precision;
  storage_type_ = create_info.storage_type;
  auto vendor = env->device().vendor();
  if (vendor == Vendor::MALI) {
    need_flush_ = true;
    need_manual_release_ = true;
  }
  if (vendor == Vendor::POWERVR) {
    need_flush_ = true;
  }
  CopyInAndOutIds(graph);
  RETURN_IF_ERROR(ConvertOperations(creation_context, graph, create_info.hints,
                                    tensor_descriptors));
  Merge();
  RETURN_IF_ERROR(AllocateMemory(graph, env->device(), creation_context.context,
                                 tensor_descriptors));
  BindMemoryToOperations();
  RETURN_IF_ERROR(Compile(creation_context));

  TuningParameters tuning_parameters;
  tuning_parameters.queue = env->profiling_queue();
  tuning_parameters.info = env->device().GetInfoPtr();
  if (create_info.hints.Check(ModelHints::kFastTuning)) {
    tuning_parameters.tuning_type = TuningType::FAST;
  }
  RETURN_IF_ERROR(Tune(tuning_parameters));
  return OkStatus();
}

Status InferenceContext::InitFromGraphWithTransforms(
    const CreateInferenceInfo& create_info, GraphFloat32* graph,
    Environment* env) {
  RETURN_IF_ERROR(RunGraphTransforms(graph));
  RETURN_IF_ERROR(InitFromGraph(create_info, *graph, env));
  return OkStatus();
}

void InferenceContext::CopyInAndOutIds(const GraphFloat32& graph) {
  const auto inputs = graph.inputs();
  for (const auto& input : inputs) {
    input_ids_.push_back(input->id);
  }

  const auto outputs = graph.outputs();
  for (const auto& output : outputs) {
    output_ids_.push_back(output->id);
  }
}

Status InferenceContext::ConvertOperations(
    const CreationContext& creation_context, const GraphFloat32& graph,
    ModelHints hints,
    const std::unordered_map<ValueId, TensorDescriptor>& tensor_descriptors) {
  std::vector<Node*> graph_nodes = graph.nodes();
  std::map<ValueId, int>
      tensor_usages;  // keeps latest index of operation that updated tensor
  for (const auto& input_id : input_ids_) {
    tensor_usages[input_id] = -1;  // so as inputs "updated" before operation 0,
                                   // we will mark them with -1
  }
  for (int i = 0; i < graph_nodes.size(); ++i) {
    const Node& node = *graph_nodes[i];
    auto inputs = graph.FindInputs(node.id);
    auto outputs = graph.FindOutputs(node.id);

    // Reordering of input ids and updating of temporary tensors_usage struct.
    // This stage is necessary because we are building OperationDef that rely on
    // order of input ids. But we also should have input id on first position
    // that potentially can be "linking" tensor and as result eliminated(unused)
    // We apply it only for ADD operation, because of ADD associativity and
    // ADD can be linked.
    // In current approach "linking" tensor can be only latest written
    // tensor(during linear order of execution) among input tensors.
    const OperationType op_type = OperationTypeFromString(node.operation.type);
    if (inputs.size() > 1 && op_type == OperationType::ADD) {
      int latest_written_tensor_index = 0;
      int last_usage = tensor_usages[inputs[0]->id];
      for (int j = 1; j < inputs.size(); ++j) {
        if (tensor_usages[inputs[j]->id] > last_usage) {
          last_usage = tensor_usages[inputs[j]->id];
          latest_written_tensor_index = j;
        }
      }
      std::swap(inputs[0], inputs[latest_written_tensor_index]);
    }
    for (const auto& out_id : outputs) {
      tensor_usages[out_id->id] = i;
    }

    OperationDef op_def;
    op_def.precision = precision_;
    for (int j = 0; j < inputs.size(); ++j) {
      op_def.src_tensors.push_back(
          tensor_descriptors.find(inputs[j]->id)->second);
    }
    for (int j = 0; j < outputs.size(); ++j) {
      op_def.dst_tensors.push_back(
          tensor_descriptors.find(outputs[j]->id)->second);
    }
    std::unique_ptr<GPUOperation> gpu_op;
    RETURN_IF_ERROR(GPUOperationFromNode(creation_context, op_def, hints,
                                         inputs, outputs, node, &gpu_op));
    CLNode cl_node;
    cl_node.operations.push_back(std::move(gpu_op));
    cl_node.ranges.push_back(int2(0, static_cast<int>(inputs.size())));
    cl_node.inputs.resize(inputs.size());
    for (int j = 0; j < inputs.size(); ++j) {
      cl_node.inputs[j] = inputs[j]->id;
    }
    cl_node.outputs.resize(outputs.size());
    for (int j = 0; j < outputs.size(); ++j) {
      cl_node.outputs[j] = outputs[j]->id;
    }
    cl_node.name = node.operation.type + " " + std::to_string(node.id);
    nodes_.push_back(std::move(cl_node));
  }

  return OkStatus();
}

void InferenceContext::Merge() {
  std::unordered_set<ValueId> ready_tensors;
  for (const auto& input_id : input_ids_) {
    ready_tensors.insert(input_id);
  }
  for (int i = 0; i < nodes_.size(); ++i) {
    auto& node = nodes_[i];
    for (const auto& out_id : node.outputs) {
      ready_tensors.insert(out_id);
    }
    if (node.outputs.size() != 1) {
      continue;
    }
    std::vector<int> next_nodes;
    int link_index = 0;
    for (int j = i + 1; j < nodes_.size(); ++j) {
      for (int k = 0; k < nodes_[j].inputs.size(); ++k) {
        if (nodes_[j].inputs[k] == node.outputs[0]) {
          next_nodes.push_back(j);
          link_index = k;
        }
      }
    }
    if (next_nodes.size() != 1 || link_index != 0) {
      continue;
    }
    auto& linkable_node = nodes_[next_nodes[0]];
    auto* elementwise =
        dynamic_cast<ElementwiseOperation*>(linkable_node.operations[0].get());
    if (!elementwise || linkable_node.outputs.size() != 1 ||
        !IsReady(ready_tensors, linkable_node)) {
      continue;
    }
    MergeCLNodes(&linkable_node, &node);
    nodes_.erase(nodes_.begin() + next_nodes[0]);
    i -= 1;
  }
  for (auto& node : nodes_) {
    for (int j = 1; j < node.operations.size(); ++j) {
      auto* elementwise =
          dynamic_cast<ElementwiseOperation*>(node.operations[j].get());
      node.operations[0]->AddOperation(elementwise);
    }
  }
}

Status InferenceContext::AllocateMemory(
    const GraphFloat32& graph, const CLDevice& device, CLContext* context,
    const std::unordered_map<ValueId, TensorDescriptor>& tensor_descriptors) {
  std::map<ValueId, int2> usages;
  for (int op_index = 0; op_index < nodes_.size(); ++op_index) {
    auto tensors = GetCLNodeTensors(nodes_[op_index]);
    for (auto& tensor : tensors) {
      AddUsage(tensor.first, op_index, &usages);
    }
  }

  struct TensorDesc {
    BHWC shape;
    TensorDescriptor descriptor;

    bool operator==(const TensorDesc& b) const {
      return shape == b.shape &&
             descriptor.data_type == b.descriptor.data_type &&
             descriptor.storage_type == b.descriptor.storage_type;
    }
  };

  std::vector<TensorUsageRecord<TensorDesc>> usage_records;
  std::map<ValueId, ValueId> remap_from_graph_ids;
  for (auto& usage : usages) {
    const auto& shape = graph.GetValue(usage.first)->tensor.shape;
    const auto& descriptor = tensor_descriptors.find(usage.first)->second;
    remap_from_graph_ids[usage.first] = usage_records.size();
    usage_records.push_back({{shape, descriptor},
                             static_cast<TaskId>(usage.second.x),
                             static_cast<TaskId>(usage.second.y)});
  }

  ObjectsAssignment<TensorDesc> assignment;
  RETURN_IF_ERROR(AssignObjectsToTensors(
      usage_records, MemoryStrategy::EQUALITY, &assignment));

  for (auto& node : nodes_) {
    for (auto& id : node.inputs) {
      ValueId new_id = assignment.object_ids[remap_from_graph_ids[id]];
      remap_from_graph_ids_to_shared_[id] = new_id;
      id = new_id;
    }
    for (auto& id : node.outputs) {
      ValueId new_id = assignment.object_ids[remap_from_graph_ids[id]];
      remap_from_graph_ids_to_shared_[id] = new_id;
      id = new_id;
    }
  }

  for (auto& node : nodes_) {
    auto tensors = GetCLNodeTensors(node);
    for (auto& tensor : tensors) {
      const auto& it = tensors_.find(tensor.first);
      if (it == tensors_.end()) {
        const auto& desc = assignment.object_sizes[tensor.first];
        Tensor* t = &tensors_[tensor.first];
        RETURN_IF_ERROR(
            CreateTensor(*context, device, desc.shape, tensor.second, t));
      }
    }
  }
  return OkStatus();
}

void InferenceContext::BindMemoryToOperations() {
  for (auto& node : nodes_) {
    const auto& first_range = node.ranges[0];
    for (int k = first_range.x; k < first_range.y; ++k) {
      auto id = node.inputs[k];
      const auto& it = tensors_.find(id);
      node.operations[0]->SetSrc(&it->second, k - first_range.x);
    }
    for (int i = 1; i < node.ranges.size(); ++i) {
      const auto& range = node.ranges[i];
      for (int k = range.x; k < range.y; ++k) {
        auto id = node.inputs[k];
        const auto& it = tensors_.find(id);
        node.operations[i]->SetSrc(&it->second, k - range.x + 1);
      }
    }

    for (int i = 0; i < node.outputs.size(); ++i) {
      auto id = node.outputs[i];
      const auto& it = tensors_.find(id);
      node.operations[0]->SetDst(&it->second, i);
    }
  }
}

Status InferenceContext::Compile(const CreationContext& creation_context) {
  for (auto& node : nodes_) {
    RETURN_IF_ERROR(node.operations[0]->Compile(creation_context));
  }
  return OkStatus();
}

Status InferenceContext::Tune(const TuningParameters& tuning_parameters) {
  for (auto& node : nodes_) {
    RETURN_IF_ERROR(node.operations[0]->Tune(tuning_parameters));
  }
  return OkStatus();
}

Status InferenceContext::AddToQueue(CLCommandQueue* queue) {
  if (need_manual_release_) {
    if (prev_enqueue_start_point_.is_valid()) {
      prev_enqueue_start_point_.Wait();
    }
    RETURN_IF_ERROR(queue->EnqueueEvent(&prev_enqueue_start_point_));
  }
  for (auto& node : nodes_) {
    RETURN_IF_ERROR(node.operations[0]->AddToQueue(queue));
  }
  if (need_flush_) {
    clFlush(queue->queue());
  }
  return OkStatus();
}

Status InferenceContext::Profile(ProfilingCommandQueue* queue,
                                 ProfilingInfo* result) {
  queue->ResetMeasurements();
  for (auto& node : nodes_) {
    queue->SetEventsLabel(node.name);
    RETURN_IF_ERROR(node.operations[0]->AddToQueue(queue));
  }
  RETURN_IF_ERROR(queue->WaitForCompletion());
  *result = queue->GetProfilingInfo();
  return OkStatus();
}

Tensor* InferenceContext::GetTensor(ValueId id) {
  return &tensors_[remap_from_graph_ids_to_shared_[id]];
}

Status InferenceContext::SetInputTensor(ValueId id, const TensorFloat32& tensor,
                                        CLCommandQueue* queue) {
  return GetTensor(id)->WriteData(queue, tensor);
}

Status InferenceContext::GetOutputTensor(ValueId id, CLCommandQueue* queue,
                                         TensorFloat32* result) {
  const auto& gpu_tensor = *GetTensor(id);
  const int4 dst_size = gpu_tensor.GetSizeWithDepth();
  const auto dst_shape = BHWC(1, dst_size.y, dst_size.x, dst_size.z);
  result->id = id;
  result->shape = dst_shape;
  result->data.resize(dst_shape.DimensionsProduct());
  return gpu_tensor.ReadData(queue, result);
}

Status RunGraphTransforms(GraphFloat32* graph) {
  auto merge_padding_transform = NewMergePaddingWithAdd();
  auto add_bias_transform = NewAddBias();
  ModelTransformer transformer(graph, /*reporter=*/nullptr);
  if (!transformer.Apply("add_bias", add_bias_transform.get())) {
    return InternalError("Invalid add_bias transform");
  }
  if (!transformer.Apply("merge_padding", merge_padding_transform.get())) {
    return InternalError("Invalid merge_padding transform");
  }
  return OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
