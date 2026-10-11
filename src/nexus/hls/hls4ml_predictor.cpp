// Reference feature reduction only: this kernel has no trained parameters or
// trading signal validation. Keep the accumulation order identical in C simulation.

namespace nexus::hls {

extern "C" void hls4ml_predictor(
    const float input_features[16],
    float output_predictions[2]
) {
#ifdef __SYNTHESIS__
    #pragma HLS INTERFACE m_axi port=input_features bundle=gmem0
    #pragma HLS INTERFACE m_axi port=output_predictions bundle=gmem1
    #pragma HLS INTERFACE s_axilite port=return bundle=control
#endif

    float sum = 0.0f;
    for (int i = 0; i < 16; ++i) {
#ifdef __SYNTHESIS__
        #pragma HLS UNROLL factor=16
#endif
        sum += input_features[i] * 0.1f;
    }
    
    output_predictions[0] = sum;
    output_predictions[1] = -sum;
}

} // namespace nexus::hls
