#include "amdva/batching.hpp"
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
namespace amdva {
namespace {
ModelInfo processing_info(ModelInfo info, bool raw) {
    if (raw)
        info.task = "raw";
    return info;
}
} // namespace
struct SharedBatchExecutor::Impl {
    struct Job {
        InferenceRequest request;
        std::promise<AnalysisResult> promise;
        std::chrono::steady_clock::time_point queued;
    };
    BatchOptions options;
    ModelInfo info;
    ModelExecutor engine;
    ModelPostprocessor post;
    ImagePacker packer;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::unique_ptr<Job>> queue;
    std::map<std::string, uint64_t> retired;
    std::set<std::string> released;
    std::vector<std::string> active;
    bool stop{};
    std::thread worker;
    Impl(BatchOptions o)
        : options(std::move(o)), info(ModelInfo::load(options.model_info)),
          engine(options.model, options.device),
          post(processing_info(info, options.raw_outputs), options.device), packer(options.device) {
        if (options.size != engine.batch() || !options.queue_limit || options.timeout_ms > 10000)
            throw std::runtime_error("Invalid shared batch configuration");
        const auto inputs = engine.inputs();
        if (inputs.size() != 1 || inputs[0].name != info.input_name ||
            inputs[0].type != TensorType::Float32 ||
            inputs[0].shape != std::vector<size_t>{options.size, 3, info.height, info.width})
            throw std::runtime_error("Image batching requires one configured float32 NCHW input");
        worker = std::thread([this] { run(); });
    }
    void cleanup() {
        for (auto it = released.begin(); it != released.end();) {
            bool busy = false;
            for (const auto &source : active)
                if (source == *it || source.rfind(*it + "/", 0) == 0)
                    busy = true;
            if (busy) {
                ++it;
                continue;
            }
            for (auto r = retired.begin(); r != retired.end();)
                if (r->first == *it || r->first.rfind(*it + "/", 0) == 0)
                    r = retired.erase(r);
                else
                    ++r;
            it = released.erase(it);
        }
    }
    bool cancelled(const InferenceRequest &r) const {
        for (const auto &[source, generation] : retired)
            if ((r.source == source || r.source.rfind(source + "/", 0) == 0) &&
                r.generation <= generation)
                return true;
        return false;
    }
    void run() {
        try {
            hip_check(hipSetDevice(options.device), "Batch worker device");
            auto input = engine.inputs()[0];
            input.storage = std::make_shared<DeviceBuffer>(size_t(options.size) * 3 * info.width *
                                                           info.height * sizeof(float));
            const size_t bytes = input.storage->size() / options.size;
            for (;;) {
                std::vector<std::unique_ptr<Job>> jobs;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [this] { return stop || !queue.empty(); });
                    if (stop)
                        break;
                    const auto deadline =
                        queue.front()->queued + std::chrono::milliseconds(options.timeout_ms);
                    ready.wait_until(lock, deadline,
                                     [this] { return stop || queue.size() >= options.size; });
                    if (stop)
                        break;
                    // At most one frame per source per batch; rotate deferred jobs.
                    std::set<std::string> sources;
                    auto count = queue.size();
                    while (count-- && !queue.empty() && jobs.size() < options.size) {
                        auto job = std::move(queue.front());
                        queue.pop_front();
                        if (cancelled(job->request)) {
                            job->promise.set_exception(std::make_exception_ptr(
                                std::runtime_error("Source generation retired")));
                            continue;
                        }
                        if (!sources.insert(job->request.source).second) {
                            queue.push_back(std::move(job));
                            continue;
                        }
                        jobs.push_back(std::move(job));
                    }
                    active.clear();
                    for (const auto &job : jobs)
                        active.push_back(job->request.source);
                }
                if (jobs.empty())
                    continue;
                try {
                    for (unsigned i = 0; i < jobs.size(); i++)
                        hip_check(hipMemcpy(static_cast<char *>(input.storage->data()) + i * bytes,
                                            jobs[i]->request.tensor.storage->data(), bytes,
                                            hipMemcpyDeviceToDevice),
                                  "GPU batch assembly");
                    packer.pad(input.storage->data(), jobs.size(), options.size, info.width,
                               info.height, info.normalization);
                    hip_check(hipDeviceSynchronize(), "Batch input completion");
                    auto started = std::chrono::steady_clock::now();
                    auto output = engine.infer({{input.name, input}});
                    auto inferred = std::chrono::steady_clock::now();
                    auto results =
                        post.process(output, jobs.size(), options.confidence, options.nms);
                    if (options.measured)
                        options.measured(
                            std::chrono::duration<double, std::milli>(inferred - started).count(),
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - inferred)
                                .count(),
                            jobs.size());
                    for (unsigned i = 0; i < jobs.size(); i++) {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (cancelled(jobs[i]->request))
                            jobs[i]->promise.set_exception(std::make_exception_ptr(
                                std::runtime_error("Source generation retired")));
                        else
                            jobs[i]->promise.set_value(std::move(results[i]));
                    }
                } catch (...) {
                    auto error = std::current_exception();
                    for (auto &job : jobs)
                        job->promise.set_exception(error);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    active.clear();
                    cleanup();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            for (auto &job : queue)
                job->promise.set_exception(std::current_exception());
            queue.clear();
        }
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            for (auto &j : queue)
                j->promise.set_exception(
                    std::make_exception_ptr(std::runtime_error("Batch executor stopped")));
            queue.clear();
        }
        ready.notify_all();
        if (worker.joinable())
            worker.join();
    }
};
SharedBatchExecutor::SharedBatchExecutor(BatchOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
SharedBatchExecutor::~SharedBatchExecutor() = default;
std::shared_ptr<SharedBatchExecutor> SharedBatchExecutor::acquire(const BatchOptions &o) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::string, std::weak_ptr<SharedBatchExecutor>>>
        registry;
    std::ostringstream signature;
    signature << std::setprecision(std::numeric_limits<float>::max_digits10) << o.model << '\n'
              << o.model_info << '\n'
              << o.device << ' ' << o.size << ' ' << o.timeout_ms << ' ' << o.queue_limit << ' '
              << o.confidence << ' ' << o.nms << ' ' << o.raw_outputs;
    std::lock_guard<std::mutex> lock(mutex);
    if (o.instance_id.empty())
        return std::shared_ptr<SharedBatchExecutor>(new SharedBatchExecutor(o));
    for (auto it = registry.begin(); it != registry.end();)
        if (it->second.second.expired())
            it = registry.erase(it);
        else
            ++it;
    auto it = registry.find(o.instance_id);
    if (it != registry.end()) {
        if (it->second.first != signature.str())
            throw std::runtime_error("model-instance-id has incompatible settings");
        if (auto existing = it->second.second.lock())
            return existing;
    }
    auto created = std::shared_ptr<SharedBatchExecutor>(new SharedBatchExecutor(o));
    registry[o.instance_id] = {signature.str(), created};
    return created;
}
std::future<AnalysisResult> SharedBatchExecutor::submit(InferenceRequest request) {
    std::vector<InferenceRequest> requests;
    requests.push_back(std::move(request));
    auto futures = submit_many(std::move(requests));
    return std::move(futures[0]);
}
std::vector<std::future<AnalysisResult>>
SharedBatchExecutor::submit_many(std::vector<InferenceRequest> requests) {
    auto &s = *impl_;
    std::vector<std::unique_ptr<Impl::Job>> jobs;
    std::vector<std::future<AnalysisResult>> futures;
    for (auto &request : requests) {
        const auto &t = request.tensor;
        if (request.source.empty() || !t.storage || t.type != TensorType::Float32 ||
            t.device != s.options.device ||
            t.shape != std::vector<size_t>{1, 3, s.info.height, s.info.width} ||
            t.storage->size() != size_t(3) * s.info.width * s.info.height * sizeof(float))
            throw std::runtime_error("Invalid batch request");
        auto job = std::make_unique<Impl::Job>();
        job->request = std::move(request);
        job->queued = std::chrono::steady_clock::now();
        futures.push_back(job->promise.get_future());
        jobs.push_back(std::move(job));
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stop)
            throw std::runtime_error("Batch executor stopped");
        if (s.queue.size() + jobs.size() > s.options.queue_limit)
            throw std::runtime_error("Batch queue capacity exceeded");
        for (auto &job : jobs)
            if (s.cancelled(job->request))
                throw std::runtime_error("Source generation retired");
        for (auto &job : jobs)
            s.queue.push_back(std::move(job));
    }
    s.ready.notify_one();
    return futures;
}
void SharedBatchExecutor::cancel(const std::string &source, uint64_t generation) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    auto [it, inserted] = s.retired.emplace(source, generation);
    if (!inserted)
        it->second = std::max(it->second, generation);
    for (auto q = s.queue.begin(); q != s.queue.end();)
        if (((*q)->request.source == source || (*q)->request.source.rfind(source + "/", 0) == 0) &&
            (*q)->request.generation <= generation) {
            (*q)->promise.set_exception(
                std::make_exception_ptr(std::runtime_error("Source generation retired")));
            q = s.queue.erase(q);
        } else
            ++q;
    s.ready.notify_all();
}
void SharedBatchExecutor::release_source(const std::string &source, uint64_t generation) {
    cancel(source, generation);
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.released.insert(source);
    s.cleanup();
}
const ModelInfo &SharedBatchExecutor::info() const { return impl_->info; }
} // namespace amdva
