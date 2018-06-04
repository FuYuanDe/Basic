#include <stdio.h>
#include <mqueue.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>



void main () {
    char *file = "/posix";
    mqd_t msgq_id;
    int count = 0;
    char i[5] = "aa";
    char p[30] = "/posix";
    char *pp;
    /*mq_open() for creating a new queue (using default attributes) */
    /*mq_open() 创建一个新的 POSIX 消息队列或打开一个存在的队列*/

#if 0
    while (count < 1) {
        i[1]++;
        pp = strcat(p,i);
        if (pp) {
            count++;
            msgq_id = mq_open(pp, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG, NULL);
            if(msgq_id == (mqd_t)-1) {
                printf("create %s mq fail \n", pp);
                return ;
            }else {
                printf("create %s mq success \n", pp);
            }
        }
    }
    close(msgq_id);
#endif    
#if 0
    pp = strcat(p,i);

    msgq_id = mq_open(pp, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG, NULL);
    if(msgq_id == (mqd_t)-1) {
                printf("create %s mq fail \n", pp);
            return;
            }else {
                printf("create %s mq success \n", pp);
                close(msgq_id);
            }
    #endif            
    i[1]++;    
    pp = strcat(p,i);
    if (pp) {    
        msgq_id = mq_open(pp, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG, NULL);
        if(msgq_id == (mqd_t)-1) {
            printf("create %s mq fail \n", pp);
            return ;
        }else {
            printf("create %s mq success \n", pp);
        }
    }
    close(msgq_id);
    mq_unlink(pp);   
    return;

}



