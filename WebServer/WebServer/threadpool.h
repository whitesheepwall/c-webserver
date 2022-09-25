#pragma once
#include <pthread.h>

//任务结构体
typedef struct Task
{
	void (*function)(void* arg);  //任务处理函数
	void* arg;                    //函数参数
}Task;

typedef struct ThreadPool
{
	//线程池里包含一个任务队列，队列中每个成员都是Task
	//这里放一个Task指针指向任务队列
	Task* taskQ;       //任务队列指针
	int queueCapacity; //任务队列容量
	int queueSize;     //当前任务个数
	int queueFront;    //队头
	int queueRear;     //队尾

	pthread_t managerID;   //管理者线程ID
	pthread_t* threadIDs;  //工作线程ID，指针指向一个工作线程数组
	int minNum;    //最小线程数
	int maxNum;    //最大线程数
	int busyNum;   //当前忙碌线程
	int liveNum;   //当前存活线程数
	int exitNum;   //需要杀死的线程数
	pthread_mutex_t mutexPool;  //互斥锁，锁整个线程池,主要用于锁任务队列
	pthread_mutex_t mutexBusy;  //单独给busyNum加一个锁
	pthread_cond_t notFull;     //条件变量，判断任务队列是不是满了
	pthread_cond_t notEmpty;    //条件变量，判断任务队列是不是空了

	int shutdown;   //是不是要销毁线程池，销毁为1，不销毁为0

}ThreadPool;


//创建线程池,需要传递：任务队列容量，最大最小线程数
ThreadPool* threadPoolCreate(int minThread, int maxThread, int queueSize);

//销毁线程池
int threadPoolDestroy(ThreadPool* pool);

//添加任务
void threadPoolAdd(ThreadPool* pool, void(*func)(void*), void* arg);

//获取线程池中工作线程个数
int threadPoolBusyNum(ThreadPool* pool);

//获取线程池中活着的线程个数
int threadPoolLiveNum(ThreadPool* pool);

//工作函数
void* worker(void* arg);
//worker执行线程池里的任务队列里的任务
//而任务里有回调函数和回调函数的参数

//管理函数
void* manager(void* arg);

void threadExit(ThreadPool* pool);
