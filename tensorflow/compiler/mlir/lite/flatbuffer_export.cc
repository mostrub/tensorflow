/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/compiler/mlir/lite/flatbuffer_export.h"

#include <stddef.h>
#include <stdlib.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "flatbuffers/buffer.h"  // from @flatbuffers
#include "flatbuffers/flatbuffer_builder.h"  // from @flatbuffers
#include "flatbuffers/flexbuffers.h"  // from @flatbuffers
#include "flatbuffers/vector.h"  // from @flatbuffers
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantTypes.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributeInterfaces.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypeInterfaces.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/Diagnostics.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "stablehlo/dialect/StablehloOps.h"  // from @stablehlo
#include "tensorflow/compiler/mlir/lite/flatbuffer_operator.h"
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"
#include "tensorflow/compiler/mlir/lite/metrics/error_collector_inst.h"
#include "tensorflow/compiler/mlir/lite/quantization/ir/QuantOps.h"
#include "tensorflow/compiler/mlir/lite/utils/convert_type.h"
#include "tensorflow/compiler/mlir/lite/utils/low_bit_utils.h"
#include "tensorflow/compiler/mlir/lite/utils/stateful_ops_utils.h"
#include "tensorflow/compiler/mlir/op_or_arg_name_mapper.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_dialect.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_executor.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_saved_model.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/export_tf_dialect_op.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_tensor.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/dynamic_shape_utils.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/platform/tstring.h"
#include "tensorflow/lite/core/c/builtin_op_data.h"
#include "tensorflow/lite/core/interpreter.h"
#include "tensorflow/lite/core/macros.h"
#include "tensorflow/lite/delegates/flex/allowlisted_flex_ops.h"
#include "tensorflow/lite/experimental/remat/metadata_util.h"
#include "tensorflow/lite/graph_info.h"
#include "tensorflow/lite/python/metrics/converter_error_data.pb.h"
#include "tensorflow/lite/schema/mutable/schema_generated.h"
#include "tensorflow/lite/schema/schema_conversion_utils.h"
#include "tensorflow/lite/string_util.h"
#include "tensorflow/lite/toco/toco_flags.pb.h"
#include "tensorflow/lite/tools/versioning/gpu_compatibility.h"
#include "tensorflow/lite/tools/versioning/op_version.h"
#include "tensorflow/lite/tools/versioning/runtime_version.h"
#include "tensorflow/lite/version.h"
#include "tsl/platform/fingerprint.h"
#include "tsl/platform/status.h"
#include "tsl/platform/tstring.h"

using llvm::dyn_cast;
using llvm::formatv;
using llvm::isa;
using llvm::StringRef;
using mlir::Dialect;
using mlir::ElementsAttr;
using mlir::MLIRContext;
using mlir::ModuleOp;
using mlir::NoneType;
using mlir::Operation;
using mlir::Region;
using mlir::StringAttr;
using mlir::TensorType;
using mlir::Twine;
using mlir::Type;
using mlir::UnknownLoc;
using mlir::Value;
using mlir::WalkResult;
using mlir::func::FuncOp;
using tensorflow::OpOrArgLocNameMapper;
using tensorflow::OpOrArgNameMapper;
using tensorflow::Status;
using tflite::flex::IsAllowlistedFlexOp;
using xla::StatusOr;

template <typename T>
using BufferOffset = flatbuffers::Offset<T>;

template <typename T>
using VectorBufferOffset = flatbuffers::Offset<flatbuffers::Vector<T>>;

using CustomOptionsOffset = VectorBufferOffset<uint8_t>;

namespace tfl = mlir::TFL;

ABSL_CONST_INIT const absl::string_view kFlexOpNamePrefix = "Flex";

// Use initial buffer size in flatbuffer builder to be same as the initial size
// used by the TOCO export. (It does not explain rationale for this choice.)
constexpr size_t kInitialBufferSize = 10240;

// Set `isSigned` to false if the `type` is an 8-bit unsigned integer type.
// Since tflite doesn't support unsigned for other types, returns error if
// `isSigned` is set to false for other types.
static StatusOr<tflite::TensorType> GetTFLiteType(Type type,
                                                  bool is_signed = true) {
  if (!is_signed) {
    if (type.isSignlessInteger(8)) {
      return tflite::TensorType_UINT8;
    } else if (type.isSignlessInteger(16)) {
      return tflite::TensorType_UINT16;
    } else {
      return Status(absl::StatusCode::kInvalidArgument,
                    "'isSigned' can only be set for 8/16-bits integer type");
    }
  }

  if (type.isF32()) {
    return tflite::TensorType_FLOAT32;
  } else if (type.isF16()) {
    return tflite::TensorType_FLOAT16;
  } else if (type.isF64()) {
    return tflite::TensorType_FLOAT64;
  } else if (type.isa<mlir::TF::StringType>()) {
    return tflite::TensorType_STRING;
  } else if (type.isa<mlir::TF::Quint8Type>()) {
    return tflite::TensorType_UINT8;
  } else if (auto complex_type = type.dyn_cast<mlir::ComplexType>()) {
    auto ftype = complex_type.getElementType();
    if (ftype.isF32()) {
      return tflite::TensorType_COMPLEX64;
    }
    if (ftype.isF64()) {
      return tflite::TensorType_COMPLEX128;
    }
    return Status(absl::StatusCode::kInvalidArgument, "Unsupported type");
  } else if (auto itype = type.dyn_cast<mlir::IntegerType>()) {
    switch (itype.getWidth()) {
      case 1:
        return tflite::TensorType_BOOL;
      case 4:
        if (itype.isUnsigned()) {
          return Status(absl::StatusCode::kInvalidArgument,
                        "Unsupported 4bit unsigned int type");
        } else {
          return tflite::TensorType_INT4;
        }
      case 8:
        return itype.isUnsigned() ? tflite::TensorType_UINT8
                                  : tflite::TensorType_INT8;
      case 16:
        return itype.isUnsigned() ? tflite::TensorType_UINT16
                                  : tflite::TensorType_INT16;
      case 32:
        return itype.isUnsigned() ? tflite::TensorType_UINT32
                                  : tflite::TensorType_INT32;
      case 64:
        return itype.isUnsigned() ? tflite::TensorType_UINT64
                                  : tflite::TensorType_INT64;
    }
  } else if (auto q_uniform_type =
                 type.dyn_cast<mlir::quant::UniformQuantizedType>()) {
    return GetTFLiteType(q_uniform_type.getStorageType(),
                         q_uniform_type.isSigned());
  } else if (auto q_peraxis_type =
                 type.dyn_cast<mlir::quant::UniformQuantizedPerAxisType>()) {
    return GetTFLiteType(q_peraxis_type.getStorageType(),
                         q_peraxis_type.isSigned());
  } else if (auto q_calibrated_type =
                 type.dyn_cast<mlir::quant::CalibratedQuantizedType>()) {
    return GetTFLiteType(q_calibrated_type.getExpressedType());
  } else if (type.isa<mlir::TF::ResourceType>()) {
    return tflite::TensorType_RESOURCE;
  } else if (type.isa<mlir::TF::VariantType>()) {
    return tflite::TensorType_VARIANT;
  }
  // TFLite export fills FLOAT32 for unknown data types. Returning an error
  // for now for safety and this could be revisited when required.
  return Status(absl::StatusCode::kInvalidArgument, "Unsupported type");
}

static bool IsConst(Operation* op) {
  return isa<mlir::func::ConstantOp, mlir::arith::ConstantOp, mlir::TF::ConstOp,
             tfl::ConstOp, tfl::QConstOp, tfl::SparseConstOp,
             tfl::SparseQConstOp, mlir::TFL::NoValueOp,
             mlir::stablehlo::ConstantOp>(op);
}

static bool IsTFResourceOp(Operation* op) {
  for (const auto& operand : op->getOperands()) {
    auto elementType = getElementTypeOrSelf(operand.getType());
    if (elementType.isa<mlir::TF::ResourceType>()) {
      return true;
    }
  }
  for (const auto& result : op->getResults()) {
    auto elementType = getElementTypeOrSelf(result.getType());
    if (elementType.isa<mlir::TF::ResourceType>()) {
      return true;
    }
  }
  return false;
}

// Returns whether the current op is not supported by the TF Lite runtime.
static bool IsUnsupportedFlexOp(const std::string& op_name) {
  return op_name == "PartitionedCall" || op_name == "StatefulPartitionedCall";
}

// Create description of operation that could not be converted.
static std::string GetOpDescriptionForDebug(Operation* inst) {
  const int kLargeElementsAttr = 16;
  std::string op_str;
  llvm::raw_string_ostream os(op_str);
  inst->getName().print(os);
  os << "(";
  if (!inst->getOperandTypes().empty()) {
    bool first = true;
    for (Type operand_type : inst->getOperandTypes()) {
      os << (!first ? ", " : "");
      first = false;
      os << operand_type;
    }
  }
  os << ") -> (";
  if (!inst->getResultTypes().empty()) {
    bool first = true;
    for (Type result_type : inst->getResultTypes()) {
      os << (!first ? ", " : "");
      first = false;
      os << result_type;
    }
  }
  os << ")";
  // Print out attributes except for large elementsattributes (which should
  // rarely be the cause why the legalization didn't happen).
  if (!inst->getAttrDictionary().empty()) {
    os << " : {";
    bool first = true;
    for (auto& named_attr : inst->getAttrDictionary()) {
      os << (!first ? ", " : "");
      first = false;
      os << named_attr.getName().getValue() << " = ";
      if (auto element_attr = named_attr.getValue().dyn_cast<ElementsAttr>()) {
        if (element_attr.getNumElements() <= kLargeElementsAttr) {
          element_attr.print(os);
        } else {
          os << "<large>";
        }
      } else {
        named_attr.getValue().print(os);
      }
    }
    os << "}";
  }
  return os.str();
}

// Create a summary with the given information regarding op names and
// descriptions.
static std::string GetOpsSummary(
    const std::map<std::string, std::set<std::string>>& ops,
    const std::string& summary_title) {
  std::string op_str;
  llvm::raw_string_ostream os(op_str);

  std::vector<std::string> keys;
  keys.reserve(ops.size());

  std::vector<std::string> values;
  values.reserve(ops.size());

  for (auto const& op_name_and_details : ops) {
    keys.push_back(op_name_and_details.first);
    for (auto const& op_detail : op_name_and_details.second) {
      values.push_back(op_detail);
    }
  }

  os << summary_title << " ops: " << absl::StrJoin(keys, ", ") << "\n";
  os << "Details:\n\t" << absl::StrJoin(values, "\n\t");

  return os.str();
}

template <typename T>
static bool HasValidTFLiteType(Value value, T& error_handler) {
  // None type is allowed to represent unspecified operands.
  if (value.getType().isa<NoneType>()) return true;

  auto type = value.getType().dyn_cast<TensorType>();
  if (!type) {
    if (auto op = value.getDefiningOp()) {
      error_handler.emitError()
          << '\'' << op << "' should produce value of tensor type instead of "
          << value.getType();
      return false;
    }
    error_handler.emitError("expected tensor type, got ") << value.getType();
    return false;
  }

  Type element_type = type.getElementType();
  auto status = GetTFLiteType(element_type);
  if (!status.ok()) {
    return error_handler.emitError(
               formatv("Failed to convert element type '{0}': {1}",
                       element_type, status.status().message())),
           false;
  }
  return true;
}

// Returns true if the module holds all the invariants expected by the
// Translator class.
// TODO(hinsu): Now that translation is done by making a single pass over the
// MLIR module, consider inlining these validation checks at the place where
// these invariants are assumed instead of checking upfront.
static bool IsValidTFLiteMlirModule(ModuleOp module) {
  MLIRContext* context = module.getContext();

  // Verify that module has a function named main.
  FuncOp main_fn = module.lookupSymbol<FuncOp>("main");
  if (!main_fn) {
    int entry_func_count = 0;
    for (auto fn : module.getOps<FuncOp>()) {
      auto attrs = fn->getAttrOfType<mlir::DictionaryAttr>("tf.entry_function");
      if (attrs && !attrs.empty()) {
        ++entry_func_count;
      }
    }

    // Verify that module has a least one enrty function.
    if (entry_func_count == 0) {
      return emitError(UnknownLoc::get(context),
                       "should have a least one entry function"),
             false;
    }
  }

  for (auto fn : module.getOps<FuncOp>()) {
    if (!llvm::hasSingleElement(fn)) {
      return fn.emitError("should have exactly one basic block"), false;
    }
    auto& bb = fn.front();

    for (auto arg : bb.getArguments()) {
      if (!HasValidTFLiteType(arg, fn)) {
        auto elementType = getElementTypeOrSelf(arg.getType());
        if (elementType.isa<mlir::TF::VariantType>()) {
          return fn.emitError(
                     "function argument uses variant type. Currently, the "
                     "variant type is not natively supported in TFLite. Please "
                     "consider not using the variant type: ")
                     << arg.getType(),
                 false;
        }
        return fn.emitError("invalid TFLite type: ") << arg.getType(), false;
      }
    }

    // Verify that all operations except the terminator have exactly one
    // result of type supported by TFLite (or is a ControlType, which
    // will be removed later by ExtractControlEdges.)
    for (auto& inst : bb) {
      if (inst.hasTrait<mlir::OpTrait::IsTerminator>()) break;

      for (auto result : inst.getResults()) {
        if (result.getType().isa<mlir::TFL::ControlType>()) continue;
        if (!HasValidTFLiteType(result, inst)) {
          auto elementType = getElementTypeOrSelf(result.getType());
          if (elementType.isa<mlir::TF::VariantType>()) {
            return inst.emitError(
                       "operand result uses variant type. Currently, the "
                       "variant type is not natively supported in TFLite. "
                       "Please "
                       "consider not using the variant type: ")
                       << result.getType(),
                   false;
          }
          return fn.emitError("invalid TFLite type: ") << result.getType(),
                 false;
        }
      }
    }
  }

  return true;
}

static std::unique_ptr<::tensorflow::NodeDef> GetTensorFlowNodeDef(
    ::mlir::Operation* inst) {
  // We pass empty string for the original node_def name since Flex runtime
  // does not care about this being set correctly on node_def. There is no
  // "easy" (see b/120948529) way yet to get this from MLIR inst.
  auto status_or_node_def = tensorflow::ConvertTFDialectOpToNodeDef(
      inst, /*name=*/"", /*ignore_unregistered_attrs=*/true);
  if (!status_or_node_def.ok()) {
    inst->emitOpError(
        Twine("failed to obtain TensorFlow nodedef with status: " +
              status_or_node_def.status().ToString()));
    return {};
  }
  return std::move(status_or_node_def.value());
}

// Converts a mlir padding StringRef to TfLitePadding.
// Returns std::nullopt if conversion fails.
static std::optional<TfLitePadding> GetTflitePadding(Operation* inst,
                                                     llvm::StringRef padding) {
  const tflite::Padding padding_attr =
      std::move(llvm::StringSwitch<tflite::Padding>(padding)
                    .Case("SAME", tflite::Padding_SAME)
                    .Case("VALID", tflite::Padding_VALID));
  if (padding_attr == tflite::Padding_SAME) {
    return kTfLitePaddingSame;
  }
  if (padding_attr == tflite::Padding_VALID) {
    return kTfLitePaddingValid;
  }

  return inst->emitOpError() << "Invalid padding attribute: " << padding,
         std::nullopt;
}

// Extracts TfLitePoolParams from a TFL custom op.
// Template parameter, TFLOp, should be a TFL custom op containing attributes
// generated from TfLitePoolParams.
// Returns std::nullopt if conversion fails.
template <typename TFLOp>
static std::optional<TfLitePoolParams> GetTflitePoolParams(Operation* inst,
                                                           TFLOp op) {
  TfLitePoolParams pool_params;
  pool_params.stride_height = op.stride_h().getSExtValue();
  pool_params.stride_width = op.stride_w().getSExtValue();
  pool_params.filter_height = op.filter_h().getSExtValue();
  pool_params.filter_width = op.filter_w().getSExtValue();
  const auto padding = GetTflitePadding(inst, op.padding());
  if (padding) {
    pool_params.padding = *padding;
    pool_params.activation = kTfLiteActNone;
    pool_params.computed.padding = TfLitePaddingValues{0, 0, 0, 0};
    return pool_params;
  }

  return std::nullopt;
}

namespace {

using ::mlir::tf_saved_model::kTfSavedModelExportedNamesAttr;
using ::mlir::tf_saved_model::kTfSavedModelIndexPathAttr;

// Helper struct that wraps inputs/outputs of a single SignatureDef.
struct SignatureDefData {
  // Note, we are using maps here to make order deterministic
  // for easily testing only.

  // Inputs defined in the signature def mapped to tensor names.
  std::map<std::string, std::string> inputs;
  // Outputs defined in the signature def mapped to tensor names.
  std::map<std::string, std::string> outputs;
  // Signature key.
  std::string signature_key;
  // Subgraph index.
  uint32_t subgraph_index;
};

// Translates an MLIR module in TFLite dialect to TFLite FlatBuffer.
class Translator {
 public:
  // Translates the given MLIR module into TFLite FlatBuffer format and returns
  // the serialized output. Returns std::nullopt on unsupported, invalid inputs
  // or internal error.
  static std::optional<std::string> Translate(
      ModuleOp module, const toco::TocoFlags& toco_flags,
      const std::unordered_set<std::string>& tags,
      OpOrArgNameMapper* op_or_arg_name_mapper,
      const std::map<std::string, std::string>& metadata,
      bool serialize_stablehlo_ops,
      std::optional<size_t> custom_option_alignment);

 private:
  enum class OpType : char { kTfliteBuiltin, kSelectTf, kCustomOp };
  explicit Translator(ModuleOp module, const toco::TocoFlags& toco_flags,
                      const std::unordered_set<std::string>& saved_model_tags,
                      OpOrArgNameMapper* op_or_arg_name_mapper,
                      const std::map<std::string, std::string>& metadata,
                      std::optional<size_t> custom_option_alignment)
      : module_(module),
        name_mapper_(*op_or_arg_name_mapper),
        builder_(kInitialBufferSize),
        saved_model_tags_(saved_model_tags),
        allow_all_select_tf_ops_(toco_flags.allow_all_select_tf_ops()),
        select_user_tf_ops_(toco_flags.select_user_tf_ops().begin(),
                            toco_flags.select_user_tf_ops().end()),
        metadata_(metadata),
        supported_backends_(toco_flags.supported_backends().begin(),
                            toco_flags.supported_backends().end()),
        use_buffer_offset_(toco_flags.use_buffer_offset()),
        custom_option_alignment_(custom_option_alignment) {
    // The first buffer must be empty according to the schema definition.
    empty_buffer_ = tflite::CreateBuffer(builder_);
    buffers_.push_back(empty_buffer_);
    if (!toco_flags.force_select_tf_ops()) {
      enabled_op_types_.emplace(OpType::kTfliteBuiltin);
    }
    if (toco_flags.enable_select_tf_ops()) {
      enabled_op_types_.emplace(OpType::kSelectTf);
    }
    if (toco_flags.allow_custom_ops()) {
      enabled_op_types_.emplace(OpType::kCustomOp);
    }
    tf_dialect_ =
        module.getContext()->getOrLoadDialect<mlir::TF::TensorFlowDialect>();
    tfl_dialect_ = module.getContext()
                       ->getOrLoadDialect<mlir::TFL::TensorFlowLiteDialect>();
    stablehlo_dialect_ =
        module.getContext()
            ->getOrLoadDialect<mlir::stablehlo::StablehloDialect>();
    // Right now the TF executor dialect is still needed to build NodeDef.
    module.getContext()
        ->getOrLoadDialect<mlir::tf_executor::TensorFlowExecutorDialect>();
  }

  std::optional<std::string> TranslateInternal();

  // Returns TFLite buffer populated with constant value if the operation is
  // TFLite constant operation. Otherwise, returns an empty buffer. Emits error
  // and returns std::nullopt on failure. The buffer index may be changed if
  // duplicated buffer is found.
  std::optional<BufferOffset<tflite::Buffer>> BuildBuffer(
      Value value, bool can_be_deduplicated, int& index);

  // Build TFLite tensor from the given type. This function is for tfl.lstm
  // intermediates, which should have UniformQuantizedType.
  std::optional<BufferOffset<tflite::Tensor>> BuildTensorFromType(
      mlir::Type type, const std::string& name);

  // Builds TF::VariantType from the given element type. Returns std::nullopt if
  // failure. Returns empty vector if the element type is not TF::VariantType or
  // there is empty TensorType in the TF::VariantType.
  std::optional<std::vector<BufferOffset<tflite::VariantSubType>>>
  BuildTFVariantType(mlir::Type element_type);

  // Builds TFLite tensor from the given value. `buffer_idx` is index of the
  // corresponding buffer. Emits error and returns std::nullopt on failure.
  std::optional<BufferOffset<tflite::Tensor>> BuildTensor(
      Value value, const std::string& name, unsigned buffer_idx,
      const std::optional<BufferOffset<tflite::QuantizationParameters>>&
          quant_parameters);

  // TODO(b/137395003): Legalize tf.IfOp to TFLite dialect, and change the
  // following method to handle TFL::IfOp.
  BufferOffset<tflite::Operator> BuildIfOperator(
      mlir::TF::IfOp op, const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  // Build while operator where cond & body are regions.
  std::optional<BufferOffset<tflite::Operator>> BuildWhileOperator(
      mlir::TFL::WhileOp op, const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  // Build call once operator.
  BufferOffset<tflite::Operator> BuildCallOnceOperator(
      mlir::TFL::CallOnceOp op, const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  BufferOffset<tflite::Operator> BuildNumericVerifyOperator(
      mlir::TFL::NumericVerifyOp op, const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  BufferOffset<tflite::Operator> BuildCustomOperator(
      Operation* inst, mlir::TFL::CustomOp op,
      const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  std::optional<CustomOptionsOffset> CreateFlexOpCustomOptions(
      const ::tensorflow::NodeDef& node_def, const mlir::Location& loc);

  std::optional<CustomOptionsOffset> CreateCustomOpCustomOptions(
      const ::tensorflow::NodeDef& node_def, const mlir::Location& loc);

  std::unique_ptr<flexbuffers::Builder> CreateFlexBuilderWithNodeAttrs(
      const ::tensorflow::NodeDef& node_def, const mlir::Location& loc);

  // Returns opcode index for op identified by the op_name, if already
  // available. Otherwise, creates a new OperatorCode using the given `builtin`
  // operator and associates it with `op_name`.
  uint32_t GetOpcodeIndex(const std::string& op_name,
                          tflite::BuiltinOperator builtin);

  // Builds operator for the given operation with specified operand and result
  // tensor indices. Emits an error and returns std::nullopt on failure.
  std::optional<BufferOffset<tflite::Operator>> BuildOperator(
      Operation* inst, std::vector<int32_t> operands,
      const std::vector<int32_t>& results,
      const std::vector<int32_t>& intermediates);

  // Returns the quantization parameters for output value of "quant.stats" op.
  BufferOffset<tflite::QuantizationParameters>
  GetQuantizationForQuantStatsOpOutput(mlir::quantfork::StatisticsOp stats_op);

  // Build a subgraph with a given name out of the region either corresponding
  // to a function's body or while op. Modifies *region by calling
  // ExtractControlEdges.
  std::optional<BufferOffset<tflite::SubGraph>> BuildSubGraph(
      const std::string& name, Region* region, int index);

  // Modifies *block by unwrapping all ControlNodeOps. The DAG of the control
  // dependencies is returned as a vector of its edges, with node indices into
  // *block.
  std::vector<std::pair<int, int>> ExtractControlEdges(mlir::Block* block);

  // Builds Metadata with the given `name` and buffer `content`.
  BufferOffset<tflite::Metadata> BuildMetadata(StringRef name,
                                               StringRef content);

  // Encodes the `tfl.metadata` dictionary attribute of the module to the
  // metadata section in the final model.
  std::optional<VectorBufferOffset<BufferOffset<tflite::Metadata>>>
  CreateMetadataVector();

  // Builds and returns list of tfl.SignatureDef sections in the model.
  std::optional<VectorBufferOffset<BufferOffset<tflite::SignatureDef>>>
  CreateSignatureDefs(const std::vector<SignatureDefData>& signature_defs);

  // Returns list of offsets for the passed 'items' in TensorMap structure
  // inside the flatbuffer.
  // 'items' is a map from tensor name in signatureDef to tensor name in
  // the subgraph, specified by the 'subgraph_index' argument.
  std::vector<BufferOffset<tflite::TensorMap>> GetList(
      int subgraph_index, const std::map<std::string, std::string>& items);

  // Uses the tf.entry_function attribute (if set) to initialize the op to name
  // mapping.
  void InitializeNamesFromAttribute(FuncOp fn, bool* has_input_attr);

  // Determines if the specified operation op's operand at operand_index
  // is marked as a stateful operand.
  bool IsStatefulOperand(mlir::Operation* op, int operand_index);

  // Returns a unique name for `val`.
  std::string UniqueName(mlir::Value val);

  BufferOffset<tflite::SparsityParameters> BuildSparsityParameters(
      const mlir::TFL::SparsityParameterAttr& s_attr);

  bool EstimateArithmeticCount(int64_t* count);

  // Check compatibility with GPU delegate and returns the compatibility.
  bool CheckGpuDelegateCompatibility(uint8_t* model_buffer_pointer);

  // Append constant and custom op buffers at the end of the flatbuffer and
  // calculate the offsets
  void AppendBufferData(std::string& result);

  // Update constant & custom op buffer offsets
  // Return false if fail to update offset
  bool UpdateBufferOffsets(tflite::Model* mutable_model);

  // check if Flatbuffer builder can no longer hold the given amount of the data
  inline bool IsModelBiggerThan2GB(const uint64_t data_size) {
    return data_size > flatbuffer_size_max - builder_.GetSize();
  }

  // helper function for build stablehlo operators
  std::optional<BufferOffset<tflite::Operator>>
  BuildStablehloOperatorwithoutOptions(Operation* inst,
                                       const std::vector<int32_t>& operands,
                                       const std::vector<int32_t>& results,
                                       tflite::BuiltinOperator op_code);
  BufferOffset<flatbuffers::Vector<unsigned int>> BuildStablehloPrecisionConfig(
      ::mlir::ArrayAttr precisionConfig);

  std::optional<BufferOffset<tflite::Operator>> BuildStablehloGatherOp(
      mlir::stablehlo::GatherOp gather_op, const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  std::optional<BufferOffset<tflite::Operator>> BuildStablehloScatterOp(
      mlir::stablehlo::ScatterOp scatter_op,
      const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  std::optional<BufferOffset<tflite::Operator>> BuildStablehloReduceWindowOp(
      mlir::stablehlo::ReduceWindowOp reduce_window_op,
      const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  std::optional<BufferOffset<tflite::Operator>> BuildStablehloRngBitGeneratorOp(
      mlir::stablehlo::RngBitGeneratorOp rng_op,
      const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  std::optional<BufferOffset<tflite::Operator>> BuildStablehloPadOp(
      mlir::stablehlo::PadOp pad_op, const std::vector<int32_t>& operands,
      const std::vector<int32_t>& results);

  // create a subgraph given a unnamed mlir region, return the corresponding
  // subgraph index
  int32_t UnnamedRegionToSubgraph(mlir::Region* region,
                                  tflite::BuiltinOperator op_code);

  ModuleOp module_;

  tensorflow::OpOrArgNameMapper& name_mapper_;

  flatbuffers::FlatBufferBuilder builder_;
  BufferOffset<tflite::Buffer> empty_buffer_;

  std::vector<BufferOffset<tflite::Buffer>> buffers_;
  // Maps subgraph index and tensor name in the graph to the tensor index.
  absl::flat_hash_map<int, absl::flat_hash_map<std::string, int>>
      tensor_index_map_;

  // Maps op name to index of the corresponding OperatorCode in opcodes_ vector.
  absl::flat_hash_map<std::string, uint32_t> opcode_index_map_;
  std::vector<BufferOffset<tflite::OperatorCode>> opcodes_;

  // Maps function name to index of the corresponding subgraph in the FlatBuffer
  // model.
  absl::flat_hash_map<std::string, int> subgraph_index_map_;
  absl::flat_hash_set<OpType> enabled_op_types_;

  // Maps buffer data to corresponding buffer index
  // in the idx map, the value is a pair of offset and size
  absl::flat_hash_map<int, std::pair<uint64_t, uint64_t>> buffer_idx_map_;
  absl::flat_hash_map<int, std::vector<uint8_t>> buffer_data_map_;

  // Maps custom options data to corresponding node
  // Key is set to be the list of input tensor indices and list of output tensor
  // indices
  // in the idx map, the value is a pair of offset and size
  absl::flat_hash_map<std::pair<std::vector<int32_t>, std::vector<int32_t>>,
                      std::vector<uint8_t>>
      custom_op_data_map_;
  absl::flat_hash_map<std::pair<std::vector<int32_t>, std::vector<int32_t>>,
                      std::pair<uint64_t, uint64_t>>
      custom_op_idx_map_;

  // Points to TensorFlow and TFLite dialects, respectively. nullptr if the
  // dialect is not registered.
  const Dialect* tf_dialect_;
  const Dialect* tfl_dialect_;
  const Dialect* stablehlo_dialect_;

  // The failed ops during legalization.
  std::map<std::string, std::set<std::string>> failed_flex_ops_;
  std::map<std::string, std::set<std::string>> failed_custom_ops_;

  // Ops to provide warning messages.
  std::map<std::string, std::set<std::string>> custom_ops_;
  std::map<std::string, std::set<std::string>> flex_ops_;

  // Resource ops to provide warning messages.
  std::map<std::string, std::set<std::string>> resource_ops_;

  // Set of saved model tags, if any.
  const std::unordered_set<std::string> saved_model_tags_;
  // Allows automatic pass through of TF ops as select Tensorflow ops.
  const bool allow_all_select_tf_ops_;
  // User's defined ops allowed with Flex.
  const std::unordered_set<std::string> select_user_tf_ops_;
  // Map of key value pairs of metadata to export.
  const std::map<std::string, std::string> metadata_;
  // User's defined supported backends.
  const std::unordered_set<std::string> supported_backends_;
  // A mapping table to mlir::Operation objects for TFL subgraph and operator
  // index in a flatbuffer.
  std::vector<std::vector<Operation*>> subgraph_op_inst_map_;
  // A list of subgraphs in the model
  std::vector<BufferOffset<tflite::SubGraph>> subgraphs_;

  // Will be populated by ExtractControlEdges to contain the control
  // dependencies contained in the ControlNodeOps. Will then be used to populate
  // metadata in the exported flatbuffer file.
  tflite::ModelControlDependencies model_control_dependencies_;

  // Decide if we convert stablehlo ops in flatbuffer
  bool convert_stablehlo_ = true;

  bool use_buffer_offset_ = false;

  bool require_use_buffer_offset_ = false;

  std::optional<size_t> custom_option_alignment_ = std::nullopt;

  // Map from mlir constant attribute to the buffer index. This is used to
  // deduplicate the buffers in the flatbuffer.
  llvm::DenseMap<mlir::ElementsAttr, int> const_attribute_to_buffer_map_;
};

bool Translator::EstimateArithmeticCount(int64_t* count) {
  int64_t result = 0;
  bool encounter_undetermined_mac = false;
  module_->walk([&](mlir::TFL::TflArithmeticCountOpInterface op) {
    int64_t mac_count = op.GetArithmeticCount(op);
    if (mac_count < 0) {
      encounter_undetermined_mac = true;
      return;
    }
    result += mac_count;
  });

  *count = result;
  return !encounter_undetermined_mac;
}

std::string Translator::UniqueName(mlir::Value val) {
  return std::string(name_mapper_.GetUniqueName(val));
}

std::optional<BufferOffset<tflite::Buffer>> Translator::BuildBuffer(
    mlir::Value value, bool can_be_deduplicated, int& index) {
  auto inst = value.getDefiningOp();
  ElementsAttr attr;
  if (auto cst = dyn_cast<mlir::arith::ConstantOp>(inst)) {
    // arith::ConstantOp have ElementAttr at this point due to validation of the
    // TFLite module.
    attr = cst.getValue().cast<ElementsAttr>();
  } else if (auto cst = dyn_cast<mlir::TF::ConstOp>(inst)) {
    attr = cst.getValue();
  } else if (auto cst = dyn_cast<tfl::ConstOp>(inst)) {
    attr = cst.getValue();
  } else if (auto cst = dyn_cast<tfl::QConstOp>(inst)) {
    attr = cst.getValue();
  } else if (auto cst = dyn_cast<mlir::stablehlo::ConstantOp>(inst)) {
    attr = cst.getValue();
  } else if (auto cst = dyn_cast<tfl::SparseConstOp>(inst)) {
    attr = cst.getCompressedData();
  } else if (auto cst = dyn_cast<tfl::SparseQConstOp>(inst)) {
    attr = cst.getCompressedData();
  } else {
    return empty_buffer_;
  }

  if (can_be_deduplicated) {
    if (const_attribute_to_buffer_map_.find(attr) !=
        const_attribute_to_buffer_map_.end()) {
      index = const_attribute_to_buffer_map_[attr];
      return empty_buffer_;
    }
    const_attribute_to_buffer_map_[attr] = index;
  }

  // TF doesn't currently support 4-bit types (DT_INT4), so we'll run into
  // trouble calling ConvertToTensor(). For now, extract the tensor data from
  // ElementsAttr directly in this and read type from tflite::TensorType instead
  // of tensorflow::DataType.
  auto type = value.getType().cast<TensorType>();
  tflite::TensorType tflite_element_type =
      GetTFLiteType(type.getElementType()).value();
  if (tflite_element_type == tflite::TensorType_INT4) {
    std::vector<uint8_t> data;
    for (mlir::APInt v : attr.getValues<mlir::APInt>()) {
      data.emplace_back(static_cast<uint8_t>(*(v.getRawData())));
    }
    auto packed_buffer = tflite::PackInt4ValuesDensely(data);
    if (use_buffer_offset_) {
      buffer_data_map_[index] = packed_buffer;
      return tflite::CreateBuffer(builder_, 0, 1, 1);
    } else {
      if (IsModelBiggerThan2GB(packed_buffer.size())) {
        require_use_buffer_offset_ = true;
        return empty_buffer_;
      }
      auto buffer_data =
          builder_.CreateVector(packed_buffer.data(), packed_buffer.size());
      return tflite::CreateBuffer(builder_, buffer_data);
    }
  }

  tensorflow::Tensor tensor;
  auto status = tensorflow::ConvertToTensor(attr, &tensor);
  if (!status.ok()) {
    inst->emitError(
        Twine("failed to convert value attribute to tensor with error: " +
              status.ToString()));
    return std::nullopt;
  }

  // TensorFlow and TensorFlow Lite use different string encoding formats.
  // Convert to TensorFlow Lite format is it's a constant string tensor.
  if (tensor.dtype() == tensorflow::DT_STRING) {
    ::tflite::DynamicBuffer dynamic_buffer;
    auto flat = tensor.flat<::tensorflow::tstring>();
    for (int i = 0; i < flat.size(); ++i) {
      const auto& str = flat(i);
      dynamic_buffer.AddString(str.c_str(), str.length());
    }
    char* tensor_buffer;
    int bytes = dynamic_buffer.WriteToBuffer(&tensor_buffer);
    if (use_buffer_offset_) {
      std::vector<uint8_t> buffer_data(tensor_buffer, tensor_buffer + bytes);
      free(tensor_buffer);
      buffer_data_map_[index] = buffer_data;
      return tflite::CreateBuffer(builder_, 0, 1, 1);
    } else {
      if (IsModelBiggerThan2GB(bytes)) {
        require_use_buffer_offset_ = true;
        return empty_buffer_;
      }
      auto buffer_data = builder_.CreateVector(
          reinterpret_cast<uint8_t*>(tensor_buffer), bytes);
      free(tensor_buffer);
      return tflite::CreateBuffer(builder_, buffer_data);
    }
  }

  absl::string_view tensor_data = tensor.tensor_data();
  if (use_buffer_offset_) {
    std::vector<uint8_t> buffer_data(tensor_data.data(),
                                     tensor_data.data() + tensor_data.size());
    buffer_data_map_[index] = buffer_data;
    return tflite::CreateBuffer(builder_, 0, 1, 1);
  } else {
    if (IsModelBiggerThan2GB(tensor_data.size())) {
      require_use_buffer_offset_ = true;
      return empty_buffer_;
    }
    auto buffer_data = builder_.CreateVector(
        reinterpret_cast<const uint8_t*>(tensor_data.data()),
        tensor_data.size());
    return tflite::CreateBuffer(builder_, buffer_data);
  }
}

int32_t Translator::UnnamedRegionToSubgraph(
    mlir::Region* region, const tflite::BuiltinOperator op_code) {
  int32_t subgraph_index = subgraphs_.size();
  std::string op_name = tflite::EnumNamesBuiltinOperator()[op_code];
  std::string graph_name = op_name + std::to_string(subgraph_index);
  auto subgraph = BuildSubGraph(graph_name, region, subgraph_index);
  if (!subgraph.has_value()) {
    mlir::emitError(region->getLoc(), "failed to build subgraph");
    return -1;
  }
  subgraphs_.push_back(subgraph.value());
  subgraph_index_map_[graph_name] = subgraph_index;
  return subgraph_index;
}

std::optional<std::vector<BufferOffset<tflite::VariantSubType>>>
Translator::BuildTFVariantType(mlir::Type element_type) {
  std::vector<BufferOffset<tflite::VariantSubType>> variant_params;
  auto variant_type = element_type.dyn_cast<mlir::TF::VariantType>();
  if (!variant_type) {
    return variant_params;
  }

  // We only support up to one nested type in tf_type.variant_type.
  if (variant_type.getSubtypes().size() > 1) {
    return std::nullopt;
  }
  if (variant_type.getSubtypes().empty()) {
    return variant_params;
  }
  mlir::TensorType tensor_type = variant_type.getSubtypes().front();
  tflite::TensorType tflite_element_type =
      GetTFLiteType(tensor_type.getElementType()).value();
  std::vector<int32_t> shape;
  if (tensor_type.hasRank()) {
    llvm::ArrayRef<int64_t> shape_ref = tensor_type.getShape();
    shape = std::vector<int32_t>(shape_ref.begin(), shape_ref.end());
  }

  variant_params.push_back(
      tflite::CreateVariantSubType(builder_, builder_.CreateVector(shape),
                                   tflite_element_type, tensor_type.hasRank()));
  return variant_params;
}

std::optional<BufferOffset<tflite::Tensor>> Translator::BuildTensorFromType(
    mlir::Type type, const std::string& name) {
  auto tensor_type = type.cast<TensorType>();

  llvm::ArrayRef<int64_t> shape_ref;
  std::vector<int32_t> shape;

  if (tensor_type.hasRank()) {
    if (tensor_type.hasStaticShape()) {
      shape_ref = tensor_type.getShape();
      shape = std::vector<int32_t>(shape_ref.begin(), shape_ref.end());
    } else {
      return std::nullopt;
    }
  }

  auto element_type = tensor_type.getElementType();
  tflite::TensorType tflite_element_type =
      GetTFLiteType(tensor_type.getElementType()).value();
  std::optional<std::vector<BufferOffset<tflite::VariantSubType>>>
      variant_params = BuildTFVariantType(element_type);
  if (!variant_params.has_value()) {
    return std::nullopt;
  }
  BufferOffset<tflite::QuantizationParameters> q_params = 0;
  if (auto qtype = element_type.dyn_cast<mlir::quant::UniformQuantizedType>()) {
    std::vector<float> scales = {static_cast<float>(qtype.getScale())};
    std::vector<int64_t> zero_points = {qtype.getZeroPoint()};
    q_params = tflite::CreateQuantizationParameters(
        builder_, /*min=*/0, /*max=*/0, builder_.CreateVector<float>(scales),
        builder_.CreateVector<int64_t>(zero_points));
  } else if (auto qtype =
                 element_type
                     .dyn_cast<mlir::quant::CalibratedQuantizedType>()) {
    std::vector<float> mins = {static_cast<float>(qtype.getMin())};
    std::vector<float> maxs = {static_cast<float>(qtype.getMax())};
    q_params = tflite::CreateQuantizationParameters(
        builder_, builder_.CreateVector<float>(mins),
        builder_.CreateVector<float>(maxs));
  }
  return tflite::CreateTensor(
      builder_, builder_.CreateVector(shape), tflite_element_type,
      /*buffer=*/0, builder_.CreateString(name), q_params,
      /*is_variable=*/false, /*sparsity=*/0, /*shape_signature=*/0,
      /*has_rank=*/tensor_type.hasRank(),
      variant_params->empty() ? 0 : builder_.CreateVector(*variant_params));
}

std::optional<BufferOffset<tflite::Tensor>> Translator::BuildTensor(
    Value value, const std::string& name, unsigned buffer_idx,
    const std::optional<BufferOffset<tflite::QuantizationParameters>>&
        quant_parameters) {
  auto type = value.getType().cast<TensorType>();

  // TFLite requires tensor shape only for the inputs and constants.
  // However, we output all known shapes for better round-tripping
  auto check_shape =
      [&](llvm::ArrayRef<int64_t> shape_ref) -> mlir::LogicalResult {
    auto is_out_of_range = [](int64_t dim) {
      return dim > std::numeric_limits<int32_t>::max();
    };

    if (std::any_of(shape_ref.begin(), shape_ref.end(), is_out_of_range))
      return mlir::emitError(
          value.getLoc(),
          "result shape dimensions out of 32 bit int type range");

    return mlir::success();
  };

  std::vector<int32_t> shape;
  std::vector<int32_t> shape_signature;
  auto* inst = value.getDefiningOp();
  if (type.hasStaticShape()) {
    llvm::ArrayRef<int64_t> shape_ref = type.getShape();
    if (mlir::failed(check_shape(shape_ref))) return std::nullopt;

    shape = std::vector<int32_t>(shape_ref.begin(), shape_ref.end());
  } else if (inst && IsConst(inst)) {
    // Const op can have a result of dynamic shaped type (e.g. due to constant
    // folding), but we can still derive the shape of a constant tensor for
    // its attribute type.
    auto tensor_attr = inst->getAttr("value").cast<mlir::TypedAttr>();
    llvm::ArrayRef<int64_t> shape_ref =
        tensor_attr.getType().cast<TensorType>().getShape();
    if (mlir::failed(check_shape(shape_ref))) return std::nullopt;

    shape = std::vector<int32_t>(shape_ref.begin(), shape_ref.end());
  } else if (type.hasRank()) {
    llvm::ArrayRef<int64_t> shape_ref = type.getShape();
    if (mlir::failed(check_shape(shape_ref))) return std::nullopt;

    shape.reserve(shape_ref.size());
    for (auto& dim : shape_ref) {
      // translate dynamic shapes from mlir to tfl values
      shape.push_back(
          dim == mlir::ShapedType::kDynamic ? 1 : static_cast<int>(dim));
      shape_signature.push_back(static_cast<int>(
          dim == mlir::ShapedType::kDynamic ? tensorflow::kTFDynamicSize
                                            : dim));
    }
  }

  BufferOffset<tflite::SparsityParameters> s_params = 0;
  if (auto* inst = value.getDefiningOp()) {
    if (auto cst = dyn_cast<tfl::SparseConstOp>(inst)) {
      s_params = BuildSparsityParameters(cst.getSParam());
    } else if (auto cst = dyn_cast<tfl::SparseQConstOp>(inst)) {
      s_params = BuildSparsityParameters(cst.getSParam());
    }
  }

  Type element_type = type.getElementType();
  tflite::TensorType tflite_element_type =
      GetTFLiteType(type.getElementType()).value();

  std::optional<std::vector<BufferOffset<tflite::VariantSubType>>>
      variant_params = BuildTFVariantType(element_type);
  if (!variant_params.has_value()) {
    return std::nullopt;
  }

  BufferOffset<tflite::QuantizationParameters> q_params;
  if (auto qtype = element_type.dyn_cast<mlir::quant::UniformQuantizedType>()) {
    std::vector<float> scales = {static_cast<float>(qtype.getScale())};
    std::vector<int64_t> zero_points = {qtype.getZeroPoint()};
    q_params = tflite::CreateQuantizationParameters(
        // min and max values are not stored in the quantized type from MLIR, so
        // both are set to 0 in the flatbuffer when they are exported.
        builder_, /*min=*/0, /*max=*/0, builder_.CreateVector<float>(scales),
        builder_.CreateVector<int64_t>(zero_points));
  } else if (auto qtype =
                 element_type
                     .dyn_cast<mlir::quant::UniformQuantizedPerAxisType>()) {
    std::vector<float> scales(qtype.getScales().begin(),
                              qtype.getScales().end());
    std::vector<int64_t> zero_points(qtype.getZeroPoints().begin(),
                                     qtype.getZeroPoints().end());
    q_params = tflite::CreateQuantizationParameters(
        builder_, /*min=*/0, /*max=*/0, builder_.CreateVector<float>(scales),
        builder_.CreateVector<int64_t>(zero_points),
        tflite::QuantizationDetails_NONE, /*details=*/0,
        qtype.getQuantizedDimension());
  } else if (quant_parameters.has_value()) {
    q_params = quant_parameters.value();
  } else {
    q_params = tflite::CreateQuantizationParameters(builder_);
  }
  // Check if the value's uses includes an op and usage at an operand index
  // marked as a stateful. If so, set the tensor's is_variable as true
  // This is v1 ref variable semantics in the TFLite runtime.
  bool is_variable = false;
  for (auto& use : value.getUses()) {
    is_variable = IsStatefulOperand(use.getOwner(), use.getOperandNumber());
    if (is_variable) {
      break;
    }
  }
  // The value is used as a variable if produced by an op with "tfl.is_variable"
  // attribute. This provides a hook for the user to represent the variable
  // tensor in the MLIR level.
  if (auto* inst = value.getDefiningOp();
      inst && inst->hasAttr("tfl.is_variable")) {
    is_variable = true;
  }

  bool has_rank = type.hasRank();

  if (shape_signature.empty()) {
    return tflite::CreateTensor(
        builder_, builder_.CreateVector(shape), tflite_element_type,
        (is_variable ? 0 : buffer_idx), builder_.CreateString(name), q_params,
        /*is_variable=*/is_variable, s_params, /*shape_signature=*/0,
        /*has_rank=*/has_rank,
        variant_params->empty() ? 0 : builder_.CreateVector(*variant_params));
  } else {
    return tflite::CreateTensor(
        builder_, builder_.CreateVector(shape), tflite_element_type,
        (is_variable ? 0 : buffer_idx), builder_.CreateString(name), q_params,
        /*is_variable=*/is_variable, s_params,
        /*shape_signature=*/builder_.CreateVector(shape_signature),
        /*has_rank=*/has_rank,
        variant_params->empty() ? 0 : builder_.CreateVector(*variant_params));
  }
}

BufferOffset<tflite::Operator> Translator::BuildIfOperator(
    mlir::TF::IfOp op, const std::vector<int32_t>& operands,
    const std::vector<int32_t>& results) {
  auto opcode_index = GetOpcodeIndex("if", tflite::BuiltinOperator_IF);
  int then_subgraph_index = subgraph_index_map_.at(op.getThenBranch().str());
  int else_subgraph_index = subgraph_index_map_.at(op.getElseBranch().str());
  auto builtin_options = tflite::CreateIfOptions(builder_, then_subgraph_index,
                                                 else_subgraph_index)
                             .Union();
  auto inputs = builder_.CreateVector(operands);
  auto outputs = builder_.CreateVector(results);
  return tflite::CreateOperator(builder_, opcode_index, inputs, outputs,
                                tflite::BuiltinOptions_IfOptions,
                                builtin_options);
}

BufferOffset<tflite::Operator> Translator::BuildCallOnceOperator(
    mlir::TFL::CallOnceOp op, const std::vector<int32_t>& operands,
    const std::vector<int32_t>& results) {
  auto opcode_index =
      GetOpcodeIndex("call_once", tflite::BuiltinOperator_CALL_ONCE);
  int init_subgraph_index =
      subgraph_index_map_.at(op.getSessionInitFunction().str());
  auto builtin_options =
      tflite::CreateCallOnceOptions(builder_, init_subgraph_index).Union();
  auto inputs = builder_.CreateVector(operands);
  auto outputs = builder_.CreateVector(results);
  return tflite::CreateOperator(builder_, opcode_index, inputs, outputs,
                                tflite::BuiltinOptions_CallOnceOptions,
                                builtin_options);
}

std::optional<BufferOffset<tflite::Operator>> Translator::BuildWhileOperator(
    mlir::TFL::WhileOp op, const std::vector<int32_t>& operands,
    const std::vector<int32_t>& results) {
  auto opcode_index = GetOpcodeIndex("while", tflite::BuiltinOperator_WHILE);
  auto get_call_index = [&](mlir::Block& b) -> std::optional<int> {
    if (b.getOperations().size() != 2) return std::nullopt;
    if (auto call_op = dyn_cast<mlir::func::CallOp>(b.front()))
      return subgraph_index_map_.at(call_op.getCallee().str());
    return std::nullopt;
  };
  auto body_subgraph_index = get_call_index(op.getBody().front());
  auto cond_subgraph_index = get_call_index(op.getCond().front());
  if (!body_subgraph_index || !cond_subgraph_index)
    return op.emitOpError("only single call cond/body while export supported"),
           std::nullopt;
  auto builtin_options =
      tflite::CreateWhileOptions(builder_, *cond_subgraph_index,
                                 *body_subgraph_index)
          .Union();
  auto inputs = builder_.CreateVector(operands);
  auto outputs = builder_.CreateVector(results);
  return tflite::CreateOperator(builder_, opcode_index, inputs, outputs,
                                tflite::BuiltinOptions_WhileOptions,
                                builtin_options);
}

BufferOffset<tflite::Operator> Translator::BuildNumericVerifyOperator(
    mlir::TFL::NumericVerifyOp op, const std::vector<int32_t>& operands,
    const std::vector<int32_t>& results) {
  float tolerance = op.getTolerance().convertToFloat();
  bool log_if_failed = op.getLogIfFailed();
  auto fbb = std::make_unique<flexbuffers::Builder>();
  fbb->Map([&]() {
    fbb->Float("tolerance", tolerance);
    fbb->Bool("log_if_failed", log_if_failed);
  });
  fbb->Finish();
  auto f = std::unique_ptr<flexbuffers::Builder>(fbb.release());
  auto custom_option = f->GetBuffer();
  auto opcode_index =
      GetOpcodeIndex("NumericVerify", tflite::BuiltinOperator_CUSTOM);

  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE,
      /*builtin_options=*/0, builder_.CreateVector<uint8_t>(custom_option),
      tflite::CustomOptionsFormat_FLEXBUFFERS);
}

BufferOffset<tflite::Operator> Translator::BuildCustomOperator(
    Operation* inst, mlir::TFL::CustomOp op,
    const std::vector<int32_t>& operands, const std::vector<int32_t>& results) {
  const std::string attrs =
      op.getCustomOption().cast<mlir::TFL::ConstBytesAttr>().getValue().str();
  std::vector<uint8_t> custom_option_vector(attrs.size(), 0);
  memcpy(custom_option_vector.data(), attrs.data(), attrs.size());
  auto opcode_index =
      GetOpcodeIndex(op.getCustomCode().str(), tflite::BuiltinOperator_CUSTOM);
  if (use_buffer_offset_) {
    custom_op_data_map_[std::make_pair(operands, results)] =
        custom_option_vector;
    return tflite::CreateOperator(
        builder_, opcode_index, builder_.CreateVector(operands),
        builder_.CreateVector(results), tflite::BuiltinOptions_NONE,
        /*builtin_options=*/0, 0, tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0,
        1, 1);
  }
  if (IsModelBiggerThan2GB(custom_option_vector.size())) {
    require_use_buffer_offset_ = true;
    return tflite::CreateOperator(
        builder_, opcode_index, builder_.CreateVector(operands),
        builder_.CreateVector(results), tflite::BuiltinOptions_NONE,
        /*builtin_options=*/0,
        /*custom_options=*/0, tflite::CustomOptionsFormat_FLEXBUFFERS);
  }
  if (custom_option_alignment_.has_value()) {
    builder_.ForceVectorAlignment(custom_option_vector.size(), sizeof(uint8_t),
                                  custom_option_alignment_.value());
  }
  auto custom_option_fbs_vector =
      builder_.CreateVector<uint8_t>(custom_option_vector);
  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE,
      /*builtin_options=*/0, custom_option_fbs_vector,
      tflite::CustomOptionsFormat_FLEXBUFFERS);
}

std::optional<CustomOptionsOffset> Translator::CreateFlexOpCustomOptions(
    const ::tensorflow::NodeDef& node_def, const mlir::Location& loc) {
  std::string node_def_str;
  if (!node_def.SerializeToString(&node_def_str)) {
    return emitError(loc, "failed to serialize tensorflow node_def"),
           std::nullopt;
  }

  auto flex_builder = std::make_unique<flexbuffers::Builder>();
  flex_builder->Vector([&]() {
    flex_builder->String(node_def.op());
    flex_builder->String(node_def_str);
  });
  flex_builder->Finish();
  if (IsModelBiggerThan2GB(flex_builder->GetSize()) && !use_buffer_offset_) {
    require_use_buffer_offset_ = true;
    return builder_.CreateVector({});
  }
  return builder_.CreateVector(flex_builder->GetBuffer());
}

std::optional<CustomOptionsOffset> Translator::CreateCustomOpCustomOptions(
    const ::tensorflow::NodeDef& node_def, const mlir::Location& loc) {
  auto flex_builder = CreateFlexBuilderWithNodeAttrs(node_def, loc);
  return builder_.CreateVector(flex_builder->GetBuffer());
}

std::unique_ptr<flexbuffers::Builder>
Translator::CreateFlexBuilderWithNodeAttrs(
    const ::tensorflow::NodeDef& node_def, const mlir::Location& loc) {
  auto flex_builder = std::make_unique<flexbuffers::Builder>();
  size_t map_start = flex_builder->StartMap();
  using Item = std::pair<std::string, ::tensorflow::AttrValue>;
  std::vector<Item> attrs(node_def.attr().begin(), node_def.attr().end());
  std::sort(attrs.begin(), attrs.end(),
            [](Item& p1, Item& p2) -> bool { return p1.first < p2.first; });
  for (const Item& pair : attrs) {
    const char* key = pair.first.c_str();
    const ::tensorflow::AttrValue& attr = pair.second;
    switch (attr.value_case()) {
      case ::tensorflow::AttrValue::kS:
        flex_builder->String(key, attr.s());
        break;
      case ::tensorflow::AttrValue::kType: {
        auto status_or_tfl_type = tflite::TfTypeToTflType(attr.type());
        if (status_or_tfl_type.ok()) {
          flex_builder->Int(key, status_or_tfl_type.value());
        } else {
          emitWarning(loc, "ignoring unsupported tensorflow type: ")
              << std::to_string(attr.type());
        }
        break;
      }
      case ::tensorflow::AttrValue::kI:
        flex_builder->Int(key, attr.i());
        break;
      case ::tensorflow::AttrValue::kF:
        flex_builder->Float(key, attr.f());
        break;
      case ::tensorflow::AttrValue::kB:
        flex_builder->Bool(key, attr.b());
        break;
      case tensorflow::AttrValue::kList:
        if (attr.list().s_size() > 0) {
          auto start = flex_builder->StartVector(key);
          for (const std::string& v : attr.list().s()) {
            flex_builder->Add(v);
          }
          flex_builder->EndVector(start, /*typed=*/true, /*fixed=*/false);
        } else if (attr.list().i_size() > 0) {
          auto start = flex_builder->StartVector(key);
          for (const int64_t v : attr.list().i()) {
            flex_builder->Add(v);
          }
          flex_builder->EndVector(start, /*typed=*/true, /*fixed=*/false);
        } else if (attr.list().f_size() > 0) {
          auto start = flex_builder->StartVector(key);
          for (const float v : attr.list().f()) {
            flex_builder->Add(v);
          }
          flex_builder->EndVector(start, /*typed=*/true, /*fixed=*/false);
        } else {
          emitWarning(loc,
                      "ignoring unsupported type in list attribute with key: ")
              << key;
        }
        break;
      default:
        emitWarning(loc, "ignoring unsupported attribute type with key: ")
            << key;
        break;
    }
  }
  flex_builder->EndMap(map_start);
  flex_builder->Finish();
  return flex_builder;
}

uint32_t Translator::GetOpcodeIndex(const std::string& op_name,
                                    tflite::BuiltinOperator builtin) {
  auto it = opcode_index_map_.insert({op_name, 0});

  // If the insert succeeded, the opcode has not been created already. Create a
  // new operator code and update its index value in the map.
  if (it.second) {
    it.first->second = opcodes_.size();
    auto custom_code = builtin == tflite::BuiltinOperator_CUSTOM
                           ? builder_.CreateString(op_name)
                           : BufferOffset<flatbuffers::String>();
    // Use version 0 for builtin op. This is a way to serialize version field to
    // flatbuffer (since 0 is non default) and it will be corrected later.
    int32_t op_version = builtin != tflite::BuiltinOperator_CUSTOM ? 0 : 1;
    opcodes_.push_back(CreateOperatorCode(builder_, /*builtin_code=*/builtin,
                                          custom_code, op_version));
  }
  return it.first->second;
}

std::optional<BufferOffset<tflite::Operator>>
Translator::BuildStablehloOperatorwithoutOptions(
    Operation* inst, const std::vector<int32_t>& operands,
    const std::vector<int32_t>& results,
    const tflite::BuiltinOperator op_code) {
  std::string op_name = inst->getName().getStringRef().str();
  uint32_t opcode_index = GetOpcodeIndex(op_name, op_code);

  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0);
}

BufferOffset<flatbuffers::Vector<unsigned int>>
Translator::BuildStablehloPrecisionConfig(::mlir::ArrayAttr precisionConfig) {
  std::vector<uint32_t> precision_config_vec;

  for (auto it = precisionConfig.begin(); it != precisionConfig.end(); it++) {
    precision_config_vec.push_back(static_cast<uint32_t>(
        (it->cast<mlir::stablehlo::PrecisionAttr>()).getValue()));
  }
  return builder_.CreateVector(precision_config_vec);
}

std::optional<BufferOffset<tflite::Operator>>
Translator::BuildStablehloGatherOp(mlir::stablehlo::GatherOp gather_op,
                                   const std::vector<int32_t>& operands,
                                   const std::vector<int32_t>& results) {
  std::string op_name =
      gather_op.getOperation()->getName().getStringRef().str();
  uint32_t opcode_index =
      GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_GATHER);

  std::vector<int64_t> offset_dims_vec(
      gather_op.getDimensionNumbers().getOffsetDims().begin(),
      gather_op.getDimensionNumbers().getOffsetDims().end());
  std::vector<int64_t> collapsed_slice_dims_vec(
      gather_op.getDimensionNumbers().getCollapsedSliceDims().begin(),
      gather_op.getDimensionNumbers().getCollapsedSliceDims().end());
  std::vector<int64_t> start_index_map_vec(
      gather_op.getDimensionNumbers().getStartIndexMap().begin(),
      gather_op.getDimensionNumbers().getStartIndexMap().end());

  auto offset_dims = builder_.CreateVector(offset_dims_vec);
  auto collapsed_slice_dims = builder_.CreateVector(collapsed_slice_dims_vec);
  auto start_index_map = builder_.CreateVector(start_index_map_vec);
  auto slice_sizes = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(gather_op.getSliceSizes()));

  auto gather_option = tflite::CreateStablehloGatherOptions(
      builder_, offset_dims, collapsed_slice_dims, start_index_map,
      gather_op.getDimensionNumbers().getIndexVectorDim(), slice_sizes,
      gather_op.getIndicesAreSorted());

  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
      tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
      tflite::BuiltinOptions2_StablehloGatherOptions, gather_option.Union());
}

std::optional<BufferOffset<tflite::Operator>>
Translator::BuildStablehloScatterOp(mlir::stablehlo::ScatterOp scatter_op,
                                    const std::vector<int32_t>& operands,
                                    const std::vector<int32_t>& results) {
  std::string op_name =
      scatter_op.getOperation()->getName().getStringRef().str();
  uint32_t opcode_index =
      GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_SCATTER);

  Region& body = scatter_op.getUpdateComputation();
  int32_t subgraph_index =
      UnnamedRegionToSubgraph(&body, tflite::BuiltinOperator_STABLEHLO_SCATTER);
  if (subgraph_index < 0) return std::nullopt;

  mlir::stablehlo::ScatterDimensionNumbersAttr scatter_dimension_numbers =
      scatter_op.getScatterDimensionNumbers();
  llvm::ArrayRef<int64_t> update_window_dims_mlir =
      scatter_dimension_numbers.getUpdateWindowDims();
  llvm::ArrayRef<int64_t> inserted_window_dims_mlir =
      scatter_dimension_numbers.getInsertedWindowDims();
  llvm::ArrayRef<int64_t> scatter_dims_to_operand_dims_mlir =
      scatter_dimension_numbers.getScatterDimsToOperandDims();

  std::vector<int64_t> update_window_dims_vec(update_window_dims_mlir.begin(),
                                              update_window_dims_mlir.end());
  std::vector<int64_t> inserted_window_dims_vec(
      inserted_window_dims_mlir.begin(), inserted_window_dims_mlir.end());
  std::vector<int64_t> scatter_dims_to_operand_dims_vec(
      scatter_dims_to_operand_dims_mlir.begin(),
      scatter_dims_to_operand_dims_mlir.end());

  int64_t index_vector_dim = scatter_dimension_numbers.getIndexVectorDim();
  bool unique_indices = scatter_op.getUniqueIndices();
  bool indices_are_sorted = scatter_op.getIndicesAreSorted();

  auto update_window_dims = builder_.CreateVector(update_window_dims_vec);
  auto inserted_window_dims = builder_.CreateVector(inserted_window_dims_vec);
  auto scatter_dims_to_operand_dims =
      builder_.CreateVector(scatter_dims_to_operand_dims_vec);

  auto options = tflite::CreateStablehloScatterOptions(
      builder_, indices_are_sorted, update_window_dims, inserted_window_dims,
      scatter_dims_to_operand_dims, index_vector_dim, unique_indices,
      subgraph_index);

  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
      tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
      tflite::BuiltinOptions2_StablehloScatterOptions, options.Union());
}

std::optional<BufferOffset<tflite::Operator>>
Translator::BuildStablehloReduceWindowOp(
    mlir::stablehlo::ReduceWindowOp reduce_window_op,
    const std::vector<int32_t>& operands, const std::vector<int32_t>& results) {
  std::string op_name =
      reduce_window_op.getOperation()->getName().getStringRef().str();
  uint32_t opcode_index =
      GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_REDUCE_WINDOW);

  auto window_dimensions = builder_.CreateVector(
      mlir::GetVector<int64_t>(reduce_window_op.getWindowDimensions()));
  auto window_strides = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(reduce_window_op.getWindowStrides()));
  auto base_dilations = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(reduce_window_op.getBaseDilations()));
  auto window_dilations = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(reduce_window_op.getWindowDilations()));
  auto padding = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(reduce_window_op.getPadding()));

  auto& body = reduce_window_op.getBody();
  int32_t subgraph_index = UnnamedRegionToSubgraph(
      &body, tflite::BuiltinOperator_STABLEHLO_REDUCE_WINDOW);
  if (subgraph_index < 0) return std::nullopt;

  auto reduce_window_option = tflite::CreateStablehloReduceWindowOptions(
      builder_, window_dimensions, window_strides, base_dilations,
      window_dilations, padding, subgraph_index);

  return tflite::CreateOperator(
      builder_, opcode_index, /*inputs=*/builder_.CreateVector(operands),
      /*outputs=*/builder_.CreateVector(results), tflite::BuiltinOptions_NONE,
      /*builtin_options=*/0, /*custom_options=*/0,
      tflite::CustomOptionsFormat_FLEXBUFFERS, /*mutating_variable_inputs=*/0,
      /*intermediates=*/0, /*large_custom_options_offset=*/0,
      /*large_custom_options_size=*/0,
      tflite::BuiltinOptions2_StablehloReduceWindowOptions,
      reduce_window_option.Union());
}

std::optional<BufferOffset<tflite::Operator>>
Translator::BuildStablehloRngBitGeneratorOp(
    mlir::stablehlo::RngBitGeneratorOp rng_op,
    const std::vector<int32_t>& operands, const std::vector<int32_t>& results) {
  std::string op_name = rng_op.getOperation()->getName().getStringRef().str();
  uint32_t opcode_index = GetOpcodeIndex(
      op_name, tflite::BuiltinOperator_STABLEHLO_RNG_BIT_GENERATOR);
  tflite::RngAlgorithm algorithm = tflite::RngAlgorithm_DEFAULT;
  switch (rng_op.getRngAlgorithm()) {
    case mlir::stablehlo::RngAlgorithm::THREE_FRY:
      algorithm = tflite::RngAlgorithm_THREEFRY;
      break;
    case mlir::stablehlo::RngAlgorithm::PHILOX:
      algorithm = tflite::RngAlgorithm_PHILOX;
      break;
    case mlir::stablehlo::RngAlgorithm::DEFAULT:
      break;
  }
  auto rng_options =
      tflite::CreateStablehloRngBitGeneratorOptions(builder_, algorithm);
  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
      tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
      tflite::BuiltinOptions2_StablehloRngBitGeneratorOptions,
      rng_options.Union());
}

std::optional<BufferOffset<tflite::Operator>> Translator::BuildStablehloPadOp(
    mlir::stablehlo::PadOp pad_op, const std::vector<int32_t>& operands,
    const std::vector<int32_t>& results) {
  std::string op_name = pad_op->getName().getStringRef().str();
  uint32_t opcode_index =
      GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_PAD);

  auto edge_padding_low = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(pad_op.getEdgePaddingLowAttr()));
  auto edge_padding_high = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(pad_op.getEdgePaddingHighAttr()));
  auto interior_padding = builder_.CreateVector(
      mlir::GetOptionalVector<int64_t>(pad_op.getInteriorPaddingAttr()));

  auto pad_option = tflite::CreateStablehloPadOptions(
      builder_, edge_padding_low, edge_padding_high, interior_padding);

  return tflite::CreateOperator(
      builder_, opcode_index, builder_.CreateVector(operands),
      builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
      tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
      tflite::BuiltinOptions2_StablehloPadOptions, pad_option.Union());
}

std::optional<BufferOffset<tflite::Operator>> Translator::BuildOperator(
    Operation* inst, std::vector<int32_t> operands,
    const std::vector<int32_t>& results,
    const std::vector<int32_t>& intermediates) {
  const auto* dialect = inst->getDialect();
  if (!dialect) {
    inst->emitOpError("dialect is not registered");
    return std::nullopt;
  }

  // If TFLite built in op, create operator as a builtin op.
  if (dialect == tfl_dialect_) {
    // Only if built-in TFLite op emission is enabled, would legalization have
    // converted any TF->TFL.
    if (!enabled_op_types_.contains(OpType::kTfliteBuiltin)) {
      return inst->emitOpError(
                 "is a TFLite builtin op but builtin emission is not enabled"),
             std::nullopt;
    }

    auto builtin_code = GetBuiltinOpCode(inst);
    if (!builtin_code) {
      if (auto verify_op = dyn_cast<mlir::TFL::NumericVerifyOp>(inst)) {
        return BuildNumericVerifyOperator(verify_op, operands, results);
      }
      if (auto custom_op = dyn_cast<mlir::TFL::CustomOp>(inst)) {
        return BuildCustomOperator(inst, custom_op, operands, results);
      }
      if (auto whileOp = dyn_cast<mlir::TFL::WhileOp>(inst)) {
        if (inst->getNumOperands() != inst->getNumResults()) {
          inst->emitOpError(
              "number of operands and results don't match, only canonical "
              "TFL While supported");
          return std::nullopt;
        }
        return BuildWhileOperator(whileOp, operands, results);
      }

      inst->emitOpError("is not a supported TFLite op");
      return std::nullopt;
    }

    if (*builtin_code == tflite::BuiltinOperator_CALL_ONCE) {
      if (auto initOp = dyn_cast<mlir::TFL::CallOnceOp>(inst)) {
        return BuildCallOnceOperator(initOp, operands, results);
      }
    }

    std::string op_name = inst->getName().getStringRef().str();
    uint32_t opcode_index = GetOpcodeIndex(op_name, *builtin_code);

    // If this is TransposeConv we need to do a special case of ignoring the
    // optional tensor, to allow newly created models to run on old runtimes.
    if (*builtin_code == tflite::BuiltinOperator_TRANSPOSE_CONV) {
      if (operands.size() == 4 && operands.at(3) == -1) {
        operands.pop_back();
      }
    }

    auto offset = CreateFlatBufferOperator(inst, opcode_index, operands,
                                           results, intermediates, &builder_);
    if (!offset) {
      inst->emitOpError("is not a supported TFLite op");
    }
    return offset;
  }

  // EXPERIMENTAL: If the source is in stablehlo dialect, also create them as
  // builtin ops
  if (dialect == stablehlo_dialect_) {
    // for stablehlo ops with kernels, we directly serialize them whenever
    // possible
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ScatterOp>(inst)) {
      return BuildStablehloScatterOp(shlo_op, operands, results);
    }
    if (auto shlo_op =
            llvm::dyn_cast<mlir::stablehlo::RngBitGeneratorOp>(inst)) {
      return BuildStablehloRngBitGeneratorOp(shlo_op, operands, results);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::GatherOp>(inst)) {
      return BuildStablehloGatherOp(shlo_op, operands, results);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::AddOp>(inst)) {
      return BuildStablehloOperatorwithoutOptions(
          inst, operands, results, tflite::BuiltinOperator_STABLEHLO_ADD);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::MulOp>(inst)) {
      return BuildStablehloOperatorwithoutOptions(
          inst, operands, results, tflite::BuiltinOperator_STABLEHLO_MULTIPLY);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ReduceWindowOp>(inst)) {
      return BuildStablehloReduceWindowOp(shlo_op, operands, results);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::MaxOp>(inst)) {
      return BuildStablehloOperatorwithoutOptions(
          inst, operands, results, tflite::BuiltinOperator_STABLEHLO_MAXIMUM);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::MinOp>(inst)) {
      return BuildStablehloOperatorwithoutOptions(
          inst, operands, results, tflite::BuiltinOperator_STABLEHLO_MINIMUM);
    }
    if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::PadOp>(inst)) {
      return BuildStablehloPadOp(shlo_op, operands, results);
    }
    // for ops don't have kernels, only serialize when conversion is set to true
    if (convert_stablehlo_) {
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::LogisticOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results,
            tflite::BuiltinOperator_STABLEHLO_LOGISTIC);
      }

      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::DivOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_DIVIDE);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ReshapeOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_RESHAPE);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ClampOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_CLAMP);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::AbsOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_ABS);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::AddOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_ADD);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::AndOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_AND);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::CosineOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_COSINE);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ExpOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results,
            tflite::BuiltinOperator_STABLEHLO_EXPONENTIAL);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::FloorOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_FLOOR);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::LogOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_LOG);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::NegOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_NEGATE);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::OrOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_OR);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::PowOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_POWER);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::RemOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results,
            tflite::BuiltinOperator_STABLEHLO_REMAINDER);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::RsqrtOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_RSQRT);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::SelectOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_SELECT);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::SubtractOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results,
            tflite::BuiltinOperator_STABLEHLO_SUBTRACT);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::TanhOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_TANH);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ConvertOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results, tflite::BuiltinOperator_STABLEHLO_CONVERT);
      }
      if (auto shlo_op =
              llvm::dyn_cast<mlir::stablehlo::DynamicUpdateSliceOp>(inst)) {
        return BuildStablehloOperatorwithoutOptions(
            inst, operands, results,
            tflite::BuiltinOperator_STABLEHLO_DYNAMIC_UPDATE_SLICE);
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::IotaOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index =
            GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_IOTA);

        auto iota_option = tflite::CreateStablehloIotaOptions(
            builder_, shlo_op.getIotaDimension());

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloIotaOptions, iota_option.Union());
      }
      if (auto shlo_op =
              llvm::dyn_cast<mlir::stablehlo::DynamicSliceOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_DYNAMIC_SLICE);

        auto slice_sizes = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getSliceSizes()));

        auto dynamic_slice_option =
            tflite::CreateStablehloDynamicSliceOptions(builder_, slice_sizes);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloDynamicSliceOptions,
            dynamic_slice_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::CompareOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index =
            GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_COMPARE);

        auto compare_type_attr = shlo_op.getCompareType();
        tflite::StablehloComparisonType compare_type =
            tflite::StablehloComparisonType_STABLEHLO_COMPARISON_TYPE_NOTYPE;
        if (compare_type_attr)
          compare_type = static_cast<tflite::StablehloComparisonType>(
              compare_type_attr.value());
        auto compare_option = tflite::CreateStablehloCompareOptions(
            builder_,
            static_cast<tflite::StablehloComparisonDirection>(
                shlo_op.getComparisonDirection()),
            compare_type);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloCompareOptions,
            compare_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ConcatenateOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_CONCATENATE);

        auto concat_option = tflite::CreateStablehloConcatenateOptions(
            builder_, shlo_op.getDimension());

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloConcatenateOptions,
            concat_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::SliceOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index =
            GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_SLICE);

        auto start_indices = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getStartIndicesAttr()));
        auto limit_indices = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getLimitIndicesAttr()));
        auto strides = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getStridesAttr()));

        auto slice_option = tflite::CreateStablehloSliceOptions(
            builder_, start_indices, limit_indices, strides);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloSliceOptions,
            slice_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ConvolutionOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_CONVOLUTION);

        auto window_strides = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getWindowStrides()));
        auto padding = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getPadding()));
        auto lhs_dialation = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getLhsDilation()));
        auto rhs_dialation = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getRhsDilation()));
        auto window_reversal = builder_.CreateVector(
            mlir::GetOptionalVector<bool>(shlo_op.getWindowReversal()));
        auto input_batch_dimension =
            shlo_op.getDimensionNumbersAttr().getInputBatchDimension();
        auto input_feature_dimension =
            shlo_op.getDimensionNumbersAttr().getInputFeatureDimension();
        auto kernel_input_feature_dimension =
            shlo_op.getDimensionNumbersAttr().getKernelInputFeatureDimension();
        auto kernel_output_feature_dimension =
            shlo_op.getDimensionNumbersAttr().getKernelOutputFeatureDimension();
        auto output_batch_dimension =
            shlo_op.getDimensionNumbersAttr().getOutputBatchDimension();
        auto output_feature_dimension =
            shlo_op.getDimensionNumbersAttr().getOutputFeatureDimension();
        std::vector<int64_t> input_spatial_dimension_vec(
            shlo_op.getDimensionNumbersAttr()
                .getInputSpatialDimensions()
                .begin(),
            shlo_op.getDimensionNumbersAttr()
                .getInputSpatialDimensions()
                .end());
        std::vector<int64_t> kernel_spatial_dimension_vec(
            shlo_op.getDimensionNumbersAttr()
                .getKernelSpatialDimensions()
                .begin(),
            shlo_op.getDimensionNumbersAttr()
                .getKernelSpatialDimensions()
                .end());
        std::vector<int64_t> output_spatial_dimension_vec(
            shlo_op.getDimensionNumbersAttr()
                .getOutputSpatialDimensions()
                .begin(),
            shlo_op.getDimensionNumbersAttr()
                .getOutputSpatialDimensions()
                .end());
        auto kernel_spatial_dimensions =
            builder_.CreateVector(kernel_spatial_dimension_vec);
        auto output_spatial_dimension =
            builder_.CreateVector(output_spatial_dimension_vec);
        auto input_spatial_dimension =
            builder_.CreateVector(input_spatial_dimension_vec);
        BufferOffset<flatbuffers::Vector<unsigned int>> precision_config = 0;
        if (shlo_op.getPrecisionConfig()) {
          precision_config = BuildStablehloPrecisionConfig(
              shlo_op.getPrecisionConfig().value());
        }

        auto convolution_option = tflite::CreateStablehloConvolutionOptions(
            builder_, window_strides, padding, lhs_dialation, rhs_dialation,
            window_reversal, input_batch_dimension, input_feature_dimension,
            input_spatial_dimension, kernel_input_feature_dimension,
            kernel_output_feature_dimension, kernel_spatial_dimensions,
            output_batch_dimension, output_feature_dimension,
            output_spatial_dimension, shlo_op.getFeatureGroupCount(),
            shlo_op.getBatchGroupCount(), precision_config);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloConvolutionOptions,
            convolution_option.Union());
      }
      if (auto shlo_op =
              llvm::dyn_cast<mlir::stablehlo::BroadcastInDimOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_BROADCAST_IN_DIM);

        auto broadcast_dimensions =
            builder_.CreateVector(mlir::GetOptionalVector<int64_t>(
                shlo_op.getBroadcastDimensionsAttr()));

        auto broadcast_option = tflite::CreateStablehloBroadcastInDimOptions(
            builder_, broadcast_dimensions);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloBroadcastInDimOptions,
            broadcast_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::CustomCallOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_CUSTOM_CALL);
        auto call_target_name =
            builder_.CreateString(shlo_op.getCallTargetName().str());
        auto backend_config =
            builder_.CreateString(shlo_op.getBackendConfig().str());
        // building the computation info
        auto flex_builder = std::make_unique<flexbuffers::Builder>();
        size_t map_start = flex_builder->StartMap();
        auto attrs = shlo_op->getAttrs();
        std::vector<mlir::NamedAttribute> extra_attributes;

        for (size_t i = 0; i < attrs.size(); ++i) {
          auto name = attrs[i].getName().str();
          auto attr = attrs[i].getValue();
          if (name == "call_target_name" || name == "backend_config") continue;
          if (llvm::isa<mlir::BoolAttr>(attr))
            flex_builder->Bool(name.c_str(),
                               attr.cast<mlir::BoolAttr>().getValue());
          if (llvm::isa<mlir::StringAttr>(attr))
            flex_builder->String(
                name.c_str(), attr.cast<mlir::StringAttr>().getValue().str());
        }
        flex_builder->EndMap(map_start);
        flex_builder->Finish();
        auto custom_call_option = tflite::CreateStablehloCustomCallOptions(
            builder_, call_target_name, shlo_op.getHasSideEffect(),
            backend_config, 0, 0,
            builder_.CreateVector(flex_builder->GetBuffer()));

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloCustomCallOptions,
            custom_call_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::ReduceOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index =
            GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_REDUCE);

        auto dimension = builder_.CreateVector(
            mlir::GetOptionalVector<int64_t>(shlo_op.getDimensions()));
        auto& body = shlo_op.getBody();
        int32_t subgraph_index = UnnamedRegionToSubgraph(
            &body, tflite::BuiltinOperator_STABLEHLO_REDUCE);
        if (subgraph_index < 0) return std::nullopt;

        auto reduce_option = tflite::CreateStablehloReduceOptions(
            builder_, dimension, subgraph_index);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloReduceOptions,
            reduce_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::DotGeneralOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_DOT_GENERAL);

        std::vector<int64_t> lhs_batching_dimensions_vec(
            shlo_op.getDotDimensionNumbers().getLhsBatchingDimensions().begin(),
            shlo_op.getDotDimensionNumbers().getLhsBatchingDimensions().end());
        std::vector<int64_t> rhs_batching_dimensions_vec(
            shlo_op.getDotDimensionNumbers().getRhsBatchingDimensions().begin(),
            shlo_op.getDotDimensionNumbers().getRhsBatchingDimensions().end());
        std::vector<int64_t> lhs_contracting_dimensions_vec(
            shlo_op.getDotDimensionNumbers()
                .getLhsContractingDimensions()
                .begin(),
            shlo_op.getDotDimensionNumbers()
                .getLhsContractingDimensions()
                .end());
        std::vector<int64_t> rhs_contracting_dimensions_vec(
            shlo_op.getDotDimensionNumbers()
                .getRhsContractingDimensions()
                .begin(),
            shlo_op.getDotDimensionNumbers()
                .getRhsContractingDimensions()
                .end());

        auto lhs_batching_dimensions =
            builder_.CreateVector(lhs_batching_dimensions_vec);
        auto rhs_batching_dimensions =
            builder_.CreateVector(rhs_batching_dimensions_vec);
        auto lhs_contracting_dimensions =
            builder_.CreateVector(lhs_contracting_dimensions_vec);
        auto rhs_contracting_dimensions =
            builder_.CreateVector(rhs_contracting_dimensions_vec);

        BufferOffset<flatbuffers::Vector<unsigned int>> precision_config = 0;
        if (shlo_op.getPrecisionConfig()) {
          precision_config = BuildStablehloPrecisionConfig(
              shlo_op.getPrecisionConfig().value());
        }

        auto dot_geneoral_option = tflite::CreateStablehloDotGeneralOptions(
            builder_, lhs_batching_dimensions, rhs_batching_dimensions,
            lhs_contracting_dimensions, rhs_contracting_dimensions,
            precision_config);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloDotGeneralOptions,
            dot_geneoral_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::SortOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index =
            GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_SORT);
        auto& comparator = shlo_op.getComparator();
        int32_t comparator_subgraph_index = UnnamedRegionToSubgraph(
            &comparator, tflite::BuiltinOperator_STABLEHLO_SORT);
        if (comparator_subgraph_index < 0) return std::nullopt;

        auto sort_option = tflite::CreateStablehloSortOptions(
            builder_, shlo_op.getDimension(), shlo_op.getIsStable(),
            comparator_subgraph_index);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloSortOptions, sort_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::WhileOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index =
            GetOpcodeIndex(op_name, tflite::BuiltinOperator_STABLEHLO_WHILE);

        auto& cond = shlo_op.getCond();
        int32_t cond_subgraph_index = UnnamedRegionToSubgraph(
            &cond, tflite::BuiltinOperator_STABLEHLO_WHILE);
        if (cond_subgraph_index < 0) return std::nullopt;

        auto& body = shlo_op.getBody();
        int32_t body_subgraph_index = UnnamedRegionToSubgraph(
            &body, tflite::BuiltinOperator_STABLEHLO_WHILE);
        if (body_subgraph_index < 0) return std::nullopt;

        auto while_option = tflite::CreateStablehloWhileOptions(
            builder_, cond_subgraph_index, body_subgraph_index);

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloWhileOptions,
            while_option.Union());
      }
      if (auto shlo_op = llvm::dyn_cast<mlir::stablehlo::TransposeOp>(inst)) {
        std::string op_name = inst->getName().getStringRef().str();
        uint32_t opcode_index = GetOpcodeIndex(
            op_name, tflite::BuiltinOperator_STABLEHLO_TRANSPOSE);

        auto transpose_option = tflite::CreateStablehloTransposeOptions(
            builder_, builder_.CreateVector(mlir::GetOptionalVector<int64_t>(
                          shlo_op.getPermutation())));

        return tflite::CreateOperator(
            builder_, opcode_index, builder_.CreateVector(operands),
            builder_.CreateVector(results), tflite::BuiltinOptions_NONE, 0, 0,
            tflite::CustomOptionsFormat_FLEXBUFFERS, 0, 0, 0, 0,
            tflite::BuiltinOptions2_StablehloTransposeOptions,
            transpose_option.Union());
      }
    }

    return inst->emitOpError("is not part of the stablehlo support yet."),
           std::nullopt;
  }

  if (dialect == tf_dialect_) {
    if (auto ifOp = dyn_cast<mlir::TF::IfOp>(inst)) {
      return BuildIfOperator(ifOp, operands, results);
    }

    CustomOptionsOffset custom_options;

    // Ops in TF dialect can either be custom ops or flex ops.
    // The reason we go directly from TensorFlow dialect MLIR to tensorflow
    // node instead of going to TF table gen'd ops via generated code is that
    // we do not want to restrict custom and flex op conversion support to
    // only those TF ops that are currently registered in MLIR. The current
    // model is of an open op system.
    //
    //  The following algorithm is followed:
    //   if flex is enabled and the op is allowlisted as flex
    //     we emit op as flex.
    //   if custom is enabled
    //    we emit the op as custom.
    auto node_def = GetTensorFlowNodeDef(inst);
    if (!node_def) {
      return std::nullopt;
    }

    std::string op_name = node_def->op();
    std::string op_desc = GetOpDescriptionForDebug(inst);

    if (IsTFResourceOp(inst)) {
      resource_ops_[op_name].insert(op_desc);
    }

    const bool is_allowed_flex_op =
        !IsUnsupportedFlexOp(node_def->op()) &&
        (IsAllowlistedFlexOp(node_def->op()) ||
         (((select_user_tf_ops_.count(node_def->op()) != 0) ||
           allow_all_select_tf_ops_) &&
          (tensorflow::OpRegistry::Global()->LookUp(node_def->op()) !=
           nullptr)));

    // Flex op case
    // Eventually, the allowlist will go away and we will rely on some TF op
    // trait (e.g. No side effect) to determine if it is a supported "Flex"
    // op or not.
    if (is_allowed_flex_op && enabled_op_types_.contains(OpType::kSelectTf)) {
      // Construct ops as flex op encoding TensorFlow node definition
      // as custom options.
      // Flex ops are named with the kFlexOpNamePrefix prefix to the actual
      // TF op name.
      op_name = std::string(kFlexOpNamePrefix) + node_def->op();
      if (auto options = CreateFlexOpCustomOptions(*node_def, inst->getLoc())) {
        custom_options = *options;
      } else {
        return std::nullopt;
      }

      // Gather flex ops.
      flex_ops_[op_name].insert(op_desc);
    } else if (enabled_op_types_.contains(OpType::kCustomOp)) {
      // Generic case of custom ops - write using flex buffers since that
      // is the only custom options supported by TFLite today.
      op_name = node_def->op();
      if (auto options =
              CreateCustomOpCustomOptions(*node_def, inst->getLoc())) {
        custom_options = *options;
      } else {
        return std::nullopt;
      }

      // Gather custom ops.
      custom_ops_[op_name].insert(op_desc);
    } else {
      // Insert failed op to `flex_ops` or `custom_ops`.
      if (is_allowed_flex_op) {
        failed_flex_ops_[op_name].insert(op_desc);
        tfl::AttachErrorCode(
            inst->emitOpError("is neither a custom op nor a flex op"),
            tflite::metrics::ConverterErrorData::ERROR_NEEDS_FLEX_OPS);
      } else {
        failed_custom_ops_[op_name].insert(op_desc);
        tfl::AttachErrorCode(
            inst->emitOpError("is neither a custom op nor a flex op"),
            tflite::metrics::ConverterErrorData::ERROR_NEEDS_CUSTOM_OPS);
      }
      return std::nullopt;
    }

    uint32_t opcode_index =
        GetOpcodeIndex(op_name, tflite::BuiltinOperator_CUSTOM);
    auto inputs = builder_.CreateVector(operands);
    auto outputs = builder_.CreateVector(results);

    return tflite::CreateOperator(builder_, opcode_index, inputs, outputs,
                                  tflite::BuiltinOptions_NONE,
                                  /*builtin_options=*/0,
                                  /*custom_options=*/custom_options,
                                  tflite::CustomOptionsFormat_FLEXBUFFERS,
                                  /*mutating_variable_inputs=*/0);
  }

  return inst->emitOpError(
             "is not any of a builtin TFLite op, a flex TensorFlow op or a "
             "custom TensorFlow op"),
         std::nullopt;
}

void Translator::InitializeNamesFromAttribute(FuncOp fn, bool* has_input_attr) {
  auto dict_attr = fn->getAttrOfType<mlir::DictionaryAttr>("tf.entry_function");
  if (!dict_attr) return;

  llvm::SmallVector<llvm::StringRef, 2> input_names;
  llvm::SmallVector<llvm::StringRef, 2> output_names;
  if (auto str = dict_attr.get("inputs").dyn_cast_or_null<mlir::StringAttr>()) {
    str.getValue().split(input_names, ',', /*MaxSplit=*/-1,
                         /*KeepEmpty=*/false);
    if (input_names.size() != fn.getNumArguments()) {
      fn.emitWarning() << "invalid entry function specification";
      return;
    }
    for (const auto& it : llvm::enumerate(fn.getArguments())) {
      name_mapper_.InitOpName(it.value(), input_names[it.index()].trim());
    }
    *has_input_attr = true;
  }

  if (auto str =
          dict_attr.get("outputs").dyn_cast_or_null<mlir::StringAttr>()) {
    str.getValue().split(output_names, ',', /*MaxSplit=*/-1,
                         /*KeepEmpty=*/false);
    auto term = fn.back().getTerminator();
    if (output_names.size() != term->getNumOperands()) {
      fn.emitWarning() << "output names (" << output_names.size()
                       << ") != terminator operands (" << term->getNumOperands()
                       << ")";
      return;
    }
    for (const auto& it : llvm::enumerate(term->getOperands())) {
      name_mapper_.InitOpName(it.value(), output_names[it.index()].trim());
    }
  }
}

bool Translator::IsStatefulOperand(mlir::Operation* op, int operand_index) {
  std::vector<int> operand_indices;
  if (!mlir::TFL::IsStatefulOp(op, &operand_indices)) return false;
  return absl::c_find(operand_indices, operand_index) != operand_indices.end();
}

BufferOffset<tflite::QuantizationParameters>
Translator::GetQuantizationForQuantStatsOpOutput(
    mlir::quantfork::StatisticsOp stats_op) {
  auto layer_stats = stats_op.getLayerStats().cast<mlir::DenseFPElementsAttr>();
  std::optional<mlir::ElementsAttr> axis_stats = stats_op.getAxisStats();
  std::optional<uint64_t> axis = stats_op.getAxis();
  std::vector<float> mins, maxs;
  mlir::DenseFPElementsAttr min_max_attr =
      axis_stats.has_value()
          ? axis_stats.value().cast<mlir::DenseFPElementsAttr>()
          : layer_stats;

  for (const auto& index_and_value :
       llvm::enumerate(min_max_attr.getValues<llvm::APFloat>())) {
    const llvm::APFloat value = index_and_value.value();
    if (index_and_value.index() % 2 == 0) {
      mins.push_back(value.convertToFloat());
    } else {
      maxs.push_back(value.convertToFloat());
    }
  }

  return tflite::CreateQuantizationParameters(
      builder_, builder_.CreateVector<float>(mins),
      builder_.CreateVector<float>(maxs), /*scale=*/0, /*zero_point=*/0,
      tflite::QuantizationDetails_NONE, /*details=*/0,
      /*quantized_dimension=*/axis.has_value() ? axis.value() : 0);
}

std::optional<BufferOffset<tflite::SubGraph>> Translator::BuildSubGraph(
    const std::string& name, Region* region, const int index) {
  const auto control_edges = ExtractControlEdges(&region->front());
  bool has_input_attr = false;
  if (auto fn = dyn_cast<FuncOp>(region->getParentOp())) {
    InitializeNamesFromAttribute(fn, &has_input_attr);
  }
  std::vector<BufferOffset<tflite::Tensor>> tensors;
  llvm::DenseMap<Value, int> tensor_index_map;

  // Builds tensor and buffer for argument or operation result. Returns false
  // on failure.
  auto build_tensor_and_buffer = [&](Value value, const int subgraph_index,
                                     const std::string& tensor_name) {
    // NoneType represents optional and may be skipped here.
    if (value.getType().isa<NoneType>()) {
      return true;
    }

    tensor_index_map.insert({value, tensors.size()});
    tensor_index_map_[subgraph_index][tensor_name] = tensors.size();
    std::optional<BufferOffset<tflite::QuantizationParameters>>
        quant_parameters;
    if (value.hasOneUse()) {
      auto stats_op =
          llvm::dyn_cast<mlir::quantfork::StatisticsOp>(*value.user_begin());
      if (stats_op) {
        quant_parameters = GetQuantizationForQuantStatsOpOutput(stats_op);
      }
    }

    int buffer_index = buffers_.size();
    // If a constant is returned as subgraph's output, this constant cannot be
    // deduplicated.
    const bool not_returned_by_subgraph = llvm::none_of(
        value.getUsers(),
        [](Operation* user) { return llvm::isa<mlir::func::ReturnOp>(user); });
    // TODO(ashwinm): Check if for stateful tensors, if it is also needed to
    // make the Buffer empty apart from setting the buffer_idx=0 in the
    // Tensor. This does not seem to affect runtime behavior for RNN/LSTM,
    // but would be good for reducing memory footprint.
    if (value.getDefiningOp()) {
      auto buffer_or =
          BuildBuffer(value, not_returned_by_subgraph, buffer_index);
      if (!buffer_or) return false;
      buffers_.push_back(*buffer_or);
    } else {
      buffers_.push_back(empty_buffer_);
    }

    auto tensor_or =
        BuildTensor(value, tensor_name, buffer_index, quant_parameters);
    if (!tensor_or) return false;
    tensors.push_back(*tensor_or);

    return true;
  };

  std::vector<BufferOffset<tflite::Operator>> operators;

  // Maps positions of operations in bb to positions in operators
  llvm::DenseMap<int, int> operation_index_to_operator_index;
  std::vector<Operation*> operators_in_mlir;
  auto& bb = region->front();

  // Main function's arguments are first passed to `input` op so they don't
  // have associated tensor and buffer. Build FlatBuffer tensor and buffer for
  // other functions.
  for (unsigned i = 0, e = bb.getNumArguments(); i < e; ++i) {
    mlir::BlockArgument arg = bb.getArgument(i);
    std::string tensor_name;
    if (has_input_attr)
      tensor_name = std::string(name_mapper_.GetUniqueName(arg));
    if (tensor_name.empty()) tensor_name = absl::StrCat("arg", i);
    if (!build_tensor_and_buffer(arg, index, tensor_name)) return std::nullopt;
  }

  bool failed_once = false;
  for (const auto& item : llvm::enumerate(bb)) {
    Operation& inst = item.value();
    const int operation_index = item.index();
    if (inst.hasTrait<mlir::OpTrait::IsTerminator>()) break;
    // For "quant.stats" op, it's used to store the quantization parameters
    // info and its output should be then replaced by its input value.
    if (auto quant_stats_op =
            llvm::dyn_cast<mlir::quantfork::StatisticsOp>(inst)) {
      continue;
    }
    std::vector<int32_t> intermediates;
    // Build intermediate tensors for tfl.lstm and insert these tensors into
    // flatbuffer.
    if (llvm::isa<mlir::TFL::LSTMOp, mlir::TFL::UnidirectionalSequenceLSTMOp>(
            inst)) {
      std::vector<std::string> intermediate_names = {
          "input_to_input_intermediate", "input_to_forget_intermediate",
          "input_to_cell_intermediate", "input_to_output_intermediate",
          "effective_hidden_scale_intermediate"};
      for (const std::string& intermediate : intermediate_names) {
        auto intermediate_attr = inst.getAttr(intermediate);
        if (auto attr = intermediate_attr.dyn_cast_or_null<mlir::TypeAttr>()) {
          Type qtype = attr.getValue();
          auto tensor_or = BuildTensorFromType(
              qtype, name_mapper_.GetUniqueName(intermediate).str());
          if (!tensor_or.has_value()) {
            continue;
          } else {
            intermediates.push_back(tensors.size());
            tensors.push_back(tensor_or.value());
          }
        }
      }
    }

    for (auto val : inst.getResults()) {
      std::string tensor_name = UniqueName(val);
      // For "tfl.numeric_verify" op, the name is used to find out the
      // original activation tensor rather than its own unique name in the
      // visualization or debugging tools.
      auto builtin_code = GetBuiltinOpCode(&inst);
      if (!builtin_code && dyn_cast<mlir::TFL::NumericVerifyOp>(&inst)) {
        // The first operand is the quantized activation, the target of this
        // NumericVerify op.
        auto quantized_op_val = inst.getOperands().front();
        tensor_name = "NumericVerify/" + UniqueName(quantized_op_val) + ":" +
                      std::to_string(tensor_index_map[quantized_op_val]);
      }
      if (!build_tensor_and_buffer(val, index, tensor_name))
        return std::nullopt;
    }

    if (require_use_buffer_offset_) return std::nullopt;

    // Skip constant ops as they don't represent a TFLite operator.
    if (IsConst(&inst)) continue;

    // Fetch operand and result tensor indices.
    std::vector<int32_t> results;
    results.reserve(inst.getNumResults());
    for (auto result : inst.getResults()) {
      results.push_back(tensor_index_map.lookup(result));
    }
    Operation* real_inst = &inst;
    std::vector<int32_t> operands;
    operands.reserve(real_inst->getNumOperands());
    for (auto operand : real_inst->getOperands()) {
      if (operand.getType().isa<NoneType>())
        operands.push_back(kTfLiteOptionalTensor);
      else if (auto stats_op =
                   llvm::dyn_cast_or_null<mlir::quantfork::StatisticsOp>(
                       operand.getDefiningOp()))
        operands.push_back(tensor_index_map.lookup(stats_op.getArg()));
      else
        operands.push_back(tensor_index_map.lookup(operand));
    }

    // CustomTfOp is just a wrapper around a TF op, we export the custom Op
    // not the wrapper, so we fetch the op from the region.
    if (auto custom_op = dyn_cast<mlir::TFL::CustomTfOp>(inst)) {
      // If we have custom op with a region, then use the first op in the
      // region, if it exists, otherwise just use params for custom op.
      if (!custom_op.getBody().empty()) {
        real_inst = &custom_op.getBody().front().front();
      } else {
        module_.emitError(
            "Invalid CustomTfOp: Custom TF Op have empty region.");
      }
    }
    if (auto tfl_operator =
            BuildOperator(real_inst, operands, results, intermediates)) {
      operation_index_to_operator_index.try_emplace(operation_index,
                                                    operators.size());
      operators.push_back(*tfl_operator);
      operators_in_mlir.push_back(real_inst);
    } else {
      failed_once = true;
    }
  }
  if (index + 1 > subgraph_op_inst_map_.size()) {
    subgraph_op_inst_map_.resize(index + 1);
  }
  subgraph_op_inst_map_[index] = operators_in_mlir;
  if (failed_once) return std::nullopt;

  // Get input and output tensor indices for the subgraph.
  std::vector<int32_t> inputs, outputs;
  for (auto arg : bb.getArguments()) {
    inputs.push_back(tensor_index_map[arg]);
  }
  for (auto result : bb.getTerminator()->getOperands()) {
    outputs.push_back(tensor_index_map[result]);
  }
  for (const auto& [from, to] : control_edges) {
    for (int what : {from, to}) {
      if (operation_index_to_operator_index.count(what) == 0) {
        module_.emitError(
            "dangling control edge -- at least one vertex Operation isn't a "
            "flatbuffer Operator.");
      }
    }
    model_control_dependencies_[index].emplace_back(
        operation_index_to_operator_index[from],
        operation_index_to_operator_index[to]);
  }
  return tflite::CreateSubGraph(
      builder_, builder_.CreateVector(tensors), builder_.CreateVector(inputs),
      builder_.CreateVector(outputs), builder_.CreateVector(operators),
      /*name=*/builder_.CreateString(name));
}

BufferOffset<tflite::Metadata> Translator::BuildMetadata(StringRef name,
                                                         StringRef content) {
  auto buffer_index = buffers_.size();
  auto buffer_data = builder_.CreateVector(
      reinterpret_cast<const uint8_t*>(content.data()), content.size());
  buffers_.push_back(tflite::CreateBuffer(builder_, buffer_data));
  return tflite::CreateMetadataDirect(builder_, name.data(), buffer_index);
}

std::optional<VectorBufferOffset<BufferOffset<tflite::Metadata>>>
Translator::CreateMetadataVector() {
  auto dict_attr = module_->getAttrOfType<mlir::DictionaryAttr>("tfl.metadata");
  std::vector<BufferOffset<tflite::Metadata>> metadata;
  if (dict_attr) {
    for (const auto& named_attr : dict_attr) {
      StringRef name = named_attr.getName();
      mlir::Attribute attr = named_attr.getValue();
      if (auto content = attr.dyn_cast<StringAttr>()) {
        metadata.push_back(BuildMetadata(name, content.getValue()));
      } else {
        module_.emitError(
            "all values in tfl.metadata's dictionary key-value pairs should "
            "be "
            "string attributes");
        return std::nullopt;
      }
    }
  }
  // Runtime version string is generated after we update the op
  // versions. Here we put a 16-byte dummy string as a placeholder. We choose
  // 16-byte because it's the alignment of buffers in flatbuffer, so it won't
  // cause any waste of space if the actual string is shorter than 16 bytes.
  constexpr std::size_t kByteStringSize = 16;
  metadata.push_back(
      BuildMetadata("min_runtime_version", std::string(kByteStringSize, '\0')));
  if (use_buffer_offset_) {
    metadata.push_back(
        BuildMetadata(tflite_metadata_buffer_location, "outside flatbuffers"));
  }
  for (const auto& kv : metadata_) {
    const std::string& val = kv.second;
    // Only take the first kByteStringSize values.
    const int count = std::min(kByteStringSize, val.length());
    std::string value = std::string(kByteStringSize, '\0')
                            .assign(val.begin(), val.begin() + count);
    metadata.push_back(BuildMetadata(kv.first, value));
  }

  // Populate the model control dependencies metadata entry.
  if (std::any_of(
          model_control_dependencies_.begin(),
          model_control_dependencies_.end(),
          [](const tflite::ControlEdges& edges) { return !edges.empty(); })) {
    metadata.push_back(
        BuildMetadata(tflite::kModelControlDependenciesMetadataKey,
                      tflite::SerializeModelControlDependencies(
                          model_control_dependencies_)));
  }
  return builder_.CreateVector(metadata);
}

// Helper method that returns list of all strings in a StringAttr identified
// by 'attr_key' and values are separated by a comma.
llvm::SmallVector<llvm::StringRef, 2> GetStringsFromAttrWithSeparator(
    mlir::DictionaryAttr attr, const std::string& attr_key) {
  llvm::SmallVector<llvm::StringRef, 2> result;
  if (auto str = attr.get(attr_key).dyn_cast_or_null<mlir::StringAttr>()) {
    str.getValue().split(result, ',', /*MaxSplit=*/-1,
                         /*KeepEmpty=*/false);
  }
  return result;
}

// Helper method that return list of string for all the StringAttr in the
// Attribute identified by 'attr_name'.
std::vector<std::string> GetStringsFromDictionaryAttr(
    const llvm::SmallVector<mlir::DictionaryAttr, 4>& dict_attrs,
    const StringRef attr_name) {
  std::vector<std::string> result;
  for (const auto& arg_attr : dict_attrs) {
    if (!arg_attr) continue;

    auto attrs = arg_attr.getValue();
    for (const auto attr : attrs) {
      if (attr.getName() == attr_name) {
        auto array_attr = attr.getValue().dyn_cast_or_null<mlir::ArrayAttr>();
        if (!array_attr || array_attr.empty()) continue;
        auto string_attr = array_attr[0].dyn_cast_or_null<mlir::StringAttr>();
        if (!string_attr) continue;
        result.push_back(string_attr.getValue().str());
      }
    }
  }
  return result;
}

std::vector<SignatureDefData> BuildSignaturedef(
    FuncOp main_op, const std::string& saved_model_tag,
    const uint32_t subgraph_index, tensorflow::OpOrArgNameMapper& name_mapper) {
  static const char kEntryFunctionAttributes[] = "tf.entry_function";

  // Fetch inputs and outputs from the signature.
  llvm::SmallVector<mlir::DictionaryAttr, 4> arg_attrs, res_attrs;
  main_op.getAllArgAttrs(arg_attrs);
  main_op.getAllResultAttrs(res_attrs);
  std::vector<std::string> sig_def_inputs =
      GetStringsFromDictionaryAttr(arg_attrs, kTfSavedModelIndexPathAttr);
  std::vector<std::string> sig_def_outputs =
      GetStringsFromDictionaryAttr(res_attrs, kTfSavedModelIndexPathAttr);

  // If no defined saved model signature, then return empty list.
  // This can happen when we are converting model not from SavedModel.
  if (sig_def_inputs.empty() && sig_def_outputs.empty()) return {};

  // Fetch function inputs and outputs tensor names.
  auto dict_attr =
      main_op->getAttrOfType<mlir::DictionaryAttr>(kEntryFunctionAttributes);
  if (!dict_attr) {
    main_op.emitWarning() << "failed to get entry function attr.";
    return {};
  }

  // Get Input and output tensor names from attribute.
  llvm::SmallVector<llvm::StringRef, 2> input_names =
      GetStringsFromAttrWithSeparator(dict_attr, /*attr_key=*/"inputs");
  llvm::SmallVector<llvm::StringRef, 2> output_names =
      GetStringsFromAttrWithSeparator(dict_attr, /*attr_key=*/"outputs");

  // Verify input size match the number of arguments.
  if (input_names.size() != main_op.getNumArguments()) {
    main_op.emitWarning() << "invalid entry function specification.";
    return {};
  }
  // Verify output size match the number of arguments.
  auto term = main_op.back().getTerminator();
  if (output_names.size() != term->getNumOperands()) {
    main_op.emitWarning() << "output names (" << output_names.size()
                          << ") != terminator operands ("
                          << term->getNumOperands() << ")";
    return {};
  }
  // Verify number of tensors for inputs and outputs matches size
  // of the list in the signature def.
  if (input_names.size() != sig_def_inputs.size() ||
      output_names.size() != sig_def_outputs.size()) {
    main_op.emitWarning(
        "Mismatch between signature def inputs/outputs and main function "
        "arguments.");
    return {};
  }
  // Exported method name.
  auto exported_name =
      main_op->getAttrOfType<mlir::ArrayAttr>(kTfSavedModelExportedNamesAttr);
  if (exported_name.empty()) {
    main_op.emitError("Empty exported names for main Function.");
    return {};
  }
  // Fill the SignatureDefData container.
  // We create vector of size 1 as TFLite now supports only 1 signatureDef.
  std::vector<SignatureDefData> result(1);
  for (int i = 0; i < input_names.size(); ++i) {
    result[0].inputs[sig_def_inputs[i]] = input_names[i].str();
  }
  for (int i = 0; i < output_names.size(); ++i) {
    // Fetch the name from the actual operand and not rely on names from
    // outputs as deduping can make them invalid after conversion.
    auto& operand = term->getOpOperand(i);
    auto unique_name = std::string(name_mapper.GetUniqueName(operand.get()));
    result[0].outputs[sig_def_outputs[i]] = unique_name;
  }
  if (auto name_attr = exported_name[0].dyn_cast_or_null<StringAttr>())
    result[0].signature_key = name_attr.getValue().str();
  result[0].subgraph_index = subgraph_index;
  return result;
}

std::vector<BufferOffset<tflite::TensorMap>> Translator::GetList(
    const int subgraph_index, const std::map<std::string, std::string>& items) {
  std::vector<BufferOffset<tflite::TensorMap>> result;
  for (const auto& item : items) {
    auto name_buf = builder_.CreateString(item.first);
    tflite::TensorMapBuilder tensor_map_builder(builder_);
    tensor_map_builder.add_name(name_buf);
    tensor_map_builder.add_tensor_index(
        tensor_index_map_[subgraph_index][item.second]);
    result.push_back(tensor_map_builder.Finish());
  }
  return result;
}

std::optional<VectorBufferOffset<BufferOffset<tflite::SignatureDef>>>
Translator::CreateSignatureDefs(
    const std::vector<SignatureDefData>& signature_defs) {
  std::vector<BufferOffset<tflite::SignatureDef>> signature_defs_buffer;
  // When we export each function in the module op, intentionally, we export
  // the entry functions at the beginning of the subgraph list and the
  // subgraph_index is the index in entry functions and at the same, is the
  // index in the subgraph list.
  int subgraph_index = 0;
  for (const auto& signature_def_data : signature_defs) {
    auto inputs = GetList(subgraph_index, signature_def_data.inputs);
    auto outputs = GetList(subgraph_index, signature_def_data.outputs);
    auto inputs_buf = builder_.CreateVector(inputs);
    auto outputs_buf = builder_.CreateVector(outputs);
    auto signature_key_buf =
        builder_.CreateString(signature_def_data.signature_key);
    tflite::SignatureDefBuilder sig_def_builder(builder_);
    sig_def_builder.add_inputs(inputs_buf);
    sig_def_builder.add_outputs(outputs_buf);
    sig_def_builder.add_signature_key(signature_key_buf);
    sig_def_builder.add_subgraph_index(signature_def_data.subgraph_index);
    signature_defs_buffer.push_back(sig_def_builder.Finish());
    ++subgraph_index;
  }

  return builder_.CreateVector(signature_defs_buffer);
}

bool UpdateEntryFunction(ModuleOp module) {
  if (module.lookupSymbol<FuncOp>("main") != nullptr) {
    // We already have an entry function.
    return true;
  }

  int entry_func_count = 0;
  FuncOp entry_func = nullptr;
  for (auto fn : module.getOps<FuncOp>()) {
    auto attrs = fn->getAttrOfType<mlir::DictionaryAttr>("tf.entry_function");
    if (!attrs || attrs.empty()) continue;
    ++entry_func_count;
    entry_func = fn;
  }

  // We should have at least one entry function.
  if (entry_func_count == 0) return false;

  if (entry_func_count == 1) {
    // Update the entry func to main when the entry func is only & one.
    entry_func.setName(StringAttr::get(module.getContext(), "main"));
  }
  return true;
}

std::optional<std::string> Translator::Translate(
    ModuleOp module, const toco::TocoFlags& toco_flags,
    const std::unordered_set<std::string>& tags,
    OpOrArgNameMapper* op_or_arg_name_mapper,
    const std::map<std::string, std::string>& metadata,
    bool serialize_stablehlo_ops,
    std::optional<size_t> custom_option_alignment) {
  OpOrArgLocNameMapper default_op_or_arg_name_mapper;
  if (!op_or_arg_name_mapper)
    op_or_arg_name_mapper = &default_op_or_arg_name_mapper;
  if (!UpdateEntryFunction(module)) return std::nullopt;
  if (!IsValidTFLiteMlirModule(module)) return std::nullopt;
  Translator translator(module, toco_flags, tags, op_or_arg_name_mapper,
                        metadata, custom_option_alignment);
  translator.convert_stablehlo_ = serialize_stablehlo_ops;
  auto ret = translator.TranslateInternal();
  if (translator.require_use_buffer_offset_) {
    auto new_toco_flags = toco_flags;
    new_toco_flags.set_use_buffer_offset(true);
    Translator new_translator(module, new_toco_flags, tags,
                              op_or_arg_name_mapper, metadata,
                              custom_option_alignment);
    return new_translator.TranslateInternal();
  }
  return ret;
}

bool Translator::CheckGpuDelegateCompatibility(uint8_t* model_buffer_pointer) {
  bool gpu_compatibile = true;
  auto model = tflite::GetModel(model_buffer_pointer);
  auto subgraphs = model->subgraphs();

  for (int i = 0; i < subgraphs->Length(); ++i) {
    const tflite::SubGraph* subgraph = subgraphs->Get(i);
    for (int j = 0; j < subgraph->operators()->Length(); ++j) {
      const tflite::Operator* op = subgraph->operators()->Get(j);
      const tflite::OperatorCode* op_code =
          model->operator_codes()->Get(op->opcode_index());
      auto status =
          tflite::CheckGpuDelegateCompatibility(op_code, op, subgraph, model);
      if (!status.ok()) {
        gpu_compatibile = false;
        auto inst = subgraph_op_inst_map_[i][j];
        tfl::AttachErrorCode(
            inst->emitOpError()
                << "is not GPU compatible: " << std::string(status.message()),
            tflite::metrics::ConverterErrorData::ERROR_GPU_NOT_COMPATIBLE);
      }
    }
  }
  return gpu_compatibile;
}

std::optional<std::string> Translator::TranslateInternal() {
  // A list of named regions in the module with main function being the first
  // in the list. The main function is required as the first subgraph in the
  // model is entry point for the model.
  std::vector<std::pair<std::string, Region*>> named_regions;
  named_regions.reserve(std::distance(module_.begin(), module_.end()));

  int subgraph_idx = 0;

  // Entry functions for signature defs.
  std::vector<FuncOp> entry_functions;
  std::vector<FuncOp> non_entry_functions;
  FuncOp main_fn = module_.lookupSymbol<FuncOp>("main");
  if (main_fn != nullptr) {
    // Treat the main function as a signature def when the given main function
    // contains on the tf.entry_function attribute.
    auto attrs =
        main_fn->getAttrOfType<mlir::DictionaryAttr>("tf.entry_function");
    if (attrs && !attrs.empty()) {
      entry_functions.push_back(main_fn);
    } else {
      non_entry_functions.push_back(main_fn);
    }
  }

  // Walk over the module collection ops with functions and while ops.
  module_.walk([&](FuncOp fn) {
    if (main_fn == fn) return WalkResult::advance();
    auto attrs = fn->getAttrOfType<mlir::DictionaryAttr>("tf.entry_function");
    if (attrs && !attrs.empty()) {
      entry_functions.push_back(fn);
    } else {
      non_entry_functions.push_back(fn);
    }
    return WalkResult::advance();
  });

  // Assign the subgraph index. Among the given functions, it will put entry
  // functions at the beginning of the list of the subgraphs.
  for (auto fn : entry_functions) {
    subgraph_index_map_[fn.getName().str()] = subgraph_idx++;
    named_regions.emplace_back(fn.getName().str(), &fn.getBody());
  }
  for (auto fn : non_entry_functions) {
    subgraph_index_map_[fn.getName().str()] = subgraph_idx++;
    named_regions.emplace_back(fn.getName().str(), &fn.getBody());
  }

  // Build subgraph for each of the named regions.
  subgraphs_.resize(named_regions.size());
  model_control_dependencies_.assign(named_regions.size(), {});
  int first_failed_func = -1;

  // When we export each function in the module op, intentionally, we export
  // the entry functions at the beginning of the subgraph list and the
  // subgraph_index is the index in entry functions and at the same, is the
  // index in the subgraph list.
  int subgraph_index = 0;
  for (const auto& it : llvm::enumerate(named_regions)) {
    if (require_use_buffer_offset_ && !use_buffer_offset_) return std::nullopt;
    auto subgraph_or =
        BuildSubGraph(it.value().first, it.value().second, subgraph_index);
    if (!subgraph_or) {
      if (first_failed_func == -1)
        // Record the index of the first region that cannot be converted.
        // Keep looping through all subgraphs in the module to make sure that
        // we collect the list of missing ops from the entire module.
        first_failed_func = it.index();
    } else {
      subgraphs_[subgraph_index] = *subgraph_or;
      ++subgraph_index;
    }
  }

  if (!resource_ops_.empty()) {
    std::string resource_ops_summary =
        GetOpsSummary(resource_ops_, /*summary_title=*/"Resource");
    LOG(WARNING) << "Graph contains the following resource op(s), that use(s) "
                    "resource type. Currently, the "
                    "resource type is not natively supported in TFLite. Please "
                    "consider not using the resource type if there are issues "
                    "with either TFLite converter or TFLite runtime:\n"
                 << resource_ops_summary;
  }

  if (!flex_ops_.empty()) {
    std::string flex_ops_summary =
        GetOpsSummary(flex_ops_, /*summary_title=*/"Flex");
    LOG(WARNING) << "TFLite interpreter needs to link Flex delegate in order "
                    "to run the model since it contains the following Select TF"
                    "op(s):\n"
                 << flex_ops_summary
                 << "\nSee instructions: "
                    "https://www.tensorflow.org/lite/guide/ops_select";
  }

  if (!custom_ops_.empty()) {
    std::string custom_ops_summary =
        GetOpsSummary(custom_ops_, /*summary_title=*/"Custom");
    LOG(WARNING) << "The following operation(s) need TFLite custom op "
                    "implementation(s):\n"
                 << custom_ops_summary
                 << "\nSee instructions: "
                    "https://www.tensorflow.org/lite/guide/ops_custom";
  }

  if (require_use_buffer_offset_) return std::nullopt;

  if (first_failed_func != -1) {
    std::string failed_flex_ops_summary =
        GetOpsSummary(failed_flex_ops_, /*summary_title=*/"TF Select");
    std::string failed_custom_ops_summary =
        GetOpsSummary(failed_custom_ops_, /*summary_title=*/"Custom");
    std::string err;
    if (!failed_flex_ops_.empty())
      err +=
          "\nSome ops are not supported by the native TFLite runtime, you "
          "can "
          "enable TF kernels fallback using TF Select. See instructions: "
          "https://www.tensorflow.org/lite/guide/ops_select \n" +
          failed_flex_ops_summary + "\n";
    if (!failed_custom_ops_.empty())
      err +=
          "\nSome ops in the model are custom ops, "
          "See instructions to implement "
          "custom ops: https://www.tensorflow.org/lite/guide/ops_custom \n" +
          failed_custom_ops_summary + "\n";

    auto& failed_region = named_regions[first_failed_func];
    return failed_region.second->getParentOp()->emitError()
               << "failed while converting: '" << failed_region.first
               << "': " << err,
           std::nullopt;
  }

  // Log MAC count.
  int64_t ops_count;
  if (EstimateArithmeticCount(&ops_count)) {
    const int64_t million = 1e6;
    const int64_t billion = 1e9;
    std::string flops_str;
    std::string mac_str;
    if (ops_count < 10000) {
      flops_str = absl::StrFormat("%ld ", ops_count);
      mac_str = absl::StrFormat("%ld ", ops_count / 2);
    } else if (ops_count < billion) {
      flops_str =
          absl::StrFormat("%.3f M ", static_cast<double>(ops_count) / million);
      mac_str = absl::StrFormat("%.3f M ",
                                static_cast<double>(ops_count / 2) / million);
    } else {
      flops_str =
          absl::StrFormat("%.3f G ", static_cast<double>(ops_count) / billion);
      mac_str = absl::StrFormat("%.3f G ",
                                static_cast<double>(ops_count / 2) / billion);
    }
    LOG(INFO) << "Estimated count of arithmetic ops: " << flops_str
              << " ops, equivalently " << mac_str << " MACs";
  }

  std::string model_description;
  if (auto attr = module_->getAttrOfType<StringAttr>("tfl.description")) {
    model_description = attr.getValue().str();
  } else {
    model_description = "MLIR Converted.";
  }

  // Build the model and finish the model building process.
  auto description = builder_.CreateString(model_description.data());
  VectorBufferOffset<int32_t> metadata_buffer = 0;  // Deprecated
  auto metadata = CreateMetadataVector();
  if (!metadata) return std::nullopt;

  std::vector<SignatureDefData> signature_defs_vec;
  subgraph_index = 0;
  // Build SignatureDefs for the tf.entry_function based func ops.
  for (auto fn : entry_functions) {
    auto signature_defs = BuildSignaturedef(
        fn, saved_model_tags_.empty() ? "" : *saved_model_tags_.begin(),
        subgraph_index, name_mapper_);
    for (const auto& signature_def : signature_defs) {
      signature_defs_vec.push_back(signature_def);
    }
    // When we export each function in the module op, intentionally, we export
    // the entry functions at the beginning of the subgraph list and the
    // subgraph_index is the index in entry functions and at the same, is the
    // index in the subgraph list.
    ++subgraph_index;
  }
  auto signature_defs = CreateSignatureDefs(signature_defs_vec);

  auto model = tflite::CreateModel(builder_, TFLITE_SCHEMA_VERSION,
                                   builder_.CreateVector(opcodes_),
                                   builder_.CreateVector(subgraphs_),
                                   description, builder_.CreateVector(buffers_),
                                   metadata_buffer, *metadata, *signature_defs);
  tflite::FinishModelBuffer(builder_, model);
  // There is a limit of 2GB for a flatbuffer.
  bool flatbuffer_limit_exceeded = builder_.GetSize() > flatbuffer_size_max;
  if (flatbuffer_limit_exceeded && require_use_buffer_offset_ == false) {
    require_use_buffer_offset_ = true;
    return std::nullopt;
  }
  if (flatbuffer_limit_exceeded) {
    LOG(ERROR) << "Model structure size is bigger than 2gb";
    return std::nullopt;
  }
  tflite::UpdateOpVersion(builder_.GetBufferPointer());
  tflite::UpdateMinimumRuntimeVersionForModel(builder_.GetBufferPointer());
  if (supported_backends_.find("GPU") != supported_backends_.end()) {
    if (!CheckGpuDelegateCompatibility(builder_.GetBufferPointer())) {
      return std::nullopt;
    }
  }

  auto result =
      std::string(reinterpret_cast<const char*>(builder_.GetBufferPointer()),
                  builder_.GetSize());

  // Return serialized string for the built FlatBuffer.
  if (use_buffer_offset_) {
    AppendBufferData(result);
    auto mutable_model = tflite::GetMutableModel(result.data());
    bool ret = UpdateBufferOffsets(mutable_model);
    if (!ret) {
      return std::nullopt;
    }
    return result;
  }
  return result;
}

void Translator::AppendBufferData(std::string& result) {
  std::unordered_map<uint64_t, std::pair<int64_t, int64_t>> hashcode_to_pos;
  // Pad to be 16 bytes aligned
  while (result.size() % 16 != 0) result += '\0';
  for (auto& it : buffer_data_map_) {
    auto buffer = std::string(it.second.begin(), it.second.end());
    int64_t index = it.first;
    int64_t offset = result.size();
    int64_t size = it.second.size();
    uint64_t hash = tsl::Fingerprint64(buffer);
    if (hashcode_to_pos.find(hash) == hashcode_to_pos.end()) {
      hashcode_to_pos[hash] = std::make_pair(offset, size);
      buffer_idx_map_[index] = std::make_pair(offset, size);
      result += std::string(it.second.begin(), it.second.end());
      // Pad to be 16 bytes aligned
      while (result.size() % 16 != 0) result += '\0';
    } else {
      // only update offset/index.
      buffer_idx_map_[index] = hashcode_to_pos[hash];
    }
  }
  // pad 16 bytes for the last buffer for XNNPack
  result += "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
  // pad to be 16 bytes aligned
  while (result.size() % 16 != 0) result += '\0';

  for (auto& it : custom_op_data_map_) {
    while (result.size() % 16 != 0) result += '\0';
    if (custom_option_alignment_.has_value()) {
      while (result.size() % custom_option_alignment_.value() != 0)
        result += '\0';
    }
    auto buffer = std::string(it.second.begin(), it.second.end());
    int64_t offset = result.size();
    int64_t size = it.second.size();
    custom_op_idx_map_[it.first] = std::make_pair(offset, size);
    result += buffer;
  }
  // pad to be 16 bytes aligned
  while (result.size() % 16 != 0) result += '\0';
}

bool Translator::UpdateBufferOffsets(tflite::Model* mutable_model) {
  auto buffers = mutable_model->mutable_buffers();
  bool ret = true;
  for (auto& it : buffer_idx_map_) {
    tflite::Buffer* buffer = buffers->GetMutableObject(it.first);

    ret &= buffer->mutate_offset(it.second.first);
    ret &= buffer->mutate_size(it.second.second);
  }

  if (!ret) {
    LOG(ERROR) << "failed to update buffer offsets\n";
    return ret;
  }

  auto mutable_subgraphs = mutable_model->mutable_subgraphs();
  for (auto msubgraph : *mutable_subgraphs) {
    auto operators = msubgraph->mutable_operators();
    for (auto op : *operators) {
      auto opcode_idx = op->opcode_index();
      auto opcodes = mutable_model->operator_codes();
      auto opcode = (*opcodes)[opcode_idx]->builtin_code();
      if (opcode == tflite::BuiltinOperator_CUSTOM) {
        std::vector<int32_t> inputs(op->inputs()->begin(), op->inputs()->end());
        std::vector<int32_t> outputs(op->outputs()->begin(),
                                     op->outputs()->end());
        for (auto& custom_op : custom_op_idx_map_) {
          std::vector<int32_t> key_inputs = custom_op.first.first;
          std::vector<int32_t> key_outputs = custom_op.first.second;
          // TODO(b/287306548): we need more rigorous check to make sure the
          // node we're updating is the right one for now we're just ensuring
          // they have the same number of input, output and first output is
          // the same
          if (key_inputs.size() == inputs.size() &&
              key_outputs.size() == outputs.size() &&
              key_outputs[0] == outputs[0]) {
            ret &=
                op->mutate_large_custom_options_offset(custom_op.second.first);
            ret &=
                op->mutate_large_custom_options_size(custom_op.second.second);
          }
        }
      }
    }
  }
  if (!ret) LOG(ERROR) << "failed to update custom op offsets\n";

  return ret;
}

BufferOffset<tflite::SparsityParameters> Translator::BuildSparsityParameters(
    const mlir::TFL::SparsityParameterAttr& s_attr) {
  const int dim_size = s_attr.getDimMetadata().size();
  std::vector<flatbuffers::Offset<tflite::DimensionMetadata>> fb_dim_metadata(
      dim_size);
  for (int i = 0; i < dim_size; i++) {
    const auto dim_metadata =
        s_attr.getDimMetadata()[i].dyn_cast<mlir::TFL::DimensionMetadataAttr>();
    if (dim_metadata.getFormat().getValue() ==
        mlir::TFL::DimensionType::DENSE) {
      fb_dim_metadata[i] = tflite::CreateDimensionMetadata(
          builder_, tflite::DimensionType_DENSE, dim_metadata.getDenseSize());

    } else {
      auto segments = dim_metadata.getSegments();
      std::vector<int> vector_segments(segments.size(), 0);
      for (int j = 0, end = segments.size(); j < end; j++) {
        vector_segments[j] = segments[j];
      }
      tflite::SparseIndexVector segments_type;
      BufferOffset<void> array_segments;
      // The segment array is sorted.
      // TODO(b/147449640): Clean this up with util functions.
      int max_of_segments = vector_segments[segments.size() - 1];
      if (max_of_segments <= UINT8_MAX) {
        segments_type = tflite::SparseIndexVector_Uint8Vector;
        std::vector<uint8_t> uint8_vector(vector_segments.begin(),
                                          vector_segments.end());
        array_segments = tflite::CreateUint8Vector(
                             builder_, builder_.CreateVector(uint8_vector))
                             .Union();
      } else if (max_of_segments <= UINT16_MAX) {
        segments_type = tflite::SparseIndexVector_Uint16Vector;
        std::vector<uint16_t> uint16_vector(vector_segments.begin(),
                                            vector_segments.end());
        array_segments = tflite::CreateUint16Vector(
                             builder_, builder_.CreateVector(uint16_vector))
                             .Union();
      } else {
        segments_type = tflite::SparseIndexVector_Int32Vector;
        array_segments = tflite::CreateInt32Vector(
                             builder_, builder_.CreateVector(vector_segments))
                             .Union();
      }

      auto indices = dim_metadata.getIndices();
      std::vector<int> vector_indices(indices.size(), 0);
      int max_of_indices = 0;
      for (int j = 0, end = indices.size(); j < end; j++) {
        vector_indices[j] = indices[j];
        if (vector_indices[j] > max_of_indices) {
          max_of_indices = vector_indices[j];
        }
      }
      tflite::SparseIndexVector indices_type;
      BufferOffset<void> array_indices;
      if (max_of_indices <= UINT8_MAX) {
        indices_type = tflite::SparseIndexVector_Uint8Vector;
        std::vector<uint8_t> uint8_vector(vector_indices.begin(),
                                          vector_indices.end());
        array_indices = tflite::CreateUint8Vector(
                            builder_, builder_.CreateVector(uint8_vector))
                            .Union();
      } else if (max_of_indices <= UINT16_MAX) {
        indices_type = tflite::SparseIndexVector_Uint16Vector;
        std::vector<uint16_t> uint16_vector(vector_indices.begin(),
                                            vector_indices.end());
        array_indices = tflite::CreateUint16Vector(
                            builder_, builder_.CreateVector(uint16_vector))
                            .Union();
      } else {
        indices_type = tflite::SparseIndexVector_Int32Vector;
        array_indices = tflite::CreateInt32Vector(
                            builder_, builder_.CreateVector(vector_indices))
                            .Union();
      }

      fb_dim_metadata[i] = tflite::CreateDimensionMetadata(
          builder_, tflite::DimensionType_SPARSE_CSR, 0, segments_type,
          array_segments, indices_type, array_indices);
    }
  }

  std::vector<int> traversal_order(dim_size);
  for (int i = 0; i < dim_size; i++) {
    traversal_order[i] = s_attr.getTraversalOrder()[i];
  }
  const int block_map_size = s_attr.getBlockMap().size();
  std::vector<int> block_map(block_map_size);
  for (int i = 0; i < block_map_size; i++) {
    block_map[i] = s_attr.getBlockMap()[i];
  }

  return tflite::CreateSparsityParameters(
      builder_, builder_.CreateVector(traversal_order),
      builder_.CreateVector(block_map), builder_.CreateVector(fb_dim_metadata));
}

std::vector<std::pair<int, int>> Translator::ExtractControlEdges(
    mlir::Block* block) {
  std::vector<std::pair<int, int>> control_edges;

  mlir::IRRewriter rewriter(block->getParentOp()->getContext());

  // Since we're modifying *block, we store integer offsets to block->begin().
  llvm::DenseMap<Operation*, int> control_nodes_at;
  std::vector<Operation*> control_nodes;
  for (const auto& item : llvm::enumerate(*block)) {
    if (llvm::isa<mlir::TFL::ControlNodeOp>(item.value())) {
      control_nodes.push_back(&item.value());
      control_nodes_at.try_emplace(&item.value(), item.index());
    }
  }

  for (auto outer_op : control_nodes) {
    auto control_node_op = dyn_cast<mlir::TFL::ControlNodeOp>(outer_op);
    auto* inner_op = &control_node_op.getBody().front().front();
    auto control_token = control_node_op.getControl();

    // Now go through all uses. Since *block is in executable order, control
    // edges always point to operations we haven't modified yet.
    for (auto& use : control_token.getUses()) {
      auto owner = use.getOwner();
      // Control tokens can only be consumed by other ControlNodeOps,
      assert(llvm::isa<mlir::TFL::ControlNodeOp>(owner));
      assert(control_nodes_at.find(owner) != control_nodes_at.end());
      // Control edge in terms of offsets.
      control_edges.emplace_back(control_nodes_at[outer_op],
                                 control_nodes_at[owner]);
    }
    control_token.dropAllUses();

    // Replace the ControlNodeOp with the wrapped operation.
    rewriter.setInsertionPointAfter(outer_op);
    auto* cloned_inner = rewriter.clone(*inner_op);
    for (auto it :
         llvm::zip(control_node_op.getOutputs(), cloned_inner->getResults())) {
      std::get<0>(it).replaceAllUsesWith(std::get<1>(it));
    }
    rewriter.eraseOp(outer_op);
  }
  return control_edges;
}

}  // namespace

namespace tflite {

bool MlirToFlatBufferTranslateFunction(mlir::ModuleOp module,
                                       const FlatbufferExportOptions& options,
                                       std::string* serialized_flatbuffer,
                                       bool serialize_stablehlo_ops) {
  auto maybe_translated = Translator::Translate(
      module, options.toco_flags, options.saved_model_tags,
      options.op_or_arg_name_mapper, options.metadata, serialize_stablehlo_ops,
      options.custom_option_alignment);
  if (!maybe_translated) return false;
  *serialized_flatbuffer = std::move(*maybe_translated);
  return true;
}

}  // namespace tflite
