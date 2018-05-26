/*
 *  UDP套接字编程获取源地址和目的地址
 */

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/errno.h>
#include <sys/uio.h> 
#include <stdint.h>  
#include <stdlib.h>
#include <unistd.h>  
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "udptest.h"


void main() {
    int on = 1;
    int ret, fd, addr_len, recv_len;
    
	char buffer[512] = {0};        /* 接收缓存 */
	struct msghdr msg;
	struct iovec iov[1];    
	struct sockaddr_in saddr; /* 来源地址 */
	struct sockaddr_in daddr; /* 本地地址 */	
	struct cmsghdr *cmhp;           
    struct sockaddr_in server_addr;
	struct in_pktinfo *pktinfo = NULL;	 /* 用于指向获取的本地地址信息 */	
	char buff[CMSG_SPACE(sizeof(struct in_pktinfo) + CMSG_SPACE(sizeof(int)))] = {0};   /* 控制信息 */
	struct cmsghdr *cmh = (struct cmsghdr *)buff;   /* 控制信息 */
    addr_len = sizeof(struct sockaddr_in);

    memset(&buffer, 0, sizeof(buffer));
    memset(&msg, 0, sizeof(struct msghdr));
    memset(&iov, 0, sizeof(struct iovec));
    memset(&saddr, 0, sizeof(struct sockaddr_in));
    memset(&daddr, 0, sizeof(struct sockaddr_in)); 

    // 创建UDP套接字
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        log("create socket fail \r\n");
        return ;
    }    

    // 设置监听地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; 
    server_addr.sin_port = htons(3500);

    // 设置SO_REUSEADDR属性, 地址复用    
    if ((ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on))) != 0) {
        log("setsockopt reuseaddr fail, ret : %d,error : %d \r\n", ret, errno);
        close(fd);
        return ;
    }

    /* 设置IP_PKTINFO属性 */
    if (0 != setsockopt(fd, IPPROTO_IP, IP_PKTINFO, (char *)&on, sizeof(on))) {
        log("setsockopt ip_pktinfo fail, errno : %d \r\n", errno);
        close(fd);
        return ;  
    }    

    // 绑定本地监听地址
    if (0 != bind(fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in))) {
        printf("bind local listening addr fail，errno : %d \r\n", errno);
        close(fd);
        return ;
    }    

    // 接收信息
     while(1) {
         msg.msg_name = &saddr;     // 存储报文来源地址
         msg.msg_namelen = addr_len;
         msg.msg_iov = &iov[0];
         msg.msg_iovlen = 1;
         msg.msg_control = cmh;
         msg.msg_controllen = sizeof(buff);    
         iov[0].iov_base = &buffer;
         iov[0].iov_len = sizeof(buffer);                     
    
         // 超时退出
         recv_len = recvmsg(fd, &msg, 0);
         if (recv_len > 0)
         {
             /* 辅助信息 */
             msg.msg_control = cmh;
             msg.msg_controllen = sizeof(buff);
             for (cmhp = CMSG_FIRSTHDR(&msg); cmhp; cmhp = CMSG_NXTHDR(&msg, cmhp)) {
                 if (cmhp->cmsg_level == IPPROTO_IP) {
                     if (cmhp->cmsg_type == IP_PKTINFO) {
                         pktinfo = (struct in_pktinfo *)CMSG_DATA(cmhp);
                         daddr.sin_family = AF_INET;
                         daddr.sin_addr = pktinfo->ipi_addr;
                         log("saddr : %u:%u:%u:%u \r\n", NIPQUAD(saddr.sin_addr));
                         log("daddr : %u:%u:%u:%u \r\n", NIPQUAD(daddr.sin_addr));                        
                     }
                 }
             }

         }
         memset(buffer, 0, sizeof(buffer));
         memset(buff, 0, sizeof(buff));
         memset(&saddr, 0, sizeof(struct sockaddr_in));
         memset(&daddr, 0, sizeof(struct sockaddr_in));        
     }

    return ;
}

