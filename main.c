#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符，可以建立的连接数
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;//定时器容器链表
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    /*int send(SOCKET s, const char *buf, int len, int flags)。
    将信号值从管道写端写入，传输字符类型，而非整型
    s-已建立连接的套接字；buf-存放将要发送的数据的缓冲区指针；len-发送缓冲区中的字符数；flags-控制数据传输方式
    */
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数:仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;//指定信号处理函数
    if (restart)
        sa.sa_flags |= SA_RESTART;//指定信号处理方式，SA_RESTART，使被信号打断的系统调用自动重新发起
    sigfillset(&sa.sa_mask);//将所有信号添加到信号集中
    assert(sigaction(sig, &sa, NULL) != -1);//sigaction对信号sig设置新的处理方式sa
                                            //assert()计算括号内的表达式，如果为false(0),程序将报告错误，并终止执行
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//删除非活动连接在socket上的注册事件
    assert(user_data);
    close(user_data->sockfd);//关闭文件描述符
    http_conn::m_user_count--;//减少连接数
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN); //避免进程退出，忽略该信号

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "123456", "yourdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);//创建监听socket文件描述符
/*socket(domain,type,protocol)
通信域domain:PF_UNIX(unix域)，PF_INET(IPv4)，PF_INET6(IPv6)等；
通信类型type:SOCK_STREAM数据流(TCP/IP),SOCK_DGRAM数据包(UDP)；
protocol：可以对同一个协议族/通信域指定不同的的协议参数，但通常只有一个，
          TCP参数可指定为IPPROTO_TCP,而UDP可以用IPPROTO_UDP，使用0则根据前两个参数使用默认协议。
*/
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;//创建监听socket的TCP/IP的IPV4 socket地址
    bzero(&address, sizeof(address));//地址字节清零
    address.sin_family = AF_INET;//地址族，这里是IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY);//IP地址,htonl将本地字节序转换为网络字节序(大端)
                                                //INADDR_ANY：服务器端计算机上所有网卡IP地址都可当做服务器IP地址
    address.sin_port = htons(port);//端口号

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
/*setsockopt用于任意类型、任意状态套接口的设置选项值
int setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen)
    SOL_SOCKET:操作套接字层的选型
    SO_REUSEADDR：允许端口被重复使用
    optval指向存放选项值的缓冲区
    optlen为缓冲区的长度
*/
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));//绑定socket和它的地址
    assert(ret >= 0);
    ret = listen(listenfd, 5);//创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前,设置5为最大队列长度
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5); //返回epollfd句柄，size参数无用
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);//将listenfd放在epoll树上
    http_conn::m_epollfd = epollfd;

    //创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);//设置管道写端为非阻塞
    addfd(epollfd, pipefd[0], false);//设置管道读端为ET非阻塞，并添加到epoll内核事件表

    //传递给主循环的信号值，这里只关注SIGALRM（时间到了触发）和SIGTERM（kill会触发，Ctrl+C）
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;//循环条件

    client_data *users_timer = new client_data[MAX_FD];//创建连接资源数组

    bool timeout = false;
    alarm(TIMESLOT);//每隔TIMESLOT时间触发SIGALRM信号

    while (!stop_server)
    {
        //等待所监控文件描述符上有事件的发生
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        //对所有就绪事件进行处理
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;//初始化客户端连接地址
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);//为该连接分配新的文件描述符
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);

                //初始化该连接对应的连接资源
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;//创建定时器临时变量
                timer->user_data = &users_timer[connfd];//设置定时器对应的连接资源
                timer->cb_func = cb_func;//设置回调函数
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;//设置绝对超时时间
                users_timer[connfd].timer = timer;//创建该连接对应的定时器，初始化为前述临时变量
                timer_lst.add_timer(timer); //将该定时器添加到链表中
#endif

#ifdef listenfdET
                while (1)//需要循环接收数据
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }

            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))//处理异常事件
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            //管道读端对应文件描述符发生读事件，处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)  //信号本身是整型数值，管道中传递的是ASCII码表中整型数值对应的字符
                    {   //这里面明明是字符
                        switch (signals[i])        //当switch的变量为字符时，case中可以是字符，也可以是字符对应的ASCII码
                        { //这里是整型
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())//读入对应缓冲区
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {   //服务器端关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
