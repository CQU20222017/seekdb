// Minimal stub of HIP runtime APIs, only for host-side syntax checking of
// distance_hipvs.hip without a ROCm installation. NOT for real builds.
#pragma once
#include <cstddef>
#include <cstdint>

#define __global__
#define __device__
#define __host__
#define __restrict__

struct dim3 {
  unsigned x, y, z;
  dim3(unsigned a = 1, unsigned b = 1, unsigned c = 1) : x(a), y(b), z(c) {}
};

extern unsigned blockIdx_x_stub;
extern unsigned blockDim_x_stub;
extern unsigned threadIdx_x_stub;
#define blockIdx blockIdx_x_stub_obj
#define blockDim blockDim_x_stub_obj
#define threadIdx threadIdx_x_stub_obj
struct DimStub { unsigned x; };
extern DimStub blockIdx_x_stub_obj;
extern DimStub blockDim_x_stub_obj;
extern DimStub threadIdx_x_stub_obj;

typedef int hipError_t;
#define hipSuccess 0
#define hipMemcpyHostToDevice 1
#define hipMemcpyDeviceToHost 2

inline hipError_t hipSetDevice(int) { return 0; }
inline hipError_t hipGetDeviceCount(int *c) { *c = 0; return 0; }
inline hipError_t hipMalloc(void **p, size_t) { *p = nullptr; return 0; }
inline hipError_t hipFree(void *) { return 0; }
inline hipError_t hipMemcpy(void *, const void *, size_t, int) { return 0; }
inline hipError_t hipGetLastError() { return 0; }
inline hipError_t hipDeviceSynchronize() { return 0; }

#define hipLaunchKernelGGL(kernel, grid, block, shmem, stream, ...) \
  do { (void)sizeof(grid); (void)sizeof(block); } while (0)

inline float sqrtf(float x) { return __builtin_sqrtf(x); }