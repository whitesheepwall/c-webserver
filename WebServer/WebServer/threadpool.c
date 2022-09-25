#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const int THREAD_ADD_NUM = 2;

ThreadPool* threadPoolCreate(int minThread, int maxThread, int queueSize)
{
	//1.申请堆空间初始化线程池
	ThreadPool* pool = (ThreadPool*)malloc(sizeof(ThreadPool));
	
	do {  //do while可用break退出，循环体里如果都成功，则直接返回线程池
		if (pool == NULL) {
			printf("malloc threadpool fail...\n");
			break;
		}

		//2.初始化工作线程数组
		pool->threadIDs = (pthread_t*)malloc(sizeof(pthread_t) * maxThread);
		if (pool->threadIDs == NULL) {
			printf("malloc threadIDs fail...\n");
			break;
		}
		memset(pool->threadIDs, 0, sizeof(pool->threadIDs));

		//3.初始化线程池属性
		pool->minNum = minThread;
		pool->maxNum = maxThread;
		pool->busyNum = 0;
		pool->liveNum = minThread;  //最开始只初始化最小数量个线程
		pool->exitNum = 0;
		pool->shutdown = 0;  //不销毁线程池

		//4.初始化锁,返回值为0说明初始化成功
		if (pthread_mutex_init(&pool->mutexPool, NULL) != 0 ||
			pthread_mutex_init(&pool->mutexBusy, NULL) != 0 ||
			pthread_cond_init(&pool->notEmpty, NULL) != 0 ||
			pthread_cond_init(&pool->notFull, NULL) != 0)
		{
			printf("mutex or condition init fail...\n");
			break;
		}

		//5.初始化任务队列
		pool->taskQ = (Task*)malloc(sizeof(Task) * queueSize);
		pool->queueCapacity = queueSize;
		pool->queueSize = 0;  //当前任务数
		pool->queueFront = 0; //队头指针
		pool->queueRear = 0;  //队尾指针

		//6.创建管理者线程
		pthread_create(&pool->managerID, NULL, manager, pool);

		//7.初始化工作线程
		for (int i = 0; i < minThread; ++i) {
			pthread_create(&pool->threadIDs[i], NULL, worker, pool);
		}

		return pool;

	} while (0);

	//创建失败释放资源
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

	//关闭线程池
	pool->shutdown = 1;

	//唤醒阻塞的工作线程
	for (int i = 0; i < pool->liveNum; ++i)
	{
		pthread_cond_signal(&pool->notEmpty);
	}
	//确保子线程全部退出
	for (int i = 0; i < pool->liveNum; ++i) {
		if (pool->threadIDs[i] != 0)
			pthread_join(pool->threadIDs[i], NULL);
	}

	//阻塞回收管理者线程
	pthread_join(pool->managerID, NULL);

	//释放堆内存
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
		//阻塞生产者线程
		pthread_cond_wait(&pool->notFull, &pool->mutexPool);
	}
	if (pool->shutdown) {
		pthread_mutex_unlock(&pool->mutexPool);
		return;
	}
	//添加任务
	pool->taskQ[pool->queueRear].function = func;
	pool->taskQ[pool->queueRear].arg = arg;
	pool->queueRear = (pool->queueRear + 1) % pool->queueCapacity;
	pool->queueSize++;

	//发信号告诉工作线程有任务了
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

	//需要不停的从任务队列里取任务，所以无限循环
	while (1)
	{
		//线程池是共享资源，需要加锁
		pthread_mutex_lock(&pool->mutexPool);

		//当前任务队列是否为空
		while (pool->queueSize == 0 && !pool->shutdown) {
			//阻塞工作线程,收到notEmpty信号唤醒阻塞线程
			pthread_cond_wait(&pool->notEmpty, &pool->mutexPool);
		
			//判断是不是要销毁线程
			if (pool->exitNum > 0) {
				pool->exitNum--;

				//如果活着的线程数大于最小线程数就可以销毁
				if (pool->liveNum > pool->minNum) {
					pool->liveNum--;
					pthread_mutex_unlock(&pool->mutexPool);
					threadExit(pool);
				}
			}
		}

		if (pool->shutdown) { //如果线程池被关闭了，先解锁再退出子线程
			pthread_mutex_unlock(&pool->mutexPool);
			threadExit(pool);
		}

		//从任务队列中取出一个任务
		Task task;
		task.function = pool->taskQ[pool->queueFront].function;
		task.arg = pool->taskQ[pool->queueFront].arg;
		//移动队头指针
		pool->queueFront = (pool->queueFront + 1) % pool->queueCapacity;
		pool->queueSize--;
		
		//取完任务就可以解锁了
		//发信号告诉生产者有空位了
		pthread_cond_signal(&pool->notFull);
		pthread_mutex_unlock(&pool->mutexPool);

		//给busyNum加锁
		pthread_mutex_lock(&pool->mutexBusy);
		pool->busyNum++;  //忙碌线程数+1
		//解锁
		pthread_mutex_unlock(&pool->mutexBusy);

		//执行任务函数
		//printf("thread %ld start working...\n",pthread_self());
		task.function(task.arg);
		//printf("thread %ld end working...\n",pthread_self());
		if (task.arg) {  //执行完成后释放内存
			free(task.arg);
			task.arg = NULL;
		}	

		//执行完任务，需要把忙碌线程数-1
		//给busyNum加锁
		pthread_mutex_lock(&pool->mutexBusy);
		pool->busyNum--;  //忙碌线程数+1
		//解锁
		pthread_mutex_unlock(&pool->mutexBusy);
	}
	return NULL;
}

void* manager(void* arg)
{
	ThreadPool* pool = (ThreadPool*)arg;
	while (!pool->shutdown) 
	{
		sleep(5);//每隔5秒检测一次

		//取出线程池中任务的数量和当前线程的数量
		pthread_mutex_lock(&pool->mutexPool);
		int queueSize = pool->queueSize;
		int liveNum = pool->liveNum;
		pthread_mutex_unlock(&pool->mutexPool);

		//取出忙的线程的数量
		pthread_mutex_lock(&pool->mutexBusy);
		int busyNum = pool->busyNum;
		pthread_mutex_unlock(&pool->mutexBusy);

		//添加线程
		//任务的个数 > 存活的线程数 && 存活的线程数 < 最大线程数
		if (queueSize > liveNum && liveNum < pool->maxNum)
		{
			//加锁保护liveNum
			pthread_mutex_lock(&pool->mutexPool);
			int counter = 0;
			for (int i = 0; i < pool->maxNum && counter < THREAD_ADD_NUM && pool->liveNum < pool->maxNum; ++i) 
			{	//从0开始是为了从线程数组里找一个空闲的位置
				if (pool->threadIDs[i] == 0) {
					pthread_create(&pool->threadIDs[i], NULL, worker, pool);
					++counter;
					pool->liveNum++;
				}
			}
			pthread_mutex_unlock(&pool->mutexPool);
		}

		//销毁线程
		//忙的线程*2 < 存活的线程数 && 存活的线程 > 最小线程数
		if (busyNum * 2 < liveNum && liveNum > pool->minNum)
		{
			pthread_mutex_lock(&pool->mutexPool);
			pool->exitNum = THREAD_ADD_NUM;
			pthread_mutex_unlock(&pool->mutexPool);
			//让工作的线程自杀
			for (int i = 0; i < THREAD_ADD_NUM; ++i) {
				pthread_cond_signal(&pool->notEmpty);
			}
		}
	}

	return NULL;
}

void threadExit(ThreadPool* pool)
{
	pthread_t tid = pthread_self(); //获取线程id
	for (int i = 0; i < pool->maxNum; ++i) {
		if (pool->threadIDs[i] == tid) {
			pool->threadIDs[i] = 0;
			printf("threadExit() called,%ld exiting...\n", tid);
			break;
		}
	}
	pthread_exit(NULL);
}
