
http连接处理类
===============
根据状态转移,通过主从状态机封装了http连接类。其中,主状态机在内部调用从状态机,从状态机将处理状态和数据传给主状态机
> * 客户端发出http连接请求
> * 从状态机读取数据,更新自身状态和接收数据,传给主状态机
> * 主状态机根据从状态机状态,更新自身状态,决定响应请求还是继续读取

* http报文处理流程
浏览器端发出http连接请求，主线程创建http对象接收请求并将所有数据读入对应buffer，将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
工作线程取出任务后，调用process_read函数，通过主、从状态机对请求报文进行解析。
解析完之后，跳转do_request函数生成响应报文，通过process_write写入buffer，返回给浏览器端.

http请求接收部分，涉及到init和read_once函数，init仅仅是对私有成员变量进行初始化，
read_once读取浏览器端发送来的请求报文，直到无数据可读或对方关闭连接，读取到m_read_buffer中，并更新m_read_idx。

