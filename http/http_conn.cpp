#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/myproject/TinyWebServer/root";//网站根目录，文件夹内存放请求的资源和跳转的html文件

//将表中的用户名和密码放入map
map<string, string> users;
locker m_lock;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞   先获得旧设置，以便返回，再更改为新设置
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);//获取文件打开方式标志
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);//设置文件打开方式标志
    return old_option;
}

//向内核事件表注册读事件，选择ET/LT模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot)//将fd上设置的事件注册到epollfd指示的epoll内核事件表中
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;//EPOLLRDHUP:对端断开.有了这个事件，对端断开连接的异常可以在底层进行处理，不用再移交到上层
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif
//开启EPOLLONESHOT，只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入EPOLL队列 
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从event内核事件表删除监听的文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);//从表中删除这个fd
    close(fd); //关闭文件
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);//修改事件
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,给当前http对象初始化套接字地址，并注册给epoll时间表
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

//初始化新接受的连接的其他参数
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;//check_state默认为分析请求行状态
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机的具体实现，用于分析出一行内容 \r回车\n换行，一起遇到说明到达该行末尾
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];//将要分析的字节数据
        //如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)//下一个字符达到了缓冲区中数据结尾，则接收不完整，需要继续接收
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')//下一个字符是\n，说明这行读完了，将\r\n改为\0\0
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;//如果都不符合，则返回语法错误
        }
        //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')//前一个字符是\r，则接收完整
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;//并没有找到\r\n，需要继续接收
}

//从socket循环读取客户数据，直到无数据可读或对方关闭连接，读取到m_read_buffer中，并更新m_read_idx
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) //读缓冲区溢出
    {
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT
    //int recv( SOCKET s, char *buf, int len, int flags) 
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);//从套接字接收数据，存储在m_read_buf缓冲区
    m_read_idx += bytes_read;//根据读取字节数修改数据结束位置m_read_idx

    if (bytes_read <= 0)
    {
        return false;
    }

    return true;

#endif

#ifdef connfdET
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)//非阻塞ET模式下，需要一次性将数据读完
                break;
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行包括请求方法,请求访问资源(URL)，HTTP协议及版本，各个部分之间用\t或空格分隔。
    m_url = strpbrk(text, " \t");//返回请求行中最先含有空格或\t字符(水平制表符)的位置
    if (!m_url)//如果没有空格或\t，则报文格式有误
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';//将该位置改为\0，用于将前面数据取出
    //取出数据，并通过与GET和POST比较，以确定请求方法
    char *method = text;
    if (strcasecmp(method, "GET") == 0)//strcasecmp忽略大小写比较字符串
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    m_url += strspn(m_url, " \t");//检索m_url中第一个不在" \t"中出现的字符下标，即继续跳过空格和\t字符，指向请求资源的第一个字符
    
    m_version = strpbrk(m_url, " \t");//使用与判断请求方法的相同逻辑，判断HTTP版本号
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)//仅支持HTTP/1.1
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)//对请求资源前7个字符进行判断
    {                                         //这里主要是有些报文的请求资源中会带有http://，需要对这种情况进行单独处理
        m_url += 7;
        m_url = strchr(m_url, '/');//m_url中第一次出现'/'的位置
    }

    if (strncasecmp(m_url, "https://", 8) == 0)//同样增加https情况
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')//一般不会带有上述两种符号，直接是单独的/或/后面带访问资源
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;//请求行处理完毕，将主状态机转移处理请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')//判断是空行还是请求头
    {
        if (m_content_length != 0) //判断是GET还是POST请求,GET没有消息体
        {
            m_check_state = CHECK_STATE_CONTENT;//POST需要跳转到消息体处理状态
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //解析请求头部connection字段，判断是keep-alive还是close，决定是长连接还是短连接
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");//跳过空格和\t字符
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;//如果是长连接，则将linger标志设置为true
        }
    }
    //解析请求头部content-length字段，表示读取post请求的消息体长度
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);//把字符串转换成长整型数
    }
    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{   //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';//完整读入，设置字符串结束标志
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//解析报文，通过while循环，将主从状态机进行封装，对报文的每一行进行循环处理
http_conn::HTTP_CODE http_conn::process_read()
{   //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    /*
    在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可;
    而在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，因此使用主状态机的状态作为循环入口条件，
    解析完消息体后，报文的完整解析就完成了，但此时主状态机的状态还是CHECK_STATE_CONTENT，符合循环入口条件，还会再次进入循环，
    因此增加&& line_status==LINE_OK，在完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务
    */
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();//将从状态机读取的数据赋给text
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        //主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE://解析请求行
        {   
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER://解析请求头
        {   
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)//完整解析GET请求后，跳转到报文响应函数
            {   
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT://解析消息体
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)//完整解析POST请求后，跳转到报文响应函数
                return do_request();
            line_status = LINE_OPEN; //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
            break;
        }
        default:
            return INTERNAL_ERROR;//case都没匹配上
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{   
    strcpy(m_real_file, doc_root);//将初始化的m_real_file赋值为网站根目录
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    //查找字符在指定字符串中最后一次出现的位置
    const char *p = strrchr(m_url, '/');

    //实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);//strcat将两个字符串连接
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); //将以m_url_real为起点的指定长度字节复制到目标数组
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        //以&为分隔符，前面的为用户名，后面的为密码
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            //判断map中能否找到重复的用户名
            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)//校验成功，跳转登录页面
                    strcpy(m_url, "/log.html");
                else//校验失败，跳转注册失败页面
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    //如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取指定路径文件信息(文件类型和权限，文件大小等)，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    /*void* mmap(void* start,size_t length,int prot,int flags,int fd,off_t offset); 返回指向映射区地址的指针
    用于将一个文件或其他对象映射到内存，提高文件的访问速度
    start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址
    length：映射区的长度
    prot：期望的内存保护标志，不能与文件的打开模式冲突，PROT_READ 表示页内容可以被读取
    flags：指定映射对象的类型，映射选项和映射页是否可以共享，MAP_PRIVATE建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
    fd：有效的文件描述符，一般是由open()函数返回
    off_toffset：被映射对象内容的起点
    */
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); 
    close(fd);//避免文件描述符的浪费和占用
    return FILE_REQUEST;//表示请求文件存在，且可以访问
}

void http_conn::unmap()//解除映射
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)//表示响应报文为空，一般不会出现这种情况
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {   
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        //writev函数用于在一次函数调用中写多个非连续缓冲区，有时也将这该函数称为聚集写
        //参数：文件描述符，向量结构体iovec，结构体个数
        //writev以顺序iov[0]，iov[1]至iov[iovcnt-1]从缓冲区中聚集输出数据。
        //writev返回输出的字节总数，通常，它应等于所有缓冲区长度之和
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)//判断缓冲区是否满了
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);//重新注册写事件
                return true;
            }
            unmap();//如果发送失败，但不是缓冲区问题，取消映射
            return false;
        }

        bytes_have_send += temp;//更新已发送字节
        bytes_to_send -= temp;//更新未发送字节
        if (bytes_have_send >= m_iv[0].iov_len)//第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        {
            //不再继续发送头部信息
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else//继续发送第一个iovec头部信息的数据
        {   
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)//判断条件，数据已全部发送完
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);//在epoll树上重置EPOLLONESHOT事件

            if (m_linger)//浏览器的请求为长连接
            {
                init();//重新初始化HTTP对象
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;//可变参数列表
    va_start(arg_list, format);//将变量arg_list初始化为传入参数
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);//清空可变参列表
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}
bool http_conn::add_status_line(int status, const char *title)//添加状态行
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)//添加消息报头，具体的添加文本长度、连接状态和空行
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length(int content_len)//添加Content-Length，表示响应报文的长度
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()//添加文本类型，这里是html
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()//添加连接状态，通知浏览器端是保持连接还是关闭
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()//添加空行
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)//添加文本content
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form)); 
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        //如果请求的资源存在  根据mmap
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {   //如果请求的资源大小为0，则返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()//对读写过程封装
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)//NO_REQUEST，表示请求不完整，需要继续接收请求数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);//注册并监听事件
        return;
    }
    bool write_ret = process_write(read_ret);//调用process_write完成报文响应
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);//注册并监听写事件
}
