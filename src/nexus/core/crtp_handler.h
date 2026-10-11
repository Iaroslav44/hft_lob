#pragma once
#include <utility>

namespace nexus::core {

template <typename Derived>
class MarketDataHandlerBase {
public:
    template <typename Message>
    void on_message(const Message& msg) {
        static_cast<Derived*>(this)->handle_message(msg);
    }
};

} // namespace nexus::core
