#include "webserver.h"

using namespace std;
//WebServer构造函数
WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
    {

    srcDir_ = getcwd(nullptr, 256);
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);                    //获取静态资源路径

    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;                             //初始化http连接数量以及资源路径

    //初始化数据库连接池实例
    SqlConnPool::Instance()->Init("192.168.160.1", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    InitEventMode_(trigMode);                               //初始化事件模式（3为ET，默认为LT）
    if(!InitSocket_()) { isClose_ = true;}                  //初始化socket

    if(openLog) {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

//设置监听和通信文件描述符的模式
void WebServer::InitEventMode_(int trigMode) {
    listenEvent_ = EPOLLRDHUP;                              //断开连接或异常终止检查事件
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;                 //oneshot避免多线程处理同一fd
    switch (trigMode)
    {
    case 0:
        break;
    case 1:                                                 //仅连接事件ET
        connEvent_ |= EPOLLET;                              
        break;
    case 2:                                                 //监听事件LT
        listenEvent_ |= EPOLLET;                            
        break;
    case 3:                                                 //监听事件和连接事件均ET
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    default:                                                //其他情况同3
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);                //是否为ET模式
}

void WebServer::Start() {
    int timeMS = -1;  /* epoll wait timeout == -1 无事件将阻塞 */
    if(!isClose_) { LOG_INFO("========== Server start =========="); }
    while(!isClose_) {
        if(timeoutMS_ > 0) {
            timeMS = timer_->GetNextTick();
        }
        int eventCnt = epoller_->Wait(timeMS);                  //等待到的事件数量，接着遍历每个事件
        for(int i = 0; i < eventCnt; i++) {
            /* 处理事件 */
            int fd = epoller_->GetEventFd(i);                   //得到读事件或者相应事件的文件描述符
            uint32_t events = epoller_->GetEvents(i);           //得到相应的事件
            if(fd == listenFd_) {                               //处理监听的文件描述符，接收客户端连接
                DealListen_();
            }
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {  //错误处理  对端关闭/tcp异常/未知错误
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);                            //关闭连接
            }
            else if(events & EPOLLIN) {                             //处理读事件
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            else if(events & EPOLLOUT) {                            //处理写事件
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            } else {
                LOG_ERROR("Unexpected event");//
            }
        }
    }
}

void WebServer::SendError_(int fd, const char*info) {
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if(ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn* client) {
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

//添加新的客户端到用户表里，并对其初始化，注册读事件
void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    users_[fd].init(fd, addr);                          //将客户端的文件描述符和端口初始化保存到用户表
    if(timeoutMS_ > 0) {
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);          //给该文件描述符注册读事件并将其设为连接事件
    SetFdNonblock(fd);                                  //因为设置了ET模式则需要将fd设置为非阻塞
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

//处理监听事件
void WebServer::DealListen_() {
    struct sockaddr_in addr;                        //创建新socket地址保存新连接的客户端信息
    socklen_t len = sizeof(addr);                   //获取长度
    do {
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);     
        //accept监听的文件描述符存入和客户端socket地址
        //服务端与之通信的socket文件描述符为fd
        if(fd <= 0) { return;}
        else if(HttpConn::userCount >= MAX_FD) {    //太多连接了超过了最大文件描述符的数量
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);                       //添加客户端信息
    } while(listenEvent_ & EPOLLET);                //监听的文件描述符为ET模式，则通过循环一个个的accept，全部数据读取连接。
}

//处理读事件
void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    //reactor模式将读操作交给子线程完成
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client)); //通过绑定类成员函数，以及该对象指针
}

void WebServer::DealWrite_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn* client) {
    assert(client);
    if(timeoutMS_ > 0) { timer_->adjust(client->GetFd(), timeoutMS_); }
}
//子线程读数据
void WebServer::OnRead_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);             //读取客户端的数据/非阻塞
    if(ret <= 0 && readErrno != EAGAIN) {
        CloseConn_(client);
        return;
    }
    OnProcess(client);//处理业务逻辑
}

void WebServer::OnProcess(HttpConn* client) {
    if(client->process()) {//读完及解析完
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);//注册为写事件
    } else {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

void WebServer::OnWrite_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if(client->ToWriteBytes() == 0) {
        /* 传输完成 */
        if(client->IsKeepAlive()) {
            OnProcess(client);
            return;
        }
    }
    else if(ret < 0) {
        if(writeErrno == EAGAIN) {
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

/* Create listenFd */                                           
//创建监听的文件描述符
bool WebServer::InitSocket_() {
    int ret;
    struct sockaddr_in addr;                                    //初始化socket地址
    if(port_ > 65535 || port_ < 1024) {                         //端口号有效判断
        LOG_ERROR("Port:%d error!",  port_);                    //日志输出
        return false;
    }
    addr.sin_family = AF_INET;                                  //设置socket协议族ipv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY);                   //主机字节序转换为网络字节序
    addr.sin_port = htons(port_);                               //端口号转换为网络字节序
    struct linger optLinger = { 0 };   
    if(openLinger_) {
        /* 优雅关闭: 直到所剩数据发送完毕或超时 */
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;                                 //超时时间内得到另一端确认
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);                //设置TCP协议的socket

    if(listenFd_ < 0) {                                         
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));//设置socket优雅关闭
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    /* 端口复用 */
    /* 只有最后一个套接字会正常接收数据。 */
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));//允许地址重用
    if(ret == -1) {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));          
    //绑定监听的文件描述符和socket端口和ip
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    ret = listen(listenFd_, 6);                                 
    //监听 最多同时处理6个客户端的最大等待队列长度6
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    
    ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN); 
    //对监听的文件描述符时间加上读事件
    if(ret == 0) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    SetFdNonblock(listenFd_);                               //设置监听的文件描述符非阻塞
    LOG_INFO("Server port:%d", port_);
    return true;
}
//设置文件描述符非阻塞
int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}


