#include <stdio.h>
#include <unistd.h>
#include "server.h"
#include <stdlib.h>
#include "threadpool.h"
#include <mysql/mysql.h>

ThreadPool* pool;
pthread_mutex_t mutexLog;
pthread_mutex_t mutexSql;
MYSQL* mysql;

int main(int argc, char* argv[])
{
    if (argc < 3) {
        printf("./a.out port path\n");  //让用户输入
        return -1;
    }
    unsigned short port = atoi(argv[1]);

    //把服务器进程的工作目录切换到资源目录
    chdir(argv[2]);

    //创建线程池
    pool = threadPoolCreate(3, 20, 1024);
    //创建服务器线程锁
    if (pthread_mutex_init(&mutexLog, NULL) != 0 ||
        pthread_mutex_init(&mutexSql, NULL) != 0) {
        printf("server mutex init error\n");
        return -1;
    } 

    //初始化数据库连接
    mysql = mysql_init(NULL);
    if (mysql == NULL) {
        perror("database error");
    }
    //连接数据库
    if (!mysql_real_connect(mysql, "127.0.0.1", "dwx", "123456", "webserver", 3306, NULL, 0)) {
        printf("database connect error: %s\n", mysql_error(mysql));
        mysql_close(mysql);
        return -1;
    }

    //初始化监听套接字
    int lfd = initListenFd(port);

    //启动服务器程序
    epollRun(lfd);

    //线程池关闭后工作线程有可能没执行完，主线程等待一段时间再销毁；
    for (int i = 1; i <= 10; ++i) {
        printf("%d\n", i);
        sleep(1);
    }
    int n = threadPoolDestroy(pool);
    if (n == -1) {
        printf("threadPool destroy fail...\n");
    }

    //关闭数据库连接
    mysql_close(mysql);

    return 0;
}