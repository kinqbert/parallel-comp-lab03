#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include <random>
#include <unordered_set>

// ThreadPool class - a thread pool with a single task queue
class ThreadPool
{
public:
    // default constructor
    ThreadPool() : m_stop(false), m_taskCounter(0) {}

    // destructor, terminates the thread pool
    ~ThreadPool()
    {
        terminate();
    }

    // initializes the thread pool, creating 'workerCount' worker threads
    void initialize(size_t workerCount)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_workers.empty() || m_stop)
            return;

        m_stop = false;
        m_workers.reserve(workerCount);

        // create 'workerCount' threads, each running routine()
        for (size_t i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back([this]() {
                this->routine();
            });
        }
    }

    // adds a new task to the thread pool, returns an integer ID assigned to this task
    template <typename Func, typename... Args>
    int addTask(Func&& func, Args&&... args)
    {
        int taskID;
        {
            // lock the mutex to modify the task queue
            std::unique_lock<std::mutex> lock(m_mutex);

            // if the pool is already stopping, do not add new tasks
            if (m_stop)
                return -1;

            // generate a unique ID for this new task
            taskID = ++m_taskCounter;

            // wrap the user’s callable in a lambda that:
            //  1) marks the task as in-progress
            //  2) executes the actual function
            //  3) unmarks the task as in-progress
            auto userFunction = std::bind(std::forward<Func>(func), std::forward<Args>(args)...);
            auto wrapper = [this, taskID, userFunction]() {
                markTaskStart(taskID);
                userFunction();
                markTaskEnd(taskID);
            };

            m_tasks.push(std::move(wrapper));
        }
        // notify worker thread that a new task is available
        m_cv.notify_one();
        return taskID;
    }

    // terminates the thread pool
    void terminate()
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_stop)
                return;
            m_stop = true;
        }
        // wake all threads so they can exit waiting
        m_cv.notify_all();

        // join all worker threads
        for (std::thread &worker : m_workers)
        {
            if (worker.joinable())
                worker.join();
        }
        m_workers.clear();

        // clearing the task queue
        {
            std::queue<std::function<void()>> emptyQueue;
            std::swap(m_tasks, emptyQueue);
        }

        // clear the in-progress set too (if desired)
        {
            std::lock_guard<std::mutex> lock(m_inProgressMutex);
            m_inProgress.clear();
        }
    }

    // returns a vector of task IDs that are currently running
    std::vector<int> getInProgressTasks()
    {
        std::lock_guard<std::mutex> lock(m_inProgressMutex);
        return std::vector<int>(m_inProgress.begin(), m_inProgress.end());
    }

private:
    // Main function for each worker thread:
    //   - Waits for new tasks in the queue
    //   - Executes them
    //   - Exits when the pool is stopped and the queue is empty
    void routine()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);

                // wait while the queue is empty AND the pool is not stopped
                m_cv.wait(lock, [this] {
                    return !m_tasks.empty() || m_stop;
                });

                // if the pool is stopped and the queue is empty, exit
                if (m_stop && m_tasks.empty())
                    return;

                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            // execute the task outside the mutex lock
            task();
        }
    }

    // mark the start of a task by inserting its ID into an in-progress set
    void markTaskStart(const int id)
    {
        std::lock_guard<std::mutex> lock(m_inProgressMutex);
        m_inProgress.insert(id);
    }

    // mark the end of a task by removing its ID from the set
    void markTaskEnd(const int id)
    {
        std::lock_guard<std::mutex> lock(m_inProgressMutex);
        m_inProgress.erase(id);
    }

    // the task queue
    std::queue<std::function<void()>> m_tasks;

    // vector of worker threads
    std::vector<std::thread> m_workers;

    // mutex for protecting access to the queue
    std::mutex m_mutex;

    // condition variable to notify worker threads of new tasks
    std::condition_variable m_cv;

    // flag indicating whether the pool should stop or not
    bool m_stop;

    // variable to generate unique task IDs
    std::atomic<int> m_taskCounter;

    // variables to track which tasks are currently running
    std::unordered_set<int> m_inProgress;
    std::mutex m_inProgressMutex;
};

void exampleTask(const int taskID, const int sleepSeconds)
{
    std::cout << "[Task #" << taskID
              << "] Will run for " << sleepSeconds << " seconds.\n";

    std::this_thread::sleep_for(std::chrono::seconds(sleepSeconds));

    std::cout << "[Task #" << taskID << "] Completed!\n";
}

int main()
{
    ThreadPool pool;
    pool.initialize(8);

    const int producerCount = 3;
    const int tasksPerProducer = 4;

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(10, 20);

    std::atomic<int> globalTaskID{1};

    for (int i = 0; i < producerCount; ++i)
    {
        producers.emplace_back([&pool, &dist, &gen, &globalTaskID, tasksPerProducer, i]() {
            for (int j = 0; j < tasksPerProducer; ++j)
            {
                int localID = globalTaskID.fetch_add(1);
                int sleepTime = dist(gen);

                int assignedID = pool.addTask(exampleTask, localID, sleepTime);
                if (assignedID < 0)
                {
                    std::cout << "[Producer #" << i << "] Failed to add task!\n";
                }

                // simulating async behaviour
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::cout << "[Producer Thread #" << i
                      << "] Finished adding tasks.\n";
        });
    }

    std::thread monitor([&pool]() {
        for (int round = 0; round < 10; ++round)
        {
            auto running = pool.getInProgressTasks();
            if (running.empty())
            {
                std::cout << "[Monitor] No tasks in progress right now.\n";
            }
            else
            {
                std::cout << "[Monitor] Currently running tasks: ";
                for (auto id : running)
                    std::cout << id << " ";
                std::cout << "\n";
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    });

    for (auto &p : producers)
    {
        if (p.joinable())
            p.join();
    }

    std::cout << "[main] All tasks are submitted. Letting them run for 30 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(30));

    std::cout << "[main] Terminating the thread pool...\n";
    pool.terminate();

    if (monitor.joinable())
        monitor.join();

    std::cout << "[main] Program finished!\n";
    return 0;
}