#include <c10/cuda/CUDAStream.h>
#include "perPixelKernel.hpp"

inline int iceil(int x, int y) {
    return 1 + ((x - 1) / y);
}
__global__ void perPixelKernelForwardKernel(
        torch::PackedTensorAccessor32<float, 4> image,
        torch::PackedTensorAccessor32<float, 4> weights,
        torch::PackedTensorAccessor32<float, 4> output,
        const int kernelSize, const int h, const int w,
        const int batch) {

    const int x = threadIdx.x + blockIdx.x;
    const int y = threadIdx.y + blockIdx.y;
    const int numChannels = 4;
    float result[numChannels];

    if (x >= w || y >= h) {
        return;
    }
    int weightIdx = 0;
    for (int dx = -kernelSize/2; dx <= kernelSize/2; dx++){
        int kx = x + dx;
        for (int dy = -kernelSize/2; dy <= kernelSize/2; dy++){
            int ky = y + dy;
            if (kx >= 0 && kx < w && ky >= 0 && ky < h){
                float weight = weights[batch][weightIdx][y][x];
                for (int c = 0; c < numChannels; c++){
                    result[c] += weight * image[batch][c][ky][kx];
                }
            }
            weightIdx++;
        }
    }

    for (int c = 0; c < numChannels; c++){
        output[batch][c][x][y] = result[c];
    }
}

torch::Tensor perPixelKernelCuda(torch::Tensor image, torch::Tensor weights, int64_t kernelSize){
    if (image.sizes().size() != 4) {
        throw std::runtime_error("Error in perPixelKernelCuda: image.sizes().size() != 4.");
    }
    if (weights.sizes().size() != 4) {
        throw std::runtime_error("Error in perPixelKernelCuda: weights.sizes().size() != 4.");
    }

    cudaStream_t stream = at::cuda::getCurrentCUDAStream();

    const int64_t N = image.size(0);
    const int64_t C = image.size(1);
    const int64_t H = image.size(2);
    const int64_t W = image.size(3);

    if (N > 1) {
        throw std::runtime_error("Error in perPixelKernelCuda: Batch Size larger than 1.");
    }

    torch::Tensor result = torch::zeros_like(image);
    auto imageAccessor = image.packed_accessor32<float, 4>();
    auto weightsAccessor = weights.packed_accessor32<float, 4>();
    auto resultAccessor = result.packed_accessor32<float, 4>();

    dim3 blockDim(16, 16, 1);
    dim3 gridDim(iceil(W, blockDim.x), iceil(H, blockDim.y), 1);

    perPixelKernelForwardKernel<<<gridDim, blockDim, 0, stream>>> (
        imageAccessor, weightsAccessor, resultAccessor,
        kernelSize, H, W, 0
    );

    return result;
}