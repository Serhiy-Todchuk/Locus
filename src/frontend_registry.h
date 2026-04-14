#pragma once

#include "frontend.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

namespace locus {

// Thread-safe frontend registry. Manages registration and fan-out dispatch.
// Exception-safe: one frontend throwing does not prevent others from receiving
// the event.
class FrontendRegistry {
public:
    void register_frontend(IFrontend* fe)
    {
        std::lock_guard lock(mutex_);
        frontends_.push_back(fe);
        spdlog::trace("FrontendRegistry: registered ({} total)", frontends_.size());
    }

    void unregister_frontend(IFrontend* fe)
    {
        std::lock_guard lock(mutex_);
        frontends_.erase(
            std::remove(frontends_.begin(), frontends_.end(), fe),
            frontends_.end());
        spdlog::trace("FrontendRegistry: unregistered ({} remain)", frontends_.size());
    }

    bool empty() const
    {
        std::lock_guard lock(mutex_);
        return frontends_.empty();
    }

    // Fan-out: call fn on every registered frontend. If a frontend throws,
    // log the error and continue to the next.
    void broadcast(const std::function<void(IFrontend&)>& fn) const
    {
        std::lock_guard lock(mutex_);
        for (auto* fe : frontends_) {
            try {
                fn(*fe);
            } catch (const std::exception& ex) {
                spdlog::error("FrontendRegistry: callback threw: {}", ex.what());
            }
        }
    }

private:
    mutable std::mutex       mutex_;
    std::vector<IFrontend*>  frontends_;
};

} // namespace locus
