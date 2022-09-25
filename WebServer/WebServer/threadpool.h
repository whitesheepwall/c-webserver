#pragma once
#include <pthread.h>

//����ṹ��
typedef struct Task
{
	void (*function)(void* arg);  //��������
	void* arg;                    //��������
}Task;

typedef struct ThreadPool
{
	//�̳߳������һ��������У�������ÿ����Ա����Task
	//�����һ��Taskָ��ָ���������
	Task* taskQ;       //�������ָ��
	int queueCapacity; //�����������
	int queueSize;     //��ǰ�������
	int queueFront;    //��ͷ
	int queueRear;     //��β

	pthread_t managerID;   //�������߳�ID
	pthread_t* threadIDs;  //�����߳�ID��ָ��ָ��һ�������߳�����
	int minNum;    //��С�߳���
	int maxNum;    //����߳���
	int busyNum;   //��ǰæµ�߳�
	int liveNum;   //��ǰ����߳���
	int exitNum;   //��Ҫɱ�����߳���
	pthread_mutex_t mutexPool;  //���������������̳߳�,��Ҫ�������������
	pthread_mutex_t mutexBusy;  //������busyNum��һ����
	pthread_cond_t notFull;     //�����������ж���������ǲ�������
	pthread_cond_t notEmpty;    //�����������ж���������ǲ��ǿ���

	int shutdown;   //�ǲ���Ҫ�����̳߳أ�����Ϊ1��������Ϊ0

}ThreadPool;


//�����̳߳�,��Ҫ���ݣ�������������������С�߳���
ThreadPool* threadPoolCreate(int minThread, int maxThread, int queueSize);

//�����̳߳�
int threadPoolDestroy(ThreadPool* pool);

//�������
void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg);

//��ȡ�̳߳��й����̸߳���
int threadPoolBusyNum(ThreadPool* pool);

//��ȡ�̳߳��л��ŵ��̸߳���
int threadPoolLiveNum(ThreadPool* pool);

//��������
void* worker(void* arg);
//workerִ���̳߳������������������
//���������лص������ͻص������Ĳ���

//������
void* manager(void* arg);

void threadExit(ThreadPool* pool);
