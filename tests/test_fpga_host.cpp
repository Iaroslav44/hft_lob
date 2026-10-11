#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include "nexus/hw/xdma_bridge.h"

namespace nexus::hls {
extern "C" void hls4ml_predictor(const float input_features[16],
                                 float output_predictions[2]);
}

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                         __FILE__, __LINE__, #condition);                        \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

// Descriptor ownership cannot be duplicated: destruction of a copied bridge
// used to close the original's descriptors, potentially closing reused FDs too.
static_assert(!std::is_copy_constructible_v<nexus::hw::XdmaBridge>);
static_assert(!std::is_copy_assignable_v<nexus::hw::XdmaBridge>);

void test_reference_kernel() {
    std::array<float, 16> inputs{};
    std::array<float, 4> outputs{17.0F, 0.0F, 0.0F, 19.0F};
    inputs[7] = 1.0F;
    const auto original = inputs;
    nexus::hls::hls4ml_predictor(inputs.data(), outputs.data() + 1);
    CHECK(inputs == original);
    CHECK(outputs[0] == 17.0F && outputs[3] == 19.0F);
    CHECK(outputs[1] == 0.1F && outputs[2] == -0.1F);

    for (std::size_t i = 0; i < inputs.size(); ++i)
        inputs[i] = (i & 1) == 0 ? 1.0F : -1.0F;
    nexus::hls::hls4ml_predictor(inputs.data(), outputs.data() + 1);
    CHECK(outputs[1] == 0.0F && outputs[2] == 0.0F);
}

void test_descriptor_owner() {
    // Host-only descriptor smoke; /dev/null supplies no XDMA or DMA semantics.
    nexus::hw::XdmaBridge bridge("/dev/null", "/dev/null");
    char byte{};
    CHECK(bridge.read_fpga(0, &byte, 1) == 0);
    CHECK(bridge.write_fpga(0, &byte, 1) == 1);
}

}  // namespace

int main() {
    test_reference_kernel();
    test_descriptor_owner();
    std::puts("FPGA host reference tests passed; no board or DMA transaction exercised");
}
