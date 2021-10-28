#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;//日志行数
    m_is_async = false;//同步异步标志位
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);//关闭日志文件
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);//异步日志阻塞队列
        pthread_t tid;
        //创建线程，flush_log_thread为线程处理函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);//日志缓冲区内存赋值
    m_split_lines = split_lines;//日志的最大行数

    time_t t = time(NULL);//返回值是从1970年1月1日至今所经历的时间（秒）
    struct tm *sys_tm = localtime(&t);//填充tm结构体
    struct tm my_tm = *sys_tm;

 
    const char *p = strrchr(file_name, '/');//在file_name中找最后一次出现'/'的位置
    char log_full_name[256] = {0};

    if (p == NULL)//相当于自定义日志名，若输入的文件名没有/，则直接将时间+文件名作为日志名
    {
        /*int snprintf(char*str, size_t size,constchar*format, ...);
         将可变参数(...)，按照 format 格式化成字符串，并将字符串复制到 str 中，
         size 是要写入的字符的最大数目，超过 size 会被截断
        */
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);//将/的位置向后移动一个位置，然后复制到logname中
        strncpy(dir_name, file_name, p - file_name + 1);//p - file_name + 1是文件所在路径文件夹的长度，dirname相当于./
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");//以附加的方式打开只写文件。若文件不存在，则会建立该文件，如果文件存在，写入的数据会被加到文件尾
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    //日志分级
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    
    m_mutex.lock();
    m_count++;//更新现有行数，m_split_lines为最大行数

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //更新日志(日志不是今天或写入日志行数是最大行的倍数)
    {
        
        char new_log[256] = {0};
        fflush(m_fp);//强迫将缓冲区内的数据写回指定的文件中，如果参数为NULL，fflush()会将所有打开的文件数据更新
        fclose(m_fp);
        char tail[16] = {0};
        //格式化日志名中的时间部分
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        //如果是时间不是今天,则创建今天的日志，更新m_today和m_count
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {   //超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    va_list valst;//可选参数宏
    //将传入的format参数赋值给valst，便于格式化输出
    va_start(valst, format);//指向函数参数列表中的第一个可选参数(fomat是位于第一个可选参数之前的固定参数)

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    //时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    //内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);//vsnprintf是为了支持va_list，实现sprint功能的封装
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();
    //若m_is_async为true表示异步，默认为同步.若异步,则将日志信息加入阻塞队列,同步则加锁向文件中写
    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);//清楚可选参数列表，并置指针无效
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
