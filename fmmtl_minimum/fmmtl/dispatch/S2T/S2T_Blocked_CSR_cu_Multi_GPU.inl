#pragma once
#include <cuda_runtime.h>
//#include <thrust/system/cuda/detail/detail/uninitialized.h>
//using thrust::system::cuda::detail::detail::uninitialized_array;
template<class T, size_t N>
struct uninitialized_array
{
  typedef T value_type;
  typedef T ref[N];
  enum {SIZE = N};
 private:
  char data_[N * sizeof(T)];

 public:
  __host__ __device__       T* data()       { return data_; }
  __host__ __device__ const T* data() const { return data_; }
  __host__ __device__       T& operator[](unsigned idx)       { return ((T*)data_)[idx]; }
  __host__ __device__ const T& operator[](unsigned idx) const { return ((T*)data_)[idx]; }
  __host__ __device__       T& operator[](     int idx)       { return ((T*)data_)[idx]; }
  __host__ __device__ const T& operator[](     int idx) const { return ((T*)data_)[idx]; }
  __host__ __device__ unsigned size() const { return N; }
  __host__ __device__ operator ref&() { return *reinterpret_cast<ref*>(data_); }
  __host__ __device__ ref& get_ref() { return (ref&)*this; }
};

#include <thrust/pair.h>
#include "fmmtl/meta/kernel_traits.hpp"

/** CSR Blocked S2T in CUDA
* @brief  Computes the kernel matrix-vector product using a blocked CSR-like
*     format. Each target range has a range of source ranges to compute.
*
* @param[in] K                 The kernel to generate matrix elements.
* @param[in] target_range      Maps blockIdx.x to pair<uint,uint> representing
the [start,end) of targets for this threadblock.
* @param[in] source_range_ptr  Maps blockIdx.x to pair<uint,uint> representing
the [start,end) of the source ranges interacting
the target range for this threadblock.
* @param[in] source_range      Maps each index of the source_range_ptr range to
a [start,end) of a source range to interact
*                               with each target of this threadblock..
*
* @param[in]     source  Array of sources to index into
* @param[in]     charge  Array of charges associated with sources to index into
* @param[in]     target  Array of targets to index into
* @param[in,out] result  Array of results associated with targets to accumulate
*
* @pre For all k, target_range[k].second - target_range[k].first <= blockDim.x
*       -- One target/thread = each target range is smaller than the blocksize
*/
template <unsigned BLOCKDIM,
  typename _Kernel_Ty_,
  typename Indexable1,
  typename Indexable2,
  typename Indexable3>
  __global__ void
  blocked_p2p(const _Kernel_Ty_ K,
              Indexable1 target_range,
              Indexable2 source_range_ptr,
              Indexable3 source_range,
              const typename KernelTraits<_Kernel_Ty_>::source_type* source,
              const typename KernelTraits<_Kernel_Ty_>::charge_type* charge,
              const typename KernelTraits<_Kernel_Ty_>::target_type* target,
              typename KernelTraits<_Kernel_Ty_>::result_type* result,unsigned Tbox_offset=0) {
  typedef typename KernelTraits<_Kernel_Ty_>::source_type source_type;
  typedef typename KernelTraits<_Kernel_Ty_>::charge_type charge_type;
  typedef typename KernelTraits<_Kernel_Ty_>::target_type target_type;
  typedef typename KernelTraits<_Kernel_Ty_>::result_type result_type;

  typedef thrust::pair<unsigned, const unsigned> upair;

  // Allocate shared memory -- prevent initialization of non-POD
  __shared__ uninitialized_array<source_type, BLOCKDIM> sh_s;
  __shared__ uninitialized_array<charge_type, BLOCKDIM> sh_c;

  // Get the target range this block is responsible for
  upair t_range = target_range[blockIdx.x+Tbox_offset];
  // The target index this thread is responsible for
  t_range.first += threadIdx.x;

  // Get the range of source ranges this block is responsible for
  upair s_range_ptr = source_range_ptr[blockIdx.x+Tbox_offset];

  // Each thread is assigned to one target in the target range
  result_type r = result_type()-result_type();
  target_type t = ((t_range.first < t_range.second)
                   ? target[t_range.first] : target_type());


  int nbodys_in_smem = 0;


  // For each source range
  for (; s_range_ptr.first < s_range_ptr.second;) {
    //before each load, reset body count.
    nbodys_in_smem = 0;
    // load as much source boxes into the shared memory as possible
    for (; s_range_ptr.first < s_range_ptr.second; s_range_ptr.first++) {
      //try to load this sourcebox
      int load_offset = source_range[s_range_ptr.first].first;
      int nbody_in_this_sbox = source_range[s_range_ptr.first].second
        - load_offset;

      if (nbodys_in_smem + nbody_in_this_sbox> BLOCKDIM) {
        break;
      }
      //this box can be loaded
      if (threadIdx.x<nbody_in_this_sbox) {
        sh_s[nbodys_in_smem + threadIdx.x] = source[load_offset + threadIdx.x];
        sh_c[nbodys_in_smem + threadIdx.x] = charge[load_offset + threadIdx.x];
      }
      nbodys_in_smem += nbody_in_this_sbox;
    }
    //after this loop, there is either no sbox left or this round of loading is finished.
    __syncthreads();


    //end of this round loading data.
    // Read up to blockDim.x sources into shared memory

    // Each target computes its interaction with each source in smem
    if (t_range.first < t_range.second) {
      while (nbodys_in_smem>0) {
        --nbodys_in_smem;
        r += K(t, sh_s[nbodys_in_smem]) * sh_c[nbodys_in_smem];
      }
    }
    __syncthreads();
    // TODO: Unroll to prevent an extra __syncthreads()?
  }

  if (t_range.first < t_range.second)
    result[t_range.first] += r;
}
