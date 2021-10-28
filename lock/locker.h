#ifndef LOCKER_H
#define LOCKER_H

#include <exception>//异常处理
#include <pthread.h>//Linux系统下的多线程遵循POSIX线程接口
#include <semaphore.h>//信号量

class sem//信号量：成功返回0，失败返回errno
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)//初始化信号量，第一个0设置信号量为进程内共享(非0为进程间共享)，第二个0为初始值
        {                               //成功返回0，错误返回-1，并把 errno 设置为合适的值
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);//销毁信号量
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;//以原子操作把信号量减1，如果信号量的当前值为0则进入阻塞
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;//以原子操作把信号量加1，如果信号量>0，唤醒调用线程
    }

private:
    sem_t m_sem;//信号量的数据类型，本质是个长整型的数
};

class locker//互斥锁：成功返回0，失败返回errno
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)//初始化互斥锁，NULL表示使用默认互斥锁类型
        {                                           //成功返回0，互斥锁初始化为锁住状态
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);//销毁互斥锁
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;//以原子操作给互斥锁加锁，如果尝试锁定已经加锁的互斥锁则阻塞至可用为止
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;//以原子操作给互斥锁解锁
    }
    pthread_mutex_t *get() //获得锁
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;//互斥锁的数据类型
};

class cond//条件变量：提供了一种线程间的通知机制,当某个共享数据达到某个值时,唤醒等待这个共享数据的线程
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)//初始化条件变量,NULL创建缺省的条件变量
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);//等待目标条件变量.该函数调用时需要传入 mutex参数(加锁的互斥锁) ,
                                                 //函数执行时,先把调用线程放入条件变量的请求队列,然后将互斥锁mutex解锁,
                                                 //当函数成功返回为0时,互斥锁会再次被锁上. 也就是说函数内部会有一次解锁和加锁操作
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) 
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);//到时间t，即使条件未发生也会解除阻塞
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;//唤醒等待目标条件变量的一个线程
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;//以广播的方式唤醒所有等待目标条件变量的线程
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;//条件变量的数据结构
};
#endif
