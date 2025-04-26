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
#include <sstream>

using namespace std;

static mutex g_outputMutex;

// safePrint prints a single line atomically
inline void safePrint(const string &message)
{
    lock_guard lock(g_outputMutex);
    cout << message << endl;
}

// ThreadPool - a thread pool calss with a single task queue
class ThreadPool
{
public:
    // flag indicating whether the pool should stop or not
    bool m_stop;

    // default constructor
    ThreadPool() : m_stop(false), m_taskCounter(0) {}

    // destructor, terminates the thread pool
    ~ThreadPool()
    {
        terminate();
    }

    // initializes the thread pool, creating 'workerCount' worker threads
    void initialize(const size_t workerCount)
    {
        unique_lock lock(m_queueMutex);
        if (!m_workers.empty() || m_stop)
            return;

        m_stop = false;
        m_workers.reserve(workerCount);

        // create 'workerCount' threads, each running routine()
        for (size_t i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back([this] {
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
            unique_lock lock(m_queueMutex);

            // if the pool is already stopping, do not add new tasks
            if (m_stop)
                return -1;

            // generate a unique ID for this new task
            taskID = ++m_taskCounter;

            // wrap the user’s callable in a lambda that:
            //  1) marks the task as in-progress
            //  2) executes the actual function
            //  3) unmarks the task as in-progress
            auto userFunction = bind(forward<Func>(func), forward<Args>(args)...);
            auto wrapper = [this, taskID, userFunction] {
                markTaskStart(taskID);
                userFunction();
                markTaskEnd(taskID);
            };

            m_tasks.push(move(wrapper));
        }
        // notify worker thread that a new task is available
        cv_newTaskAvailable.notify_one();
        return taskID;
    }

    // terminates the thread pool
    void terminate()
    {
        {
            unique_lock lock(m_queueMutex);
            if (m_stop)
                return;
            m_stop = true;
        }
        // wake all threads so they can exit waiting
        cv_newTaskAvailable.notify_all();

        // join all worker threads
        for (thread &worker : m_workers)
        {
            if (worker.joinable())
                worker.join();
        }
        m_workers.clear();

        // clearing the task queue
        {
            queue<function<void()>> emptyQueue;
            swap(m_tasks, emptyQueue);
        }

        // clear the in-progress set too (if desired)
        {
            lock_guard lock(m_inProgressMutex);
            m_inProgress.clear();
        }
    }

    // returns a vector of task IDs that are currently running
    vector<int> getInProgressTasks()
    {
        lock_guard lock(m_inProgressMutex);
        return vector(m_inProgress.begin(), m_inProgress.end());
    }

    bool isEverythingDoneUnsafe() const
    {
        return m_inProgress.empty() && m_tasks.empty();
    }

private:
    // the task queue
    queue<function<void()>> m_tasks;
    // mutex for protecting access to the queue
    mutex m_queueMutex;

    // vector of worker threads
    vector<thread> m_workers;

    // condition variable to notify worker threads of new tasks
    condition_variable cv_newTaskAvailable;

    // variable to generate unique task IDs
    atomic<int> m_taskCounter;

    // variables to track which tasks are currently running
    unordered_set<int> m_inProgress;

    mutex m_inProgressMutex;

    // marks completion of all tasks
    condition_variable m_allTasksDone;

    // main function for each worker thread:
    //   1) waits for new tasks in the queue
    //   2) executes them
    //   3) exits when the pool is stopped and the queue is empty
    void routine()
    {
        while (true)
        {
            function<void()> task;
            {
                unique_lock lock(m_queueMutex);

                // wait while the queue is empty AND the pool is not stopped
                cv_newTaskAvailable.wait(lock, [this] {
                    return !m_tasks.empty() || m_stop;
                });

                // if the pool is stopped and the queue is empty, exit
                if (m_stop && m_tasks.empty())
                    return;

                task = move(m_tasks.front());
                m_tasks.pop();
            }
            // execute the task outside the mutex lock
            task();
        }
    }

    // mark the start of a task by inserting its ID into an in-progress set
    void markTaskStart(const int id)
    {
        lock_guard lock(m_inProgressMutex);
        m_inProgress.insert(id);
    }

    // mark the end of a task by removing its ID from the set
    void markTaskEnd(const int id)
    {
        lock_guard lock(m_inProgressMutex);
        m_inProgress.erase(id);

        // no more tasks - awake everyone waiting
        if (m_inProgress.empty() && m_tasks.empty())
        {
            m_allTasksDone.notify_all();
        }
    }
};

void exampleTask(const int taskID, const int sleepSeconds)
{
    {
        // Build a single-line message and send to safePrint
        ostringstream oss;
        oss << "[Task #" << taskID
            << "] Started! Will run for " << sleepSeconds << " seconds.";
        safePrint(oss.str());
    }

    this_thread::sleep_for(chrono::seconds(sleepSeconds));

    {
        ostringstream oss;
        oss << "[Task #" << taskID << "] Completed!";
        safePrint(oss.str());
    }
}

int main()
{
    const int producerCount = 3;
    const int tasksPerProducer = 5;
    const int workerCount = 8;

    ThreadPool pool;
    pool.initialize(workerCount);

    vector<thread> producers;
    producers.reserve(producerCount);

    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution dist(10, 16);

    atomic globalTaskID{1};

    for (int i = 0; i < producerCount; ++i)
    {
        producers.emplace_back([&pool, &dist, &gen, &globalTaskID, tasksPerProducer, i] {
            for (int j = 0; j < tasksPerProducer; ++j)
            {
                int localID = globalTaskID.fetch_add(1);
                int sleepTime = dist(gen);

                int assignedID = pool.addTask(exampleTask, localID, sleepTime);
                if (assignedID < 0)
                {
                    ostringstream oss;
                    oss << "[Producer #" << i << "] Failed to add task!";
                    safePrint(oss.str());
                }

                // simulating async behaviour
                this_thread::sleep_for(chrono::milliseconds(500));
            }
            {
                ostringstream oss;
                oss << "[Producer Thread #" << i
                    << "] Finished adding tasks.";
                safePrint(oss.str());
            }
        });
    }

    thread monitor([&pool] {
        while (true)
        {
            auto running = pool.getInProgressTasks();

            if (running.empty()) {
                safePrint("[Monitor] No tasks in progress right now.");
            }
            else
            {
                // Build a single-line message for the running tasks
                ostringstream oss;
                oss << "[Monitor] Currently running tasks: ";
                for (const auto id : running)
                    oss << id << " ";
                safePrint(oss.str());
            }

            if (running.empty() && pool.m_stop) {
                safePrint("[Monitor] Pool has stopped and there are no more tasks.");
                break;
            }

            this_thread::sleep_for(chrono::seconds(3));
        }
    });

    for (auto &p : producers)
    {
        if (p.joinable()) {
            p.join();
        }
    }

    {
        unique_lock lk(pool.m_inProgressMutex);
        pool.m_allTasksDone.wait(lk, [&pool]{
            return pool.isEverythingDoneUnsafe();
        });
    }

    {
        ostringstream oss;
        oss << "[main] All tasks completed. Terminating the thread pool...";
        safePrint(oss.str());
    }
    pool.terminate();

    if (monitor.joinable())
        monitor.join();

    {
        ostringstream oss;
        oss << "[main] Program finished!";
        safePrint(oss.str());
    }
    return 0;
}
