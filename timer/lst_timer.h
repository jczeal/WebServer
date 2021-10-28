#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include "../log/log.h"

class util_timer;

struct client_data //连接资源结构体
{
    sockaddr_in address;//客户端套接字地址
    int sockfd;//文件描述符
    util_timer *timer;//定时器
};

class util_timer//定时器类
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;//超时时间
    void (*cb_func)(client_data *);//回调函数
    client_data *user_data;//连接资源
    util_timer *prev;//前向定时器
    util_timer *next;//后继定时器
};

class sort_timer_lst//定时器容器类
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    void add_timer(util_timer *timer)//添加定时器，内部调用私有成员add_timer
    {
        if (!timer)
        {
            return;
        }
        if (!head)
        {
            head = tail = timer;
            return;
        }
        //如果新的定时器超时时间小于当前头部结点，直接将当前定时器结点作为头部结点
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);//否则调用私有成员，调整内部结点
    }

    void adjust_timer(util_timer *timer)//调整定时器，任务发生变化时，调整定时器在链表中的位置
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        //被调整的定时器在链表尾部或定时器超时值仍然小于下一个定时器超时值，不调整
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        //被调整定时器是链表头结点，将定时器取出，重新插入
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        //被调整定时器在内部，将定时器取出，重新插入
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    void del_timer(util_timer *timer)//删除定时器
    {
        if (!timer)
        {
            return;
        }
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    void tick()//定时任务处理函数
    {
        if (!head)
        {
            return;
        }
        //printf( "timer tick\n" );
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();
        time_t cur = time(NULL);
        util_timer *tmp = head;
        while (tmp)
        {
            if (cur < tmp->expire)//当前时间小于定时器的超时时间，后面的定时器也没有到期
            {
                break;
            }
            tmp->cb_func(tmp->user_data);//当前定时器到期，则调用回调函数，执行定时事件
            head = tmp->next;//将处理后的定时器从链表容器中删除，并重置头结点
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    void add_timer(util_timer *timer, util_timer *lst_head)
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        //遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;//头尾节点没有实际意义，仅仅统一方便调整
    util_timer *tail;
};

#endif
