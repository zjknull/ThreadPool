#include "ThreadPool.h"

int main() {
    auto& pool = ThreadPool::getInstance();

    // ===== 测试1: 基本任务执行 =====
    {
        auto future = pool.submit([]() {
            return 42;
        });

        assert(future.get() == 42);
        std::cout << "测试1: 基本任务执行成功" << std::endl;
    }

    // ===== 测试2: 多任务并发执行 =====
    {
        constexpr int TASK_COUNT = 100;
        std::atomic<int> counter(0);
        std::vector<std::future<void>> futures;

        for (int i = 0; i < TASK_COUNT; ++i) {
            futures.emplace_back(pool.submit([&counter]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        // 等待所有任务完成
        for (auto& f : futures) {
            f.wait();
        }

        assert(counter.load() == TASK_COUNT);
        std::cout << "测试2: 多任务并发执行成功" << std::endl;
    }

    // ===== 测试3: 异常处理 =====
    {
        auto future = pool.submit([]() {
            throw std::runtime_error("测试异常");
        });

        try {
            future.get();
            assert(false); // 不应该执行到这里
        } catch (const std::runtime_error& e) {
            assert(std::string(e.what()) == "测试异常");
            std::cout << "测试3: 异常处理成功" << std::endl;
        }
    }

    // ===== 测试4: 关闭线程池后无法提交新任务 =====
    {
        pool.shutdown();

        try {
            auto future = pool.submit([]() { return 1; });
            assert(false); // 不应该执行到这里，因为线程池已关闭
        } catch (const std::exception& e) {
            std::cout << "测试4: 线程池关闭后拒绝新任务 - " << e.what() << std::endl;
        }
    }

    std::cout << "所有测试用例执行成功！" << std::endl;
    return 0;
}
