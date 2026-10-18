/// SPDX-License-Identifier: MIT

#include "core/adapter.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fy::core {

struct IoChannel::Impl {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> pending;
    bool stopping = false;
    std::thread worker;

    Impl() : worker([this] {
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return stopping || !pending.empty(); });
                if (pending.empty()) return;
                action = std::move(pending.front());
                pending.pop_front();
            }
            action();
        }
    }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        worker.join();
    }
};

IoChannel::IoChannel() : impl_(std::make_unique<Impl>()) {}
IoChannel::~IoChannel() = default;

bool IoChannel::on_worker() const { return std::this_thread::get_id() == impl_->worker.get_id(); }

void IoChannel::enqueue(std::function<void()> action) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) throw std::runtime_error("I/O channel is stopping");
        impl_->pending.push_back(std::move(action));
    }
    impl_->ready.notify_one();
}

std::shared_ptr<IoChannel> IoChannel::shared() {
    static auto channel = std::make_shared<IoChannel>();
    return channel;
}

struct SensorSession::Impl {
    std::shared_ptr<IoChannel> channel;
    std::unique_ptr<SensorAdapter> adapter;

    ~Impl() {
        if (adapter) channel->run([this] { adapter.reset(); });
    }
};

SensorSession::SensorSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SensorSession::SensorSession(SensorSession&&) noexcept = default;
SensorSession& SensorSession::operator=(SensorSession&&) noexcept = default;
SensorSession::~SensorSession() = default;

SensorMetadata SensorSession::metadata() const {
    if (!impl_) throw std::logic_error("Moved sensor session");
    return impl_->channel->run([this] {
        auto metadata = impl_->adapter->metadata();
        metadata.validate();
        return metadata;
    });
}

std::vector<BandDescriptor> SensorSession::bands() const {
    if (!impl_) throw std::logic_error("Moved sensor session");
    return impl_->channel->run([this] { return impl_->adapter->bands(); });
}

SampleBlock SensorSession::read_window(int band, Window window) {
    if (!impl_) throw std::logic_error("Moved sensor session");
    return impl_->channel->run([this, band, window] {
        auto descriptors = impl_->adapter->bands();
        auto found = std::ranges::find(descriptors, band, &BandDescriptor::id);
        if (found == descriptors.end()) throw std::invalid_argument("Unknown adapter band");
        auto metadata = impl_->adapter->metadata();
        window.validate(metadata.grid(found->grid).shape);
        auto block = impl_->adapter->read_window(band, window);
        if (block.band != band || block.window.row != window.row || block.window.col != window.col ||
            block.window.shape != window.shape)
            throw std::invalid_argument("Adapter returned wrong window");
        block.validate();
        return block;
    });
}

NativeGeometry SensorSession::read_geometry(const std::string& grid, Window window) {
    if (!impl_) throw std::logic_error("Moved sensor session");
    return impl_->channel->run([this, &grid, window] {
        auto metadata = impl_->adapter->metadata();
        window.validate(metadata.grid(grid).shape);
        auto geometry = impl_->adapter->read_geometry(grid, window);
        if (geometry_shape(geometry) != window.shape)
            throw std::invalid_argument("Adapter returned wrong geometry window");
        validate_geometry(geometry);
        return geometry;
    });
}

void AdapterRegistry::add(std::string key, Factory factory) {
    if (key.empty() || !factory) throw std::invalid_argument("Invalid adapter registration");
    if (!factories_.emplace(std::move(key), std::move(factory)).second)
        throw std::invalid_argument("Duplicate adapter registration");
}

SensorSession AdapterRegistry::open(const std::string& key, const SourceFiles& sources,
                                    std::shared_ptr<IoChannel> channel) const {
    if (!channel) throw std::invalid_argument("Missing I/O channel");
    const auto& factory = factories_.at(key);
    auto impl = std::make_unique<SensorSession::Impl>();
    impl->channel = std::move(channel);
    impl->channel->run([&] {
        impl->adapter = factory(sources);
        if (!impl->adapter) throw std::runtime_error("Adapter factory returned null");
        impl->adapter->metadata().validate();
    });
    return SensorSession(std::move(impl));
}

}
