#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
class http_conn
{
public:
    static const int FILENAME_LEN = 200;//读取文件名称大小
    static const int READ_BUFFER_SIZE = 2048;//读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;//写缓冲区大小
    enum METHOD//报文请求方法，本项目只用到GET和POST
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE//主状态机的状态
    {
        CHECK_STATE_REQUESTLINE = 0,//解析请求行
        CHECK_STATE_HEADER,//解析请求头
        CHECK_STATE_CONTENT//解析消息体，仅用于POST
    };
    enum HTTP_CODE//报文解析的结果
    {
        NO_REQUEST,//请求不完整，需要继续读取请求报文数据.跳转主线程继续监测读事件
        GET_REQUEST,//获得了完整的HTTP请求.调用do_request完成请求资源映射
        BAD_REQUEST,//HTTP请求报文有语法错误.跳转process_write完成响应报文
        NO_RESOURCE,//请求资源不存在.跳转process_write完成响应报文
        FORBIDDEN_REQUEST,//请求资源禁止访问，没有读取权限.跳转process_write完成响应报文
        FILE_REQUEST,//请求资源可以正常访问.跳转process_write完成响应报文
        INTERNAL_ERROR,//服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION
    };
    enum LINE_STATUS//从状态机的状态
    {
        LINE_OK = 0,//完整读取一行
        LINE_BAD,//报文语法错误
        LINE_OPEN//读取的行不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr);//初始化套接字地址，函数内部会调用私有方法init
    void close_conn(bool real_close = true);//关闭http连接
    void process();
    bool read_once();//读取浏览器端发来的全部数据
    bool write();//响应报文写入函数
    sockaddr_in *get_address() //sockaddr_in解决了sockaddr的缺陷(port和addr保存在同一个char数组中，不好区分)
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);//同步线程初始化数据库读取表

private:
    void init();
    HTTP_CODE process_read();//从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret);//向m_write_buf写入响应报文数据
    HTTP_CODE parse_request_line(char *text);//主状态机解析报文中的请求行数据
    HTTP_CODE parse_headers(char *text);//主状态机解析报文中的请求头数据
    HTTP_CODE parse_content(char *text);//主状态机解析报文中的请求内容
    HTTP_CODE do_request();//生成响应报文
    char *get_line() { return m_read_buf + m_start_line; }; //用于将指针向后偏移，指向未处理的字符.m_start_line是已经解析的字符
    LINE_STATUS parse_line();//从状态机读取一行，分析是请求报文的哪一部分
    void unmap();
    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd; //这个http对象的epollid
    static int m_user_count; //用户连接数
    MYSQL *mysql;

private:
    int m_sockfd; //socket的文件描述符
    sockaddr_in m_address;
    //调用memset为类buffer分配空间
    char m_read_buf[READ_BUFFER_SIZE];//存储读取的请求报文数据 类读缓冲区2mb  char数组
    int m_read_idx;//读缓冲区m_read_buf中数据的最后一个字节的下一个位置，也就是数据结束位置
    int m_checked_idx;//读缓冲区读取的位置
    int m_start_line;//每个数据行在读缓冲区的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];//存储发出的响应报文数据 类写缓冲区1mb
    int m_write_idx;//写缓冲区数据结束位置

    CHECK_STATE m_check_state;//主状态机的状态
    METHOD m_method;//请求方法

    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];//读取文件名，也是个类buffer
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length; //消息体长度，判断get/post
    bool m_linger; //长连接标记

    char *m_file_address;//mmap文件映射地址，读取服务器上的文件
    struct stat m_file_stat; //保存请求资源的文件信息
    struct iovec m_iv[2];//io向量结构体iovec,成员iov_base为指向数据的地址,成员iov_len表示数据的长度。
    int m_iv_count; //iovec结构体个数
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;//剩余发送字节数
    int bytes_have_send;//已发送字节数
};

#endif
