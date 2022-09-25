#include "server.h"
#include "threadpool.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <mysql/mysql.h>
#include <time.h>

extern ThreadPool* pool; //声明使用外部变量
extern pthread_mutex_t mutexLog;
extern pthread_mutex_t mutexSql;
extern MYSQL* mysql;

struct fdInfo
{
	int fd;
	int epfd;
	pthread_t tid;
};

int initListenFd(unsigned short port)
{
	//1.创建监听fd
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1) {
		perror("socket error");
		return -1;
	}

	//2.设置端口复用
	int opt = 1;
	int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	//3.绑定
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1) {
		perror("bind error");
		return -1;
	}

	//4.设置监听
	ret = listen(lfd, 128);
	if (ret == -1) {
		perror("listen error");
		return -1;
	}
	//返回fd
	return lfd;
}

int epollRun(int lfd)
{
	//1.创建epoll实例
	int epfd = epoll_create(1);//参数已弃用
	if (epfd == -1) {
		perror("epoll create error");
		return -1;
	}

	//2.lfd上树
	struct epoll_event ev;
	ev.data.fd = lfd;
	ev.events = EPOLLIN;
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	if (ret == -1) {
		perror("epoll_ctl error");
		return -1;
	}

	//3.检测
	int num;
	struct epoll_event evs[1024];
	int length = sizeof(evs) / sizeof(struct epoll_event);
	int fd;
	while (1)
	{
		num = epoll_wait(epfd, evs, length, -1);
		for (int i = 0; i < num; ++i) {
			fd = evs[i].data.fd;
			struct fdInfo* info = (struct fdInfo*)malloc(sizeof(struct fdInfo));
			info->fd = fd;
			info->epfd = epfd;
			//printf("fd:%d, lfd:%d, epfd:%d\n", fd, lfd, epfd);
			if (fd == lfd) {
				//建立新连接 accept
				acceptClient(fd, epfd);//单线程
				//pthread_create(&info->tid, NULL, acceptClient, info);//多线程
				//printf("**************************%d\n", fd);
				//threadPoolAdd(pool, acceptClient, info); //线程池
			}
			else {
				//接收对端的数据
				//recvHttpRequest(fd, epfd);
				//pthread_create(&info->tid, NULL, recvHttpRequest, info);
				threadPoolAdd(pool, recvHttpRequest, info);
			}
		}
	}
	return 0;
}

//int acceptClient(int lfd, int epfd)
//void acceptClient(void* arg)
//{
//	//pthread_detach(pthread_self());
//	printf("accept threadId: %ld\n", pthread_self());
//	struct fdInfo* info = (struct fdInfo*)arg;
//
//	//1.建立连接
//	int cfd = accept(info->fd, NULL, NULL);
//	if (cfd == -1) {
//		perror("accept error");
//		return;
//	}
//	printf("%d,connected\n",cfd);
//
//	//2.设置非阻塞
//	int flag = fcntl(cfd, F_GETFL);
//	flag |= O_NONBLOCK;
//	fcntl(cfd, F_SETFL, flag);
//
//	//3.cfd添加到epoll树上
//	struct epoll_event ev;
//	ev.data.fd = cfd;
//	ev.events = EPOLLIN | EPOLLET;  //非阻塞边缘模式效率高
//	int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
//	if (ret == -1) {
//		perror("epoll_cfd error");
//		return;
//	}
//
//	/*if (info) {
//		free(info);
//		info = NULL;
//	}*/
//	return;
//}

//int recvHttpRequest(int cfd, int epfd)
void recvHttpRequest(void* arg)
{
	//pthread_detach(pthread_self());
	//printf("recv threadId: %ld\n", pthread_self());
	struct fdInfo* info = (struct fdInfo*)arg;

	char buf[4096] = { 0 };
	char tmp[1024] = { 0 };
	int len = 0;   //接收数据长度
	int total = 0; //写入buf位置指针
	while ((len = recv(info->fd, tmp, sizeof(tmp), 0)) > 0)
	{
		if (total + len < sizeof(buf)) {
			memcpy(buf + total, tmp, len);
		}
		total += len;
	}
	printf("received: \n%s\n", buf);
	//判断数据是否被接收完毕
	if (len == -1 && errno == EAGAIN) {
		//解析请求行
		char* pt = strstr(buf, "\r\n");
		int reqLen = pt - buf;
		//通过在末尾加'\0'来截断
		//buf[reqLen] = '\0';
		char temp[1024];
		memcpy(temp, buf, reqLen);
		temp[reqLen] = '\0';
		parseRequestLine(temp, buf, info->fd);
	}
	else if (len == 0) {
		//客户端断开了连接，cfd下树
		printf("%d, disconnected\n", info->fd);
		epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
		close(info->fd);
	}
	else {
		perror("recv error");
	}
	
	/*if (info) {
		free(info);
		info = NULL;
	}*/
	return;
}

int parseRequestLine(const char* head, const char* line, int cfd)
{
	//解析请求行 get /xxx/1.jpg http/1.1
	char method[12];
	char path[1024];
	sscanf(head, "%[^ ] %[^ ]", method, path);
	printf("method:%s, path:%s\n", method, path);
	if (strcasecmp(method, "get") == 0) {
		parseGet(path, cfd);
	}
	else if (strcasecmp(method, "post") == 0) {
		parsePost(path, line, cfd);
	}
	else if (strcasecmp(method, "head") == 0) {
		parseHead(path, cfd);
	}
	else {
		struct stat st;
		//无效请求 -- 回复400页面
		stat("400.html", &st);
		sendHeadMsg(cfd, 400, "Bad Request", getFileType(".html"), st.st_size);
		sendFile("400.html", cfd);
		return -1;
	}
	
	return 0;
}

int parseGet(char* path, int cfd)
{
	//请求资源解码
	decodeMsg(path, path);

	//处理客户端请求的动态资源(目录或文件)
	char* file = NULL;
	if (strcmp(path, "/") == 0) {
		//如果请求的路径就是当前资源路径，就把file路径改为当前路径的相对路径./
		file = "./index.html";
	}
	else {
		//如果请求的路径不是当前资源，取得其相对路径，移动一下指针把最前边的/取代
		file = path + 1;
	}
	//获取文件属性
	//定义一个传入参数，这个结构体里会被写入文件属性，大小类型...
	struct stat st;
	int ret = stat(file, &st);
	if (ret == -1) {
		//文件不存在 -- 回复404页面
		stat("404.html", &st);
		sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
		sendFile("404.html", cfd);
		return 0;
	}

	//触发403forbidden
	if (strcmp(file, "forbidden.html") == 0) {
		stat("403.html", &st);
		sendHeadMsg(cfd, 403, "Forbidden", getFileType(".html"), st.st_size);
		sendFile("403.html", cfd);
		return 0;
	}

	//判断文件类型
	if (S_ISDIR(st.st_mode)) {  //返回1是目录，返回0是文件
		//把目录中的内容发送给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
		sendDir(file, cfd);
	}
	else {
		//把文件的内容发送给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
		sendFile(file, cfd);
	}

	//写日志
	char* log = (char*)malloc(512);//任务参数开在堆里迎合线程池构造
	char ip[16] = { 0 };
	struct sockaddr_in client;
	socklen_t len = sizeof(client);

	//获取对方ip，端口
	getpeername(cfd, (struct sockaddr*)&client, &len);
	time_t t;
	time(&t);
	sprintf(log, "access:[%s]-[%d]-[GET]-[192.168.188.130:6789%s] %s", inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)), ntohs(client.sin_port),path, ctime(&t));
	threadPoolAdd(pool, writeLog, log);

	return 0;
}

int parsePost(char* path, char* line, int cfd)
{
	//请求资源解码
	decodeMsg(path, path);

	//触发403forbidden
	struct stat st;
	if (strcmp(path+1, "forbidden.html") == 0) {
		stat("403.html", &st);
		sendHeadMsg(cfd, 403, "Forbidden", getFileType(".html"), st.st_size);
		sendFile("403.html", cfd);
		return 0;
	}

	char* pt = strstr(line, "\r\n\r\n")+4;
	//写日志
	char* log = (char*)malloc(512);//任务参数开在堆里迎合线程池构造
	char ip[16] = { 0 };
	struct sockaddr_in client;
	socklen_t len = sizeof(client);

	//获取对方ip，端口
	getpeername(cfd, (struct sockaddr*)&client, &len);
	time_t t;
	time(&t);
	sprintf(log, "access:[%s]-[%d]-[POST]-[192.168.188.130:6789%s]-[values=%s] %s", inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)), ntohs(client.sin_port), path, pt, ctime(&t));
	threadPoolAdd(pool, writeLog, log);

	//获取post数据
	if (strcmp(path, "/login.html") == 0) {
		int ret = login(pt, cfd);
		if (ret == -1) {
			printf("login error...\n");
			return -1;
		}
	}
	else if (strcmp(path, "/register.html") == 0) {
		int ret = signup(pt, cfd);
		if (ret == -1) {
			printf("register error...\n");
			return -1;
		}
	}

	return 0;
}

int parseHead(char* path, int cfd)
{
	//请求资源解码
	decodeMsg(path, path);

	//处理客户端请求的动态资源(目录或文件)
	char* file = NULL;
	if (strcmp(path, "/") == 0) {
		//如果请求的路径就是当前资源路径，就把file路径改为当前路径的相对路径./
		file = "./index.html";
	}
	else {
		//如果请求的路径不是当前资源，取得其相对路径，移动一下指针把最前边的/取代
		file = path + 1;
	}
	//获取文件属性
	//定义一个传入参数，这个结构体里会被写入文件属性，大小类型...
	struct stat st;
	int ret = stat(file, &st);
	if (ret == -1) {
		//文件不存在 -- 回复404页面
		stat("404.html", &st);
		sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
		//sendFile("404.html", cfd);
		return 0;
	}

	//触发403forbidden
	if (strcmp(file, "forbidden.html") == 0) {
		stat("403.html", &st);
		sendHeadMsg(cfd, 403, "Forbidden", getFileType(".html"), st.st_size);
		//sendFile("403.html", cfd);
		return 0;
	}

	//判断文件类型
	if (S_ISDIR(st.st_mode)) {  //返回1是目录，返回0是文件
		//把目录中的内容发送给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
		//sendDir(file, cfd);
	}
	else {
		//把文件的内容发送给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
		//sendFile(file, cfd);
	}

	return 0;
}

int login(char* pt, int cfd)
{
	char query[512];
	char username[20];
	char password[20];
	sscanf(pt, "username=%[a-z0-9A-Z]&password=%[a-z0-9A-Z]", username, password);
	printf("%s\n%s\n", username, password);

	memset(query, 0, sizeof(query));
	sprintf(query, "select * from tbl_user where username='%s' and password='%s';", username, password);
	//执行查询
	pthread_mutex_lock(&mutexSql);
	int ret = mysql_query(mysql, query);
	if (ret != 0) {
		//打印错误信息
		pthread_mutex_unlock(&mutexSql);
		printf("query error: %s\n", mysql_error(mysql));
		return -1;
	}
	//获取查询结果
	MYSQL_RES* mysql_res = mysql_store_result(mysql);
	if (mysql_res == NULL) {
		pthread_mutex_unlock(&mutexSql);
		printf("get data error: %s\n", mysql_error(mysql));
		return -1;
	}
	//获取结果行数
	int num = mysql_num_rows(mysql_res);
	struct stat st;
	if (num == 0) {
		//返回登录失败信息
		printf("用户名或密码错误\n");
		int ret = stat("login.html", &st);
		if (ret == -1) {
			//文件不存在 -- 回复404页面
			pthread_mutex_unlock(&mutexSql);
			stat("404.html", &st);
			sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
			sendFile("404.html", cfd);
			return 0;
		}
		pthread_mutex_unlock(&mutexSql);
		sendHeadMsg(cfd, 200, "username or password error", getFileType(".html"), st.st_size);
		sendFile("login.html", cfd);
	}
	else {
		//登录成功信息
		int ret = stat("index.html", &st);
		if (ret == -1) {
			//文件不存在 -- 回复404页面
			pthread_mutex_unlock(&mutexSql);
			stat("404.html", &st);
			sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
			sendFile("404.html", cfd);
			return 0;
		}
		pthread_mutex_unlock(&mutexSql);
		sendHeadMsg(cfd, 200, "login success", getFileType(".html"), st.st_size);
		sendFile("index.html", cfd);
	}
	//释放结果集占用的内存
	mysql_free_result(mysql_res);
	return 0;
}

int signup(char* pt, int cfd)
{
	char query[512];
	char username[20];
	char password[20];
	char confirmPassword[20];
	sscanf(pt, "username=%[a-z0-9A-Z]&password=%[a-z0-9A-Z]&confirmPassword=%[a-z0-9A-Z]", username, password,confirmPassword);
	printf("%s\n%s\n%s\n", username, password, confirmPassword);

	memset(query, 0, sizeof(query));
	sprintf(query, "select * from tbl_user where username='%s';", username);
	//执行查询
	pthread_mutex_lock(&mutexSql);
	int ret = mysql_query(mysql, query);
	if (ret != 0) {
		//打印错误信息
		printf("query error: %s\n", mysql_error(mysql));
		pthread_mutex_unlock(&mutexSql);
		return -1;
	}
	//获取查询结果
	MYSQL_RES* mysql_res = mysql_store_result(mysql);
	if (mysql_res == NULL) {
		printf("get data error: %s\n", mysql_error(mysql));
		pthread_mutex_unlock(&mutexSql);
		return -1;
	}
	//获取结果行数
	int num = mysql_num_rows(mysql_res);
	struct stat st;
	if (num == 0) {
		//将账号密码插入表中
		memset(query, 0, sizeof(query));
		sprintf(query, "insert into tbl_user(username,password) values('%s','%s') ;", username, password);
		if (mysql_query(mysql, query)!= 0) {
			//打印错误信息
			printf("insert error: %s\n", mysql_error(mysql));
			pthread_mutex_unlock(&mutexSql);
			return -1;
		}

		//返回注册成功信息
		printf("该账号可用\n");
		int ret = stat("login.html", &st);
		if (ret == -1) {
			//文件不存在 -- 回复404页面
			pthread_mutex_unlock(&mutexSql);
			stat("404.html", &st);
			sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
			sendFile("404.html", cfd);
			return 0;
		}
		pthread_mutex_unlock(&mutexSql);
		sendHeadMsg(cfd, 200, "register success", getFileType(".html"), st.st_size);
		sendFile("login.html", cfd);
	}
	else {
		//注册失败信息
		printf("该账号已存在\n");
		int ret = stat("register.html", &st);
		if (ret == -1) {
			//文件不存在 -- 回复404页面
			stat("404.html", &st);
			pthread_mutex_unlock(&mutexSql);
			sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
			sendFile("404.html", cfd);
			return 0;
		}
		pthread_mutex_unlock(&mutexSql);
		sendHeadMsg(cfd, 200, "account existsed", getFileType(".html"), st.st_size);
		sendFile("register.html", cfd);
	}
	//释放结果集占用的内存
	mysql_free_result(mysql_res);
	return 0;
}

int sendFile(const char* fileName, int cfd)
{
	//1.打开文件
	int fd = open(fileName, O_RDONLY);
	assert(fd > 0);

#if 0
	char buf[1024];
	int len;
	while (1) {
		len = read(fd, buf, sizeof(buf));
		if (len > 0) {
			send(cfd, buf, len, 0);
			usleep(10); //这非常重要,发的太快可能导致接收端出现问题
		}
		else if (len == 0) {
			break;
		}
		else {
			perror("read file error");
			break;
		}
	}
#else
	//linux自带的发送文件函数sendfile，效率高
	//arg1:通信描述符,arg2:打开的文件描述符,arg3:文件内容的偏移量(从第几个位置开始发送文件，偏移量会自动修改),arg4:文件的大小
	off_t offset = 0;
	int size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	int ret;
	while (offset < size) {
		//fd去读文件内容，cfd从fd拿到内容发送出去，但有时候cfd发的太快，又设置了非阻塞，
		//所以即使fd没有数据，cfd也会去读，所以ret会返回-1
		ret = sendfile(cfd, fd, &offset, size);
		if (ret == -1 && errno == EAGAIN) {
			//printf("No Data...\n");
		}
	}

#endif
	close(fd);
	return 0;
}

int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length)
{
	//响应体：1.状态行，2.响应头，3.空行\r\n，4.数据
	//状态行
	char buf[4096] = { 0 };
	sprintf(buf, "http/1.1 %d %s\r\n", status, descr);

	//响应头
	sprintf(buf + strlen(buf), "content-type: %s\r\n", type);
	//sprintf(buf + strlen(buf), "max-http-header-size: 1024000\r\n");
	//sprintf(buf + strlen(buf), "Transfer-Encoding: chunked\r\n\r\n");
	sprintf(buf + strlen(buf), "content-length: %d\r\n\r\n", length);
	send(cfd, buf, strlen(buf), 0);
	printf("%s", buf);
	return 0;
}

const char* getFileType(const char* name)
{
	char* dot;
	//从右向左查找.
	dot = strrchr(name, '.');
	if (dot == NULL)return "text/plain; charset=utf-8";
	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)return "text/html; charset=utf-8";
	if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)return "image/jpeg";
	if (strcmp(dot, ".gif") == 0)return "image/gif";
	if (strcmp(dot, ".png") == 0)return "image/png";
	if (strcmp(dot, ".css") == 0)return "text/css";
	if (strcmp(dot, ".au") == 0)return "audio/basic";
	if (strcmp(dot, ".wav") == 0)return "audio/wav";
	if (strcmp(dot, ".avi") == 0)return "video/x-msvideo";
	if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)return "video/quicktime";
	if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)return "video/mpeg";
	if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)return "model/vrml";
	if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)return "audio/midi";
	if (strcmp(dot, ".mp3") == 0)return "audio/mpeg";
	if (strcmp(dot, ".ogg") == 0)return "application/ogg";
	if (strcmp(dot, ".pac") == 0)return "application/x-ns-proxy-autoconfig";

	return "text/plain; charset=utf-8";
}

int sendDir(const char* dirName, int cfd)
{
	char buf[4096] = { 0 };
	sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);

	struct dirent** namelist;
	int num = scandir(dirName, &namelist, NULL, alphasort);
	char subPath[1024];
	for (int i = 0; i < num; ++i) {
		//取出文件名,相对路径需要拼接
		char* name = namelist[i]->d_name;
		struct stat st;
		memset(subPath,0,sizeof(subPath));
		sprintf(subPath, "%s/%s", dirName, name);
		stat(subPath, &st);
		if (S_ISDIR(st.st_mode)) {
			//添加<a href=""></a>标签可以变超链接
			sprintf(buf + strlen(buf), "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
		}
		else {
			//拼接的是文件，路径不需要加斜杠
			sprintf(buf + strlen(buf), "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
		}
		send(cfd, buf, strlen(buf), 0);
		printf("%s", buf);
		memset(buf, 0, sizeof(buf));

		free(namelist[i]);
	}
	sprintf(buf, "</table></body></html>\r\n");
	send(cfd, buf, strlen(buf), 0);
	printf("%s", buf);
	free(namelist);
	return 0;
}

int hexToDec(char c)  //十六进制转十进制
{
	if (c >= '0' && c <= '9')return c - '0';
	if (c >= 'a' && c <= 'f')return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')return c - 'A' + 10;
	return 0;
}

void decodeMsg(char* to, char* from)
{
	for (; *from != '\0'; ++to, ++from) {
		//isxdigit -> 判断字符是不是16进制格式，取值在0-f
		//Linux%E5%86%85%E6%A0%B8.jpg
		if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2]))
		{
			//将16进制的数 ->十进制 将这个数值赋给字符 int->char
			//B2  178
			//将3个字符，变成了一个字符，这个字符就是原始数据
			*to = hexToDec(from[1]) * 16 + hexToDec(from[2]);

			//跳过from[1]和from[2]因此在当前循环中已经处理过了
			from += 2;
		}
		else {
			*to = *from;
		}
	}
	*to = '\0';
}

void writeLog(void* arg)
{
	char* log = (char*)arg;
	pthread_mutex_lock(&mutexLog);
	FILE* fd = fopen("log.txt", "a");
	if (!fd) {
		pthread_mutex_unlock(&mutexLog);
		printf("open file error\n");
		return;
	}
	fputs(log, fd);
	fclose(fd);
	pthread_mutex_unlock(&mutexLog);
}





//int recvHttpRequest(int cfd, int epfd)
//{
//	char buf[4096] = { 0 };
//	char tmp[1024] = { 0 };
//	int len = 0;   //接收数据长度
//	int total = 0; //写入buf位置指针
//	while ((len = recv(cfd, tmp, sizeof(tmp), 0)) > 0)
//	{
//		if (total + len < sizeof(buf)) {
//			memcpy(buf + total, tmp, len);
//		}
//		total += len;
//	}
//	printf("received: \n%s\n", buf);
//	//判断数据是否被接收完毕
//	if (len == -1 && errno == EAGAIN) {
//		//解析请求行
//		char* pt = strstr(buf, "\r\n");
//		int reqLen = pt - buf;
//		//通过在末尾加'\0'来截断
//		buf[reqLen] = '\0';
//		parseRequestLine(buf, cfd);
//	}
//	else if (len == 0) {
//		//客户端断开了连接，cfd下树
//		printf("%d, disconnected\n", cfd);
//		epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
//		close(cfd);
//	}
//	else {
//		perror("recv error");
//	}
//
//	/*if (info) {
//		free(info);
//		info = NULL;
//	}*/
//	return;
//}

int acceptClient(int lfd, int epfd)
{
	struct sockaddr_in client;
	socklen_t len = sizeof(client);

	//1.建立连接
	int cfd = accept(lfd, (struct sockaddr *)&client, &len);
	if (cfd == -1) {
		perror("accept error");
		return;
	}
	printf("%d,connected\n", cfd);

	//2.设置非阻塞
	int flag = fcntl(cfd, F_GETFL);
	flag |= O_NONBLOCK;
	fcntl(cfd, F_SETFL, flag);

	//3.cfd添加到epoll树上
	struct epoll_event ev;
	ev.data.fd = cfd;
	ev.events = EPOLLIN | EPOLLET;  //非阻塞边缘模式效率高
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1) {
		perror("epoll_cfd error");
		return;
	}

	/*if (info) {
		free(info);
		info = NULL;
	}*/

	//记录连接日志
	char *log = (char*)malloc(256);//任务参数开在堆里迎合线程池构造
	char ip[16] = { 0 };
	time_t t;
	time(&t);
	sprintf(log, "client connected:[%s]-[%d]- %s", inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)), ntohs(client.sin_port), ctime(&t));
	threadPoolAdd(pool, writeLog, log);
	return;
}