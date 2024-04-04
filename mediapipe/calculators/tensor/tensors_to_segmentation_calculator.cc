// Copyright 2021 The MediaPipe Authors.
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

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "mediapipe/calculators/tensor/tensors_to_segmentation_calculator.pb.h"
#include "mediapipe/calculators/tensor/tensors_to_segmentation_converter.h"
#include "mediapipe/calculators/tensor/tensors_to_segmentation_utils.h"
#include "mediapipe/framework/calculator_context.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/framework/port.h"
#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/port/status_macros.h"

#if !MEDIAPIPE_DISABLE_GPU
#include "mediapipe/gpu/gl_calculator_helper.h"

#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
#include "mediapipe/calculators/tensor/tensors_to_segmentation_converter_gl_buffer.h"
#elif MEDIAPIPE_METAL_ENABLED
#include "mediapipe/calculators/tensor/tensors_to_segmentation_converter_metal.h"
#import "mediapipe/gpu/MPPMetalHelper.h"
#else
#include "mediapipe/calculators/tensor/tensors_to_segmentation_converter_gl_texture.h"
#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
#endif  // !MEDIAPIPE_DISABLE_GPU

#if !MEDIAPIPE_DISABLE_OPENCV
#include "mediapipe/calculators/tensor/tensors_to_segmentation_converter_opencv.h"
#endif  // !MEDIAPIPE_DISABLE_OPENCV

namespace {
constexpr int kWorkgroupSize = 8;  // Block size for GPU shader.
enum { ATTRIB_VERTEX, ATTRIB_TEXTURE_POSITION, NUM_ATTRIBUTES };

constexpr char kTensorsTag[] = "TENSORS";
constexpr char kOutputSizeTag[] = "OUTPUT_SIZE";
constexpr char kMaskTag[] = "MASK";
}  // namespace

namespace mediapipe {

using ::mediapipe::tensors_to_segmentation_utils::CanUseGpu;
using ::mediapipe::tensors_to_segmentation_utils::GetHwcFromDims;

// Converts Tensors from a tflite segmentation model to an image mask.
//
// Performs optional upscale to OUTPUT_SIZE dimensions if provided,
// otherwise the mask is the same size as input tensor.
//
// If at least one input tensor is already on GPU, processing happens on GPU and
// the output mask is also stored on GPU. Otherwise, processing and the output
// mask are both on CPU.
//
// On GPU, the mask is an RGBA image, in both the R & A channels, scaled 0-1.
// On CPU, the mask is a ImageFormat::VEC32F1 image, with values scaled 0-1.
//
//
// Inputs:
//   One of the following TENSORS tags:
//   TENSORS: Vector of Tensors of type kFloat32. Only the first tensor will be
//            used. The tensor dimensions are specified in this calculator's
//            options.
//   OUTPUT_SIZE(optional): std::pair<int, int>,
//                          If provided, the size to upscale mask to.
//
// Output:
//   MASK: An Image output mask, RGBA(GPU) / VEC32F1(CPU).
//
// Options:
//   See tensors_to_segmentation_calculator.proto
//
// Usage example:
// node {
//   calculator: "TensorsToSegmentationCalculator"
//   input_stream: "TENSORS:tensors"
//   input_stream: "OUTPUT_SIZE:size"
//   output_stream: "MASK:hair_mask"
//   node_options: {
//     [mediapipe.TensorsToSegmentationCalculatorOptions] {
//       output_layer_index: 1
//       # gpu_origin: CONVENTIONAL # or TOP_LEFT
//     }
//   }
// }
//
// TODO Refactor and add support for other backends/platforms.
//
class TensorsToSegmentationCalculator : public CalculatorBase {
 public:
  static absl::Status GetContract(CalculatorContract* cc);

  absl::Status Open(CalculatorContext* cc) override;
  absl::Status Process(CalculatorContext* cc) override;

 private:
  absl::Status LoadOptions(CalculatorContext* cc);
  absl::Status InitConverterIfNecessary(bool use_gpu, CalculatorContext* cc) {
    if (use_gpu) {
#if !MEDIAPIPE_DISABLE_GPU
      if (!gpu_converter_) {
#if MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
        MP_ASSIGN_OR_RETURN(gpu_converter_,
                            CreateGlBufferConverter(cc, options_));
#elif MEDIAPIPE_METAL_ENABLED
        MP_ASSIGN_OR_RETURN(gpu_converter_, CreateMetalConverter(cc, options_));
#else
        MP_ASSIGN_OR_RETURN(gpu_converter_,
                            CreateGlTextureConverter(cc, options_));
#endif  // MEDIAPIPE_OPENGL_ES_VERSION >= MEDIAPIPE_OPENGL_ES_31
      }
#else
      RET_CHECK_FAIL() << "Cannot initialize GPU converter because GPU "
                          "processing is disabled.";
#endif  // !MEDIAPIPE_DISABLE_GPU
    } else {
#if !MEDIAPIPE_DISABLE_OPENCV
      if (!cpu_converter_) {
        MP_ASSIGN_OR_RETURN(cpu_converter_, CreateOpenCvConverter(options_));
      }
#else
      RET_CHECK_FAIL() << "Cannot initialize OpenCV converter because OpenCV "
                          "processing is disabled.";
#endif  // !MEDIAPIPE_DISABLE_OPENCV
    }
    return absl::OkStatus();
  }

  mediapipe::TensorsToSegmentationCalculatorOptions options_;
  std::unique_ptr<TensorsToSegmentationConverter> cpu_converter_;
  std::unique_ptr<TensorsToSegmentationConverter> gpu_converter_;
};
REGISTER_CALCULATOR(TensorsToSegmentationCalculator);

// static
absl::Status TensorsToSegmentationCalculator::GetContract(
    CalculatorContract* cc) {
  RET_CHECK(!cc->Inputs().GetTags().empty());
  RET_CHECK(!cc->Outputs().GetTags().empty());

  // Inputs.
  cc->Inputs().Tag(kTensorsTag).Set<std::vector<Tensor>>();
  if (cc->Inputs().HasTag(kOutputSizeTag)) {
    cc->Inputs().Tag(kOutputSizeTag).Set<std::pair<int, int>>();
  }

  // Outputs.
  cc->Outputs().Tag(kMaskTag).Set<Image>();

  if (CanUseGpu()) {
#if !MEDIAPIPE_DISABLE_GPU
    MP_RETURN_IF_ERROR(mediapipe::GlCalculatorHelper::UpdateContract(
        cc, /*request_gpu_as_optional=*/true));
#if MEDIAPIPE_METAL_ENABLED
    MP_RETURN_IF_ERROR([MPPMetalHelper updateContract:cc]);
#endif  // MEDIAPIPE_METAL_ENABLED
#endif  // !MEDIAPIPE_DISABLE_GPU
  }

  return absl::OkStatus();
}

absl::Status TensorsToSegmentationCalculator::Open(CalculatorContext* cc) {
  cc->SetOffset(TimestampDiff(0));

  MP_RETURN_IF_ERROR(LoadOptions(cc));

  return absl::OkStatus();
}

absl::Status TensorsToSegmentationCalculator::Process(CalculatorContext* cc) {
  if (cc->Inputs().Tag(kTensorsTag).IsEmpty()) {
    return absl::OkStatus();
  }

  const auto& input_tensors =
      cc->Inputs().Tag(kTensorsTag).Get<std::vector<Tensor>>();

  bool use_gpu = false;
  if (CanUseGpu()) {
    // Use GPU processing only if at least one input tensor is already on GPU.
    for (const auto& tensor : input_tensors) {
      if (tensor.ready_on_gpu()) {
        use_gpu = true;
        break;
      }
    }
  }

  // Validate tensor channels and activation type.
  {
    RET_CHECK(!input_tensors.empty());
    RET_CHECK(input_tensors[0].element_type() == Tensor::ElementType::kFloat32);
    MP_ASSIGN_OR_RETURN(auto hwc,
                        GetHwcFromDims(input_tensors[0].shape().dims));
    int tensor_channels = std::get<2>(hwc);
    using Options = ::mediapipe::TensorsToSegmentationCalculatorOptions;
    switch (options_.activation()) {
      case Options::NONE:
        RET_CHECK_EQ(tensor_channels, 1);
        break;
      case Options::SIGMOID:
        RET_CHECK_EQ(tensor_channels, 1);
        break;
      case Options::SOFTMAX:
        RET_CHECK_EQ(tensor_channels, 2);
        break;
    }
  }

  // Get dimensions.
  MP_ASSIGN_OR_RETURN(auto hwc, GetHwcFromDims(input_tensors[0].shape().dims));
  auto [tensor_height, tensor_width, tensor_channels] = hwc;
  int output_width = tensor_width, output_height = tensor_height;
  if (cc->Inputs().HasTag(kOutputSizeTag)) {
    const auto& size =
        cc->Inputs().Tag(kOutputSizeTag).Get<std::pair<int, int>>();
    output_width = size.first;
    output_height = size.second;
  }

  if (use_gpu) {
#if !MEDIAPIPE_DISABLE_GPU
    // Lazily initialize converter
    MP_RETURN_IF_ERROR(InitConverterIfNecessary(use_gpu, cc));
    MP_ASSIGN_OR_RETURN(
        std::unique_ptr<Image> output_mask,
        gpu_converter_->Convert(input_tensors, output_width, output_height));
    cc->Outputs().Tag(kMaskTag).Add(output_mask.release(),
                                    cc->InputTimestamp());
#else
    RET_CHECK_FAIL() << "GPU processing disabled.";
#endif  // !MEDIAPIPE_DISABLE_GPU
  } else {
#if !MEDIAPIPE_DISABLE_OPENCV
    // Lazily initialize converter.
    MP_RETURN_IF_ERROR(InitConverterIfNecessary(use_gpu, cc));
    MP_ASSIGN_OR_RETURN(
        std::unique_ptr<Image> output_mask,
        cpu_converter_->Convert(input_tensors, output_width, output_height));
    cc->Outputs().Tag(kMaskTag).Add(output_mask.release(),
                                    cc->InputTimestamp());
#else
    RET_CHECK_FAIL() << "OpenCV processing disabled.";
#endif  // !MEDIAPIPE_DISABLE_OPENCV
  }

  return absl::OkStatus();
}

absl::Status TensorsToSegmentationCalculator::LoadOptions(
    CalculatorContext* cc) {
  // Get calculator options specified in the graph.
  options_ = cc->Options<mediapipe::TensorsToSegmentationCalculatorOptions>();

  return absl::OkStatus();
}

}  // namespace mediapipe
