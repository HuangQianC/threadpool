#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 2; // 单位: 秒

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED, // 固定数量的线程
    MODE_CACHED // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++)
    {}

    // 线程析构
    ~Thread() = default;

    // 启动线程
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_);  // 给线程函数传入线程ID
        t.detach();  // 设置分离线程 
    }

    // 获取线程id
    int getId() const
    {
        return threadId_;
    }
private:
    ThreadFunc func_;
    static int generateId_; // 编号
    int threadId_; // 保存线程id
};

int Thread::generateId_ = 0;

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool()
        : initThreadSize_(0)
        , taskSize_(0)
        , idleThreadSize_(0)
        , curThreadSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
    {}

    // 线程池析构
    ~ThreadPool()
    {
        isPoolRunning_ = false;
        //notEmpty_.notify_all(); // 发现死锁的位置

        // 等待线程池里面所有的线程返回 有两种状态: 阻塞 & 正在执行任务中
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
    }

    // 设置线程池的工作模式
    void setMode(PoolMode mode)
    {
        if (checkRunningState())
            return;
        poolMode_ = mode;
    }

    // 开启线程池 (初始的线程数量为本机CPU的核心数量)
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // 设置线程池的运行状态
        isPoolRunning_ = true;

        // 记录初始线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象 std::vector<Thread*> threads_;
        for (int i = 0; i < initThreadSize_; i++)
        {
            // 创建thread线程，绑定线程函数
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));

            // unique_ptr不允许左值引用的拷贝构造函数和赋值运算符
            //           只允许右值引用的拷贝构造函数和赋值运算符
            //threads_.emplace_back(std::move(ptr));

            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
        }

        // 启动所有线程 
        for (int i = 0; i < initThreadSize_; i++)
        {
            threads_[i]->start();  // 需要去执行一个线程函数
            idleThreadSize_++; // 记录初始空闲线程的数量(刚开始启动的线程肯定是空闲线程, 没有去执行任务)
        }
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshhold)
    {
        if (checkRunningState())
            return;
        taskQueMaxThreshHold_ = threshhold;
    }

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreshHold(int threshhold)
    {
        if (checkRunningState())
            return;
        if (poolMode_ == PoolMode::MODE_CACHED)
        {
            threadSizeThreshHold_ = threshhold;
        }
    }

    // 给线程池提交任务
    // 使用可变参模板编程, 让submitTask可以接收任意任务函数和任意数量的参数
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 获取锁 (构造函数自动调用lock: _Pmtx->lock() )
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // wait_for()返回的是bool值,当在提交任务的时候,任务队列满了,在1s之内没有空位置,则提交任务失败,返回;
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
        {
            // 表示notFull_等待1s种，条件依然没有满足
            std::cerr << "task queue is full, submit task fail." << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType { return RType(); });
            (*task)(); // 任务需要执行才能task->get_future()
            return task->get_future();
        }

        // 如果有空余, 把任务放入任务队列中
        // taskQue_.emplace(sp);
        // using Task = std::function<void()>;
        // (*task)(): 调用 std::packaged_task<RType()> 对象;
        // 这里只是把任务对象放进队列中,任务的返回值通过task->get_future()获得;
        taskQue_.emplace([task]() {(*task)(); });
        taskSize_++;

        // 因为新放了任务, 任务队列肯定不空了, 在notEmpty_上进行通知, 分配线程执行任务
        notEmpty_.notify_all();

        // cached模式: 任务处理比较紧急; 
        if (poolMode_ == PoolMode::MODE_CACHED
            && taskSize_ > idleThreadSize_
            && curThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << ">>> create new thread... " << std::endl;

            // 创建新线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            // 修改线程个数相关的变量 
            curThreadSize_++;
            idleThreadSize_++;
        }

        return result;
    }

    // 禁用 拷贝 赋值 
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数
    void threadFunc(int threadid)
    {
        // 线程执行任务的初始时间
        auto lastTime = std::chrono::high_resolution_clock().now();

        // 所以线程必须执行完成, 线程池才可以回收所以线程资源 (修改资源回收策略)
        //while (isPoolRunning_) 
        for (;;)
        {
            Task task;
            {
                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid:" << std::this_thread::get_id()
                    << "尝试获取任务..." << std::endl;
               
                // 在这里任务队列有任务的话就进不来, 必须执行完成后才能进来, 回收资源
                while (taskQue_.size() == 0)
                {
                    // 线程池要结束, 回收线程资源 (修改资源回收策略)
                    if (!isPoolRunning_)
                    {
                        threads_.erase(threadid); // std::this_thread::get_id() 
                        std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
                        exitCond_.notify_all();
                        return;  // 线程函数结束, 线程结束
                    }

                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // 每个1s都会检查一下有没有超过60s空闲的
                        // 条件变量, 超时返回了 (timeout超时了, no_timeout没超时)
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            // 时间差转为 秒 为单位; 
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME
                                && curThreadSize_ > initThreadSize_)
                            {
                                // 开始回收当前线程
                                // 记录线程数量的相关变量的值修改
                                // 把线程对象从线程列表容器中删除  办法: threadFunc <==> thread对象
                                // threadid ==> thread对象 ==> 删除
                                threads_.erase(threadid); // std::this_thread::get_id() 
                                curThreadSize_--;
                                idleThreadSize_--;

                                std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
                                return; // 线程函数返回, 相应的线程也就结束了
                            }
                        }
                    }
                    else
                    {
                        // 等待notEmpty条件
                        notEmpty_.wait(lock);
                    }
                }

                idleThreadSize_--;

                std::cout << "tid:" << std::this_thread::get_id()
                    << "获取任务成功..." << std::endl;

                // 从任务队列中取一个任务出来
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;

                // 如果依然有剩余任务, 继续通知其他的线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }

            } // 出作用域把锁释放掉,让其他线程再来获取锁,拿取任务

            // 当前线程负责执行这个任务
            if (task != nullptr)
            {
                task();
            }

            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now();  // 更新线程执行完任务的时间
        }

    }

    // 检查pool的运行状态
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表(方便cached模式删除对应的线程)

    int initThreadSize_;  // 初始的线程数量
    int threadSizeThreshHold_; // 线程数量上限阈值
    std::atomic_int curThreadSize_; // 记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; // 记录空闲线程的数量

    // Task任务 ==> 函数对象
    // 现在的任务是线程池自己封装的,不会无缘无故的释放掉
    using Task = std::function<void()>;
    std::queue<Task> taskQue_;  // 任务队列
    std::atomic_int taskSize_;  // 任务的数量
    int taskQueMaxThreshHold_;  // 任务队列数量上限阈值

    std::mutex taskQueMtx_;  // 保证任务队列的线程安全
    std::condition_variable notFull_;  // 表示任务队列不满,可以继续生产了
    std::condition_variable notEmpty_;  // 表示任务队列不空, 可以继续消费了
    std::condition_variable exitCond_; // 等到线程资源全部回收

    PoolMode poolMode_;  // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态 
};


#endif // _THREADPOOL_H
