/*
 * @Author       : 程学斌
 * @Date         : 2023-05-01
 * @copyleft Apache 2.0
 */ 

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <cassert>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <functional>
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = 8): pool_(std::make_shared<Pool>()) {
        //初始创建8个线程（I/O密集型 2N）
            assert(threadCount > 0);
            //遍历创建线程
            for(size_t i = 0; i < threadCount; i++) {
                std::thread([pool = pool_] {
                    std::unique_lock<std::mutex> locker(pool->mtx);
                    while(true) {
                        if(!pool->tasks.empty()) {  //判断任务队列有任务
                            auto task = std::move(pool->tasks.front());
                            pool->tasks.pop();
                            locker.unlock();
                            task();
                            locker.lock();
                        } 
                        else if(pool->isClosed) break;  //连接关闭  
                        else pool->cond.wait(locker);   //阻塞被add_task唤醒
                    }
                }).detach();//设置线程分离
            }
    }

    ThreadPool() = default;

    ThreadPool(ThreadPool&&) = default;
    
    ~ThreadPool() {
        if(static_cast<bool>(pool_)) {
            {
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            pool_->cond.notify_all();
        }
    }

    template<class F>//模板类
    void AddTask(F&& task) {
        {
            std::lock_guard<std::mutex> locker(pool_->mtx);
            pool_->tasks.emplace(std::forward<F>(task));
        }
        pool_->cond.notify_one();
    }

private:
    struct Pool {
        std::mutex mtx; //互斥锁
        std::condition_variable cond; //条件变量
        bool isClosed;      //是否关闭
        std::queue<std::function<void()>> tasks;   //工作队列
    };
    std::shared_ptr<Pool> pool_;
};


#endif //THREADPOOL_H