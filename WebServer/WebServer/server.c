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

extern ThreadPool* pool; //����ʹ���ⲿ����
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
	//1.��������fd
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1) {
		perror("socket error");
		return -1;
	}

	//2.���ö˿ڸ���
	int opt = 1;
	int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	//3.��
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1) {
		perror("bind error");
		return -1;
	}

	//4.���ü���
	ret = listen(lfd, 128);
	if (ret == -1) {
		perror("listen error");
		return -1;
	}
	//����fd
	return lfd;
}

int epollRun(int lfd)
{
	//1.����epollʵ��
	int epfd = epoll_create(1);//����������
	if (epfd == -1) {
		perror("epoll create error");
		return -1;
	}

	//2.lfd����
	struct epoll_event ev;
	ev.data.fd = lfd;
	ev.events = EPOLLIN;
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	if (ret == -1) {
		perror("epoll_ctl error");
		return -1;
	}

	//3.���
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
				//���������� accept
				acceptClient(fd, epfd);//���߳�
				//pthread_create(&info->tid, NULL, acceptClient, info);//���߳�
				//printf("**************************%d\n", fd);
				//threadPoolAdd(pool, acceptClient, info); //�̳߳�
			}
			else {
				//���նԶ˵�����
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
//	//1.��������
//	int cfd = accept(info->fd, NULL, NULL);
//	if (cfd == -1) {
//		perror("accept error");
//		return;
//	}
//	printf("%d,connected\n",cfd);
//
//	//2.���÷�����
//	int flag = fcntl(cfd, F_GETFL);
//	flag |= O_NONBLOCK;
//	fcntl(cfd, F_SETFL, flag);
//
//	//3.cfd��ӵ�epoll����
//	struct epoll_event ev;
//	ev.data.fd = cfd;
//	ev.events = EPOLLIN | EPOLLET;  //��������ԵģʽЧ�ʸ�
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
	int len = 0;   //�������ݳ���
	int total = 0; //д��bufλ��ָ��
	while ((len = recv(info->fd, tmp, sizeof(tmp), 0)) > 0)
	{
		if (total + len < sizeof(buf)) {
			memcpy(buf + total, tmp, len);
		}
		total += len;
	}
	printf("received: \n%s\n", buf);
	//�ж������Ƿ񱻽������
	if (len == -1 && errno == EAGAIN) {
		//����������
		char* pt = strstr(buf, "\r\n");
		int reqLen = pt - buf;
		//ͨ����ĩβ��'\0'���ض�
		//buf[reqLen] = '\0';
		char temp[1024];
		memcpy(temp, buf, reqLen);
		temp[reqLen] = '\0';
		parseRequestLine(temp, buf, info->fd);
	}
	else if (len == 0) {
		//�ͻ��˶Ͽ������ӣ�cfd����
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
	//���������� get /xxx/1.jpg http/1.1
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
		//��Ч���� -- �ظ�400ҳ��
		stat("400.html", &st);
		sendHeadMsg(cfd, 400, "Bad Request", getFileType(".html"), st.st_size);
		sendFile("400.html", cfd);
		return -1;
	}
	
	return 0;
}

int parseGet(char* path, int cfd)
{
	//������Դ����
	decodeMsg(path, path);

	//����ͻ�������Ķ�̬��Դ(Ŀ¼���ļ�)
	char* file = NULL;
	if (strcmp(path, "/") == 0) {
		//��������·�����ǵ�ǰ��Դ·�����Ͱ�file·����Ϊ��ǰ·�������·��./
		file = "./index.html";
	}
	else {
		//��������·�����ǵ�ǰ��Դ��ȡ�������·�����ƶ�һ��ָ�����ǰ�ߵ�/ȡ��
		file = path + 1;
	}
	//��ȡ�ļ�����
	//����һ���������������ṹ����ᱻд���ļ����ԣ���С����...
	struct stat st;
	int ret = stat(file, &st);
	if (ret == -1) {
		//�ļ������� -- �ظ�404ҳ��
		stat("404.html", &st);
		sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
		sendFile("404.html", cfd);
		return 0;
	}

	//����403forbidden
	if (strcmp(file, "forbidden.html") == 0) {
		stat("403.html", &st);
		sendHeadMsg(cfd, 403, "Forbidden", getFileType(".html"), st.st_size);
		sendFile("403.html", cfd);
		return 0;
	}

	//�ж��ļ�����
	if (S_ISDIR(st.st_mode)) {  //����1��Ŀ¼������0���ļ�
		//��Ŀ¼�е����ݷ��͸��ͻ���
		sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
		sendDir(file, cfd);
	}
	else {
		//���ļ������ݷ��͸��ͻ���
		sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
		sendFile(file, cfd);
	}

	//д��־
	char* log = (char*)malloc(512);//����������ڶ���ӭ���̳߳ع���
	char ip[16] = { 0 };
	struct sockaddr_in client;
	socklen_t len = sizeof(client);

	//��ȡ�Է�ip���˿�
	getpeername(cfd, (struct sockaddr*)&client, &len);
	time_t t;
	time(&t);
	sprintf(log, "access:[%s]-[%d]-[GET]-[192.168.188.130:6789%s] %s", inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)), ntohs(client.sin_port),path, ctime(&t));
	threadPoolAdd(pool, writeLog, log);

	return 0;
}

int parsePost(char* path, char* line, int cfd)
{
	//������Դ����
	decodeMsg(path, path);

	//����403forbidden
	struct stat st;
	if (strcmp(path+1, "forbidden.html") == 0) {
		stat("403.html", &st);
		sendHeadMsg(cfd, 403, "Forbidden", getFileType(".html"), st.st_size);
		sendFile("403.html", cfd);
		return 0;
	}

	char* pt = strstr(line, "\r\n\r\n")+4;
	//д��־
	char* log = (char*)malloc(512);//����������ڶ���ӭ���̳߳ع���
	char ip[16] = { 0 };
	struct sockaddr_in client;
	socklen_t len = sizeof(client);

	//��ȡ�Է�ip���˿�
	getpeername(cfd, (struct sockaddr*)&client, &len);
	time_t t;
	time(&t);
	sprintf(log, "access:[%s]-[%d]-[POST]-[192.168.188.130:6789%s]-[values=%s] %s", inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)), ntohs(client.sin_port), path, pt, ctime(&t));
	threadPoolAdd(pool, writeLog, log);

	//��ȡpost����
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
	//������Դ����
	decodeMsg(path, path);

	//����ͻ�������Ķ�̬��Դ(Ŀ¼���ļ�)
	char* file = NULL;
	if (strcmp(path, "/") == 0) {
		//��������·�����ǵ�ǰ��Դ·�����Ͱ�file·����Ϊ��ǰ·�������·��./
		file = "./index.html";
	}
	else {
		//��������·�����ǵ�ǰ��Դ��ȡ�������·�����ƶ�һ��ָ�����ǰ�ߵ�/ȡ��
		file = path + 1;
	}
	//��ȡ�ļ�����
	//����һ���������������ṹ����ᱻд���ļ����ԣ���С����...
	struct stat st;
	int ret = stat(file, &st);
	if (ret == -1) {
		//�ļ������� -- �ظ�404ҳ��
		stat("404.html", &st);
		sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), st.st_size);
		//sendFile("404.html", cfd);
		return 0;
	}

	//����403forbidden
	if (strcmp(file, "forbidden.html") == 0) {
		stat("403.html", &st);
		sendHeadMsg(cfd, 403, "Forbidden", getFileType(".html"), st.st_size);
		//sendFile("403.html", cfd);
		return 0;
	}

	//�ж��ļ�����
	if (S_ISDIR(st.st_mode)) {  //����1��Ŀ¼������0���ļ�
		//��Ŀ¼�е����ݷ��͸��ͻ���
		sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
		//sendDir(file, cfd);
	}
	else {
		//���ļ������ݷ��͸��ͻ���
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
	//ִ�в�ѯ
	pthread_mutex_lock(&mutexSql);
	int ret = mysql_query(mysql, query);
	if (ret != 0) {
		//��ӡ������Ϣ
		pthread_mutex_unlock(&mutexSql);
		printf("query error: %s\n", mysql_error(mysql));
		return -1;
	}
	//��ȡ��ѯ���
	MYSQL_RES* mysql_res = mysql_store_result(mysql);
	if (mysql_res == NULL) {
		pthread_mutex_unlock(&mutexSql);
		printf("get data error: %s\n", mysql_error(mysql));
		return -1;
	}
	//��ȡ�������
	int num = mysql_num_rows(mysql_res);
	struct stat st;
	if (num == 0) {
		//���ص�¼ʧ����Ϣ
		printf("�û������������\n");
		int ret = stat("login.html", &st);
		if (ret == -1) {
			//�ļ������� -- �ظ�404ҳ��
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
		//��¼�ɹ���Ϣ
		int ret = stat("index.html", &st);
		if (ret == -1) {
			//�ļ������� -- �ظ�404ҳ��
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
	//�ͷŽ����ռ�õ��ڴ�
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
	//ִ�в�ѯ
	pthread_mutex_lock(&mutexSql);
	int ret = mysql_query(mysql, query);
	if (ret != 0) {
		//��ӡ������Ϣ
		printf("query error: %s\n", mysql_error(mysql));
		pthread_mutex_unlock(&mutexSql);
		return -1;
	}
	//��ȡ��ѯ���
	MYSQL_RES* mysql_res = mysql_store_result(mysql);
	if (mysql_res == NULL) {
		printf("get data error: %s\n", mysql_error(mysql));
		pthread_mutex_unlock(&mutexSql);
		return -1;
	}
	//��ȡ�������
	int num = mysql_num_rows(mysql_res);
	struct stat st;
	if (num == 0) {
		//���˺�����������
		memset(query, 0, sizeof(query));
		sprintf(query, "insert into tbl_user(username,password) values('%s','%s') ;", username, password);
		if (mysql_query(mysql, query)!= 0) {
			//��ӡ������Ϣ
			printf("insert error: %s\n", mysql_error(mysql));
			pthread_mutex_unlock(&mutexSql);
			return -1;
		}

		//����ע��ɹ���Ϣ
		printf("���˺ſ���\n");
		int ret = stat("login.html", &st);
		if (ret == -1) {
			//�ļ������� -- �ظ�404ҳ��
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
		//ע��ʧ����Ϣ
		printf("���˺��Ѵ���\n");
		int ret = stat("register.html", &st);
		if (ret == -1) {
			//�ļ������� -- �ظ�404ҳ��
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
	//�ͷŽ����ռ�õ��ڴ�
	mysql_free_result(mysql_res);
	return 0;
}

int sendFile(const char* fileName, int cfd)
{
	//1.���ļ�
	int fd = open(fileName, O_RDONLY);
	assert(fd > 0);

#if 0
	char buf[1024];
	int len;
	while (1) {
		len = read(fd, buf, sizeof(buf));
		if (len > 0) {
			send(cfd, buf, len, 0);
			usleep(10); //��ǳ���Ҫ,����̫����ܵ��½��ն˳�������
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
	//linux�Դ��ķ����ļ�����sendfile��Ч�ʸ�
	//arg1:ͨ��������,arg2:�򿪵��ļ�������,arg3:�ļ����ݵ�ƫ����(�ӵڼ���λ�ÿ�ʼ�����ļ���ƫ�������Զ��޸�),arg4:�ļ��Ĵ�С
	off_t offset = 0;
	int size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	int ret;
	while (offset < size) {
		//fdȥ���ļ����ݣ�cfd��fd�õ����ݷ��ͳ�ȥ������ʱ��cfd����̫�죬�������˷�������
		//���Լ�ʹfdû�����ݣ�cfdҲ��ȥ��������ret�᷵��-1
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
	//��Ӧ�壺1.״̬�У�2.��Ӧͷ��3.����\r\n��4.����
	//״̬��
	char buf[4096] = { 0 };
	sprintf(buf, "http/1.1 %d %s\r\n", status, descr);

	//��Ӧͷ
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
	//�����������.
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
		//ȡ���ļ���,���·����Ҫƴ��
		char* name = namelist[i]->d_name;
		struct stat st;
		memset(subPath,0,sizeof(subPath));
		sprintf(subPath, "%s/%s", dirName, name);
		stat(subPath, &st);
		if (S_ISDIR(st.st_mode)) {
			//���<a href=""></a>��ǩ���Ա䳬����
			sprintf(buf + strlen(buf), "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
		}
		else {
			//ƴ�ӵ����ļ���·������Ҫ��б��
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

int hexToDec(char c)  //ʮ������תʮ����
{
	if (c >= '0' && c <= '9')return c - '0';
	if (c >= 'a' && c <= 'f')return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')return c - 'A' + 10;
	return 0;
}

void decodeMsg(char* to, char* from)
{
	for (; *from != '\0'; ++to, ++from) {
		//isxdigit -> �ж��ַ��ǲ���16���Ƹ�ʽ��ȡֵ��0-f
		//Linux%E5%86%85%E6%A0%B8.jpg
		if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2]))
		{
			//��16���Ƶ��� ->ʮ���� �������ֵ�����ַ� int->char
			//B2  178
			//��3���ַ��������һ���ַ�������ַ�����ԭʼ����
			*to = hexToDec(from[1]) * 16 + hexToDec(from[2]);

			//����from[1]��from[2]����ڵ�ǰѭ�����Ѿ��������
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
//	int len = 0;   //�������ݳ���
//	int total = 0; //д��bufλ��ָ��
//	while ((len = recv(cfd, tmp, sizeof(tmp), 0)) > 0)
//	{
//		if (total + len < sizeof(buf)) {
//			memcpy(buf + total, tmp, len);
//		}
//		total += len;
//	}
//	printf("received: \n%s\n", buf);
//	//�ж������Ƿ񱻽������
//	if (len == -1 && errno == EAGAIN) {
//		//����������
//		char* pt = strstr(buf, "\r\n");
//		int reqLen = pt - buf;
//		//ͨ����ĩβ��'\0'���ض�
//		buf[reqLen] = '\0';
//		parseRequestLine(buf, cfd);
//	}
//	else if (len == 0) {
//		//�ͻ��˶Ͽ������ӣ�cfd����
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

	//1.��������
	int cfd = accept(lfd, (struct sockaddr *)&client, &len);
	if (cfd == -1) {
		perror("accept error");
		return;
	}
	printf("%d,connected\n", cfd);

	//2.���÷�����
	int flag = fcntl(cfd, F_GETFL);
	flag |= O_NONBLOCK;
	fcntl(cfd, F_SETFL, flag);

	//3.cfd��ӵ�epoll����
	struct epoll_event ev;
	ev.data.fd = cfd;
	ev.events = EPOLLIN | EPOLLET;  //��������ԵģʽЧ�ʸ�
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1) {
		perror("epoll_cfd error");
		return;
	}

	/*if (info) {
		free(info);
		info = NULL;
	}*/

	//��¼������־
	char *log = (char*)malloc(256);//����������ڶ���ӭ���̳߳ع���
	char ip[16] = { 0 };
	time_t t;
	time(&t);
	sprintf(log, "client connected:[%s]-[%d]- %s", inet_ntop(AF_INET, &client.sin_addr.s_addr, ip, sizeof(ip)), ntohs(client.sin_port), ctime(&t));
	threadPoolAdd(pool, writeLog, log);
	return;
}