#pragma once

#include <bulk/thread_group.hpp>
#include <bulk/malloc.hpp>
#include <thrust/reduce.h>
#include <thrust/scan.h>
#include <thrust/execution_policy.h>

namespace bulk
{
namespace detail
{
namespace scan_detail
{


template<unsigned int grainsize, typename Iterator1, typename difference_type, typename Iterator2>
__device__
void copy_n_with_grainsize(Iterator1 first, difference_type n, Iterator2 result)
{
  for(Iterator1 last = first + n;
      first < last;
      first += grainsize, result += grainsize)
  {
    for(unsigned int i = 0; i < grainsize; ++i)
    {
      if(i < (last - first))
      {
        result[i] = first[i];
      }
    }
  }
}


template<typename ThreadGroup, typename Iterator, typename difference_type, typename BinaryFunction>
__device__ void small_inclusive_scan_n(ThreadGroup &g, Iterator first, difference_type n, BinaryFunction binary_op)
{
  typedef typename ThreadGroup::size_type size_type;

  typename thrust::iterator_value<Iterator>::type x;

  size_type tid = g.this_thread.index();

  if(tid < n)
  {
    x = first[tid];
  }

  g.wait();

  for(size_type offset = 1; offset < n; offset += offset)
  {
    if(tid >= offset && tid < n)
    {
      x = binary_op(first[tid - offset], x);
    }

    g.wait();

    if(tid < n)
    {
      first[tid] = x;
    }

    g.wait();
  }
}


template<typename ThreadGroup, typename Iterator, typename difference_type, typename T, typename BinaryFunction>
__device__ T small_exclusive_scan_n(ThreadGroup &g, Iterator first, difference_type n, T init, BinaryFunction binary_op)
{
  typedef typename ThreadGroup::size_type size_type;

  T x;

  size_type tid = g.this_thread.index();

  if(n > 0 && tid == 0)
  {
    *first = binary_op(init, *first);
  }

  g.wait();

  if(tid < n)
  {
    x = first[tid];
  }

  g.wait();

  small_inclusive_scan_n(g, first, n, binary_op);

  T result = n > 0 ? first[n - 1] : init;

  x = (tid == 0 || tid - 1 >= n) ? init : first[tid - 1];

  g.wait();

  if(tid < n)
  {
    first[tid] = x;
  }

  g.wait();

  return result;
}


template<unsigned int size, typename ThreadGroup, typename T, typename BinaryFunction>
__device__ T small_inplace_exclusive_scan_with_buffer(ThreadGroup &g, T *first, T init, T *buffer, BinaryFunction binary_op)
{
  // XXX int is noticeably faster than ThreadGroup::size_type
  typedef int size_type;
  //typedef typename ThreadGroup::size_type size_type;

  // ping points to the most current data
  T *ping = first;
  T *pong = buffer;

  size_type tid = g.this_thread.index();

  if(tid == 0)
  {
    first[0] = binary_op(init, first[0]);
  }

  T x = first[tid];

  g.wait();

  #pragma unroll
  for(size_type offset = 1; offset < size; offset += offset)
  {
    if(tid >= offset)
    {
      x = binary_op(ping[tid - offset], x);
    }

    thrust::swap(ping, pong);

    ping[tid] = x;

    g.wait();
  }

  T result = ping[size - 1];

  x = (tid == 0) ? init : ping[tid - 1];

  g.wait();

  first[tid] = x;

  g.wait();

  return result;
} // end small_inplace_exclusive_scan_with_buffer()


template<std::size_t groupsize, std::size_t grainsize, typename RandomAccessIterator1, typename RandomAccessIterator2, typename T, typename BinaryFunction>
__device__ void inclusive_scan_with_buffer(bulk::static_thread_group<groupsize,grainsize> &g,
                                           RandomAccessIterator1 first, RandomAccessIterator1 last,
                                           RandomAccessIterator2 result,
                                           T carry_in,
                                           BinaryFunction binary_op,
                                           void *buffer)
{
  typedef typename thrust::iterator_value<RandomAccessIterator1>::type  input_type;
  // XXX this needs to be inferred from the iterators and binary op
  typedef typename thrust::iterator_value<RandomAccessIterator2>::type intermediate_type;

  intermediate_type *s_sums = reinterpret_cast<intermediate_type*>(buffer);

  union {
    input_type        *inputs;
    intermediate_type *results;
  } shared;

  shared.inputs = reinterpret_cast<intermediate_type*>(reinterpret_cast<char*>(buffer) + 2*groupsize*sizeof(intermediate_type));

  // XXX int is noticeably faster than ThreadGroup::size_type
  //typedef typename bulk::static_thread_group<groupsize,grainsize>::size_type size_type;
  typedef int size_type;

  size_type tid = g.this_thread.index();

  size_type elements_per_group = groupsize * grainsize;

  for(; first < last; first += elements_per_group, result += elements_per_group)
  {
    size_type partition_size = thrust::min<size_type>(elements_per_group, last - first);
    
    // stage data through shared memory
    bulk::copy_n(g, first, partition_size, shared.inputs);
    
    // Transpose out of shared memory.
    input_type local_inputs[grainsize];

    size_type local_offset = grainsize * tid;

    size_type local_size = thrust::max<size_type>(0,thrust::min<size_type>(grainsize, partition_size - grainsize * tid));

    // XXX this should be uninitialized<input_type>
    input_type x;

    // this loop is a fused copy and accumulate
    #pragma unroll
    for(size_type i = 0; i < grainsize; ++i)
    {
      size_type index = local_offset + i;
      if(index < partition_size)
      {
        local_inputs[i] = shared.inputs[index];
        x = i ? binary_op(x, local_inputs[i]) : local_inputs[i];
      } // end if
    } // end for

    if(local_size)
    {
      s_sums[tid] = x;
    } // end if

    g.wait();
    
    // exclusive scan the array of per-thread sums
    carry_in = small_inplace_exclusive_scan_with_buffer<groupsize>(g, s_sums, carry_in, s_sums + groupsize, binary_op);

    if(local_size)
    {
      x = s_sums[tid];
    } // end if
    
    // this loop is an inclusive_scan (x begins as the carry)
    // XXX this loop should be one of the things to modify when porting to exclusive_scan
    #pragma unroll
    for(size_type i = 0; i < grainsize; ++i) 
    {
      size_type index = local_offset + i;
      if(index < partition_size)
      {
        x = binary_op(x, local_inputs[i]);

        shared.results[index] = x;
      } // end if
    } // end for

    g.wait();
    
    bulk::copy_n(g, shared.results, partition_size, result);
  } // end for
} // end inclusive_scan_with_buffer()


template<std::size_t groupsize, std::size_t grainsize, typename RandomAccessIterator1, typename RandomAccessIterator2, typename T, typename BinaryFunction>
__device__ void exclusive_scan_with_buffer(bulk::static_thread_group<groupsize,grainsize> &g,
                                           RandomAccessIterator1 first, RandomAccessIterator1 last,
                                           RandomAccessIterator2 result,
                                           T carry_in,
                                           BinaryFunction binary_op,
                                           void *buffer)
{
  typedef typename thrust::iterator_value<RandomAccessIterator1>::type  input_type;
  // XXX this needs to be inferred from the iterators and binary op
  typedef typename thrust::iterator_value<RandomAccessIterator2>::type intermediate_type;

  intermediate_type *s_sums = reinterpret_cast<intermediate_type*>(buffer);

  union {
    input_type        *inputs;
    intermediate_type *results;
  } shared;

  shared.inputs = reinterpret_cast<intermediate_type*>(reinterpret_cast<char*>(buffer) + 2*groupsize*sizeof(intermediate_type));

  // XXX int is noticeably faster than ThreadGroup::size_type
  //typedef typename bulk::static_thread_group<groupsize,grainsize>::size_type size_type;
  typedef int size_type;

  size_type tid = g.this_thread.index();

  size_type elements_per_group = groupsize * grainsize;

  for(; first < last; first += elements_per_group, result += elements_per_group)
  {
    size_type partition_size = thrust::min<size_type>(elements_per_group, last - first);
    
    // stage data through shared memory
    bulk::copy_n(g, first, partition_size, shared.inputs);
    
    // Transpose out of shared memory.
    input_type local_inputs[grainsize];

    size_type local_offset = grainsize * tid;

    size_type local_size = thrust::max<size_type>(0,thrust::min<size_type>(grainsize, partition_size - grainsize * tid));

    // XXX this should be uninitialized<input_type>
    input_type x;

    // this loop is a fused copy and accumulate
    #pragma unroll
    for(size_type i = 0; i < grainsize; ++i)
    {
      size_type index = local_offset + i;
      if(index < partition_size)
      {
        local_inputs[i] = shared.inputs[index];
        x = i ? binary_op(x, local_inputs[i]) : local_inputs[i];
      } // end if
    } // end for

    if(local_size)
    {
      s_sums[tid] = x;
    } // end if

    g.wait();
    
    // exclusive scan the array of per-thread sums
    carry_in = small_inplace_exclusive_scan_with_buffer<groupsize>(g, s_sums, carry_in, s_sums + groupsize, binary_op);

    if(local_size)
    {
      x = s_sums[tid];
    } // end if
    
    // this loop is an exclusive_scan (x begins as the carry)
    #pragma unroll
    for(size_type i = 0; i < grainsize; ++i) 
    {
      size_type index = local_offset + i;
      if(index < partition_size)
      {
        shared.results[index] = x;

        x = binary_op(x, local_inputs[i]);
      } // end if
    } // end for

    g.wait();
    
    bulk::copy_n(g, shared.results, partition_size, result);
  } // end for
} // end exclusive_scan_with_buffer()


} // end scan_detail
} // end detail


template<std::size_t groupsize,
         std::size_t grainsize,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename T,
         typename BinaryFunction>
__device__ void inclusive_scan(bulk::static_thread_group<groupsize,grainsize> &g,
                               RandomAccessIterator1 first, RandomAccessIterator1 last,
                               RandomAccessIterator2 result,
                               T init,
                               BinaryFunction binary_op)
{
  typedef typename thrust::iterator_value<RandomAccessIterator1>::type  input_type;
  // XXX this needs to be inferred from the iterators and binary op
  typedef typename thrust::iterator_value<RandomAccessIterator2>::type intermediate_type;

  int num_stage_bytes = groupsize * grainsize * thrust::max<int>(sizeof(input_type),sizeof(intermediate_type));
  int num_sums_bytes = 2 * groupsize * sizeof(intermediate_type);

  void *buffer = bulk::malloc(g, num_stage_bytes + num_sums_bytes);

  if(bulk::detail::is_shared(buffer))
  {
    detail::scan_detail::inclusive_scan_with_buffer(g, first, last, result, init, binary_op, bulk::detail::on_chip_cast(buffer));
  } // end if
  else
  {
    detail::scan_detail::inclusive_scan_with_buffer(g, first, last, result, init, binary_op, buffer);
  } // end else

  bulk::free(g, buffer);
} // end inclusive_scan()


template<std::size_t size,
         std::size_t grainsize,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename BinaryFunction>
__device__
RandomAccessIterator2 inclusive_scan(static_thread_group<size,grainsize> &this_group,
                                     RandomAccessIterator1 first,
                                     RandomAccessIterator1 last,
                                     RandomAccessIterator2 result,
                                     BinaryFunction binary_op)
{
  if(first < last)
  {
    // the first input becomes the init
    typename thrust::iterator_value<RandomAccessIterator1>::type init = *first;

    if(this_group.this_thread.index() == 0)
    {
      *result = init;
    } // end if

    bulk::inclusive_scan(this_group, first + 1, last, result + 1, init, binary_op);
  } // end if

  return result + (last - first);
} // end inclusive_scan()


template<std::size_t groupsize,
         std::size_t grainsize,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename T,
         typename BinaryFunction>
__device__ void exclusive_scan(bulk::static_thread_group<groupsize,grainsize> &g,
                               RandomAccessIterator1 first, RandomAccessIterator1 last,
                               RandomAccessIterator2 result,
                               T init,
                               BinaryFunction binary_op)
{
  typedef typename thrust::iterator_value<RandomAccessIterator1>::type  input_type;
  // XXX this needs to be inferred from the iterators and binary op
  typedef typename thrust::iterator_value<RandomAccessIterator2>::type intermediate_type;

  int num_stage_bytes = groupsize * grainsize * thrust::max<int>(sizeof(input_type),sizeof(intermediate_type));
  int num_sums_bytes = 2 * groupsize * sizeof(intermediate_type);

  void *buffer = bulk::malloc(g, num_stage_bytes + num_sums_bytes);

  if(bulk::detail::is_shared(buffer))
  {
    detail::scan_detail::exclusive_scan_with_buffer(g, first, last, result, init, binary_op, bulk::detail::on_chip_cast(buffer));
  } // end if
  else
  {
    detail::scan_detail::exclusive_scan_with_buffer(g, first, last, result, init, binary_op, buffer);
  } // end else

  bulk::free(g, buffer);
} // end exclusive_scan()


} // end bulk

