/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#define EIGEN_USE_THREADS

#if GOOGLE_CUDA //|| TENSORFLOW_USE_ROCM
#define EIGEN_USE_GPU
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

#include "tensorflow/core/framework/bounds_check.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/kernels/cwise_ops_common.h"
#include "tensorflow/core/platform/prefetch.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

#ifdef TENSORFLOW_USE_SYCL
typedef Eigen::SyclDevice SYCLDevice;
#endif  // TENSORFLOW_USE_SYCL

namespace functor {
template <typename Device, typename T>
struct SelectScalarHandler;
}  // namespace functor

template <typename Device, typename T>
class SelectOp : public OpKernel {
 public:
  explicit SelectOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* ctx) override {
    const Tensor* cond;
    const Tensor* then;
    const Tensor* else_;
    OP_REQUIRES_OK(ctx, ctx->input("condition", &cond));
    OP_REQUIRES_OK(ctx, ctx->input("t", &then));
    OP_REQUIRES_OK(ctx, ctx->input("e", &else_));

    if (TensorShapeUtils::IsScalar(cond->shape())) {
      ComputeScalar(ctx, cond, then, else_);
      return;
    }

    bool broadcasting = (TensorShapeUtils::IsVector(cond->shape()) &&
                         (!TensorShapeUtils::IsVector(then->shape()) ||
			 !TensorShapeUtils::IsVector(else_->shape())));

    bool then_scalar = TensorShapeUtils::IsScalar(then->shape());
    bool else_scalar = TensorShapeUtils::IsScalar(else_->shape());

    OP_REQUIRES(
        ctx, !then_scalar || !else_scalar,
        errors::InvalidArgument("'then and else cann't be scalar at the same time"));

    if (broadcasting) {
      if (then_scalar || else_scalar) {
        ComputeBroadcastingScalar(ctx, cond, then, else_);
      } else {
        ComputeBroadcasting(ctx, cond, then, else_);
      }
    } else {
      if (then_scalar || else_scalar) {
        ComputeElementwiseScalar(ctx, cond, then, else_);
      } else {
        ComputeElementwise(ctx, cond, then, else_);
      }
    }
  }

 protected:
  void CheckParam(OpKernelContext* ctx, const Tensor* cond, const Tensor* tensor) {
    // Preliminary validation of sizes.
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(cond->shape()),
        errors::InvalidArgument("'cond' must be a vector, but saw shape: ",
                                cond->shape().DebugString()));
    OP_REQUIRES(
        ctx,
        FastBoundsCheck(cond->NumElements(),
                        std::numeric_limits<Eigen::DenseIndex>::max()),
        errors::InvalidArgument("cond vector larger than ",
                                std::numeric_limits<Eigen::DenseIndex>::max()));
    OP_REQUIRES(
        ctx,
        FastBoundsCheck(tensor->flat_outer_dims<T>().dimension(1),
                        std::numeric_limits<Eigen::DenseIndex>::max()),
        errors::InvalidArgument("flat outer dims dim 1 size >= ",
                                std::numeric_limits<Eigen::DenseIndex>::max()));
    OP_REQUIRES(ctx, TensorShapeUtils::IsVectorOrHigher(tensor->shape()),
                errors::InvalidArgument(
                    "'tensor' must be at least a vector, but saw shape: ",
                    tensor->shape().DebugString()));
    OP_REQUIRES(
        ctx, tensor->shape().dim_size(0) == cond->NumElements(),
        errors::InvalidArgument(
            "Number of batches of 'tensor' must match size of 'cond', but saw: ",
            tensor->shape().dim_size(0), " vs. ", cond->NumElements()));
  }

  void ComputeBroadcastingScalar(OpKernelContext* ctx, const Tensor* cond,
                           const Tensor* then, const Tensor* else_) {
    bool then_scalar = TensorShapeUtils::IsScalar(then->shape());
    if (then_scalar) {
      CheckParam(ctx, cond, else_);
    } else {
      CheckParam(ctx, cond, then);
    }
    OP_REQUIRES_OK(ctx, ctx->status());
    Tensor* output = nullptr;
    if (then_scalar) {
      OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                              {"e"}, "output", else_->shape(), &output));
    } else {
      OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                              {"t"}, "output", then->shape(), &output));
    }
    if (output->NumElements() > 0) {
      functor::BatchSelect4BroadcastingScalarFunctor<Device, T> func;
      OP_REQUIRES(ctx, output->flat_outer_dims<T>().data(), errors::InvalidArgument("Output flat data is NULL"));
      OP_REQUIRES(ctx, cond->vec<bool>().data(), errors::InvalidArgument("Condition flat data is NULL"));
      OP_REQUIRES(ctx, then->flat_outer_dims<T>().data(), errors::InvalidArgument("Then flat data is NULL"));
      OP_REQUIRES(ctx, else_->flat_outer_dims<T>().data(), errors::InvalidArgument("Else flat data is NULL"));
      func(ctx->eigen_device<Device>(), output->flat_outer_dims<T>(),
           cond->vec<bool>(), then->flat_outer_dims<T>(),
           else_->flat_outer_dims<T>(),
           then_scalar);
    }
  }

  void ComputeBroadcasting(OpKernelContext* ctx, const Tensor* cond,
                           const Tensor* then, const Tensor* else_) {
    // Preliminary validation of sizes.
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsVector(cond->shape()),
        errors::InvalidArgument("'cond' must be a vector, but saw shape: ",
                                cond->shape().DebugString()));
    OP_REQUIRES(
        ctx,
        FastBoundsCheck(cond->NumElements(),
                        std::numeric_limits<Eigen::DenseIndex>::max()),
        errors::InvalidArgument("cond vector larger than ",
                                std::numeric_limits<Eigen::DenseIndex>::max()));
    OP_REQUIRES(
        ctx,
        FastBoundsCheck(then->flat_outer_dims<T>().dimension(1),
                        std::numeric_limits<Eigen::DenseIndex>::max()),
        errors::InvalidArgument("flat outer dims dim 1 size >= ",
                                std::numeric_limits<Eigen::DenseIndex>::max()));

    OP_REQUIRES(ctx, TensorShapeUtils::IsVectorOrHigher(then->shape()),
                errors::InvalidArgument(
                    "'then' must be at least a vector, but saw shape: ",
                    then->shape().DebugString()));
    OP_REQUIRES(
        ctx, then->shape().dim_size(0) == cond->NumElements(),
        errors::InvalidArgument(
            "Number of batches of 'then' must match size of 'cond', but saw: ",
            then->shape().dim_size(0), " vs. ", cond->NumElements()));
    OP_REQUIRES(
        ctx, then->shape().IsSameSize(else_->shape()),
        errors::InvalidArgument(
            "'then' and 'else' must have the same size.  but received: ",
            then->shape().DebugString(), " vs. ",
            else_->shape().DebugString()));

    Tensor* output = nullptr;
    OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                            {"t", "e"}, "output", then->shape(), &output));
    if (output->NumElements() > 0) {
      functor::BatchSelectFunctor<Device, T> func;
      func(ctx->eigen_device<Device>(), output->flat_outer_dims<T>(),
           cond->vec<bool>(), then->flat_outer_dims<T>(),
           else_->flat_outer_dims<T>());
    }
  }

  void ComputeElementwiseScalar(OpKernelContext* ctx, const Tensor* cond,
                          const Tensor* then, const Tensor* else_) {
    bool then_scalar = TensorShapeUtils::IsScalar(then->shape());
    Tensor* output = nullptr;
    if (then_scalar) {
      OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                              {"e"}, "output", else_->shape(), &output));
    } else {
      OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                              {"t"}, "output", then->shape(), &output));
    }
    if (output->NumElements() > 0) {
      OP_REQUIRES(ctx, output->flat<T>().data(), errors::InvalidArgument("Output flat data is NULL"));
      OP_REQUIRES(ctx, cond->flat<bool>().data(), errors::InvalidArgument("Condition flat data is NULL"));
      OP_REQUIRES(ctx, then->flat<T>().data(), errors::InvalidArgument("Then flat data is NULL"));
      OP_REQUIRES(ctx, else_->flat<T>().data(), errors::InvalidArgument("Else flat data is NULL"));
      functor::Select4ElementScalarFunctor<Device, T> func;
      func(ctx->eigen_device<Device>(), output->flat<T>(), cond->flat<bool>(),
           then->flat<T>(), else_->flat<T>(),
           then_scalar);
    }
  }

  void ComputeElementwise(OpKernelContext* ctx, const Tensor* cond,
                          const Tensor* then, const Tensor* else_) {
    if (!ctx->ValidateInputsAreSameShape(this)) return;
    Tensor* output = nullptr;
    OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                            {"t", "e"}, "output", then->shape(), &output));
    if (output->NumElements() > 0) {
      functor::SelectFunctor<Device, T> func;
      func(ctx->eigen_device<Device>(), output->flat<T>(), cond->flat<bool>(),
           then->flat<T>(), else_->flat<T>());
    }
  }

  void ComputeScalar(OpKernelContext* ctx, const Tensor* cond,
                     const Tensor* then, const Tensor* else_) {
    OP_REQUIRES(
        ctx, then->shape().IsSameSize(else_->shape()),
        errors::InvalidArgument(
            "'then' and 'else' must have the same size.  but received: ",
            then->shape().DebugString(), " vs. ",
            else_->shape().DebugString()));

    functor::SelectScalarHandler<Device, T> handler;
    handler(ctx, cond, then, else_);
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(SelectOp);
};
template <typename Device, typename T>
class SelectV2Op : public OpKernel {
 public:
  explicit SelectV2Op(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* ctx) override {
    const Tensor* cond;
    const Tensor* then;
    const Tensor* else_;
    OP_REQUIRES_OK(ctx, ctx->input("condition", &cond));
    OP_REQUIRES_OK(ctx, ctx->input("t", &then));
    OP_REQUIRES_OK(ctx, ctx->input("e", &else_));

    // The `cond`, `then`, and `else` are broadcastable (bcast.IsValid()),
    // This matches the behavior of numpy.
    // TODO (yongtang): Consolidate into n-ary broadcast, instead of multiple
    // 2-ary broadcast.

    // Combine `then` and `else`.
    BCast then_else_bcast(BCast::FromShape(then->shape()),
                          BCast::FromShape(else_->shape()), false);
    OP_REQUIRES(ctx, then_else_bcast.IsValid(),
                errors::InvalidArgument(
                    "then ", then->shape().DebugString(), " and else ",
                    else_->shape().DebugString(), " must be broadcastable"));
    // Combine `cond` with `then` and `else`.
    BCast bcast(
        BCast::FromShape(cond->shape()),
        BCast::FromShape(BCast::ToShape(then_else_bcast.output_shape())),
        false);
    OP_REQUIRES(ctx, bcast.IsValid(),
                errors::InvalidArgument(
                    "condition ", cond->shape().DebugString(), ", then ",
                    then->shape().DebugString(), ", and else ",
                    else_->shape().DebugString(), " must be broadcastable"));

    // Broadcast `cond`, `then` and `else` to combined shape,
    // in order to obtain the reshape.
    BCast cond_bcast(BCast::FromShape(BCast::ToShape(bcast.output_shape())),
                     BCast::FromShape(cond->shape()), false);
    BCast then_bcast(BCast::FromShape(BCast::ToShape(bcast.output_shape())),
                     BCast::FromShape(then->shape()), false);
    BCast else_bcast(BCast::FromShape(BCast::ToShape(bcast.output_shape())),
                     BCast::FromShape(else_->shape()), false);
    OP_REQUIRES(
        ctx,
        cond_bcast.IsValid() && then_bcast.IsValid() && else_bcast.IsValid(),
        errors::InvalidArgument("condition ", cond->shape().DebugString(),
                                ", then ", then->shape().DebugString(),
                                ", and else ", else_->shape().DebugString(),
                                " must be broadcastable"));

    // Combined shape should be the final shape.
    OP_REQUIRES(
        ctx,
        cond_bcast.output_shape() == bcast.output_shape() &&
            then_bcast.output_shape() == bcast.output_shape() &&
            else_bcast.output_shape() == bcast.output_shape(),
        errors::InvalidArgument("condition ", cond->shape().DebugString(),
                                ", then ", then->shape().DebugString(),
                                ", and else ", else_->shape().DebugString(),
                                " must be broadcastable to the same shape"));

    Tensor* output = nullptr;
    const TensorShape output_shape = BCast::ToShape(bcast.output_shape());
    OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                            {"t", "e"}, "output", output_shape, &output));

    if (output->NumElements() == 0) {
      return;
    }

#define HANDLE_DIM(NDIMS)                                            \
  {                                                                  \
    functor::BCastSelectFunctor<Device, T, NDIMS> func;              \
    func(ctx->eigen_device<Device>(),                                \
         output->shaped<T, NDIMS>(bcast.result_shape()),             \
         cond->template shaped<bool, NDIMS>(cond_bcast.y_reshape()), \
         then->template shaped<T, NDIMS>(then_bcast.y_reshape()),    \
         else_->template shaped<T, NDIMS>(else_bcast.y_reshape()),   \
         BCast::ToIndexArray<NDIMS>(cond_bcast.y_bcast()),           \
         BCast::ToIndexArray<NDIMS>(then_bcast.y_bcast()),           \
         BCast::ToIndexArray<NDIMS>(else_bcast.y_bcast()));          \
  }

    const int ndims = static_cast<int>(bcast.result_shape().size());
    switch (ndims) {
      case 1:
        HANDLE_DIM(1);
        break;
      case 2:
        HANDLE_DIM(2);
        break;
      case 3:
        HANDLE_DIM(3);
        break;
      case 4:
        HANDLE_DIM(4);
        break;
      case 5:
        HANDLE_DIM(5);
        break;
      case 6:
        HANDLE_DIM(6);
        break;
      case 7:
        HANDLE_DIM(7);
        break;
      case 8:
        HANDLE_DIM(8);
        break;
      default:
        ctx->SetStatus(errors::Unimplemented(
            "Broadcast between ", ctx->input(0).shape().DebugString(), " and ",
            ctx->input(1).shape().DebugString(), " is not supported yet."));
        break;
    }
    return;
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(SelectV2Op);
};

#define REGISTER_SELECT(type)                                        \
  REGISTER_KERNEL_BUILDER(                                           \
      Name("Select").Device(DEVICE_CPU).TypeConstraint<type>("T"),   \
      SelectOp<CPUDevice, type>);                                    \
  REGISTER_KERNEL_BUILDER(                                           \
      Name("SelectV2").Device(DEVICE_CPU).TypeConstraint<type>("T"), \
      SelectV2Op<CPUDevice, type>);

TF_CALL_ALL_TYPES(REGISTER_SELECT);

#if GOOGLE_CUDA //|| TENSORFLOW_USE_ROCM

// Registration of the GPU implementations.
#define REGISTER_SELECT_GPU(type)                                    \
  REGISTER_KERNEL_BUILDER(                                           \
      Name("Select").Device(DEVICE_GPU).TypeConstraint<type>("T"),   \
      SelectOp<GPUDevice, type>);                                    \
  REGISTER_KERNEL_BUILDER(                                           \
      Name("SelectV2").Device(DEVICE_GPU).TypeConstraint<type>("T"), \
      SelectV2Op<GPUDevice, type>);

REGISTER_SELECT_GPU(bool);
REGISTER_SELECT_GPU(Eigen::half);
REGISTER_SELECT_GPU(float);
REGISTER_SELECT_GPU(double);
REGISTER_SELECT_GPU(int32);
REGISTER_SELECT_GPU(int64);
REGISTER_SELECT_GPU(complex64);
REGISTER_SELECT_GPU(complex128);

#undef REGISTER_SELECT_GPU

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

#ifdef TENSORFLOW_USE_SYCL
// Registration of the SYCL implementations.
#define REGISTER_SELECT_SYCL(type)                                    \
  REGISTER_KERNEL_BUILDER(                                            \
      Name("Select").Device(DEVICE_SYCL).TypeConstraint<type>("T"),   \
      SelectOp<SYCLDevice, type>);                                    \
  REGISTER_KERNEL_BUILDER(                                            \
      Name("SelectV2").Device(DEVICE_SYCL).TypeConstraint<type>("T"), \
      SelectOp<SYCLDevice, type>);

REGISTER_SELECT_SYCL(float);
REGISTER_SELECT_SYCL(double);
REGISTER_SELECT_SYCL(int32);
REGISTER_SELECT_SYCL(int64);
#undef REGISTER_SELECT_SYCL
#endif  // TENSORFLOW_USE_SYCL

namespace functor {
constexpr size_t float_alignment = 16;

// CPU Specializations of Select functors.
template <typename Device, typename T>
struct SelectFunctorBase {
  void operator()(const Device& d, typename TTypes<T>::Flat out,
                  typename TTypes<bool>::ConstFlat cond_flat,
                  typename TTypes<T>::ConstFlat then_flat,
                  typename TTypes<T>::ConstFlat else_flat) {
    Assign(d, out, cond_flat.select(then_flat, else_flat));
  }
};

template <typename T>
struct Select4ElementScalarFunctor<CPUDevice, T> {
  void operator()(const CPUDevice& d,
                  typename TTypes<T>::Flat out,
                  TTypes<bool>::ConstFlat cond_flat,
                  typename TTypes<T>::ConstFlat then_flat,
                  typename TTypes<T>::ConstFlat else_flat,
                  bool then_scalar) {
    T* output = out.data();
    const bool* c = cond_flat.data();
    const T* t = then_flat.data();
    const T* e = else_flat.data();
    auto work_when_then_scalar = [output, c, t, e](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i;
        if (offset < end - 1) {
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&t[0]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&e[offset + 1]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&c[i + 1]));
        }
        if (c[i]) {
          output[offset] = t[0];
        } else {
          output[offset] = e[offset];
        }
      }
    };
    auto work_when_else_scalar = [output, c, t, e](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i;
        if (offset < end - 1) {
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&t[offset + 1]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&e[0]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&c[i + 1]));
        }
        if (c[i]) {
          output[offset] = t[offset];
        } else {
          output[offset] = e[0];
        }
      }
    };
    auto cost = Eigen::TensorOpCost(sizeof(T) * 1 * 2,  // ld bytes
                                    sizeof(T) * 1,      // st bytes
                                    1);  // compute cycles
    if (then_scalar) {
      d.parallelFor(out.size(), cost, work_when_then_scalar);
    } else {
      d.parallelFor(out.size(), cost, work_when_else_scalar);
    }
  }
};
#ifdef TENSORFLOW_USE_SYCL
template <typename T>
struct Select4ElementScalarFunctor<SYCLDevice, T> {
  void operator()(const CPUDevice& d,
                  typename TTypes<T>::Flat out,
                  TTypes<bool>::ConstFlat cond_flat,
                  typename TTypes<T>::ConstFlat then_flat,
                  typename TTypes<T>::ConstFlat else_flat,
                  bool then_scalar) {
    typename TTypes<T>::ConstFlat scalar_flat;
    typename TTypes<T>::ConstFlat non_scalar_flat;
    if (then_scalar) {
      scalar_flat = then_flat;
      non_scalar_flat = else_flat;
    } else {
      scalar_flat = else_flat;
      non_scalar_flat = then_flat;
    }
#if !defined(EIGEN_HAS_INDEX_LIST)
    Eigen::array<Eigen::DenseIndex, 1> else_broadcast_dims{{scalar_flat.dimension(0)}};
#else
    Eigen::IndexList<Eigen::type2index<1>, Eigen::DenseIndex> else_broadcast_dims;
    broadcast_dims.set(0, scalar_flat.dimension(0));
#endif
    Assign(d, out,
        cond_flat.select(scalar_flat, non_scalar_flat.broadcast(else_broadcast_dims)));
  }
};
#endif  // TENSORFLOW_USE_SYCL

template <typename Device>
struct SelectFunctorBase<Device, float> {
  void operator()(const Device& d, typename TTypes<float>::Flat out,
                  typename TTypes<bool>::ConstFlat cond_flat,
                  typename TTypes<float>::ConstFlat then_flat,
                  typename TTypes<float>::ConstFlat else_flat) {
#if defined(__GNUC__) && (__GNUC__ >6) && (__AVX512F__)
    const size_t num = cond_flat.size();
    const bool* c = cond_flat.data();
    const float* t = then_flat.data();
    const float* e = else_flat.data();
    float* output = out.data();
    size_t quotient = num / float_alignment;
    size_t remainder = num - (quotient * float_alignment);
    __m128i zeros = _mm_setzero_si128();
    size_t offset = 0;

    for (size_t j = 0; j < quotient; ++j) {
      __m128i cb = _mm_mask_loadu_epi8(zeros, 0xffff, c + offset);
      __mmask16 mask = _mm_cmpeq_epu8_mask(zeros, cb);
      __m512 src = _mm512_loadu_ps(t + offset);
      __m512 tmp = _mm512_mask_loadu_ps(src, mask, e + offset);
      _mm512_storeu_ps(output + offset,  tmp);
      offset += float_alignment;
    }

    if (remainder != 0) {
      __mmask16 k = (remainder >= float_alignment)
          ? 0xffff : 0xffff >> (float_alignment - remainder);
      __m128i cb = _mm_mask_loadu_epi8(zeros, k, c + offset);
      __mmask16 mask = _mm_mask_cmpeq_epu8_mask(k, zeros, cb);
      __m512 src  = _mm512_mask_loadu_ps(_mm512_setzero_ps(), k, t + offset);
      __m512 tmp = _mm512_mask_loadu_ps(src, mask, e + offset);
      _mm512_mask_storeu_ps(output + offset, k, tmp);
    }

    Assign(d, out, out);
#else
    Assign(d, out, cond_flat.select(then_flat, else_flat));
#endif
  }
};

template <typename T>
struct SelectFunctor<CPUDevice, T> : SelectFunctorBase<CPUDevice, T> {};
#ifdef TENSORFLOW_USE_SYCL
template <typename T>
struct SelectFunctor<SYCLDevice, T> : SelectFunctorBase<SYCLDevice, T> {};
#endif  // TENSORFLOW_USE_SYCL

template <typename Device, typename T>
struct SelectScalarHandler {
  void operator()(OpKernelContext* ctx, const Tensor* cond, const Tensor* then,
                  const Tensor* else_) {
    Tensor* output = nullptr;
    OP_REQUIRES_OK(ctx, ctx->forward_input_or_allocate_output(
                            {"t", "e"}, "output", then->shape(), &output));

    if (output->NumElements() > 0) {
      functor::SelectScalarFunctor<Device, T> func;
      TTypes<bool>::ConstScalar cond_scalar = cond->scalar<bool>();
      func(ctx->eigen_device<Device>(), output->flat<T>(), cond_scalar,
           then->flat<T>(), else_->flat<T>());
    }
  }
};

// Specilization for CPU device. Forward input to output depending on the `cond`
// value.
// TODO(sjhwang): Consider specializing for GPUDevice as well by using
// GPUDevice::memcpyDeviceToHost() to fetch bool value.
template <typename T>
struct SelectScalarHandler<CPUDevice, T> {
  void operator()(OpKernelContext* ctx, const Tensor* cond, const Tensor* then,
                  const Tensor* else_) {
    if (cond->scalar<bool>()()) {
      OP_REQUIRES_OK(ctx, ctx->set_output("output", *then));
    } else {
      OP_REQUIRES_OK(ctx, ctx->set_output("output", *else_));
    }
  }
};

#ifdef TENSORFLOW_USE_SYCL
template <typename Device, typename T>
struct SelectScalarFunctorBase {
  void operator()(const Device& d, typename TTypes<T>::Flat out,
                  TTypes<bool>::ConstScalar cond,
                  typename TTypes<T>::ConstFlat then_flat,
                  typename TTypes<T>::ConstFlat else_flat) {
    out.device(d) = cond() ? then_flat : else_flat;
  }
};

template <typename T>
struct SelectScalarFunctor<SYCLDevice, T>
    : SelectScalarFunctorBase<SYCLDevice, T> {};
#endif  // TENSORFLOW_USE_SYCL

template <typename Device, typename T>
struct BatchSelectFunctorBase {
  void operator()(const Device& d,
                  typename TTypes<T>::Matrix output_flat_outer_dims,
                  TTypes<bool>::ConstVec cond_vec,
                  typename TTypes<T>::ConstMatrix then_flat_outer_dims,
                  typename TTypes<T>::ConstMatrix else_flat_outer_dims) {
    const Eigen::DenseIndex batch = cond_vec.size();
    const Eigen::DenseIndex all_but_batch = then_flat_outer_dims.dimension(1);

#if !defined(EIGEN_HAS_INDEX_LIST)
    Eigen::array<Eigen::DenseIndex, 2> broadcast_dims{{1, all_but_batch}};
    Eigen::Tensor<Eigen::DenseIndex, 2>::Dimensions reshape_dims{{batch, 1}};
#else
    Eigen::IndexList<Eigen::type2index<1>, Eigen::DenseIndex> broadcast_dims;
    broadcast_dims.set(1, all_but_batch);
    Eigen::IndexList<Eigen::DenseIndex, Eigen::type2index<1> > reshape_dims;
    reshape_dims.set(0, batch);
#endif

    Assign(d, output_flat_outer_dims,
           cond_vec.reshape(reshape_dims)
               .broadcast(broadcast_dims)
               .select(then_flat_outer_dims, else_flat_outer_dims));
  }
};

template <typename Device, typename T>
struct BatchSelect4BroadcastingScalarFunctorBase {
  void operator()(const Device& d,
                  typename TTypes<T>::Matrix output_flat_outer_dims,
                  TTypes<bool>::ConstVec cond_vec,
                  typename TTypes<T>::ConstMatrix then_flat_outer_dims,
                  typename TTypes<T>::ConstMatrix else_flat_outer_dims,
                  bool then_scalar) {
    const Eigen::DenseIndex batch = cond_vec.size();
    typename TTypes<T>::ConstMatrix scalar_flat_outer_dims;
    typename TTypes<T>::ConstMatrix non_scalar_flat_outer_dims;
    if (then_scalar) {
      non_scalar_flat_outer_dims = else_flat_outer_dims;
      scalar_flat_outer_dims = then_flat_outer_dims;
    } else {
      non_scalar_flat_outer_dims = then_flat_outer_dims;
      scalar_flat_outer_dims = else_flat_outer_dims;
    }
    const Eigen::DenseIndex all_but_batch = non_scalar_flat_outer_dims.dimension(1);
#if !defined(EIGEN_HAS_INDEX_LIST)
    Eigen::array<Eigen::DenseIndex, 2> else_broadcast_dims{{non_scalar_flat_outer_dims.dimension(0), non_scalar_flat_outer_dims.dimension(1)}};
    Eigen::array<Eigen::DenseIndex, 2> broadcast_dims{{1, all_but_batch}};
    Eigen::Tensor<Eigen::DenseIndex, 2>::Dimensions reshape_dims{{batch, 1}};
#else
    Eigen::IndexList<Eigen::type2index<1>, Eigen::DenseIndex> else_broadcast_dims;
    else_broadcast_dims.set(0, non_scalar_flat_outer_dims.dimension(0));
    else_broadcast_dims.set(1, non_scalar_flat_outer_dims.dimension(1));
    Eigen::IndexList<Eigen::type2index<1>, Eigen::DenseIndex> broadcast_dims;
    broadcast_dims.set(1, all_but_batch);
    Eigen::IndexList<Eigen::DenseIndex, Eigen::type2index<1> > reshape_dims;
    reshape_dims.set(0, batch);
#endif
    Assign(d, output_flat_outer_dims,
           cond_vec.reshape(reshape_dims)
               .broadcast(broadcast_dims)
               .select(non_scalar_flat_outer_dims, scalar_flat_outer_dims.broadcast(else_broadcast_dims)));
  }
};
// A fast implementation on CPU, using loop to get rid of broadcasting.
template <typename T>
struct BatchSelect4BroadcastingScalarFunctor<CPUDevice, T> {
  void operator()(const CPUDevice& d,
                  typename TTypes<T>::Matrix output_flat_outer_dims,
                  TTypes<bool>::ConstVec cond_vec,
                  typename TTypes<T>::ConstMatrix then_flat_outer_dims,
                  typename TTypes<T>::ConstMatrix else_flat_outer_dims,
                  bool then_scalar) {
    const size_t batch = cond_vec.size();
    size_t batch_size = 0;
    if (then_scalar) {
      batch_size = else_flat_outer_dims.size() / batch;
    } else {
      batch_size = then_flat_outer_dims.size() / batch;
    }
    T* output = output_flat_outer_dims.data();
    const bool* c = cond_vec.data();
    const T* t = then_flat_outer_dims.data();
    const T* e = else_flat_outer_dims.data();
    auto work_when_then_scalar = [batch_size, output, c, t, e](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i * batch_size;
        if (i < end - 1) {
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&t[0]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&e[offset + batch_size]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&c[i + 1]));
        }
        if (c[i]) {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = t[0];
          }
        } else {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = e[offset + j];
          }
        }
      }
    };
    auto work_when_else_scalar = [batch_size, output, c, t, e](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i * batch_size;
        if (i < end - 1) {
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&t[offset + batch_size]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&e[0]));
          port::prefetch<port::PREFETCH_HINT_NTA>(
              reinterpret_cast<const void*>(&c[i + 1]));
        }
        if (c[i]) {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = t[offset + j];
          }
        } else {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = e[0];
          }
        }
      }
    };
    auto cost = Eigen::TensorOpCost(sizeof(T) * batch_size * 2,  // ld bytes
                                    sizeof(T) * batch_size,      // st bytes
                                    batch_size);  // compute cycles
    if (then_scalar) {
      d.parallelFor(batch, cost, work_when_then_scalar);
    } else {
      d.parallelFor(batch, cost, work_when_else_scalar);
    }
  }
};

// A fast implementation on CPU, using loop to get rid of broadcasting.
template <typename T>
struct BatchSelectFunctor<CPUDevice, T> {
  void operator()(const CPUDevice& d,
                  typename TTypes<T>::Matrix output_flat_outer_dims,
                  TTypes<bool>::ConstVec cond_vec,
                  typename TTypes<T>::ConstMatrix then_flat_outer_dims,
                  typename TTypes<T>::ConstMatrix else_flat_outer_dims) {
    const size_t batch = cond_vec.size();
    const size_t batch_size = then_flat_outer_dims.size() / batch;
    T* output = output_flat_outer_dims.data();
    const bool* c = cond_vec.data();
    const T* t = then_flat_outer_dims.data();
    const T* e = else_flat_outer_dims.data();

    auto work = [batch_size, output, c, t, e](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i * batch_size;
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&t[offset + batch_size]));
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&e[offset + batch_size]));
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&c[i + 1]));
        if (c[i]) {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = t[offset + j];
          }
        } else {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = e[offset + j];
          }
        }
      }
    };
    auto cost = Eigen::TensorOpCost(sizeof(T) * batch_size * 2,  // ld bytes
                                    sizeof(T) * batch_size,      // st bytes
                                    batch_size);  // compute cycles
    d.parallelFor(batch, cost, work);
  }
};

template <typename Device, typename T, int NDIMS>
struct BCastSelectFunctorBase {
  void operator()(const Device& d,
                  typename TTypes<T, NDIMS>::Tensor output_tensor,
                  typename TTypes<bool, NDIMS>::ConstTensor cond_tensor,
                  typename TTypes<T, NDIMS>::ConstTensor then_tensor,
                  typename TTypes<T, NDIMS>::ConstTensor else_tensor,
                  typename Eigen::array<Eigen::DenseIndex, NDIMS> cond_bcast,
                  typename Eigen::array<Eigen::DenseIndex, NDIMS> then_bcast,
                  typename Eigen::array<Eigen::DenseIndex, NDIMS> else_bcast) {
    output_tensor.device(d) = cond_tensor.broadcast(cond_bcast)
                                  .select(then_tensor.broadcast(then_bcast),
                                          else_tensor.broadcast(else_bcast));
  }
};

template <typename T, int NDIMS>
struct BCastSelectFunctor<CPUDevice, T, NDIMS>
    : BCastSelectFunctorBase<CPUDevice, T, NDIMS> {};

// A fast implementation on CPU, using loop to get rid of broadcasting.
template <>
struct BatchSelectFunctor<CPUDevice, float> {
  void operator()(const CPUDevice& d,
                  typename TTypes<float>::Matrix output_flat_outer_dims,
                  TTypes<bool>::ConstVec cond_vec,
                  typename TTypes<float>::ConstMatrix then_flat_outer_dims,
                  typename TTypes<float>::ConstMatrix else_flat_outer_dims) {
    const size_t batch = cond_vec.size();
    const size_t batch_size = then_flat_outer_dims.size() / batch;
    float* output = output_flat_outer_dims.data();
    const bool* c = cond_vec.data();
    const float* t = then_flat_outer_dims.data();
    const float* e = else_flat_outer_dims.data();

#if defined(__GNUC__) && (__GNUC__ >6) && (__AVX512F__)
    size_t quotient = batch_size / float_alignment;
    int remainder = batch_size - (quotient * float_alignment);

    auto work = [batch_size, output, c, t, e, quotient, remainder](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i * batch_size;
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&t[offset + batch_size]));
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&e[offset + batch_size]));
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&c[i + 1]));
        __mmask16 cmask = (c[i] == false) ? 0xffff : 0x0000;  // select t/e
        size_t ofs = 0;

        for (size_t j = 0; j < quotient; ++j) {
          __m512 src = _mm512_loadu_ps(t + offset + ofs);
          __m512 tmp = _mm512_mask_loadu_ps(src, cmask, e + offset + ofs);
          _mm512_storeu_ps(output + offset + ofs,  tmp);
          ofs +=  float_alignment;
        }

        if (remainder != 0) {
          __mmask16 mask = (remainder >= float_alignment)
              ? 0xffff : 0xffff >> (float_alignment - remainder);
          cmask &= mask;
          __m512 src  = _mm512_mask_loadu_ps(_mm512_setzero_ps(), mask, t + offset + ofs);
          __m512 tmp = _mm512_mask_loadu_ps(src, cmask, e + offset + ofs);
          _mm512_mask_storeu_ps(output + offset + ofs, mask, tmp);
        }
      }
    };
#else
    auto work = [batch_size, output, c, t, e](int64 start, int64 end) {
      for (size_t i = start; i < end; ++i) {
        size_t offset = i * batch_size;
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&t[offset + batch_size]));
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&e[offset + batch_size]));
        port::prefetch<port::PREFETCH_HINT_NTA>(
            reinterpret_cast<const void*>(&c[i + 1]));
        if (c[i]) {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = t[offset + j];
          }
        } else {
          for (size_t j = 0; j < batch_size; ++j) {
            output[offset + j] = e[offset + j];
          }
        }
      }
    };
#endif
    auto cost = Eigen::TensorOpCost(sizeof(float) * batch_size * 2,  // ld bytes
                                    sizeof(float) * batch_size,      // st bytes
                                    batch_size);  // compute cycles
    d.parallelFor(batch, cost, work);
  }
};

#ifdef TENSORFLOW_USE_SYCL
template <typename T>
struct BatchSelectFunctor<SYCLDevice, T>
    : BatchSelectFunctorBase<SYCLDevice, T> {};

template <typename T, int NDIMS>
struct BCastSelectFunctor<SYCLDevice, T, NDIMS>
    : BCastSelectFunctorBase<SYCLDevice, T, NDIMS> {};

template <typename T>
struct BatchSelect4BroadcastingScalarFunctor<SYCLDevice, T>
    : BatchSelect4BroadcastingScalarFunctorBase<SYCLDevice, T> {};

#endif  // TENSORFLOW_USE_SYCL

}  // namespace functor

}  // namespace tensorflow
