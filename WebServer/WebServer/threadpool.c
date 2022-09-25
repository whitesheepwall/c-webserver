#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const int THREAD_ADD_NUM = 2;

ThreadPool* threadPoolCreate(int minThread, int maxThread, int queueSize)
{
	//1.����ѿռ��ʼ���̳߳�
	ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
	
	do {  //do while����break�˳���ѭ������������ɹ�����ֱ�ӷ����̳߳�
		if (pool == NULL) {
			printf("malloc threadpool fail...\n");
			break;
		}

		//2.��ʼ�������߳�����
		pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * maxThread);
		if (pool->threadIDs == NULL) {
			printf("malloc threadIDs fail...\n");
			break;
		}
		memset(pool->threadIDs, 0, sizeof(pool->threadIDs));

		//3.��ʼ���̳߳�����
		pool->minNum = minThread;
		pool->maxNum = maxThread;
		pool->busyNum = 0;
		pool->liveNum = minThread;  //�ʼֻ��ʼ����С�������߳�
		pool->exitNum = 0;
		pool->shutdown = 0;  //�������̳߳�

		//4.��ʼ����,����ֵΪ0˵����ʼ���ɹ�
		if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 ||
			pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
			pthread_cond_init(&pool->notEmpty, NULL) != 0 ||
			pthread_cond_init(&pool->notFull, NULL) != 0)
		{
			printf("mutex or condition init fail...\n");
			break;
		}

		//5.��ʼ���������
		pool->taskQ = (Task*)malloc(sizeof(Task) * queueSize);
		pool->queueCapacity = queueSize;
		pool->queueSize = 0;  //��ǰ������
		pool->queueFront = 0; //��ͷָ��
		pool->queueRear = 0;  //��βָ��

		//6.�����������߳�
		pthread_create(&pool->managerID, NULL, manager, pool);

		//7.��ʼ�������߳�
		for (int i = 0; i < minThread; ++i) {
			pthread_create(&pool->threadIDs[i], NULL, worker, pool);
		}

		return pool;

	} while (0);

	//����ʧ���ͷ���Դ
	if (pool && pool->threadIDs) {
		free(pool->threadIDs);
		pool->threadIDs = NULL;
	}
	if (pool && pool->taskQ) {
		free(pool->taskQ);
		pool->taskQ = NULL;
	}
	if (pool) {
		free(pool);
		pool = NULL;
	}

	return NULL;
}

int threadPoolDestroy(ThreadPool* pool)
{
	if(pool==NULL)
		return -1;

	//�ر��̳߳�
	pool->shutdown = 1;

	//���������Ĺ����߳�
	for (int i = 0; i < pool->liveNum; ++i)
	{
		pthread_cond_signal(&pool->notEmpty);
	}
	//ȷ�����߳�ȫ���˳�
	for (int i = 0; i < pool->liveNum; ++i) {
		if (pool->threadIDs[i] != 0)
			pthread_join(pool->threadIDs[i], NULL);
	}

	//�������չ������߳�
	pthread_join(pool->managerID, NULL);

	//�ͷŶ��ڴ�
	if (pool->taskQ) {
		free(pool->taskQ);
		pool->taskQ = NULL;
	}
	if (pool->threadIDs) {
		free(pool->threadIDs);
		pool->threadIDs = NULL;
	}

	pthread_mutex_destroy(&pool->mutexPool);
	pthread_mutex_destroy(&pool->mutexBusy);
	pthread_cond_destroy(&pool->notEmpty);
	pthread_cond_destroy(&pool->notFull);

	free(pool);
	pool = NULL;

	return 0;
}

void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg)
{
	pthread_mutex_lock(&pool->mutexPool);
	while (pool->queueSize == pool->queueCapacity && !pool->shutdown)
	{
		//�����������߳�
		pthread_cond_wait(&pool->notFull, &pool->mutexPool);
	}
	if (pool->shutdown) {
		pthread_mutex_unlock(&pool->mutexPool);
		return;
	}
	//�������
	pool->taskQ[pool->queueRear].function = func;
	pool->taskQ[pool->queueRear].arg = arg;
	pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
	pool->queueSize++;

	//���źŸ��߹����߳���������
	pthread_cond_signal(&pool->notEmpty);
	pthread_mutex_unlock(&pool->mutexPool);
}

int threadPoolBusyNum(ThreadPool* pool)
{
	pthread_mutex_lock(&pool->mutexBusy);
	int busyNum = pool->busyNum;
	pthread_mutex_unlock(&pool->mutexBusy);
	return busyNum;
}

int threadPoolLiveNum(ThreadPool* pool)
{
	pthread_mutex_lock(&pool->mutexPool);
	int liveNum = pool->liveNum;
	pthread_mutex_unlock(&pool->mutexPool);
	return liveNum;
}

void* worker(void* arg)
{
	ThreadPool* pool = (ThreadPool*)arg;

	//��Ҫ��ͣ�Ĵ����������ȡ������������ѭ��
	while (1)
	{
		//�̳߳��ǹ�����Դ����Ҫ����
		pthread_mutex_lock(&pool->mutexPool);

		//��ǰ��������Ƿ�Ϊ��
		while (pool->queueSize == 0 && !pool->shutdown) {
			//���������߳�,�յ�notEmpty�źŻ��������߳�
			pthread_cond_wait(&pool->notEmpty, &pool->mutexPool);
		
			//�ж��ǲ���Ҫ�����߳�
			if (pool->exitNum > 0) {
				pool->exitNum--;

				//������ŵ��߳���������С�߳����Ϳ�������
				if (pool->liveNum > pool->minNum) {
					pool->liveNum--;
					pthread_mutex_unlock(&pool->mutexPool);
					threadExit(pool);
				}
			}
		}

		if (pool->shutdown) { //����̳߳ر��ر��ˣ��Ƚ������˳����߳�
			pthread_mutex_unlock(&pool->mutexPool);
			threadExit(pool);
		}

		//�����������ȡ��һ������
		Task task;
		task.function = pool->taskQ[pool->queueFront].function;
		task.arg = pool->taskQ[pool->queueFront].arg;
		//�ƶ���ͷָ��
		pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
		pool->queueSize--;
		
		//ȡ������Ϳ��Խ�����
		//���źŸ����������п�λ��
		pthread_cond_signal(&pool->notFull);
		pthread_mutex_unlock(&pool->mutexPool);

		//��busyNum����
		pthread_mutex_lock(&pool->mutexBusy);
		pool->busyNum++;  //æµ�߳���+1
		//����
		pthread_mutex_unlock(&pool->mutexBusy);

		//ִ��������
		//printf("thread %ld start working...\n",pthread_self());
		task.function(task.arg);
		//printf("thread %ld end working...\n",pthread_self());
		if (task.arg) {  //ִ����ɺ��ͷ��ڴ�
			free(task.arg);
			task.arg = NULL;
		}	

		//ִ����������Ҫ��æµ�߳���-1
		//��busyNum����
		pthread_mutex_lock(&pool->mutexBusy);
		pool->busyNum--;  //æµ�߳���+1
		//����
		pthread_mutex_unlock(&pool->mutexBusy);
	}
	return NULL;
}

void* manager(void* arg)
{
	ThreadPool* pool = (ThreadPool*)arg;
	while (!pool->shutdown) 
	{
		sleep(5);//ÿ��5����һ��

		//ȡ���̳߳�������������͵�ǰ�̵߳�����
		pthread_mutex_lock(&pool->mutexPool);
		int queueSize = pool->queueSize;
		int liveNum = pool->liveNum;
		pthread_mutex_unlock(&pool->mutexPool);

		//ȡ��æ���̵߳�����
		pthread_mutex_lock(&pool->mutexBusy);
		int busyNum = pool->busyNum;
		pthread_mutex_unlock(&pool->mutexBusy);

		//����߳�
		//����ĸ��� > �����߳��� && �����߳��� < ����߳���
		if (queueSize > liveNum && liveNum < pool->maxNum)
		{
			//��������liveNum
			pthread_mutex_lock(&pool->mutexPool);
			int counter = 0;
			for (int i = 0; i < pool->maxNum && counter < THREAD_ADD_NUM && pool->liveNum < pool->maxNum; ++i) 
			{	//��0��ʼ��Ϊ�˴��߳���������һ�����е�λ��
				if (pool->threadIDs[i] == 0) {
					pthread_create(&pool->threadIDs[i], NULL, worker, pool);
					++counter;
					pool->liveNum++;
				}
			}
			pthread_mutex_unlock(&pool->mutexPool);
		}

		//�����߳�
		//æ���߳�*2 < �����߳��� && �����߳� > ��С�߳���
		if (busyNum * 2 < liveNum && liveNum > pool->minNum)
		{
			pthread_mutex_lock(&pool->mutexPool);
			pool->exitNum = THREAD_ADD_NUM;
			pthread_mutex_unlock(&pool->mutexPool);
			//�ù������߳���ɱ
			for (int i = 0; i < THREAD_ADD_NUM; ++i) {
				pthread_cond_signal(&pool->notEmpty);
			}
		}
	}

	return NULL;
}

void threadExit(ThreadPool* pool)
{
	pthread_t tid = pthread_self(); //��ȡ�߳�id
	for (int i = 0; i < pool->maxNum; ++i) {
		if (pool->threadIDs[i] == tid) {
			pool->threadIDs[i] = 0;
			printf("threadExit() called,%ld exiting...\n", tid);
			break;
		}
	}
	pthread_exit(NULL);
}
