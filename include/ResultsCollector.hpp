#pragma once
#include "Order.hpp"
#include <vector>
#include <mutex>

class ResultsCollector {
public:
    void add(ValidationResult r) {
        std::lock_guard<std::mutex> lock(mtx_);
        results_.push_back(std::move(r));
    }

    std::vector<ValidationResult> takeAll() {
        std::lock_guard<std::mutex> lock(mtx_);
        return results_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return results_.size();
    }

private:
    mutable std::mutex mtx_;
    std::vector<ValidationResult> results_;
};
