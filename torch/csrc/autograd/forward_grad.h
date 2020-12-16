#pragma once

#include <ATen/ATen.h>


namespace torch { namespace autograd {

struct ForwardGrad;


// This file contains two classes that are used to store forward AD gradients and
// ensure that they are scoped properly.
// Because forward AD runs concurently with the evaluation of the function, we need
// a mechanism to separate different forward AD invocations and be able to compute the
// right gradients. We model such invocations as levels here.
// The particular scoping issue mentionned above has two main drive:
//   - Ensure that we can conveniently use forward AD within a high level API without
//     leaking the forward AD states outside.
//   - Ensure that we can keep the level that we expose to the user API simple (an integer
//     that represents the nesting depth) while avoiding confusions when the level index
//     is re-used.

// The important external APIs from this file are:
//   - ForwardADLevel::get_next_idx() that can be used to enter a new level and get its index
//   - ForwardADLevel::release_idx() that can be used to exit a given level.
//   - ForwardGrad() can be used to store a given forward gradient that will handle the level
//     tracking automatically.

// The behavior above is achieved by ensuring that the ForwardGrad containing gradients for a
// given level register themselves properly with the corresponding level.
// On the other hand, the level, when it is released, will reset all the gradients for this
// level on all the ForwardGrad.


// Data structures in this file are optimized for this maximum number of levels.
// The number of levels corresponds to the degree of the gradient being
// computed using forward AD and we don't expect more than second order gradients
// to be common.
#define EXPECTED_MAX_LEVEL 2

struct TORCH_API ForwardADLevel {
    ForwardADLevel(uint64_t idx): idx_(idx) {}
    ~ForwardADLevel();

    static uint64_t get_next_idx();
    static void release_idx(uint64_t idx);
    static std::shared_ptr<ForwardADLevel> get_by_idx(uint64_t idx);
    static std::shared_ptr<ForwardADLevel> try_get_by_idx(uint64_t idx);

    void erase(const std::shared_ptr<ForwardGrad>& grad) {
        std::lock_guard<std::mutex> lock(mutex_);
        grads_.erase(grad);
    }

    void insert(const std::shared_ptr<ForwardGrad>& grad) {
        std::lock_guard<std::mutex> lock(mutex_);
        grads_.insert(grad);
    }

private:
    std::unordered_set<std::shared_ptr<ForwardGrad>> grads_;
    std::mutex mutex_;
    uint64_t idx_;

};

struct TORCH_API ForwardGrad : std::enable_shared_from_this<ForwardGrad> {

    ForwardGrad() {}

    // This function must only be called when AutogradMeta is being destructed
    // as it ensures that:
    //   - The only (potential) other references to this ForwardGrad are the
    //     different level it is registered to
    //   - No other thread will try to call `set_value` or `value` ever from now on
    //   - Any of the ForwardADLevel that this ForwardGrad is registered with migh
    //     call `reset` at any point during this function
    void clear() {
        c10::SmallVector<uint64_t, EXPECTED_MAX_LEVEL> levels_idx;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& c: content_) {
                levels_idx.push_back(c.first);
            }
        }

        for (auto l_idx: levels_idx) {
            // Use "try" version here as another thread might have deleted this
            // level before we got here
            // This is an owning reference as we want to keep the level alive
            // until we successfully unregister ourselves
            auto level = ForwardADLevel::try_get_by_idx(l_idx);
            if (level) {
                level->erase(shared_from_this());
            }
        }
    }

    void set_value(const at::Tensor& value, uint64_t level) {
        ForwardADLevel::get_by_idx(level)->insert(shared_from_this());

        std::lock_guard<std::mutex> lock(mutex_);
        content_.insert({level, value});
    }

    void reset(uint64_t level, bool update_level=true) {
        if (update_level) {
            ForwardADLevel::get_by_idx(level)->erase(shared_from_this());
        }

        std::lock_guard<std::mutex> lock(mutex_);
        content_.erase(level);
    }

    const at::Tensor& value(uint64_t level) const;

    bool contains(uint64_t level) {
        std::lock_guard<std::mutex> lock(mutex_);
        return content_.count(level) > 0;
    }

    bool empty() const {
        return content_.empty();
    }

    static const at::Tensor& undef_grad();


private:
    // TODO(albanD): replace this with a SmallVector
    std::unordered_map<uint64_t, at::Tensor> content_;
    mutable std::mutex mutex_;

};

// Temporary functions to disable forward AD
// TODO(alband) remove these when perf issues are solved
bool TORCH_API isForwardADEnabled();
void TORCH_API setForwardADEnabled(bool value);

}} // namespace torch::autograd