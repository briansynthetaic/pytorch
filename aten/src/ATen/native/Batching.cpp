#include <ATen/BatchedTensorImpl.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/VmapTransforms.h>

namespace at { namespace native {

// Adds a batch dimension to the tensor `self` out-of-place
Tensor _add_batch_dim(const Tensor& self, int64_t batch_dim, int64_t level) {
  return addBatchDim(self, level, batch_dim);
}

static bool has_level(const Tensor& self, int64_t level) {
  const auto* batched = maybeGetBatched(self);
  if (!batched) {
    return false;
  }
  auto bdims = batched->bdims();
  auto* it = std::find_if(bdims.begin(), bdims.end(), [&](const BatchDim& bdim) {
    return bdim.level() == level;
  });
  return it != bdims.end();
}

// Returns a Tensor with batch dim with level `level` turned into a regular tensor,
// as well as a logical dim index of where said dimension is in the returned tensor.
//
// For example, given
// self=BatchedTensor([2, 3, 5], bdims=[(lvl=0, dim=1), (lvl=1, dim=2)]), level=1
// we would return pair(BatchedTensor([2, 3, 5], bdims=[(lvl=0,dim=1)]), 1)
// because we'd turn the batch dim at (physical) dim 2 into a regular tensor dimension.
// Said tensor dimension has a logical dim of 1 in the returned Tensor.
static std::pair<Tensor,int64_t> remove_existing_batch_dim(const Tensor& self, int64_t level) {
  const auto* batched = maybeGetBatched(self);
  TORCH_INTERNAL_ASSERT(batched != nullptr);
  auto bdims = batched->bdims();
  if (bdims.size() == 1) {
    TORCH_INTERNAL_ASSERT(bdims[0].level() == level);
    return std::make_pair(batched->value(), bdims[0].dim());
  }
  BatchDims new_bdims;
  int64_t newly_exposed_physical_dim;
  new_bdims.reserve(bdims.size() - 1);
  for (const auto& bdim : bdims) {
    if (bdim.level() == level) {
      newly_exposed_physical_dim = bdim.dim();
    } else {
      new_bdims.push_back(bdim);
    }
  }
  int64_t num_batch_dims_before_newly_exposed_physical_dim = std::count_if(
      new_bdims.begin(), new_bdims.end(),
      [&](const BatchDim& bdim) {
        return bdim.dim() < newly_exposed_physical_dim;
      });
  int64_t newly_exposed_logical_dim =
      newly_exposed_physical_dim - num_batch_dims_before_newly_exposed_physical_dim;
  auto result_tensor = makeBatched(batched->value(), std::move(new_bdims));
  return std::make_pair(std::move(result_tensor), newly_exposed_logical_dim);
}

// Poor man's version of np.moveaxis. Moves the dimension at `dst` to `src`
// while preserving the order of other existing dimensions.
// We should probably add np.moveaxis (it is more general) to PyTorch. (#36048)
// When we do, replace the following with it.
static Tensor movedim(const Tensor& self, int64_t src, int64_t dst) {
  auto logical_dim = self.dim();
  src = maybe_wrap_dim(src, logical_dim);
  dst = maybe_wrap_dim(dst, logical_dim);
  if (src == dst) {
    return self;
  }
  VmapDimVector permutation;
  permutation.reserve(logical_dim);
  for (int64_t dim = 0; dim < logical_dim; dim++) {
    if (dim == src) {
      continue;
    }
    permutation.push_back(dim);
  }
  permutation.insert(permutation.begin() + dst, src);
  return self.permute(permutation);
}

// Removes the batch dim with level `level` from `self`. If this causes the
// last batch dim to be removed from a BatchedTensor, then this returns a
// regular Tensor.
//
// If the `level` of the batch dim to remove does not exist in `self`, then we
// add the batch dim in. This can happen if `self` didn't interact with a tensor
// inside the vmap level, for example,
//     self = torch.randn(3)
//     y = torch.randn(5)
//     out = vmap(lambda x: vmap(lambda y: x)(y))(self)
//     assert out.shape == (3, 5)
// Inside the inner vmap, `x` is a BatchedTensor with a single batch dimension
// corresponding to the *outer* vmap level and it doesn't have any dimensions that
// correspond to the inner vmap level so we need to create one for the user.
//
// `out_dim` controls where we should put the batch dimension in the output tensor.
Tensor _remove_batch_dim(const Tensor& self, int64_t level, int64_t batch_size, int64_t out_dim) {
  if (!has_level(self, level)) {
    auto self_sizes = self.sizes();
    VmapDimVector expanded_sizes(self_sizes.begin(), self_sizes.end());
    expanded_sizes.insert(expanded_sizes.begin() + out_dim, batch_size);
    return self.expand(expanded_sizes);
  }

  Tensor self_without_bdim;
  int64_t newly_exposed_logical_dim;
  std::tie(self_without_bdim, newly_exposed_logical_dim) = remove_existing_batch_dim(self, level);
  return movedim(self_without_bdim, newly_exposed_logical_dim, out_dim);
}

} // namespace native
} // namespace at