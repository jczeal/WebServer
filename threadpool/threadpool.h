#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*connPool是数据库连接池指针，thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列，list
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //信号量-是否有任务需要处理
    bool m_stop;                //是否结束线程
    connection_pool *m_connPool;  //数据库连接池指针
};

template <typename T>
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number]; //线程id初始化,用数组保存
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        //printf("create the %dth thread\n",i);
        //循环创建线程，并将工作线程按要求进行运行
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)//第三个参数类型为函数指针，指向的线程处理函数参数类型为(void *),
                                                                   //若线程函数为类成员函数，要将其设置为静态成员函数
        {
            delete[] m_threads;
            throw std::exception();
        }
        
        //linux下线程只有joinable和unjoinable两种状态，前者在线程函数返回退出或pthread_exit()时不会释放线程资源，需要调用pthread_join()来回收资源
        //pthread_detach可以改变线程状态为unjoinable，将主线程和子线程分离，这样ptherad_exit()就会自动回收线程资源(堆栈和线程描述符等，8k)
        if (pthread_detach(m_threads[i])) //调用成功返回0
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads; //析构释放线程资源
    m_stop = true;
}
template <typename T>
bool threadpool<T>::append(T *request)//向请求队列添加任务
{
    m_queuelocker.lock(); //先加锁再添加任务
    if (m_workqueue.size() > m_max_requests) //满了不能加
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//信号量+1，提醒有任务要处理
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;//将参数强转为线程池类，以调用成员方法，完成线程处理要求
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()//工作线程从请求队列中取出某个任务进行处理,注意线程同步
{
    while (!m_stop)
    {
        m_queuestat.wait();//信号量-1，通过信号量的等待来控制线程是否从请求队列拿任务
        m_queuelocker.lock();//被唤醒后先加互斥锁，保证线程同步
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();//从请求队列中取出第一个任务(实际是http对象),然后从请求队列删除
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        connectionRAII mysqlcon(&request->mysql, m_connPool);//从连接池中取出一个数据库连接
        
        request->process();//process(模板类中的方法,这里是http类)进行处理
    }
}
#endif
