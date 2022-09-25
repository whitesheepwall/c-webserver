#pragma once

//初始化监听套接字
int initListenFd(unsigned short port);

//启动epoll
int epollRun(int lfd);

//和客户端建立连接
int acceptClient(int fd, int epfd);
//void acceptClient(void* arg);

//接收http请求
//int recvHttpRequest(int fd, int epfd);
void recvHttpRequest(void* arg);

//解析请求行
int parseRequestLine(const char* head, const char* line, int cfd);

//解析get请求
int parseGet(char* path, int cfd);

//解析post请求
int parsePost(char* path, char* line, int cfd);

//解析get请求
int parseHead(char* path, int cfd);

//登录
int login(char* pt, int cfd);

//注册
int signup(char* pt, int cfd);

//发送文件
int sendFile(const char* fileName, int cfd);

//发送相应头（状态行+响应头）
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length);

//查找响应头类型
const char* getFileType(const char* name);

//发送目录
int sendDir(const char* dirName, int cfd);

//解决资源中文名的问题
//字符转整型数
int hexToDec(char c);

//to存储解码后的数据，传出参数，from是被解码的数据，传入参数
void decodeMsg(char* to, char* from);

//记录日志
void writeLog(void* arg);
