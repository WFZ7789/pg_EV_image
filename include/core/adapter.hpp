/// SPDX-License-Identifier: MIT

#pragma once

#include "core/model.hpp"

#include <future>
#include <map>

namespace fy::core {

/// @brief 所有适配器的打开、查询、读取与关闭通过同一个工作线程执行。
class IoChannel {
public:
    IoChannel();
    ~IoChannel();
    IoChannel(const IoChannel&) = delete;
    IoChannel& operator=(const IoChannel&) = delete;

    template<class F>
    auto run(F&& action) -> std::invoke_result_t<F> {
        if (on_worker()) return std::invoke(std::forward<F>(action));
        return submit(std::forward<F>(action)).get();
    }

    /// @note 异步回调借用的对象必须存活到 future 完成。
    template<class F>
    auto submit(F&& action) -> std::future<std::invoke_result_t<F>> {
        auto task = std::make_shared<std::packaged_task<std::invoke_result_t<F>()>>(std::forward<F>(action));
        auto result = task->get_future();
        if (on_worker()) (*task)();
        else enqueue([task] { (*task)(); });
        return result;
    }

    static std::shared_ptr<IoChannel> shared();

private:
    bool on_worker() const;
    void enqueue(std::function<void()> action);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SensorAdapter {
public:
    virtual ~SensorAdapter() = default;
    virtual SensorMetadata metadata() const = 0;
    virtual std::vector<BandDescriptor> bands() const = 0;
    virtual SampleBlock read_window(int band, Window window) = 0;
    virtual NativeGeometry read_geometry(const std::string& grid, Window window) = 0;
};

class SensorSession {
public:
    SensorSession(SensorSession&&) noexcept;
    SensorSession& operator=(SensorSession&&) noexcept;
    ~SensorSession();
    SensorSession(const SensorSession&) = delete;
    SensorSession& operator=(const SensorSession&) = delete;

    SensorMetadata metadata() const;
    std::vector<BandDescriptor> bands() const;
    SampleBlock read_window(int band, Window window);
    NativeGeometry read_geometry(const std::string& grid, Window window);

private:
    friend class AdapterRegistry;
    struct Impl;
    explicit SensorSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class AdapterRegistry {
public:
    using Factory = std::function<std::unique_ptr<SensorAdapter>(const SourceFiles&)>;
    void add(std::string key, Factory factory);
    SensorSession open(const std::string& key, const SourceFiles& sources,
                       std::shared_ptr<IoChannel> channel = IoChannel::shared()) const;

private:
    std::map<std::string, Factory> factories_;
};

}
