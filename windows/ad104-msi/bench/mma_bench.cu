// mma_bench.cu - register-resident MMA issue-rate probe (no memory traffic).
// iter count is a RUNTIME argument so the reported FLOP count can never drift
// from what the kernel actually executed.
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>

template <int MODE>
__global__ void mma_kernel(float* out, int iters)
{
  float d[8][4];
  unsigned a[4], b[2];
#pragma unroll
  for (int i = 0; i < 8; i++)
#pragma unroll
    for (int j = 0; j < 4; j++) d[i][j] = 0.f;
  a[0] = a[1] = a[2] = a[3] = 0x3c003c00u;
  b[0] = b[1] = 0x3c003c00u;

  for (int k = 0; k < iters; k++) {
#pragma unroll
    for (int i = 0; i < 8; i++) {
      if (MODE == 0) {
        asm volatile(
          "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 "
          "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
          : "+f"(d[i][0]), "+f"(d[i][1]), "+f"(d[i][2]), "+f"(d[i][3])
          : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
      } else if (MODE == 1) {
        asm volatile(
          "mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 "
          "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
          : "+f"(d[i][0]), "+f"(d[i][1]), "+f"(d[i][2]), "+f"(d[i][3])
          : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
      } else {
        asm volatile(
          "mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32 "
          "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
          : "+f"(d[i][0]), "+f"(d[i][1]), "+f"(d[i][2]), "+f"(d[i][3])
          : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
      }
    }
  }
  float s = 0.f;
#pragma unroll
  for (int i = 0; i < 8; i++) s += d[i][0] + d[i][1] + d[i][2] + d[i][3];
  if (s == 1234.5678f) out[threadIdx.x] = s;
}

__global__ void mma_int8_kernel(int* out, int iters)
{
  int d[8][4];
  unsigned a[4], b[2];
#pragma unroll
  for (int i = 0; i < 8; i++)
#pragma unroll
    for (int j = 0; j < 4; j++) d[i][j] = 0;
  a[0] = a[1] = a[2] = a[3] = 0x01010101u;
  b[0] = b[1] = 0x01010101u;

  for (int k = 0; k < iters; k++) {
#pragma unroll
    for (int i = 0; i < 8; i++) {
      asm volatile(
        "mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 "
        "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
        : "+r"(d[i][0]), "+r"(d[i][1]), "+r"(d[i][2]), "+r"(d[i][3])
        : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
    }
  }
  int s = 0;
#pragma unroll
  for (int i = 0; i < 8; i++) s += d[i][0] + d[i][1] + d[i][2] + d[i][3];
  if (s == 1234567) out[threadIdx.x] = s;
}

__global__ void mma_fp8_kernel(float* out, int iters)
{
  float d[8][4];
  unsigned a[4], b[2];
#pragma unroll
  for (int i = 0; i < 8; i++)
#pragma unroll
    for (int j = 0; j < 4; j++) d[i][j] = 0.f;
  a[0] = a[1] = a[2] = a[3] = 0x38383838u;   // e4m3 1.0
  b[0] = b[1] = 0x38383838u;

  for (int k = 0; k < iters; k++) {
#pragma unroll
    for (int i = 0; i < 8; i++) {
      asm volatile(
        "mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32 "
        "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
        : "+f"(d[i][0]), "+f"(d[i][1]), "+f"(d[i][2]), "+f"(d[i][3])
        : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
    }
  }
  float s = 0.f;
#pragma unroll
  for (int i = 0; i < 8; i++) s += d[i][0] + d[i][1] + d[i][2] + d[i][3];
  if (s == 1234.5678f) out[threadIdx.x] = s;
}

static double flop_of(int mode)
{
  if (mode == 2) return 2048.0;      // tf32 k=8
  if (mode == 3) return 8192.0;      // int8 k=32
  if (mode == 4) return 8192.0;      // fp8  k=32
  return 4096.0;                     // f16/bf16 k=16
}

int main(int argc, char** argv)
{
  int iters = 60000;
  for (int i = 1; i < argc; i++) if (argv[i][0] >= '0' && argv[i][0] <= '9') iters = atoi(argv[i]);

  cudaDeviceProp pr;
  if (cudaGetDeviceProperties(&pr, 0) != cudaSuccess) { printf("no device\n"); return 1; }
  int sms = pr.multiProcessorCount, threads = 256, warps = threads / 32;
  dim3 grid(sms * 8), block(threads);
  printf("GPU: %s | SMs=%d | grid=%d x %d thr | iters=%d x 8 mma\n", pr.name, sms, grid.x, threads, iters);

  void* dout; cudaMalloc(&dout, 4096);
  cudaEvent_t e0, e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
  const char* names[5] = { "FP16 / FP32 acc", "BF16 / FP32 acc", "TF32 / FP32 acc", "INT8 / INT32 acc", "FP8 E4M3 / FP32 acc" };
  printf("\n%-18s %10s %12s\n", "mode", "ms", "T/s");

  for (int m = 0; m < 5; m++) {
    // warmup
    if (m == 0) mma_kernel<0><<<grid, block>>>((float*)dout, iters);
    if (m == 1) mma_kernel<1><<<grid, block>>>((float*)dout, iters);
    if (m == 2) mma_kernel<2><<<grid, block>>>((float*)dout, iters);
    if (m == 3) mma_int8_kernel<<<grid, block>>>((int*)dout, iters);
    if (m == 4) mma_fp8_kernel<<<grid, block>>>((float*)dout, iters);
    cudaDeviceSynchronize();

    cudaEventRecord(e0);
    if (m == 0) mma_kernel<0><<<grid, block>>>((float*)dout, iters);
    if (m == 1) mma_kernel<1><<<grid, block>>>((float*)dout, iters);
    if (m == 2) mma_kernel<2><<<grid, block>>>((float*)dout, iters);
    if (m == 3) mma_int8_kernel<<<grid, block>>>((int*)dout, iters);
    if (m == 4) mma_fp8_kernel<<<grid, block>>>((float*)dout, iters);
    cudaEventRecord(e1);
    cudaEventSynchronize(e1);
    float ms = 0.f; cudaEventElapsedTime(&ms, e0, e1);

    double count = (double)grid.x * warps * (double)iters * 8.0;
    double rate = count * flop_of(m) / (ms / 1000.0) / 1e12;
    printf("%-18s %10.3f %12.2f\n", names[m], ms, rate);
    fflush(stdout);
  }
  cudaFree(dout);
  return 0;
}
