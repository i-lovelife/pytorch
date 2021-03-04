#include <ATen/ATen.h>
#include <ATen/Parallel.h>
#include <torch/custom_class.h>
#include <torch/library.h>

#include <ATen/native/quantized/cpu/quant_utils.h>
#include <caffe2/utils/threadpool/pthreadpool-cpp.h>

#include <ATen/native/mo/sparsity/cpu/packed_params.h>
#include <ATen/native/mo/sparsity/cpu/qnnpack_utils.h>

torch::class_<SparseLinearPackedParamsBase> register_sparse_linear_params();

#ifdef USE_PYTORCH_QNNPACK
template <>
at::Tensor SparsePackedLinearWeightQnnp::apply_dynamic_impl<true>(
    const at::Tensor& input) {
  TORCH_INTERNAL_ASSERT(
      false,
      "Sparse quantized dynamic linear with fused relu is not yet "
      "supported on qnnpack backend.");
  return at::Tensor();
}

template <>
at::Tensor SparsePackedLinearWeightQnnp::apply_dynamic_impl<false>(
    const at::Tensor& input) {
  TORCH_CHECK(
      input.dim() >= 2,
      "quantized_sparse_linear(): Input tensor rank should be >= 2");

  size_t rows_input = 1;
  size_t cols_input = input.size(input.dim() - 1);
  for (size_t i = 0; i < input.dim() - 1; ++i) {
    rows_input *= input.size(i);
  }
  TORCH_CHECK(
      cols_input == orig_weight_.size(1),
      "quantized_sparse_lienar: Input tensor's last and weight tensor's"
      " second dimension must match.");

  float x_min;
  float x_max;
  if (input.numel() > 0) {
    x_min = input.min().item<float>();
    x_max = input.max().item<float>();
  } else {
    // On empty input, no output data will be generated,
    // so use arbitrary qparams.
    x_min = 0;
    x_max = 0;
  }

  auto q_params = quant_utils::ChooseQuantizationParams(
      /*min=*/x_min,
      /*max=*/x_max,
      /*qmin=*/0,
      /*qmax=*/255);

  // Quantize input
  at::Tensor q_input = at::quantize_per_tensor(
      input, q_params.scale, q_params.zero_point, c10::kQUInt8);

  auto q_input_contig = q_input.contiguous();
  if (sparse_linear_op_ == nullptr) {
    // We calculate requant scale here as the vector holding the requant scale
    // is owned by this module. The pointer is then passed to qnnpack backend.
    generate_requantization_scales(
        w_scales_, q_input_contig.q_scale(), 1.f, requantization_scales_);
    input_scale_ = q_input_contig.q_scale();
    pytorch_qnnp_operator_t sparse_linear_op{nullptr};
    pytorch_qnnp_status status =
      pytorch_qnnp_create_fully_connected_sparse_dq_nc_q8(
        orig_weight_.size(1),
        orig_weight_.size(0),
        q_input_contig.q_zero_point(),
        w_zero_points_.data(),
        bcsr_matrix_->col_indices.data(),
        bcsr_matrix_->row_values.data(),
        bcsr_matrix_->values.data(),
        bcsr_matrix_->row_block_size,  /* out_features_block_size */
        bcsr_matrix_->col_block_size,  /* in_features_block_size */
        0, /* output zero point: not used */
        std::numeric_limits<uint8_t>::min(),
        std::numeric_limits<uint8_t>::max(),
        0, /* flags */
        requantization_scales_.data(),
        true, /* use prepacking kernel */
        &sparse_linear_op);
    TORCH_CHECK(
        status == pytorch_qnnp_status_success,
        "Failed to create sparse linear operator on"
        " qnnpack backend.");
    sparse_linear_op_ =
      std::unique_ptr<pytorch_qnnp_operator, QnnpackOperatorDeleter>(sparse_linear_op);
  }

  // Input on next iteration can be different, thus resulting in
  // different input scale. This will require us to recalculate requantization scales.
  if (input_scale_ != q_input_contig.q_scale()) {
    generate_requantization_scales(
        w_scales_, q_input_contig.q_scale(), 1.f, requantization_scales_);
  }
  // Update input related quantization params in the operator.
  sparse_linear_op_->dynamic_conv_quantization_params.input_zero_point =
    q_input_contig.q_zero_point();
  sparse_linear_op_->dynamic_conv_quantization_params.multipliers =
    requantization_scales_.data();

  std::vector<int64_t> out_sizes = input.sizes().vec();
  size_t rows_w = orig_weight_.size(0);
  out_sizes.back() = rows_w;

  auto output = at::empty(out_sizes, input.options().dtype(at::kFloat));

  pytorch_qnnp_status status =
    pytorch_qnnp_setup_fully_connected_sparse_dq_nc_q8(
        sparse_linear_op_.get(),
        rows_input, /* batch size */
        reinterpret_cast<uint8_t*>(q_input_contig.data_ptr<c10::quint8>()),
        cols_input, /* num input channels */
        bias_.data_ptr<float>(),
        output.data_ptr<float>(),
        rows_w /* num output channels */);
  TORCH_CHECK(
      status == pytorch_qnnp_status_success,
      "Failed to setup sparse linear operator on"
      " qnnpack backend.");

  status =
    pytorch_qnnp_run_operator(sparse_linear_op_.get(), caffe2::pthreadpool_());
  TORCH_CHECK(
      status == pytorch_qnnp_status_success,
      "Failed to run sparse linear operator on"
      " qnnpack backend.");

  return output;
}

at::Tensor SparsePackedLinearWeightQnnp::apply_dynamic(
    const at::Tensor& input) {
  return apply_dynamic_impl<false>(input);
}

at::Tensor SparsePackedLinearWeightQnnp::apply_dynamic_relu(
    const at::Tensor& input) {
  return apply_dynamic_impl<true>(input);
}

#endif // USE_PYTORCH_QNNPACK

namespace torch::mo {

namespace {

template <bool ReluFused>
class SparseQLinearDynamicInt8 final {
 public:
  static at::Tensor run(
      const at::Tensor& input,
      const c10::intrusive_ptr<SparseLinearPackedParamsBase>& packed_weight) {
    auto& ctx = at::globalContext();
#ifdef USE_PYTORCH_QNNPACK
    if (ctx.qEngine() == at::QEngine::QNNPACK) {
      if (ReluFused) {
        return packed_weight->apply_dynamic_relu(input);
      } else {
        return packed_weight->apply_dynamic(input);
      }
    }
#endif
    TORCH_CHECK(
        false,
        "Didn't find engine for operation fb::sparse_qlinear_dynamic",
        toString(ctx.qEngine()));
  }
};

TORCH_LIBRARY_IMPL(sparsity, CPU, m) {
  m.impl(
      TORCH_SELECTIVE_NAME("sparsity::sparse_qlinear_dynamic"),
      TORCH_FN(SparseQLinearDynamicInt8<false>::run));
  m.impl(
      TORCH_SELECTIVE_NAME("sparsity::sparse_qlinear_relu_dynamic"),
      TORCH_FN(SparseQLinearDynamicInt8<true>::run));
}

} // namespace
} // namespace torch::mo