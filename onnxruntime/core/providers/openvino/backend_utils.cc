// Copyright(C) 2020 Intel Corporation
// Licensed under the MIT License

#include <map>
#include <string>
#include <memory>
#include <sstream>
#include <fstream>

#include <inference_engine.hpp>
#include <ngraph/frontend/onnx_import/onnx.hpp>

// FIXME: These should not be needed after v1 ops
// are fully integrated into onnx importer
#include <ngraph/pass/manager.hpp>
#include <ngraph/pass/opset1_upgrade.hpp>
#include <ngraph/pass/convert_fp32_to_fp16.hpp>

// FIXME: Remove before production
#include <ngraph/serializer.hpp>

#include "core/session/onnxruntime_cxx_api.h"
#include "core/graph/graph.h"
#include "core/common/logging/logging.h"

namespace onnxruntime {
namespace openvino_ep {
namespace backend_utils {


//TODO: Remove this before production
bool IsDebugEnabled() {
  return (std::getenv("UEP_ENABLE_DEBUG") != nullptr);
}

void DumpOnnxModelProto(const ONNX_NAMESPACE::ModelProto& model_proto, std::string file_name) {
  std::fstream outfile(file_name, std::ios::out | std::ios::trunc | std::ios::binary);
  model_proto.SerializeToOstream(&outfile);
  outfile.close();
}

std::shared_ptr<InferenceEngine::CNNNetwork> CreateCNNNetwork(const ONNX_NAMESPACE::ModelProto& model_proto, InferenceEngine::Precision precision) {
  std::istringstream model_stream{model_proto.SerializeAsString()};
  std::shared_ptr<ngraph::Function> ng_function;
  std::cout << "CreateNgraphFunc" << std::endl;
  try {
    ng_function = ngraph::onnx_import::import_onnx_model(model_stream);
    LOGS_DEFAULT(INFO) << "ONNX Import Done";
  } catch (const std::exception& exp) {
    LOGS_DEFAULT(FATAL) << "[NGRAPHCustomOp] "
                        << "Exception while importing model to nGraph: " << std::string(exp.what());
    // << " - " << name_ << " - "
    throw;
  } catch (...) {
    LOGS_DEFAULT(FATAL) << "[NGRAPHCustomOp] "
                        << "Unknown exception while importing model to nGraph";
    // << " - " << name_ << " - "
    throw;
  }

  //Serializing nGraph function
  if (IsDebugEnabled()) {
    std::string json_string = serialize(ng_function, 4);
    std::ofstream out("serialize_function_before_PM.json");
    out << json_string;
  }

  //Pass Manager for V1 transformations
  ngraph::pass::Manager pass_manager;
  pass_manager.register_pass<ngraph::pass::Opset1Upgrade>();
  pass_manager.run_passes(ng_function);

  if (precision == InferenceEngine::Precision::FP16) {
    if (IsDebugEnabled())
      std::cout << "FP16" << std::endl;
    //FP16 transformations
    ngraph::pass::ConvertFP32ToFP16().run_on_function(ng_function);
    ng_function->validate_nodes_and_infer_types();
  }

  //Serializing nGraph function
  if (IsDebugEnabled()) {
    std::string json_string_pm = serialize(ng_function, 4);
    std::ofstream out_pm("serialize_function_after_PM.json");
    out_pm << json_string_pm;
  }

  //IE wrapper for nGraph function
  // InferenceEngine::CNNNetwork network(ng_function);

  //Serialize CNNNetwork
  //network.serialize("IR.xml", "IR.bin");

  return std::make_shared<InferenceEngine::CNNNetwork>(ng_function);
}


InferenceEngine::Precision ConvertPrecisionONNXToOpenVINO(
    const ONNX_NAMESPACE::TypeProto& onnx_type) {
  ONNX_NAMESPACE::DataType type_string = ONNX_NAMESPACE::Utils::DataTypeUtils::ToType(onnx_type);
  if (*type_string == "float" || *type_string == "tensor(float)") {
    return InferenceEngine::Precision::FP32;
  } else if (*type_string == "float16" || *type_string == "tensor(float16)") {
    return InferenceEngine::Precision::FP16;
  } else if (*type_string == "int32" || *type_string == "tensor(int32)") {
    return InferenceEngine::Precision::I32;
  } else if (*type_string == "int16" || *type_string == "tensor(int16)") {
    return InferenceEngine::Precision::I16;
  } else if (*type_string == "int8" || *type_string == "tensor(int8)") {
    return InferenceEngine::Precision::I8;
  } else if (*type_string == "uint16" || *type_string == "tensor(uint16)") {
    return InferenceEngine::Precision::U16;
  } else if (*type_string == "uint8" || *type_string == "tensor(uint8)") {
    return InferenceEngine::Precision::U8;
  } else {
    ORT_THROW("Unsupported Data type");
  }
}

void SetIODefs(const ONNX_NAMESPACE::ModelProto& model_proto, std::shared_ptr<InferenceEngine::CNNNetwork> network) {
  // Configure input & output
  // Prepare input blobs
  if (network) {
    if (IsDebugEnabled())
      std::cout << "Network is not NULL" << std::endl;
  }
  auto inputInfo = network->getInputsInfo();
  int input_idx = 0;
  for (auto iter = inputInfo.begin(); iter != inputInfo.end(); ++iter, ++input_idx) {
    // Get the onnx index for the corresponding input (ignoring initializers)
    auto precision = ConvertPrecisionONNXToOpenVINO(model_proto.graph().input(input_idx).type());
    iter->second->setPrecision(precision);

    // Choose the appropriate OpenVINO layout for input tensor
    // based on dims size
    switch (iter->second->getTensorDesc().getDims().size()) {
      case 1:
        iter->second->setLayout(InferenceEngine::Layout::C);
        break;
      case 2:
        iter->second->setLayout(InferenceEngine::Layout::NC);
        break;
      case 3:
        iter->second->setLayout(InferenceEngine::Layout::CHW);
        break;
      case 4:
        iter->second->setLayout(InferenceEngine::Layout::NCHW);
        break;
      case 5:
        iter->second->setLayout(InferenceEngine::Layout::NCDHW);
        break;
      default:
        ORT_THROW("Invalid Dims type for input data map for: " + iter->first);
    };
  }

  // Prepare output blobs
  auto outputInfo = network->getOutputsInfo();
  int output_idx = 0;
  for (auto iter = outputInfo.begin(); iter != outputInfo.end(); ++iter, ++output_idx) {
    auto precision = ConvertPrecisionONNXToOpenVINO(model_proto.graph().output(output_idx).type());
    iter->second->setPrecision(precision);

    // Choose the appropriate OpenVINO layout for output tensor
    // based on dims size
    switch (iter->second->getTensorDesc().getDims().size()) {
      case 1:
        iter->second->setLayout(InferenceEngine::Layout::C);
        break;
      case 2:
        iter->second->setLayout(InferenceEngine::Layout::NC);
        break;
      case 3:
        iter->second->setLayout(InferenceEngine::Layout::CHW);
        break;
      case 4:
        iter->second->setLayout(InferenceEngine::Layout::NCHW);
        break;
      case 5:
        iter->second->setLayout(InferenceEngine::Layout::NCDHW);
        break;
      default:
        ORT_THROW("Invalid Dims type for output data map for: " + iter->first);
    };
  }
}

std::vector<const OrtValue*> GetInputTensors(Ort::CustomOpApi& ort, OrtKernelContext* context,
                                 std::shared_ptr<InferenceEngine::CNNNetwork> ie_cnn_network, std::vector<int> input_indexes) {

  std::vector<const OrtValue*> input_tensors;
  size_t input_count = ie_cnn_network->getInputsInfo().size();

  for (size_t i = 0; i < input_count; i++) {
    input_tensors.push_back(ort.KernelContext_GetInput(context, input_indexes[i]));
  }
  return input_tensors;
}

std::vector<OrtValue*> GetOutputTensors(Ort::CustomOpApi& ort, OrtKernelContext* context, size_t batch_size, InferenceEngine::InferRequest::Ptr infer_request,
                                  std::shared_ptr<InferenceEngine::CNNNetwork> ie_cnn_network, std::unordered_map<std::string, int> output_names) {
  std::vector<OrtValue*> output_tensors;
  auto graph_output_info = ie_cnn_network->getOutputsInfo();

  size_t i = 0;
  for (auto output_info_iter = graph_output_info.begin();
       output_info_iter != graph_output_info.end(); ++output_info_iter, ++i) {
    auto graph_output_blob = infer_request->GetBlob(output_info_iter->first);
    auto graph_output_dims = graph_output_blob->getTensorDesc().getDims();

    if (batch_size > 1) {
      // Add the batch size as dim 0.
      graph_output_dims.insert(graph_output_dims.begin(), batch_size);
    }

    size_t num_dims = graph_output_dims.size();
    int64_t output_shape[num_dims];
    for (size_t j = 0; j < num_dims; j++) {
      output_shape[j] = static_cast<int64_t>(graph_output_dims[j]);
    }
    auto it = output_names.find(output_info_iter->first);
    if(it == output_names.end()){
      ORT_THROW("Output names mismatch between OpenVINO and ONNX");
    }
    int index = it->second;

    output_tensors.push_back(ort.KernelContext_GetOutput(context, index, output_shape, num_dims));
  }
  return output_tensors;
}

} // namespace backend_utils
} // namespace openvino_ep
} // namespace onnxruntime