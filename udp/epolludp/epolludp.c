/*
 * 使用epoll监听套接字。
 *
 */
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/errno.h>
#include <pthread.h>
#include <sys/uio.h>    
#include <stdint.h>     
#include <malloc.h>
#include <unistd.h>     
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define log(fmt, arg...)   \
    printf("[bfd] %s:%d "fmt,__FUNCTION__,__LINE__,##arg)



int main() {
	int err = 0;
    int on = 1;
    int len;
    int sfd;    // 套接字
    int pfd;
    int efd;    // epoll文件描述符，    
    int fds;
    int addrLen;
    char buffer[512] = {0};
    struct epoll_event g_event;  // epoll事件
    struct epoll_event *g_events; 
    struct sockaddr_in localAddr;
    struct sockaddr_in remoteAddr;
    
    localAddr.sin_addr.s_addr = htonl(0);
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(8080);
    addrLen = sizeof(struct sockaddr_in);
    // 创建发送套接字 
	if ((sfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        log("[bfd] %s:%d Error creating control socket. ret = %d, errno : %d",__FUNCTION__,__LINE__, err, errno);
        return -1;
	}

    // 设置SO_REUSEADDR属性
    if((err = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on))) != 0) {
        log("[bfd] %s:%d Error setsockopt reuseaddr. ret = %d, errno : %d ",__FUNCTION__,__LINE__, err, errno);
        close(sfd);
        return -1;
    }
    
    // 绑定本地地址和端口
    if((err = bind(sfd, (struct sockaddr *)&(localAddr), sizeof(struct sockaddr_in))) != 0)
    {
        close(sfd);
        return -1;
    }    

    efd = epoll_create1(0);
    if (efd == -1) {
        log("[bfd] %s:%d create epoll fail ",__FUNCTION__, __LINE__);
        return -1;
    }
    log("[bfd] %s:%d create epoll success ",__FUNCTION__, __LINE__);    

    g_events = (struct epoll_event *)calloc(1, sizeof(struct epoll_event));
    if (g_events == NULL) {
        log("[bfd] %s:%d calloc fail ",__FUNCTION__, __LINE__);
        close(efd);
        close(sfd);
        return -1;
    }
    //添加到epoll检测变量中
    g_event.data.fd = sfd; 
    g_event.events = EPOLLIN;
    epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &g_event);
    while(1) {
        memset(buffer, 0, sizeof(buffer));
        fds = epoll_wait(efd, g_events, 1, -1);
        pfd = g_events[0].data.fd;
        if (g_events[0].events & EPOLLIN) {   
        len = recvfrom(pfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&remoteAddr, &addrLen);    
        if (len >0)
            log("recvive len : %d, %s \r\n", len, buffer);
        }            
    }

    
    
    
}

