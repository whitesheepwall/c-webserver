#pragma once

//��ʼ�������׽���
int initListenFd(unsigned short port);

//����epoll
int epollRun(int lfd);

//�Ϳͻ��˽�������
int acceptClient(int fd, int epfd);
//void acceptClient(void* arg);

//����http����
//int recvHttpRequest(int fd, int epfd);
void recvHttpRequest(void* arg);

//����������
int parseRequestLine(const char* head, const char* line, int cfd);

//����get����
int parseGet(char* path, int cfd);

//����post����
int parsePost(char* path, char* line, int cfd);

//����get����
int parseHead(char* path, int cfd);

//��¼
int login(char* pt, int cfd);

//ע��
int signup(char* pt, int cfd);

//�����ļ�
int sendFile(const char* fileName, int cfd);

//������Ӧͷ��״̬��+��Ӧͷ��
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length);

//������Ӧͷ����
const char* getFileType(const char* name);

//����Ŀ¼
int sendDir(const char* dirName, int cfd);

//�����Դ������������
//�ַ�ת������
int hexToDec(char c);

//to�洢���������ݣ�����������from�Ǳ���������ݣ��������
void decodeMsg(char* to, char* from);

//��¼��־
void writeLog(void* arg);
