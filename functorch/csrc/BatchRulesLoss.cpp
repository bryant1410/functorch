#include <functorch/csrc/BatchRulesHelper.h>
#include <functorch/csrc/PlumbingHelper.h>
#include <functorch/csrc/BatchedFallback.h>
#include <ATen/core/dispatch/Dispatcher.h>

namespace at { namespace functorch {
std::tuple<at::Tensor,optional<int64_t>,at::Tensor,optional<int64_t>>
nll_loss_forward_self_target_batch_rule(
    const at::Tensor & self, optional<int64_t> self_bdim,
    const at::Tensor & target, optional<int64_t> target_bdim,
    int64_t reduction) {
  TORCH_INTERNAL_ASSERT(self.dim() == 3 && target.dim() == 2);

  if (reduction == Reduction::None) {
    int64_t batch_size = self.size(*self_bdim);
    auto self_ = reshape_dim_into(*self_bdim, 0, self);
    auto target_ = reshape_dim_into(*target_bdim, 0, target);
    auto result = at::nll_loss_forward(self_, target_, nullopt, reduction, -100);
    return {
      reshape_dim_outof(0, batch_size, std::get<0>(result)), 0,
      std::get<1>(result), nullopt
    };
  } else if (reduction == Reduction::Sum) {
    int64_t batch_size = self.size(*self_bdim);
    auto self_ = reshape_dim_into(*self_bdim, 0, self);
    auto target_ = reshape_dim_into(*target_bdim, 0, target);
    auto res = at::nll_loss_forward(self_, target_, nullopt, Reduction::None, -100);
    auto output = std::get<0>(res);
    output = reshape_dim_outof(0, batch_size, output);
    auto total_weight = self_.new_full({}, output.size(-1));
    return {
      output.sum(-1), 0,
      // NB: total_weight = 0 after Reduction::None
      total_weight, nullopt,
    };
  } else if (reduction == Reduction::Mean) {
    int64_t batch_size = self.size(*self_bdim);
    auto self_ = reshape_dim_into(*self_bdim, 0, self);
    auto target_ = reshape_dim_into(*target_bdim, 0, target);
    auto res = at::nll_loss_forward(self_, target_, nullopt, Reduction::None, -100);
    auto output = std::get<0>(res);
    output = reshape_dim_outof(0, batch_size, output);
    auto total_weight = self_.new_full({}, output.size(-1));
    return {
      output.mean(-1), 0,
      // NB: total_weight = 0 after Reduction::None
      total_weight, nullopt,
    };
  }
  TORCH_INTERNAL_ASSERT(false);
}

std::tuple<at::Tensor,at::Tensor> nll_loss_forward_plumbing(
    const at::Tensor & self,
    const at::Tensor & target,
    const c10::optional<at::Tensor> & weight,
    int64_t reduction, int64_t ignore_index) {
  auto maybe_layer = maybeCurrentDynamicLayer();
  TORCH_INTERNAL_ASSERT(maybe_layer.has_value());
  int64_t cur_level = maybe_layer->layerId();

  Tensor self_value;
  optional<int64_t> self_bdim;
  std::tie(self_value, self_bdim) = unwrapTensorAtLevel(self, cur_level);

  Tensor target_value;
  optional<int64_t> target_bdim;
  std::tie(target_value, target_bdim) = unwrapTensorAtLevel(target, cur_level);

  optional<Tensor> weight_value;
  optional<int64_t> weight_bdim;
  if (weight) {
    std::tie(weight_value, weight_bdim) = unwrapTensorAtLevel(*weight, cur_level);
  }

  if (self_bdim && target_bdim && !weight_bdim && ignore_index < 0) {
    c10::impl::ExcludeDispatchKeyGuard guard(kBatchedKey);
    auto results = nll_loss_forward_self_target_batch_rule(
        self_value, self_bdim, target_value, target_bdim, reduction);
    return {
      makeBatched(std::get<0>(results), std::get<1>(results), cur_level),
      makeBatched(std::get<2>(results), std::get<3>(results), cur_level)
    };
  }

  static auto op = c10::Dispatcher::singleton()
    .findSchemaOrThrow("aten::nll_loss_forward", "");
  return slow_fallback<Tensor,Tensor>(op, {self, target, weight, reduction, ignore_index});
}

TORCH_LIBRARY_IMPL(aten, FT_BATCHED_KEY, m) {
  m.impl("nll_loss_forward", nll_loss_forward_plumbing);
  m.impl("nll_loss_nd", native::nll_loss_nd);
  m.impl("nll_loss", native::nll_loss);
}

}}
