
#include <future>
#include <memory>
#include <thread>
#include <mutex>
#include <deque>
#include <iostream>
#include <vector>
#include <functional>
#include <cassert>

class TaskBase
{
public:
    virtual void execute () = 0;
    virtual ~TaskBase() {}
};

template <typename R, typename F, typename... Args>
class Task : public TaskBase
{
public:
    Task(F &&f, Args&&... args)
    : m_task(std::bind(std::forward<F>(f), std::forward<Args>(args)...))
    {}

    void execute() override
    {
        m_task();
    }

    std::future<R> getFuture()
    {
        return m_task.get_future();
    }

private:
    std::packaged_task<R()> m_task;
};

template <typename F, typename... Args>
auto createTask(F &&f, Args &&... args)->
std::unique_ptr<Task<typename std::result_of<F(Args...)>::type, F, Args...>>
{
    using ResultType = typename std::result_of<F(Args...)>::type;
    return std::make_unique<Task<ResultType, F, Args...>>(std::forward<F>(f),
                                                          std::forward<Args>(args)...);
}

class WorkThread
{
public:
    WorkThread(size_t tid)
    : m_worker([this](){this->run();})
    , m_id(tid)
    {
    }

    ~WorkThread()
    {
        stop();
        join();
    }

    void pushTask(std::unique_ptr<TaskBase> task)
    {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_taskQueue.push_back(std::move(task));
            m_taskCount.fetch_add(1, std::memory_order_relaxed); // 入队时加1
        }
        m_cv.notify_one();
    }

    size_t size()
    {
        return m_taskCount.load(std::memory_order_relaxed);
    }

    void join()
    {
        if(m_worker.joinable())
            m_worker.join();
    }

    void stop()
    {
        m_stop = true;
        m_cv.notify_all();
    }


private:
    void run()
    {
        while(!m_stop)
        {
            std::unique_ptr<TaskBase> task;
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                m_cv.wait(lock, [&](){
                    return !m_stop || m_taskQueue.empty();
                });

                // 任务都做完了，优雅退出
                if (m_stop && m_taskQueue.empty())
                    return;
                if (!m_taskQueue.empty())
                {
                    task = std::move(m_taskQueue.front());
                    m_taskQueue.pop_front();
                    m_taskCount.fetch_sub(1, std::memory_order_relaxed); // 实时更新 
                }
            }
            if (task)
                task->execute();
        }
    }

private:
    std::thread m_worker;                               // 工作线程
    std::deque<std::unique_ptr<TaskBase>> m_taskQueue;  // 线程内部的消息队列
    std::mutex m_mtx;                                   // 互斥锁
    std::condition_variable m_cv;                       // 条件变量
    std::atomic<bool> m_stop{false};                    // 停止位，用于停止线程
    std::atomic<size_t> m_taskCount{0};                 // 标志位，表示消息队列的长度，避免过度使用锁
    size_t m_id;                                        // 线程id，表示某个线程的线程id
};

class Controller
{
public:
    Controller(size_t wordNum)
    {
        for (size_t i = 0; i < wordNum; i++)
        {
            m_workerThreads.emplace_back(std::make_unique<WorkThread>(i));
        }
    }

    void dispatchTask(std::unique_ptr<TaskBase> task)
    {
    /*************方法一:顺序放入Task**************/
    //  size_t idx = m_lastSelected.fetch_add(1, std::memory_order_relaxed) % m_workerThreads.size();
    //  m_workerThreads[idx]->pushTask(std::move(task));
        /*************方法二:局部性优化+随机采样，将Task放入一个任务较少的WordThread中**************/
        // 选择任务队列最短的工作线程
        const size_t workerCount = m_workerThreads.size();

        // 快速路径：如果线程数少，直接遍历选择最短队列
        if (workerCount <= kRandomSelectionThreshold) {
            size_t selected = 0;
            size_t minSize = m_workerThreads[0]->size();

            for (size_t i = 1; i < workerCount; ++i) {
                size_t currentSize = m_workerThreads[i]->size();
                if (currentSize < minSize) {
                    minSize = currentSize;
                    selected = i;
                }
            }

            m_workerThreads[selected]->pushTask(std::move(task));
            return;
        }

        // 优化路径：对大线程池采用局部性优先+随机采样
        const size_t sampleSize = std::min(workerCount, size_t(5));  // 采样5个线程
        size_t selected = m_lastSelected.load(std::memory_order_relaxed);
        size_t minSize = m_workerThreads[selected]->size();

        // 随机采样其他线程进行比较
        for (size_t i = 1; i < sampleSize; ++i) {
            size_t candidate = (selected + i) % workerCount;
            size_t candidateSize = m_workerThreads[candidate]->size();

            // 轻微偏向当前选中线程，减少频繁切换开销
            if (candidateSize < minSize || (candidateSize == minSize && candidate == selected)) {
                minSize = candidateSize;
                selected = candidate;
            }
        }

        m_lastSelected.store(selected, std::memory_order_relaxed);
        m_workerThreads[selected]->pushTask(std::move(task));
    }

    size_t workerCount() const { return m_workerThreads.size(); }
    WorkThread& getWorker(size_t idx){ return *m_workerThreads.at(idx); }

    void stopAll()
    {
        for(auto& worker : m_workerThreads)
            worker->stop();
    }

    void joinAll()
    {
        for(auto& worker : m_workerThreads)
            worker->join();
    }

private:
    std::vector<std::unique_ptr<WorkThread>> m_workerThreads;
    std::atomic<size_t> m_lastSelected{0};                        // 上次选择的线程
    const size_t kRandomSelectionThreshold = 3;                   // 阈值，超过这个阈值选择局部性优化
};

class Supervisor
{
public:
    Supervisor(Controller& ctrl) : m_ctrl(ctrl) {}
    void monitor() {
        // 打印线程队列长度等
        for (size_t i = 0; i < m_ctrl.workerCount(); ++i) {
            std::cout << "Queue " << i << " has " << m_ctrl.getWorker(i).size() << " tasks\n";
        }
    }
private:
    Controller& m_ctrl;
};

class ThreadPool
{
public:
    static ThreadPool& getInstance(size_t n_workers = std::thread::hardware_concurrency())
    {
        static ThreadPool instance(n_workers);
        return instance;
    }

    void supervise() { m_supervisor.monitor(); }

    void shutdown()
    {
        m_controller.stopAll();
        m_controller.joinAll();
    }

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
    {
        auto task = createTask(std::forward<F>(f), std::forward<Args>(args)...);
        auto future = task->getFuture();
        m_controller.dispatchTask(std::move(task));
        return future;
    }

private:
    ThreadPool(size_t workers)
        : m_controller(workers), m_supervisor(m_controller) {}

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    
private:
    Controller m_controller;
    Supervisor m_supervisor;
};