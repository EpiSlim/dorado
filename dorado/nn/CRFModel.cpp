#include "CRFModel.h"

#include "../utils/module_utils.h"
#include "../utils/tensor_utils.h"

#ifndef __APPLE__
#include <ATen/cuda/CUDAContext.h>
#include <c10/cuda/CUDAGuard.h>

extern "C" {
#include "koi.h"
}
#include <cublas_v2.h>
#include <cuda_runtime.h>

#define USE_CUDA_LSTM 1
#define CUDA_PROFILE_TO_CERR 0
#else
#define USE_CUDA_LSTM 0
#endif

#include <math.h>
#include <nvtx3/nvtx3.hpp>
#include <spdlog/spdlog.h>
#include <toml.hpp>
#include <torch/torch.h>

#include <limits>
#include <string>

using namespace torch::nn;
namespace F = torch::nn::functional;
using Slice = torch::indexing::Slice;
using quantized_lstm = std::function<int(void *, void *, void *, void *, void *, void *, int)>;

#if USE_CUDA_LSTM
#define CUDA_CHECK(X)                                                                         \
    {                                                                                         \
        cudaError_t error = X;                                                                \
        if (error != cudaSuccess) {                                                           \
            printf("CUDA returned error %s (code %d), line(%d)\n", cudaGetErrorString(error), \
                   error, __LINE__);                                                          \
            exit(EXIT_FAILURE);                                                               \
        }                                                                                     \
    }

static void cublas_matmul_f16(torch::Tensor const &A, torch::Tensor const &B, torch::Tensor &C) {
    constexpr uint16_t HALF_ZERO = 0;      // 0.0 in __half format
    constexpr uint16_t HALF_ONE = 0x3C00;  // 1.0 in __half format
    assert(A.dtype() == torch::kF16 && B.dtype() == torch::kF16 && C.dtype() == torch::kF16);
    assert(A.stride(1) == 1 && B.stride(1) == 1 && C.stride(1) == 1);
    assert(A.size(0) == C.size(0));  // M
    assert(B.size(1) == C.size(1));  // N
    assert(A.size(1) == B.size(0));  // K
    auto res =
            cublasGemmEx(at::cuda::getCurrentCUDABlasHandle(), CUBLAS_OP_N, CUBLAS_OP_N, B.size(1),
                         A.size(0), A.size(1), &HALF_ONE, B.data_ptr(), CUDA_R_16F, B.stride(0),
                         A.data_ptr(), CUDA_R_16F, A.stride(0), &HALF_ZERO, C.data_ptr(),
                         CUDA_R_16F, C.stride(0), CUDA_R_16F, CUBLAS_GEMM_DEFAULT_TENSOR_OP);
    if (res != CUBLAS_STATUS_SUCCESS) {
        spdlog::error("CuBLAS error {}", int(res));
        exit(EXIT_FAILURE);
    }
}

static bool cuda_lstm_is_quantized(int layer_size) {
    return ((layer_size == 96) || (layer_size == 128));
}
#endif  // if USE_CUDA_LSTM

#if CUDA_PROFILE_TO_CERR
class ScopedProfileRange {
public:
    explicit ScopedProfileRange(const char *label) : m_nvtx_range(label), m_label(label) {
        m_stream = at::cuda::getCurrentCUDAStream().stream();
        CUDA_CHECK(cudaEventCreate(&m_start));
        CUDA_CHECK(cudaEventRecord(m_start, m_stream));
        m_active = true;
    }

    ~ScopedProfileRange() { finish(); }

private:
    void finish() {
        if (!m_active) {
            return;
        }
        cudaEvent_t stop;
        CUDA_CHECK(cudaEventCreate(&stop));
        CUDA_CHECK(cudaEventRecord(stop, m_stream));
        CUDA_CHECK(cudaEventSynchronize(stop));
        float timeMs = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&timeMs, m_start, stop));
        CUDA_CHECK(cudaEventDestroy(m_start));
        CUDA_CHECK(cudaEventDestroy(stop));
        std::cerr << "[" << m_label << " " << timeMs << " ms]" << std::endl;
        m_active = false;
    }

    nvtx3::scoped_range m_nvtx_range;
    const char *m_label;
    cudaStream_t m_stream;
    cudaEvent_t m_start;
    bool m_active;
};
#else  // if CUDA_PROFILE_TO_CERR
using ScopedProfileRange = nvtx3::scoped_range;
#endif

namespace {
template <class Model>
ModuleHolder<AnyModule> populate_model(Model &&model,
                                       const std::filesystem::path &path,
                                       torch::TensorOptions options,
                                       bool decomposition,
                                       bool bias) {
    auto state_dict = model->load_weights(path, decomposition, bias);
    model->load_state_dict(state_dict);
    model->to(options.dtype_opt().value().toScalarType());
    model->to(options.device_opt().value());
    model->eval();

    auto module = AnyModule(model);
    auto holder = ModuleHolder<AnyModule>(module);
    return holder;
}
}  // namespace

namespace dorado {

namespace nn {

static constexpr float SWISH_LOWER_BOUND = -0.278464543f;  // global minimum of `x * sigmoid(x)`
static constexpr float I8_RANGE = 127.f;

struct ConvolutionImpl : Module {
    ConvolutionImpl(int size,
                    int outsize,
                    int k,
                    int stride_,
                    bool clamp_,
                    float max_value_,
                    bool to_lstm_)
            : in_size(size),
              out_size(outsize),
              window_size(k),
              stride(stride_),
              clamp(clamp_),
              max_value(clamp_ ? max_value_ : std::numeric_limits<float>::max()),
              to_lstm(to_lstm_) {
        conv = register_module(
                "conv", Conv1d(Conv1dOptions(size, outsize, k).stride(stride).padding(k / 2)));
        activation = register_module("activation", SiLU());
    }

    torch::Tensor forward(torch::Tensor x) {
        // Input x is [N, C_in, T_in], contiguity optional
#if USE_CUDA_LSTM
        if (to_lstm && x.device() != torch::kCPU) {
            c10::cuda::CUDAGuard device_guard(x.device());
            auto stream = at::cuda::getCurrentCUDAStream().stream();

            int batch_size = x.size(0);
            int chunk_size_in = x.size(2);
            int chunk_size_out = chunk_size_in / stride;
            auto w_device = conv->weight.view({out_size, in_size * window_size})
                                    .t()
                                    .to(x.options())
                                    .contiguous();
            auto b_device = conv->bias.to(x.options());

            cudaDeviceProp *prop = at::cuda::getCurrentDeviceProperties();
            const bool output_NTC = cuda_lstm_is_quantized(out_size);
            // TODO: reinstate output as int8
            //            const bool output_int8 = !output_NTC && clamp && prop->major >= 8;
            const bool output_int8 = false;

            if (output_NTC) {
                auto res = torch::empty({batch_size, chunk_size_out, out_size}, x.options());
                auto res_2D = res.view({-1, out_size});
                auto ntcw_mat = torch::empty({batch_size, chunk_size_out, in_size, window_size},
                                             x.options());
                host_window_ntcw_f16(stream, x.stride(0), x.stride(2), x.stride(1), batch_size,
                                     chunk_size_in, in_size, window_size, stride,
                                     ntcw_mat.stride(0), ntcw_mat.stride(1), ntcw_mat.stride(2),
                                     ntcw_mat.stride(3), x.data_ptr(), ntcw_mat.data_ptr());
                cublas_matmul_f16(ntcw_mat.view({-1, in_size * window_size}), w_device, res_2D);
                host_bias_swish_f16_clamp(stream, res_2D.size(0), res_2D.size(1), res_2D.stride(0),
                                          res_2D.data_ptr(), b_device.data_ptr(), max_value);
                // Output is [N, T_out, C_out], contiguous
                return res;
            } else {
                torch::Tensor res, mm_out;
                if (output_int8) {
                    res = torch::empty({chunk_size_out + 1, batch_size, 2, out_size},
                                       x.options().dtype(torch::kInt8));
                    mm_out = res.slice(0, 1, chunk_size_out + 1)
                                     .view({-1, 2 * out_size})
                                     .view(torch::kF16);
                } else {
                    res = torch::empty({chunk_size_out + 1, batch_size, 2, out_size}, x.options());
                    auto res_TNC = res.slice(0, 1, chunk_size_out + 1).select(2, 1);
                    mm_out = res_TNC.view({-1, out_size});
                }

                auto tncw_mat = torch::empty({chunk_size_out, batch_size, in_size, window_size},
                                             x.options());
                host_window_ntcw_f16(stream, x.stride(0), x.stride(2), x.stride(1), batch_size,
                                     chunk_size_in, in_size, window_size, stride,
                                     tncw_mat.stride(1), tncw_mat.stride(0), tncw_mat.stride(2),
                                     tncw_mat.stride(3), x.data_ptr(), tncw_mat.data_ptr());
                cublas_matmul_f16(tncw_mat.view({-1, in_size * window_size}), w_device, mm_out);
                if (output_int8) {
                    float scale = 2 * I8_RANGE / (max_value - SWISH_LOWER_BOUND);
                    float zero_offset = scale * max_value - I8_RANGE;
                    host_bias_swish_f16_i8_inplace(stream, mm_out.size(0), mm_out.size(1), out_size,
                                                   mm_out.data_ptr(), b_device.data_ptr(), scale,
                                                   zero_offset);
                } else {
                    host_bias_swish_f16_clamp(stream, mm_out.size(0), mm_out.size(1),
                                              mm_out.stride(0), mm_out.data_ptr(),
                                              b_device.data_ptr(), max_value);
                }
                // Output is [T_out + 1, N, 2, C_out], contiguous, which serves as
                // working memory for CuBLAS LSTM
                res.index({0, Slice(), 1}) = 0;
                res.index({chunk_size_out, Slice(), 0}) = 0;
                return res;
            }
        }
#endif
        x = activation(conv(x));
        if (clamp) {
            x = x.clamp(c10::nullopt, max_value);
        }
        if (to_lstm) {
            // Output is [N, T_out, C_out], non-contiguous
            return x.transpose(1, 2);
        } else {
            // Output is [N, C_out, T_out], contiguous
            return x;
        }
    }

    Conv1d conv{nullptr};
    SiLU activation{nullptr};
    int in_size;
    int out_size;
    int window_size;
    int stride;
    const bool clamp;
    const float max_value;
    const bool to_lstm;
};

struct LinearCRFImpl : Module {
    LinearCRFImpl(int insize, int outsize) : scale(5), blank_score(2.0), expand_blanks(false) {
        linear = register_module("linear", Linear(insize, outsize));
        activation = register_module("activation", Tanh());
    };

    torch::Tensor forward(torch::Tensor x) {
        // Input x is [N, T, C], contiguity optional
        auto N = x.size(0);
        auto T = x.size(1);

        torch::Tensor scores;
#if USE_CUDA_LSTM
        if (x.device() != torch::kCPU) {
            // Optimised version of the else branch for CUDA devices
            c10::cuda::CUDAGuard device_guard(x.device());
            auto stream = at::cuda::getCurrentCUDAStream().stream();

            x = x.contiguous().reshape({N * T, -1});
            scores = torch::matmul(x, linear->weight.t());
            host_bias_tanh_scale_f16(stream, N * T, scores.size(1), scale, scores.data_ptr(),
                                     linear->bias.data_ptr());
            scores = scores.view({N, T, -1});
        } else
#endif  // if USE_CUDA_LSTM
        {
            scores = activation(linear(x)) * scale;
        }

        if (expand_blanks == true) {
            scores = scores.contiguous();
            int C = scores.size(2);
            scores = F::pad(scores.view({N, T, C / 4, 4}),
                            F::PadFuncOptions({1, 0, 0, 0, 0, 0, 0, 0}).value(blank_score))
                             .view({N, T, -1});
        }

        if (x.device() == torch::kCPU) {
            // Output is [T, N, C]
            return scores.transpose(0, 1);
        }

        // Output is [N, T, C], contiguous
        return scores;
    }

    int scale;
    int blank_score;
    bool expand_blanks;
    Linear linear{nullptr};
    Tanh activation{nullptr};
};

#if USE_CUDA_LSTM

struct CudaLSTMImpl : Module {
    CudaLSTMImpl(int layer_size, bool reverse_) : reverse(reverse_) {
        auto options = torch::TensorOptions().dtype(torch::kFloat16);
        weights = torch::empty({layer_size * 4, layer_size * 2}, options).contiguous();
        auto weight_ih = weights.slice(1, 0, layer_size);
        auto weight_hh = weights.slice(1, layer_size, 2 * layer_size);
        if (reverse) {
            std::swap(weight_ih, weight_hh);
        }
        bias = torch::empty({layer_size * 4}, options).contiguous();
        auto bias_hh = torch::empty({layer_size * 4}, options).contiguous();

        register_parameter("weight_ih", weight_ih, false);
        register_parameter("weight_hh", weight_hh, false);
        register_parameter("bias_ih", bias, false);
        register_parameter("bias_hh", bias_hh, false);
    }

    torch::Tensor weights, bias;
    bool reverse;
};

TORCH_MODULE(CudaLSTM);

struct CudaLSTMStackImpl : Module {
    CudaLSTMStackImpl(int layer_size_,
                      int batch_size,
                      int chunk_size,
                      float scale_i8_,
                      float zero_offset_i8_)
            : layer_size(layer_size_), scale_i8(scale_i8_), zero_offset_i8(zero_offset_i8_) {
        rnn1 = register_module("rnn_1", CudaLSTM(layer_size, true));
        rnn2 = register_module("rnn_2", CudaLSTM(layer_size, false));
        rnn3 = register_module("rnn_3", CudaLSTM(layer_size, true));
        rnn4 = register_module("rnn_4", CudaLSTM(layer_size, false));
        rnn5 = register_module("rnn_5", CudaLSTM(layer_size, true));

        m_quantize = cuda_lstm_is_quantized(layer_size);

        if (m_quantize) {
            // chunk_size * batch_size can not be > 2**31 (2147483648).
            // For practical purposes this is currently always the case.
            _chunks = torch::empty({batch_size, 4}).to(torch::kInt32);
            _chunks.index({torch::indexing::Slice(), 0}) =
                    torch::arange(0, chunk_size * batch_size, chunk_size);
            _chunks.index({torch::indexing::Slice(), 2}) =
                    torch::arange(0, chunk_size * batch_size, chunk_size);
            _chunks.index({torch::indexing::Slice(), 1}) = chunk_size;
            _chunks.index({torch::indexing::Slice(), 3}) = 0;
        }

        if (layer_size == 96) {
            _host_run_lstm_fwd_quantized = host_run_lstm_fwd_quantized96;
            _host_run_lstm_rev_quantized = host_run_lstm_reverse_quantized96;
        } else if (layer_size == 128) {
            _host_run_lstm_fwd_quantized = host_run_lstm_fwd_quantized128;
            _host_run_lstm_rev_quantized = host_run_lstm_reverse_quantized128;
        }
    }

    bool _weights_rearranged = false;
    bool m_quantize;
    torch::Tensor _chunks;
    std::vector<torch::Tensor> device_weights;
    std::vector<torch::Tensor> device_bias;
    std::vector<torch::Tensor> device_scale;
    std::vector<torch::Tensor> _r_wih;
    std::vector<torch::Tensor> _quantized_buffers;
    std::vector<torch::Tensor> _quantization_scale_factors;
    quantized_lstm _host_run_lstm_fwd_quantized{nullptr};
    quantized_lstm _host_run_lstm_rev_quantized{nullptr};

    torch::Tensor forward_cublas(torch::Tensor in) {
        // input is ([T+1, N, 2, C], contiguous) (see below)
        auto stream = at::cuda::getCurrentCUDAStream().stream();

        // Cutlass kernel currently requires SM8.0 (A100) or later
        cudaDeviceProp *prop = at::cuda::getCurrentDeviceProperties();
        const bool use_cutlass = prop->major >= 8;
        const bool use_int8 = use_cutlass;

        torch::Tensor mat_working_mem = in;
        const int chunk_size = in.size(0) - 1;
        const int batch_size = in.size(1);
        assert(layer_size == in.size(3));
        assert(in.dim() == 4 && in.size(2) == 2);
        assert(in.dtype() == torch::kF16 || (use_int8 && in.dtype() == torch::kInt8));
        assert(in.is_contiguous());
        auto opts_f16 = in.options().dtype(torch::kF16);
        const int gate_size = layer_size * 4;

        // Working memory is laid out as [T+1][N][2][C] in memory, where the 2 serves to
        // interleave input and output for each LSTM layer in a specific way. The reverse LSTM
        // layers (rnn1, rnn3, rnn5) use right as input and left as output, whereas the forward
        // LSTM layers (rnn2, rnn4) use left as input and right as output.
        //
        // The interleaving means that x(t) and h(t-1), i.e. the input for the current timestep
        // and the output of the previous timestep, appear concatenated in memory and we can
        // perform a single matmul with the concatenated WU matrix
        // Note that both working_mem[chunk_size][:][0][:] and working_mem[0][:][1][:] remain
        // all zeroes, representing the initial LSTM state h(-1) in either direction.

        torch::Tensor inout_all_f16, inout_left_f16, inout_right_f16;
        torch::Tensor inout_all_i8, inout_left_i8, inout_right_i8, gate_buf;
        // F16 and Int8 tensors share the same memory. We can convert in-place,
        // doubling stride(-2) for the Int8 tensor.
        int convert_to_int8_layer_idx = -1;
        if (in.dtype() == torch::kF16) {
            inout_all_f16 = in.view({chunk_size + 1, batch_size, -1});
            inout_left_f16 = in.slice(0, 0, chunk_size).select(2, 0);
            inout_right_f16 = in.slice(0, 1, chunk_size + 1).select(2, 1);
            inout_all_i8 = in.select(2, 0).view(torch::kInt8);
            inout_left_i8 = inout_all_i8.slice(0, 0, chunk_size).slice(2, 0, layer_size);
            inout_right_i8 =
                    inout_all_i8.slice(0, 1, chunk_size + 1).slice(2, layer_size, 2 * layer_size);
            convert_to_int8_layer_idx = use_int8 ? 0 : 6;  // convert after first layer, or never
        } else if (in.dtype() == torch::kInt8) {
            inout_all_i8 = in.view({chunk_size + 1, batch_size, -1});
            inout_left_i8 = in.slice(0, 0, chunk_size).select(2, 0);
            inout_right_i8 = in.slice(0, 1, chunk_size + 1).select(2, 1);
            inout_left_f16 = inout_all_i8.slice(0, 0, chunk_size).view(torch::kF16);
        }
        if (!use_cutlass) {
            gate_buf = torch::empty({batch_size, gate_size}, in.options());
        }

        int layer_idx = 0;
        for (auto &rnn : {rnn1, rnn2, rnn3, rnn4, rnn5}) {
            ScopedProfileRange spr_lstm("lstm_layer");
            auto state_buf = torch::zeros({batch_size, layer_size}, opts_f16);
            auto weights_cpu = rnn->weights;
            if (use_cutlass) {
                auto type_id = (layer_idx > convert_to_int8_layer_idx) ? KOI_I8 : KOI_F16;
                if (int(device_weights.size()) == layer_idx) {
                    auto layer_device_bias = rnn->bias.to(in.device());
                    if (type_id == KOI_I8) {
                        auto weights_f32 = weights_cpu.t().to(torch::kF32);
                        auto [scale, quantized] = quantize_tensor(weights_f32);
                        weights_cpu = quantized.t();
                        if (layer_idx == 0) {
                            scale *= scale_i8 / I8_RANGE;
                            layer_device_bias = layer_device_bias +
                                                weights_f32.sum(1) * (zero_offset_i8 / scale_i8);
                        }
                        device_scale.push_back(scale.contiguous().to(in.device()).to(torch::kF16));
                    } else {
                        device_scale.push_back(torch::ones_like(layer_device_bias));
                    }
                    device_bias.push_back(layer_device_bias);
                    // Cutlass kernel expects weights reordered as <igigigigfofofofo>
                    auto weights_cpu_cutlass = torch::empty_like(weights_cpu);
                    for (int i = 0; i < layer_size; ++i) {
                        int i0 = i / 4;
                        int i1 = i % 4;
                        weights_cpu_cutlass[i0 * 16 + i1 * 2 + 0] = weights_cpu[i + 0 * layer_size];
                        weights_cpu_cutlass[i0 * 16 + i1 * 2 + 1] = weights_cpu[i + 1 * layer_size];
                        weights_cpu_cutlass[i0 * 16 + i1 * 2 + 8] = weights_cpu[i + 2 * layer_size];
                        weights_cpu_cutlass[i0 * 16 + i1 * 2 + 9] = weights_cpu[i + 3 * layer_size];
                    }
                    device_weights.push_back(weights_cpu_cutlass.contiguous().to(in.device()));
                }

                auto in = (type_id == KOI_I8) ? inout_all_i8 : inout_all_f16;
                host_cutlass_lstm(stream, type_id, layer_idx, batch_size, layer_size, chunk_size,
                                  rnn->reverse ? -1 : 1, in.stride(1), in.data_ptr(),
                                  device_weights[layer_idx].data_ptr(),
                                  device_bias[layer_idx].data_ptr(),
                                  device_scale[layer_idx].data_ptr(), state_buf.data_ptr());

                if (layer_idx == convert_to_int8_layer_idx) {
                    ScopedProfileRange spr_convert("f16_to_int8");
                    host_f16_to_i8_inplace(stream, inout_left_f16.data_ptr(),
                                           inout_left_f16.size(0) * inout_left_f16.size(1),
                                           inout_left_f16.size(2), inout_left_f16.stride(1), 0);
                    inout_all_i8.index({chunk_size, Slice(), 0}) = 0;
                    inout_all_i8.index({0, Slice(), 1}) = 0;
                }
            } else {
                if (int(device_weights.size()) == layer_idx) {
                    device_bias.push_back(rnn->bias.to(in.device()));
                    device_weights.push_back(rnn->weights.t().contiguous().to(in.device()));
                }
                for (int ts = 0; ts < chunk_size; ++ts) {
                    auto timestep_in = inout_all_f16[rnn->reverse ? (chunk_size - ts) : ts];
                    auto timestep_out = rnn->reverse ? inout_left_f16[chunk_size - ts - 1]
                                                     : inout_right_f16[ts];

                    // Timestep matrix mulitplication (using cublasGemmEx, as using torch::matmul
                    // as below is a bit slower on A100 for some reason)
                    // gate_buf = torch::matmul(timestep_in, weights);
                    cublas_matmul_f16(timestep_in, device_weights[layer_idx], gate_buf);
                    host_lstm_step_f16(stream, batch_size, layer_size,
                                       device_bias[layer_idx].data_ptr(), gate_buf.data_ptr(),
                                       state_buf.data_ptr(), timestep_out.data_ptr());
                }
            }
            ++layer_idx;
        }

        if (use_int8) {
            ScopedProfileRange spr_convert("int8_to_f16");
            // inout_left_i8 shares storage with inout_left_f16[:][:][0:layer_size/2]
            host_i8_to_f16_inplace(stream, inout_left_f16.data_ptr(),
                                   inout_left_f16.size(0) * inout_left_f16.size(1),
                                   inout_left_f16.size(2), inout_left_f16.stride(1), 0);
        }
        // Output is [N, T, C], non-contiguous
        return inout_left_f16.transpose(1, 0);
    }

    void rearrange_individual_weights(torch::Tensor buffer) {
        torch::Tensor tmp = torch::empty_like(buffer);
        int layer_width = tmp.size(0) / 4;

        //Mapping of LSTM gate weights from IFGO to GIFO order.
        std::vector<std::pair<int, int>> idxs = {std::make_pair(0, 2), std::make_pair(1, 0),
                                                 std::make_pair(2, 1), std::make_pair(3, 3)};

        for (auto idx : idxs) {
            int start_idx = idx.second * layer_width;
            int end_idx = start_idx + layer_width;
            tmp.index({torch::indexing::Slice(idx.first * layer_width,
                                              (idx.first + 1) * layer_width)}) =
                    buffer.index({torch::indexing::Slice(start_idx, end_idx)});
        }

        buffer.index({torch::indexing::Slice()}) = tmp;
    }

    void rearrange_weights() {
        for (auto &rnn : {rnn1, rnn2, rnn3, rnn4, rnn5}) {
            rearrange_individual_weights(rnn->named_parameters()["weight_hh"]);
            rearrange_individual_weights(rnn->named_parameters()["weight_ih"]);
            _r_wih.push_back(rnn->named_parameters()["weight_ih"].transpose(0, 1).contiguous());
            rearrange_individual_weights(rnn->named_parameters()["bias_hh"]);
            rearrange_individual_weights(rnn->named_parameters()["bias_ih"]);
        }
        _weights_rearranged = true;
    }

    std::pair<torch::Tensor, torch::Tensor> quantize_tensor(torch::Tensor tensor,
                                                            int levels = 256) {
        //Qauntize a tensor to int8, returning per-channel scales and the quantized tensor
        //if weights have not been quantized we get some scaling
        auto fp_max = torch::abs(std::get<0>(torch::max(tensor, 0)));
        auto fp_min = torch::abs(std::get<0>(torch::min(tensor, 0)));

        auto fp_range =
                std::get<0>(
                        torch::cat(
                                {fp_min.index({torch::indexing::Slice(), torch::indexing::None}),
                                 fp_max.index({torch::indexing::Slice(), torch::indexing::None})},
                                1)
                                .max(1)) *
                2;
        auto quantization_scale = levels / fp_range;
        auto quantization_max = (levels / 2) - 1;

        auto tensor_quantized = (tensor * quantization_scale)
                                        .round()
                                        .clip(-quantization_max, quantization_max)
                                        .to(torch::kI8);

        return std::pair<torch::Tensor, torch::Tensor>(quantization_scale.to(torch::kFloat32),
                                                       tensor_quantized);
    }

    void quantize_weights() {
        for (auto &rnn : {rnn1, rnn2, rnn3, rnn4, rnn5}) {
            auto [factors, quantized] = quantize_tensor(rnn->named_parameters()["weight_hh"].t());
            _quantization_scale_factors.push_back(factors.contiguous());
            _quantized_buffers.push_back(quantized.contiguous());
        }
    }

    torch::Tensor forward_quantized(torch::Tensor x) {
        // Input x is [N, T, C], contiguity optional
        x = x.contiguous();

        //If this is the fist time the forward method is being applied, do some startup
        if (m_quantize && !_weights_rearranged) {
            rearrange_weights();
            quantize_weights();
            _chunks = _chunks.to(x.device());
        }
        auto buffer = torch::matmul(x, _r_wih[0]);

        _host_run_lstm_rev_quantized(
                _chunks.data_ptr(), buffer.data_ptr(), _quantized_buffers[0].data_ptr(),
                rnn1->named_parameters()["bias_ih"].data_ptr(),
                _quantization_scale_factors[0].data_ptr(), x.data_ptr(), _chunks.size(0));

        buffer = torch::matmul(x, _r_wih[1]);

        _host_run_lstm_fwd_quantized(
                _chunks.data_ptr(), buffer.data_ptr(), _quantized_buffers[1].data_ptr(),
                rnn2->named_parameters()["bias_ih"].data_ptr(),
                _quantization_scale_factors[1].data_ptr(), x.data_ptr(), _chunks.size(0));

        buffer = torch::matmul(x, _r_wih[2]);

        _host_run_lstm_rev_quantized(
                _chunks.data_ptr(), buffer.data_ptr(), _quantized_buffers[2].data_ptr(),
                rnn3->named_parameters()["bias_ih"].data_ptr(),
                _quantization_scale_factors[2].data_ptr(), x.data_ptr(), _chunks.size(0));

        buffer = torch::matmul(x, _r_wih[3]);

        _host_run_lstm_fwd_quantized(
                _chunks.data_ptr(), buffer.data_ptr(), _quantized_buffers[3].data_ptr(),
                rnn4->named_parameters()["bias_ih"].data_ptr(),
                _quantization_scale_factors[3].data_ptr(), x.data_ptr(), _chunks.size(0));

        buffer = torch::matmul(x, _r_wih[4]);

        _host_run_lstm_rev_quantized(
                _chunks.data_ptr(), buffer.data_ptr(), _quantized_buffers[4].data_ptr(),
                rnn5->named_parameters()["bias_ih"].data_ptr(),
                _quantization_scale_factors[4].data_ptr(), x.data_ptr(), _chunks.size(0));

        // Output is [N, T, C], contiguous
        return x;
    }

    // Dispatch to different forward method depending on whether we use quantized LSTMs or not
    torch::Tensor forward(torch::Tensor x) {
        // Input x is [N, T, C], contiguity optional
        c10::cuda::CUDAGuard device_guard(x.device());
        ScopedProfileRange spr("lstm_stack");

        if (m_quantize) {
            // Output is [N, T, C], contiguous
            return forward_quantized(x);
        } else {
            // Output is [N, T, C], non-contiguous
            return forward_cublas(x);
        }
    }

    int layer_size;
    float scale_i8;
    float zero_offset_i8;
    CudaLSTM rnn1{nullptr}, rnn2{nullptr}, rnn3{nullptr}, rnn4{nullptr}, rnn5{nullptr};
};

TORCH_MODULE(CudaLSTMStack);

#endif  // if USE_CUDA_LSTM

struct LSTMStackImpl : Module {
    LSTMStackImpl(int size, int batchsize, int chunksize, float, float) {
        // torch::nn::LSTM expects/produces [N, T, C] with batch_first == true
        rnn1 = register_module("rnn1", LSTM(LSTMOptions(size, size).batch_first(true)));
        rnn2 = register_module("rnn2", LSTM(LSTMOptions(size, size).batch_first(true)));
        rnn3 = register_module("rnn3", LSTM(LSTMOptions(size, size).batch_first(true)));
        rnn4 = register_module("rnn4", LSTM(LSTMOptions(size, size).batch_first(true)));
        rnn5 = register_module("rnn5", LSTM(LSTMOptions(size, size).batch_first(true)));
    };

    torch::Tensor forward(torch::Tensor x) {
        // Input is [N, T, C], contiguity optional

        auto [y1, h1] = rnn1(x.flip(1));
        auto [y2, h2] = rnn2(y1.flip(1));
        auto [y3, h3] = rnn3(y2.flip(1));
        auto [y4, h4] = rnn4(y3.flip(1));
        auto [y5, h5] = rnn5(y4.flip(1));

        // Output is [N, T, C], non-contiguous
        return y5.flip(1);
    }

    LSTM rnn1{nullptr}, rnn2{nullptr}, rnn3{nullptr}, rnn4{nullptr}, rnn5{nullptr};
};

struct ClampImpl : Module {
    ClampImpl(float _min, float _max, bool _active) : min(_min), max(_max), active(_active){};

    torch::Tensor forward(torch::Tensor x) {
        if (active) {
            return x.clamp(min, max);
        } else {
            return x;
        }
    }

    bool active;
    float min, max;
};

TORCH_MODULE(LSTMStack);
TORCH_MODULE(LinearCRF);
TORCH_MODULE(Convolution);
TORCH_MODULE(Clamp);

template <class LSTMStackType>
struct CRFModelImpl : Module {
    CRFModelImpl(int conv,
                 int size,
                 int outsize,
                 int stride,
                 int decomposition,
                 bool clamp,
                 bool expand_blanks,
                 int batch_size,
                 int chunk_size) {
        constexpr float conv3_max_value = 3.5f;
        conv1 = register_module("conv1", Convolution(1, conv, 5, 1, clamp, 3.5f, false));
        conv2 = register_module("conv2", Convolution(conv, 16, 5, 1, clamp, 3.5f, false));
        conv3 = register_module("conv3",
                                Convolution(16, size, 19, stride, clamp, conv3_max_value, true));

        float scale = 2 * I8_RANGE / (conv3_max_value - SWISH_LOWER_BOUND);
        float zero_offset = scale * conv3_max_value - I8_RANGE;
        rnns = register_module(
                "rnns", LSTMStackType(size, batch_size, chunk_size / stride, scale, zero_offset));

        if (decomposition) {
            linear1 = register_module("linear1", Linear(size, decomposition));
            linear2 = register_module("linear2",
                                      Linear(LinearOptions(decomposition, outsize).bias(false)));
            clamp1 = Clamp(-4.0, 4.0, clamp);
            encoder = Sequential(conv1, conv2, conv3, rnns, linear1, linear2, clamp1);
        } else if (conv == 16) {
            linear1 = register_module("linear1", Linear(LinearOptions(size, outsize).bias(false)));
            clamp1 = Clamp(-4.0, 4.0, clamp);
            encoder = Sequential(conv1, conv2, conv3, rnns, linear1, clamp1);
        } else {
            linear = register_module("linear1", LinearCRF(size, outsize));
            encoder = Sequential(conv1, conv2, conv3, rnns, linear);
        }
    }

    void load_state_dict(const std::vector<torch::Tensor> &weights) {
        utils::load_state_dict(*this, weights);
    }

    torch::Tensor forward(torch::Tensor x) {
        ScopedProfileRange spr("nn_forward");
        return encoder->forward(x);
    }

    std::vector<torch::Tensor> load_weights(const std::filesystem::path &dir,
                                            bool decomposition,
                                            bool bias) {
        auto tensors = std::vector<std::string>{

                "0.conv.weight.tensor",      "0.conv.bias.tensor",

                "1.conv.weight.tensor",      "1.conv.bias.tensor",

                "2.conv.weight.tensor",      "2.conv.bias.tensor",

                "4.rnn.weight_ih_l0.tensor", "4.rnn.weight_hh_l0.tensor",
                "4.rnn.bias_ih_l0.tensor",   "4.rnn.bias_hh_l0.tensor",

                "5.rnn.weight_ih_l0.tensor", "5.rnn.weight_hh_l0.tensor",
                "5.rnn.bias_ih_l0.tensor",   "5.rnn.bias_hh_l0.tensor",

                "6.rnn.weight_ih_l0.tensor", "6.rnn.weight_hh_l0.tensor",
                "6.rnn.bias_ih_l0.tensor",   "6.rnn.bias_hh_l0.tensor",

                "7.rnn.weight_ih_l0.tensor", "7.rnn.weight_hh_l0.tensor",
                "7.rnn.bias_ih_l0.tensor",   "7.rnn.bias_hh_l0.tensor",

                "8.rnn.weight_ih_l0.tensor", "8.rnn.weight_hh_l0.tensor",
                "8.rnn.bias_ih_l0.tensor",   "8.rnn.bias_hh_l0.tensor",

                "9.linear.weight.tensor"};

        if (bias) {
            tensors.push_back("9.linear.bias.tensor");
        }

        if (decomposition) {
            tensors.push_back("10.linear.weight.tensor");
        }

        return utils::load_tensors(dir, tensors);
    }

    LSTMStackType rnns{nullptr};
    LinearCRF linear{nullptr};
    Linear linear1{nullptr}, linear2{nullptr};
    Sequential encoder{nullptr};
    Convolution conv1{nullptr}, conv2{nullptr}, conv3{nullptr};
    Clamp clamp1{nullptr};
};

#if USE_CUDA_LSTM
using CudaCRFModelImpl = CRFModelImpl<CudaLSTMStack>;
TORCH_MODULE(CudaCRFModel);
#endif

using CpuCRFModelImpl = CRFModelImpl<LSTMStack>;
TORCH_MODULE(CpuCRFModel);

}  // namespace nn

std::tuple<ModuleHolder<AnyModule>, size_t> load_crf_model(const std::filesystem::path &path,
                                                           int batch_size,
                                                           int chunk_size,
                                                           torch::TensorOptions options) {
    auto config = toml::parse(path / "config.toml");

    const auto &encoder = toml::find(config, "encoder");
    const auto &global_norm = toml::find(config, "global_norm");
    const auto state_len = toml::find<int>(global_norm, "state_len");

    int conv = 4;
    int insize = 0;
    int stride = 1;
    bool bias = true;
    bool clamp = false;
    int decomposition = 0;

    if (encoder.contains("type")) {
        for (const auto &segment : toml::find(config, "encoder", "sublayers").as_array()) {
            const auto type = toml::find<std::string>(segment, "type");
            if (type.compare("convolution") == 0) {
                stride *= toml::find<int>(segment, "stride");
            } else if (type.compare("lstm") == 0) {
                insize = toml::find<int>(segment, "size");
            } else if (type.compare("linear") == 0) {
                decomposition = toml::find<int>(segment, "out_features");
            } else if (type.compare("clamp") == 0) {
                clamp = true;
            }
        }
        conv = 16;
        bias = static_cast<bool>(insize > 128);
    } else {
        stride = toml::find<int>(encoder, "stride");
        insize = toml::find<int>(encoder, "features");
    }

    int outsize = pow(4, state_len) * 4;

#if USE_CUDA_LSTM
    if (options.device() != torch::kCPU) {
        bool expand = false;
        auto model = nn::CudaCRFModel(conv, insize, outsize, stride, decomposition, clamp, expand,
                                      batch_size, chunk_size);
        auto holder = populate_model(model, path, options, static_cast<bool>(decomposition), bias);
        return {holder, static_cast<size_t>(stride)};
    } else
#endif
    {
        bool expand = true;
        auto model = nn::CpuCRFModel(conv, insize, outsize, stride, decomposition, clamp, expand,
                                     batch_size, chunk_size);
        auto holder = populate_model(model, path, options, static_cast<bool>(decomposition), bias);
        return {holder, static_cast<size_t>(stride)};
    }
}

}  // namespace dorado
