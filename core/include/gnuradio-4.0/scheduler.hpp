#ifndef GNURADIO_SCHEDULER_HPP
#define GNURADIO_SCHEDULER_HPP

#include <barrier>
#include <set>
#include <utility>
#include <queue>

#include "graph.hpp"
#include "profiler.hpp"
#include "thread/thread_pool.hpp"

namespace gr::scheduler {
using gr::thread_pool::BasicThreadPool;

enum execution_policy { single_threaded, multi_threaded };

enum SchedulerState { IDLE, INITIALISED, RUNNING, REQUESTED_STOP, REQUESTED_PAUSE, STOPPED, PAUSED, SHUTTING_DOWN, ERROR };

template<profiling::Profiler Profiler = profiling::null::profiler>
class scheduler_base {
protected:
    SchedulerState                        _state = IDLE;
    gr::graph                             _graph;
    Profiler                              _profiler;
    decltype(_profiler.for_this_thread()) _profiler_handler;
    std::shared_ptr<BasicThreadPool>      _pool;
    std::atomic_uint64_t                  _progress;
    std::atomic_size_t                    _running_threads;
    std::atomic_bool                      _stop_requested;

public:
    explicit scheduler_base(gr::graph &&graph, std::shared_ptr<BasicThreadPool> thread_pool = std::make_shared<BasicThreadPool>("simple-scheduler-pool", thread_pool::CPU_BOUND),
                            const profiling::options &profiling_options = {})
        : _graph(std::move(graph)), _profiler{ profiling_options }, _profiler_handler{ _profiler.for_this_thread() }, _pool(std::move(thread_pool)) {}

    ~scheduler_base() {
        stop();
        _state = SHUTTING_DOWN;
    }

    void
    stop() {
        if (_state == STOPPED || _state == ERROR) {
            return;
        }
        if (_state == RUNNING) {
            request_stop();
        }
        wait_done();
        _state = STOPPED;
    }

    void
    pause() {
        if (_state == PAUSED || _state == ERROR) {
            return;
        }
        if (_state == RUNNING) {
            request_pause();
        }
        wait_done();
        _state = PAUSED;
    }

    void
    wait_done() {
        [[maybe_unused]] const auto pe = _profiler_handler.start_complete_event("scheduler_base.wait_done");
        for (auto running = _running_threads.load(); running > 0ul; running = _running_threads.load()) {
            _running_threads.wait(running);
        }
        if (_state == REQUESTED_PAUSE) {
            _state = PAUSED;
        } else {
            _state = STOPPED;
        }
    }

    void
    request_stop() {
        _stop_requested = true;
        _state          = REQUESTED_STOP;
    }

    void
    request_pause() {
        _stop_requested = true;
        _state          = REQUESTED_PAUSE;
    }

    void
    init() {
        [[maybe_unused]] const auto pe = _profiler_handler.start_complete_event("scheduler_base.init");
        if (_state != IDLE) {
            return;
        }
        auto result = std::all_of(_graph.connection_definitions().begin(), _graph.connection_definitions().end(),
                                  [this](auto &connection_definition) { return connection_definition(_graph) == connection_result_t::SUCCESS; });
        if (result) {
            _graph.clear_connection_definitions();
            _state = INITIALISED;
        } else {
            _state = ERROR;
        }
    }

    void
    reset() {
        // since it is not possible to set up the graph connections a second time, this method leaves the graph in the initialized state with clear buffers.
        switch (_state) {
        case IDLE: init(); break;
        case RUNNING:
        case REQUESTED_STOP:
        case REQUESTED_PAUSE:
            pause();
            // intentional fallthrough
            FMT_FALLTHROUGH;
        case STOPPED:
            // clear buffers
            // std::for_each(_graph.edges().begin(), _graph.edges().end(), [](auto &edge) {
            //
            // });
            FMT_FALLTHROUGH;
        case PAUSED: _state = INITIALISED; break;
        case SHUTTING_DOWN:
        case INITIALISED:
        case ERROR: break;
        }
    }

    void
    run_on_pool(const std::vector<std::vector<node_model *>> &jobs, const std::function<work_return_t(const std::span<node_model *const> &)> work_function) {
        [[maybe_unused]] const auto pe = _profiler_handler.start_complete_event("scheduler_base.run_on_pool");
        _progress                      = 0;
        _running_threads               = jobs.size();
        for (auto &jobset : jobs) {
            _pool->execute([this, &jobset, work_function, &jobs]() { pool_worker([&work_function, &jobset]() { return work_function(jobset); }, jobs.size()); });
        }
    }

    void
    pool_worker(const std::function<work_return_t()> &work, std::size_t n_batches) {
        auto    &profiler_handler = _profiler.for_this_thread();

        uint32_t done             = 0;
        uint32_t progress_count   = 0;
        while (done < n_batches && !_stop_requested) {
            auto pe                 = profiler_handler.start_complete_event("scheduler_base.work");
            bool something_happened = work().status == work_return_status_t::OK;
            pe.finish();
            uint64_t progress_local, progress_new;
            if (something_happened) { // something happened in this thread => increase progress and reset done count
                do {
                    progress_local = _progress.load();
                    progress_count = static_cast<std::uint32_t>((progress_local >> 32) & ((1ULL << 32) - 1));
                    done           = static_cast<std::uint32_t>(progress_local & ((1ULL << 32) - 1));
                    progress_new   = (progress_count + 1ULL) << 32;
                } while (!_progress.compare_exchange_strong(progress_local, progress_new));
                _progress.notify_all();
            } else { // nothing happened on this thread
                uint32_t progress_count_old = progress_count;
                do {
                    progress_local = _progress.load();
                    progress_count = static_cast<std::uint32_t>((progress_local >> 32) & ((1ULL << 32) - 1));
                    done           = static_cast<std::uint32_t>(progress_local & ((1ULL << 32) - 1));
                    if (progress_count == progress_count_old) { // nothing happened => increase done count
                        progress_new = ((progress_count + 0ULL) << 32) + done + 1;
                    } else { // something happened in another thread => keep progress and done count and rerun this task without waiting
                        progress_new = ((progress_count + 0ULL) << 32) + done;
                    }
                } while (!_progress.compare_exchange_strong(progress_local, progress_new));
                _progress.notify_all();
                if (progress_count == progress_count_old && done < n_batches) {
                    _progress.wait(progress_new);
                }
            }
        } // while (done < n_batches)
        _running_threads.fetch_sub(1);
        _running_threads.notify_all();
    }
};

/**
 * Trivial loop based scheduler, which iterates over all nodes in definition order in the graph until no node did any processing
 */
template<execution_policy executionPolicy = single_threaded, profiling::Profiler Profiler = profiling::null::profiler>
class simple : public scheduler_base<Profiler> {
    std::vector<std::vector<node_model *>> _job_lists{};

public:
    explicit simple(gr::graph &&graph, std::shared_ptr<BasicThreadPool> thread_pool = std::make_shared<BasicThreadPool>("simple-scheduler-pool", thread_pool::CPU_BOUND),
                    const profiling::options &profiling_options = {})
        : scheduler_base<Profiler>(std::forward<gr::graph>(graph), thread_pool, profiling_options) {}

    void
    init() {
        scheduler_base<Profiler>::init();
        [[maybe_unused]] const auto pe = this->_profiler_handler.start_complete_event("scheduler_simple.init");
        // generate job list
        if constexpr (executionPolicy == multi_threaded) {
            const auto n_batches = std::min(static_cast<std::size_t>(this->_pool->maxThreads()), this->_graph.blocks().size());
            this->_job_lists.reserve(n_batches);
            for (std::size_t i = 0; i < n_batches; i++) {
                // create job-set for thread
                auto &job = this->_job_lists.emplace_back();
                job.reserve(this->_graph.blocks().size() / n_batches + 1);
                for (std::size_t j = i; j < this->_graph.blocks().size(); j += n_batches) {
                    job.push_back(this->_graph.blocks()[j].get());
                }
            }
        }
    }

    template<typename node_type>
    work_return_t
    work_once(const std::span<node_type> &nodes) {
        constexpr std::size_t requested_work     = std::numeric_limits<std::size_t>::max();
        bool                  something_happened = false;
        std::size_t           performed_work     = 0_UZ;
        for (auto &currentNode : nodes) {
            auto result = currentNode->work(requested_work);
            performed_work += result.performed_work;
            if (result.status == work_return_status_t::ERROR) {
                return { requested_work, performed_work, work_return_status_t::ERROR };
            } else if (result.status == work_return_status_t::INSUFFICIENT_INPUT_ITEMS || result.status == work_return_status_t::DONE) {
                // nothing
            } else if (result.status == work_return_status_t::OK || result.status == work_return_status_t::INSUFFICIENT_OUTPUT_ITEMS) {
                something_happened = true;
            }
            if (currentNode->is_blocking()) { // work-around for `DONE` issue when running with multithreaded BlockingIO blocks -> TODO: needs a better solution on a global scope
                std::vector<std::size_t> available_input_samples(20);
                std::ignore = currentNode->available_input_samples(available_input_samples);
                something_happened |= std::accumulate(available_input_samples.begin(), available_input_samples.end(), 0_UZ) > 0_UZ;
            }
        }
        return { requested_work, performed_work, something_happened ? work_return_status_t::OK : work_return_status_t::DONE };
    }

    // todo: could be moved to base class, but would make `start()` virtual or require CRTP
    // todo: iterate api for continuous flowgraphs vs ones that become "DONE" at some point
    void
    run_and_wait() {
        [[maybe_unused]] const auto pe = this->_profiler_handler.start_complete_event("scheduler_simple.run_and_wait");
        start();
        this->wait_done();
    }

    void
    start() {
        switch (this->_state) {
        case IDLE: this->init(); break;
        case STOPPED: this->reset(); break;
        case PAUSED: this->_state = INITIALISED; break;
        case INITIALISED:
        case RUNNING:
        case REQUESTED_PAUSE:
        case REQUESTED_STOP:
        case SHUTTING_DOWN:
        case ERROR: break;
        }
        if (this->_state != INITIALISED) {
            throw std::runtime_error("simple scheduler work(): graph not initialised");
        }
        if constexpr (executionPolicy == single_threaded) {
            this->_state = RUNNING;
            work_return_t result;
            auto          nodelist = std::span{ this->_graph.blocks() };
            do {
                result = work_once(nodelist);
            } while (result.status == work_return_status_t::OK);
            if (result.status == work_return_status_t::ERROR) {
                this->_state = ERROR;
            } else {
                this->_state = STOPPED;
            }
        } else if (executionPolicy == multi_threaded) {
            this->run_on_pool(this->_job_lists, [this](auto &job) { return this->work_once(job); });
        } else {
            throw std::invalid_argument("Unknown execution Policy");
        }
    }
};

/**
 * Breadth first traversal scheduler which traverses the graph starting from the source nodes in a breath first fashion
 * detecting cycles and nodes which can be reached from several source nodes.
 */
template<execution_policy executionPolicy = single_threaded, profiling::Profiler Profiler = profiling::null::profiler>
class breadth_first : public scheduler_base<Profiler> {
    std::vector<node_model *>              _nodelist;
    std::vector<std::vector<node_model *>> _job_lists{};

public:
    explicit breadth_first(gr::graph &&graph, std::shared_ptr<BasicThreadPool> thread_pool = std::make_shared<BasicThreadPool>("breadth-first-pool", thread_pool::CPU_BOUND),
                           const profiling::options &profiling_options = {})
        : scheduler_base<Profiler>(std::move(graph), thread_pool, profiling_options) {}

    void
    init() {
        [[maybe_unused]] const auto pe = this->_profiler_handler.start_complete_event("breadth_first.init");
        using node_t                   = node_model *;
        scheduler_base<Profiler>::init();
        // calculate adjacency list
        std::map<node_t, std::vector<node_t>> _adjacency_list{};
        std::vector<node_t>                   _source_nodes{};
        // compute the adjacency list
        std::set<node_t> node_reached;
        for (auto &e : this->_graph.edges()) {
            _adjacency_list[e._src_node].push_back(e._dst_node);
            _source_nodes.push_back(e._src_node);
            node_reached.insert(e._dst_node);
        }
        _source_nodes.erase(std::remove_if(_source_nodes.begin(), _source_nodes.end(), [&node_reached](auto current_node) { return node_reached.contains(current_node); }), _source_nodes.end());
        // traverse graph
        std::queue<node_t> queue{};
        std::set<node_t>   reached;
        // add all source nodes to queue
        for (node_t source_node : _source_nodes) {
            if (!reached.contains(source_node)) {
                queue.push(source_node);
            }
            reached.insert(source_node);
        }
        // process all nodes, adding all unvisited child nodes to the queue
        while (!queue.empty()) {
            node_t current_node = queue.front();
            queue.pop();
            _nodelist.push_back(current_node);
            if (_adjacency_list.contains(current_node)) { // node has outgoing edges
                for (auto &dst : _adjacency_list.at(current_node)) {
                    if (!reached.contains(dst)) { // detect cycles. this could be removed if we guarantee cycle free graphs earlier
                        queue.push(dst);
                        reached.insert(dst);
                    }
                }
            }
        }
        // generate job list
        if constexpr (executionPolicy == multi_threaded) {
            const auto n_batches = std::min(static_cast<std::size_t>(this->_pool->maxThreads()), _nodelist.size());
            this->_job_lists.reserve(n_batches);
            for (std::size_t i = 0; i < n_batches; i++) {
                // create job-set for thread
                auto &job = this->_job_lists.emplace_back();
                job.reserve(_nodelist.size() / n_batches + 1);
                for (std::size_t j = i; j < _nodelist.size(); j += n_batches) {
                    job.push_back(_nodelist[j]);
                }
            }
        }
    }

    template<typename node_type>
    work_return_t
    work_once(const std::span<node_type> &nodes) {
        constexpr std::size_t requested_work     = std::numeric_limits<std::size_t>::max();
        bool                  something_happened = false;
        std::size_t           performed_work     = 0_UZ;

        for (auto &currentNode : nodes) {
            auto result = currentNode->work(requested_work);
            performed_work += result.performed_work;
            if (result.status == work_return_status_t::ERROR) {
                return { requested_work, performed_work, work_return_status_t::ERROR };
            } else if (result.status == work_return_status_t::INSUFFICIENT_INPUT_ITEMS || result.status == work_return_status_t::DONE) {
                // nothing
            } else if (result.status == work_return_status_t::OK || result.status == work_return_status_t::INSUFFICIENT_OUTPUT_ITEMS) {
                something_happened = true;
            }

            if (currentNode->is_blocking()) { // work-around for `DONE` issue when running with multithreaded BlockingIO blocks -> TODO: needs a better solution on a global scope
                std::vector<std::size_t> available_input_samples(20);
                std::ignore = currentNode->available_input_samples(available_input_samples);
                something_happened |= std::accumulate(available_input_samples.begin(), available_input_samples.end(), 0_UZ) > 0_UZ;
            }
        }

        return { requested_work, performed_work, something_happened ? work_return_status_t::OK : work_return_status_t::DONE };
    }

    void
    run_and_wait() {
        start();
        this->wait_done();
    }

    void
    start() {
        switch (this->_state) {
        case IDLE: this->init(); break;
        case STOPPED: this->reset(); break;
        case PAUSED: this->_state = INITIALISED; break;
        case INITIALISED:
        case RUNNING:
        case REQUESTED_PAUSE:
        case REQUESTED_STOP:
        case SHUTTING_DOWN:
        case ERROR: break;
        }
        if (this->_state != INITIALISED) {
            throw std::runtime_error("simple scheduler work(): graph not initialised");
        }
        if constexpr (executionPolicy == single_threaded) {
            this->_state = RUNNING;
            work_return_t result;
            auto          nodelist = std::span{ this->_nodelist };
            while ((result = work_once(nodelist)).status == work_return_status_t::OK) {
                if (result.status == work_return_status_t::ERROR) {
                    this->_state = ERROR;
                    return;
                }
            }
            this->_state = STOPPED;
        } else if (executionPolicy == multi_threaded) {
            this->run_on_pool(this->_job_lists, [this](auto &job) { return this->work_once(job); });
        } else {
            throw std::invalid_argument("Unknown execution Policy");
        }
    }

    [[nodiscard]] const std::vector<std::vector<node_model *>> &
    getJobLists() const {
        return _job_lists;
    }
};
} // namespace gr::scheduler

#endif // GNURADIO_SCHEDULER_HPP