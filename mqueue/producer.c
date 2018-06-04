/*

 *
 *       Filename:  producer.c
 *
 *    Description:  生产者进程
 *
 *        Version:  1.0
 *        Created:  09/30/2011 04:52:23 PM
 *       Revision:  none
 *       Compiler:  gcc(g++)
 *
 *         Author:  |Zhenghe Zhang|, |zhenghe.zhang@gmail.com|
 *        Company:  |Shenzhen XXX Technology Co., Ltd.|
 *
 */

#include <stdio.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define MAXSIZE     10   //定义buf大小
#define BUFFER      8192 //定义Msg大小

struct MsgType{
    int len;
    char buf[MAXSIZE];
    char x;
    short y;
};

int main() 
{ 
    /*消息队列*/
    mqd_t msgq_id;
    struct MsgType msg;

    unsigned int prio = 1;
    unsigned int send_size = BUFFER;

    struct mq_attr msgq_attr;
    const char *file = "/abc";

    /*mq_open() for creating a new queue (using default attributes) */
    /*mq_open() 创建一个新的 POSIX 消息队列或打开一个存在的队列*/
    msgq_id = mq_open(file, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG, NULL);
    if(msgq_id == (mqd_t)-1) {
        printf("create %s mq fail \n", file);
        return ;
    }else {
        printf("create mq success \n");
    }
    
    #if 0
    msgq_id = mq_open(file, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG, NULL);
    if(msgq_id == (mqd_t)-1)
    {
        perror("mq_open");
        exit(1);
    }
    #endif
    /* getting the attributes from the queue        --  mq_getattr() */
    #if 0
    if(0 && mq_getattr(msgq_id, &msgq_attr) == -1)
    {
        perror("mq_getattr");
        exit(1);
    }

    printf("Queue \"%s\":\n\t- stores at most %ld messages\n\t- \
        large at most %ld bytes each\n\t- currently holds %ld messages\n", 
        file, msgq_attr.mq_maxmsg, msgq_attr.mq_msgsize, msgq_attr.mq_curmsgs);
   #endif
    /*setting the attributes of the queue        --  mq_setattr() */
    /*mq_setattr() 设置消息队列的属性，设置时使用由 newattr 指针指向的 mq_attr 结构的信息。*/
    /*属性中只有标志 mq_flasgs 里的 O_NONBLOCK 标志可以更改，其它在 newattr 内的域都被忽略 */
    #if 0
    if(0 && mq_setattr(msgq_id, &msgq_attr, NULL) == -1)
    {
        perror("mq_setattr");
        exit(1);
    }   
    #endif
    int i = 0; 
    while(i < 10) 
    {
        msg.len = i;
        memset(msg.buf, 0, MAXSIZE);
        sprintf(msg.buf, "0x%x", i);
        msg.x = (char)(i + 'a');
        msg.y = (short)(i + 100);

        printf("msg.len = %d, msg.buf = %s, msg.x = %c, msg.y = %d\n", msg.len, msg.buf, msg.x, msg.y);

        /*sending the message      --  mq_send() */
        /*mq_send() 把 msg_ptr 指向的消息加入由 mqdes 引用的消息队列里。*/
        /*参数 msg_len 指定消息 msg_ptr 的长度：这个长度必须小于或等于队列 mq_msgsize 属性的值。零长度的消息是允许。*/
        if(mq_send(msgq_id, (char*)&msg, sizeof(struct MsgType), prio) == -1)
        {
            perror("mq_send");
            exit(1);
        }

        i++;
        sleep(1);   
    }

    sleep(30); //等待消费者进程退出

    /*closing the queue        -- mq_close() */
    /*mq_close() 关闭消息队列描述符 mqdes。如果调用进程在消息队列 mqdes 绑定了通知请求，*/
    /*那么这个请求被删除，此后其它进程就可以绑定通知请求到此消息队列。*/
    if(mq_close(msgq_id) == -1)
    {
        perror("mq_close");
        exit(1);
    }

    /*mq_unlink() 删除名为 name 的消息队列。消息队列名将被直接删除。*/
    /*消息队列本身在所有引用这个队列的描述符被关闭时销毁。*/
    if(mq_unlink(file) == -1)
    {
        perror("mq_unlink");
        exit(1);
    }

    return 0; 
}

 
