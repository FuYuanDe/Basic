#include "bfdd.h"


// 全局callback函数指针
CALLBACK_FUNC callbackSendMsg;

static pthread_rwlock_t bfd_rwlock;          // 线程读写
static pthread_rwlockattr_t bfd_rwlock_attr; // 读写锁属性，写者优

struct bfd_master master;   // 会话
static int bfd_debug_enable = 1;
pthread_t  bfd_rx_thread;                    // 接收报文线程
pthread_t  bfd_timing_thread;                // bfd定时器管理线

int bfd_rx_sock;                             // 接收套接
static struct sockaddr_in server_addr;       // 监听本地bfd地址

int efd;    // epoll文件描述符，    
struct epoll_event g_event;  // epoll事件
struct epoll_event *g_events; 


// 获取hash   key
int hash_key(unsigned int my_disc, unsigned int daddr) {
    if (my_disc != 0) 
        return my_disc % BFD_SESSION_HASH_SIZE;
    else 
        return daddr % BFD_SESSION_HASH_SIZE;
}


/* 随机生成 My_Disc */
unsigned int bfd_create_mydisc(void)
{
    time_t t;
    unsigned int disc = 0;
    srand((unsigned int)time(&t));    

    while(1)
    {
        disc = rand();
        if(disc != 0)
            break;
    }
    return disc;
}


/* 创建发送套接字 */
int bfd_create_ctrl_socket(struct session *bfd_session)
{
	int err = 0;
	int ttl = 255;  // time to live
    int on = 1;

    /* 创建发送套接字 */
	if ((bfd_session->tx_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		log_info("Error creating control socket. ret = %d, errno : %d \r\n", err, errno);
        return -1;
	}

    // 设置SO_REUSEADDR
    if((err = setsockopt(bfd_session->tx_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on))) != 0) {
        log_info("Error setsockopt reuseaddr. ret = %d, errno : %d \r\n", err, errno);
        close(bfd_session->tx_sock);
        return -1;
    }
    
    // 设置IP_TTL属性
    if((err = setsockopt(bfd_session->tx_sock, IPPROTO_IP, IP_TTL, (char *)&ttl, sizeof (int))) != 0) {
        log_info("Error setsockopt ip_ttl. ret = %d, errno : %d \r\n", err, errno);
        close(bfd_session->tx_sock);
        return -1;    
    }    

    /* 绑定地址 */
    if((err = bind(bfd_session->tx_sock, (struct sockaddr *)&(bfd_session->laddr), sizeof(struct sockaddr_in))) != 0)
    {
        log_info("Error bind rx_socket addr. ret = %d, errno : %d, addr : %u:%u:%u:%u:%hu \r\n", err, errno,
            NIPQUAD(bfd_session->laddr), ntohs(bfd_session->laddr.sin_port));
        close(bfd_session->tx_sock);
        return -1;
    } 
    
    return 0;
}


// 创建新会话
struct session * bfd_session_new(struct session_cfg *session_cfg)
{
    int ret;
	struct session *bfd_session;

    /* 判断协议类型 */
    if (session_cfg->local_ip_type != AF_INET || (session_cfg->remote_ip_type != AF_INET)) {     
        log_info("unsupport ip type, local_ip_type :%u, remote_ip_type : %u \r\n", session_cfg->local_ip_type, 
            session_cfg->remote_ip_type);
        return NULL;
    }

    /* 判断远端端口 */
    if (session_cfg->remote_port != BFD_LISTENING_PORT) {
        log_info("bfd remote port invalid, %hu, should be 3784 \r\n", session_cfg->remote_port);
        return NULL;
    }

	bfd_session = malloc(sizeof(struct session));
	if (bfd_session) {
		memset(bfd_session, 0, sizeof(struct session));
        bfd_session->session_next = NULL;
        bfd_session->neigh_next = NULL;

        bfd_session->laddr.sin_family = AF_INET;
        bfd_session->laddr.sin_addr.s_addr = session_cfg->local_ip.ip;  // 传入地址是网络字节序
        bfd_session->laddr.sin_port = htons(session_cfg->local_port);
        
        bfd_session->raddr.sin_family = AF_INET;
        bfd_session->raddr.sin_addr.s_addr = session_cfg->remote_ip.ip;// 传入地址是网络字节序
        bfd_session->raddr.sin_port = htons(session_cfg->remote_port);

        bfd_session->bfdh.version = BFD_VERSION;    // 版本
        bfd_session->bfdh.diag = BFD_DIAG_NO_DIAG;  // 诊断
        bfd_session->bfdh.sta = BFD_STA_DOWN;       // 状态码
        bfd_session->bfdh.poll = 0;
        bfd_session->bfdh.cplane = 0;
        bfd_session->bfdh.final = 0;
        bfd_session->bfdh.auth = 0;
        bfd_session->bfdh.demand = 0;
        bfd_session->bfdh.mpoint = 0;
        bfd_session->bfdh.detect_mult = session_cfg->detect_multi;
        bfd_session->bfdh.len = BFD_CTRL_LEN;

        bfd_session->bfdh.my_disc = htonl(bfd_create_mydisc());
        while(bfd_session_lookup(bfd_session->bfdh.my_disc, 0, 0)) {
            bfd_session->bfdh.my_disc = htonl(bfd_create_mydisc());
        }
        bfd_session->bfdh.your_disc = 0;
        bfd_session->bfdh.des_min_tx_intv = htonl(BFD_DEFAULT_TX_INTERVAL);
        bfd_session->bfdh.req_min_rx_intv = htonl(BFD_DEFAULT_RX_INTERVAL);
        bfd_session->bfdh.req_min_echo_rx_intv = 0;

        //后续处理注意大小端转
        bfd_session->des_min_tx_time = session_cfg->des_min_tx_interval;
        bfd_session->req_min_rx_time = session_cfg->req_min_rx_interval;
        bfd_session->req_min_rx_echo_time = session_cfg->req_min_echo_rx;
        
        bfd_session->act_rx_intv = BFD_DEFAULT_RX_INTERVAL;
        bfd_session->act_tx_intv = BFD_DEFAULT_TX_INTERVAL;

        strcpy(bfd_session->key, session_cfg->key);
        // 创建超时检测定时器
        bfd_session->rx_fd.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        bfd_session->rx_fd.bfd_session = bfd_session;
        bfd_session->rx_fd.flag = 0;        

        // 创建定时发送定时器
        bfd_session->tx_fd.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        bfd_session->tx_fd.bfd_session = bfd_session;
        bfd_session->tx_fd.flag = 1;                
        if ((bfd_session->tx_fd.fd == -1) || (bfd_session->rx_fd.fd == -1)) 
            log_info("create epoll fail�?\r\n");
                
        // 添加到epoll检测变量中
        g_event.data.ptr = &(bfd_session->rx_fd); 
        g_event.events = EPOLLIN;
        epoll_ctl(efd, EPOLL_CTL_ADD, bfd_session->rx_fd.fd, &g_event);          

        g_event.data.ptr = &(bfd_session->tx_fd); 
        g_event.events = EPOLLIN;
        epoll_ctl(efd, EPOLL_CTL_ADD, bfd_session->tx_fd.fd, &g_event);          

        /* 创建发送套接字 */
        ret = bfd_create_ctrl_socket(bfd_session);
        if (ret != 0) {
            epoll_ctl(efd, EPOLL_CTL_DEL, bfd_session->tx_fd.fd, NULL);          
            epoll_ctl(efd, EPOLL_CTL_DEL, bfd_session->rx_fd.fd, NULL);                      
            close(bfd_session->rx_fd.fd);
            close(bfd_session->tx_fd.fd);            
            close(bfd_session->tx_sock);
            free(bfd_session);            
            log_info("create bfd ctrl socket fail  \r\n");
            return NULL;
        }        
	}
	else {	
	    log_info("session malloc failed \r\n");
	    return NULL;
    }

	return bfd_session;
}


/* 添加会话 */
int bfd_session_add(struct session_cfg *cfg)
{
	int err = 0;
	int key;
	struct sockaddr_in addr;
	struct session *bfd_session = NULL;

	addr.sin_addr.s_addr = cfg->remote_ip.ip;
	
    // 判断是否已经与对端有BFD会话，通过源地址、目的地址来判
    bfd_session = bfd_session_lookup(0, cfg->remote_ip.ip, cfg->local_ip.ip);
    if (bfd_session) {
        log_info("session update. local_addr : %u:%u:%u:%u, peer_addr : %u:%u:%u:%u \r\n", 
        NIPQUAD(cfg->local_ip.ip), NIPQUAD(cfg->remote_ip.ip));
        bfd_session->bfdh.detect_mult = cfg->detect_multi;
        bfd_change_interval_time(bfd_session, cfg->des_min_tx_interval, cfg->req_min_rx_interval);
        return 0;
    }
    
    /* 创建新的会话 */
	bfd_session = bfd_session_new(cfg);
	if (!bfd_session) {
        log_info("create session fail. \r\n");
		return -1;
    }

	/* add session */
    pthread_rwlock_wrlock(&bfd_rwlock);
    key = hash_key(bfd_session->bfdh.my_disc, 0);
    bfd_session->session_next = master.session_tbl[key];
    master.session_tbl[key] = bfd_session;
    
    key = hash_key(0, bfd_session->raddr.sin_addr.s_addr);
    bfd_session->neigh_next = master.neigh_tbl[key];
    master.neigh_tbl[key] = bfd_session;
    pthread_rwlock_unlock(&bfd_rwlock);
    
	bfd_fsm_event(bfd_session, BFD_EVENT_START);

	return err;
}

// 删除bfd会话
void bfd_session_delete(struct session *bfd_session) {
    int i, ret = 0;
    unsigned int key;
    
    struct session *session_priv = NULL;
    struct session *session_cur = NULL;
    struct session *neigh_priv = NULL;
    struct session *neigh_cur = NULL;

    pthread_rwlock_wrlock(&bfd_rwlock);   
    key = hash_key(bfd_session->bfdh.my_disc, 0);
    session_cur = master.session_tbl[key];
    
    while(session_cur && session_cur->bfdh.my_disc != bfd_session->bfdh.my_disc) {
        session_priv = session_cur;
        session_cur = session_cur->session_next;
    }

    if (session_priv == NULL)
        master.session_tbl[key] = bfd_session->session_next;
    else 
        session_priv->session_next = bfd_session->session_next; 
    
    key = hash_key(0, bfd_session->raddr.sin_addr.s_addr);
    neigh_cur = master.neigh_tbl[key];
    while(neigh_cur && neigh_cur->bfdh.my_disc != bfd_session->bfdh.my_disc) {
        neigh_priv = neigh_cur;
        neigh_cur = neigh_cur->neigh_next;
    }
    if (neigh_priv == NULL)
        master.neigh_tbl[key] = bfd_session->session_next;
    else 
        neigh_priv->neigh_next = bfd_session->session_next;    
    pthread_rwlock_unlock(&bfd_rwlock);

    // 关掉定时
    bfd_stop_xmit_timer(bfd_session);
    bfd_stop_expire_timer(bfd_session);

    // 取消epoll 对应描述
    epoll_ctl(efd, EPOLL_CTL_DEL, bfd_session->rx_fd.fd, NULL);      
    epoll_ctl(efd, EPOLL_CTL_DEL, bfd_session->tx_fd.fd, NULL);      

    // 关闭定时器描述符
    close(bfd_session->rx_fd.fd);
    close(bfd_session->tx_fd.fd);

    // 关闭发送套接字
    close(bfd_session->tx_sock);

    // 释放会话
    free(bfd_session);
    log_debug("release session done \r\n");
    return ;    
}

// 发送控制报
int bfd_send_ctrl_packet(struct session *bfd_session)
{
	int ret = 0;
	struct msghdr msg;
	struct iovec iov[1];
	struct bfdhdr bfdh;

	memcpy(&bfdh, &(bfd_session->bfdh), sizeof(struct bfdhdr));
	memset(&msg, 0, sizeof(struct msghdr));	
	msg.msg_name = &(bfd_session->raddr);    /* 设置目的地址 */
	msg.msg_namelen = sizeof(struct sockaddr_in);
	iov[0].iov_base = &bfdh;
	iov[0].iov_len  = sizeof(struct bfdhdr);	
    msg.msg_iov = &iov[0];
    msg.msg_iovlen = 1;
    ret = sendmsg(bfd_session->tx_sock, &msg, 0);
    if (ret == -1)
        log_debug("bfd send ctrl len :%d , errno :%d \r\n", ret, errno);
	return ret;
}


/* 设置发送定时器 */
void bfd_start_xmit_timer(struct session *bfd_session)
{
    int ret;
    time_t t;
	unsigned int jitter;
    struct itimerspec timeval;	
    srand((unsigned int)time(&t));

	// jitter is 0% -> 25%. if detectmult == 1, max 90%, 获取随机
	jitter = rand();
	jitter = 75 + jitter % ((bfd_session->bfdh.detect_mult == 1 ? 15 : 25) + 1);

    memset(&timeval, 0, sizeof(struct itimerspec));       
    timeval.it_value.tv_sec = (bfd_session->act_tx_intv)/1000000;
    timeval.it_value.tv_nsec = (((uint64_t)bfd_session->act_tx_intv)*1000)%1000000000;
    timeval.it_interval.tv_sec = timeval.it_value.tv_sec;
    timeval.it_interval.tv_nsec = timeval.it_value.tv_nsec;

    // 设置定时
    ret = timerfd_settime(bfd_session->tx_fd.fd, 0, &timeval, NULL);    // 相对时间
    if (ret == -1) {
        log_info("settimer fail，errno : %d\r\n", errno);
        return ;
    }
    log_debug("set xmit timer, sec:%u, nsec : %u, jitter : %u \r\n",(bfd_session->act_tx_intv*1000)/1000000000,
    (((uint64_t)bfd_session->act_tx_intv)*1000)%1000000000, jitter);

	return;
}


/* 取消发送定时器 */
void bfd_stop_xmit_timer(struct session *bfd_session)
{
    int ret;
    struct itimerspec timeval;  
    memset(&timeval, 0, sizeof(struct itimerspec));       

    ret = timerfd_settime(bfd_session->tx_fd.fd, 0, &timeval, NULL);
    if (ret == -1) 
        log_info("settimer fail，errno : %d\r\n", errno);

	return;
}


/* 重置发送定时器 */
void bfd_reset_tx_timer(struct session *bfd_session)
{
    log_debug("reset tx timer \r\n");
	bfd_stop_xmit_timer(bfd_session);  // 取消任务
	bfd_start_xmit_timer(bfd_session); // 添加任务
	
	return;
}


// 停止接收超时计时
void bfd_stop_expire_timer(struct session *bfd_session)
{
    int ret;
    struct itimerspec timeval;  
    memset(&timeval, 0, sizeof(struct itimerspec));       

    log_info("stop expire timer \r\n");
    ret = timerfd_settime(bfd_session->rx_fd.fd, 0, &timeval, NULL);
    if (ret == -1) 
        log_info("settimer fail，errno : %d\r\n", errno);

	return;
}


// 重置接收超时计时
void bfd_reset_expire_timer(struct session *bfd_session)
{
    int ret;
    struct itimerspec timeval;  
    memset(&timeval, 0, sizeof(struct itimerspec));       

    // 停止超时检测定时器
    bfd_stop_expire_timer(bfd_session);
   
    // 设置定时

    timeval.it_value.tv_sec = (bfd_session->detect_time)/1000000;
    //timeval.it_value.tv_nsec = (bfd_session->detect_time)%1000000;
 	timeval.it_value.tv_nsec = (((uint64_t)bfd_session->detect_time)*1000)%1000000000;  
 	timeval.it_interval.tv_sec = timeval.it_value.tv_sec;
    timeval.it_interval.tv_nsec = timeval.it_value.tv_nsec;
    ret = timerfd_settime(bfd_session->rx_fd.fd, 0, &timeval, NULL);
	log_debug("expire timer sec : %u, nsec : %u \r\n", (bfd_session->detect_time)/1000000, (((uint64_t)bfd_session->detect_time)*1000)%1000000000);
    if (ret == -1) {
        log_info("settimer fail，errno : %d\r\n", errno);
        return ;
    }

	return;
}


/* 定时发送bfd 控制报文 */
void bfd_xmit_timeout(struct session *bfd_session)
{    
	/* reset timer before send processing(avoid self synchronization) */	
	bfd_start_xmit_timer(bfd_session);
	bfd_send_ctrl_packet(bfd_session);

    if (bfd_session->bfdh.sta != BFD_STA_UP) {
        bfd_session->try_pkts ++;
        if (bfd_session->try_pkts > 180) {
            bfd_notify(bfd_session->key, "create bfd session fail", HaBFDSessionCreateFailRsp);
            bfd_session_delete(bfd_session);
        }
    }
    else
        bfd_session->try_pkts = 0;
    log_debug("xmit timeout \r\n");
	return;
}


/* 会话超时回调函数 */
void bfd_detect_timeout(struct session *bfd_session)
{
    int del_flag = 0;
    log_debug("timer out, current sta : %d \r\n", bfd_session->bfdh.sta);
	del_flag = bfd_fsm_event(bfd_session, BFD_EVENT_TIMER_EXPIRE);
	if (del_flag)
	    bfd_session_delete(bfd_session);
	
	return;
}


int bfd_fsm_ignore(struct session *bfd_session)    
{
    log_debug("bfd fsm ignore \r\n");

	return 0;
}


int bfd_fsm_recv_admin_down(struct session *bfd_session)
{
    log_debug("bfd fsm recv_admindown \r\n");

	if (bfd_session->bfdh.sta != BFD_STA_DOWN)
	{
		/* goes to administratively down */
		bfd_session->bfdh.diag = BFD_DIAG_ADMIN_DOWN;
		bfd_stop_xmit_timer(bfd_session);
		bfd_stop_expire_timer(bfd_session);
	}
	
	return 0;
}


/* 初始化定时器，添加任务到工作队列 */
int bfd_fsm_start(struct session *bfd_session)
{
    log_debug("bfd fsm start \r\n");
	bfd_start_xmit_timer(bfd_session);

	return 0;
}


int bfd_fsm_rcvd_down(struct session *bfd_session)
{
    log_debug("bfd fsm recv down \r\n");

    // 如果本地状态为up的话，收到down事件需要更新diag原因
	if(bfd_session->bfdh.sta == BFD_STA_UP)
	{
		bfd_session->bfdh.diag = BFD_DIAG_NBR_SESSION_DOWN;
	}
	
	return 0;
}


int bfd_fsm_rcvd_init(struct session *bfd_session)
{
    log_debug("bfd fsm recv init \r\n");
    
	return 0;
}


int bfd_fsm_rcvd_up(struct session *bfd_session)
{
    log_debug("bfd fsm recv up \r\n");
    
	return 0;
}


// 重置定时器，诊断
int bfd_fsm_timer_expire(struct session *bfd_session)
{
    log_debug("bfd fsm timer expire \r\n");
	log_debug("bfd Timeout. time = %u usec, peer-addr : %u:%u:%u:%u \r\n", bfd_session->detect_time, 
    	NIPQUAD(bfd_session->raddr.sin_addr.s_addr));
	bfd_session->bfdh.diag = BFD_DIAG_CTRL_TIME_EXPIRED;

	/* reset timer */
	bfd_session->bfdh.des_min_tx_intv = htonl(BFD_DEFAULT_TX_INTERVAL);
	bfd_session->bfdh.req_min_rx_intv = htonl(BFD_DEFAULT_RX_INTERVAL);
	return 0;
}


void bfd_change_interval_time(struct session *bfd_session, unsigned int tx, unsigned int rx)
{
	/* Section 6.7.3 Description */
	if (bfd_session->bfdh.sta == BFD_STA_UP && (tx > ntohl(bfd_session->bfdh.des_min_tx_intv)))
	{
		bfd_session->bfdh.poll = 1;
        log_debug("BFD Poll Sequence is started(tx_intv increase) \r\n");
	}
	else
	{
		bfd_session->act_tx_intv = tx < ntohl(bfd_session->peer_req_rx_time) ? ntohl(bfd_session->peer_req_rx_time) : tx;
		bfd_reset_tx_timer(bfd_session); // 取消任务&添加任务
	}

	if (bfd_session->bfdh.sta == BFD_STA_UP && (rx < ntohl(bfd_session->bfdh.req_min_rx_intv)))
	{
		bfd_session->bfdh.poll = 1;		
		log_debug("BFD Poll Sequence is started(rx_intv change).");
	}
	else
	{
		bfd_session->act_rx_intv = rx;
	}

	bfd_session->bfdh.des_min_tx_intv = htonl(tx);
	bfd_session->bfdh.req_min_rx_intv = htonl(rx);

	return;
}

/* BFD Finite State Machine structure

                                  +--+
                                  |  | UP, ADMIN DOWN, TIMER
                                  |  V
                          DOWN  +------+  INIT
                   +------------|      |------------+
                   |            | DOWN |            |
                   |  +-------->|      |<--------+  |
                   |  |         +------+         |  |
                   |  |                          |  |
                   |  |               ADMIN DOWN,|  |
                   |  |ADMIN DOWN,          DOWN,|  |
                   |  |TIMER                TIMER|  |
                   V  |                          |  V
                 +------+                      +------+
            +----|      |                      |      |----+
        DOWN|    | INIT |--------------------->|  UP  |    |INIT, UP
            +--->|      | INIT, UP             |      |<---+
                 +------+                      +------+
*/


struct
{
	int (*func)(struct session *);
	int next_state;
} FSM[BFD_STA_MAX][BFD_EVENT_MAX]
={
	{
        // 
		{bfd_fsm_ignore, BFD_STA_ADMINDOWN},				/* Start */
		{bfd_fsm_ignore, BFD_STA_ADMINDOWN},				/* Received_Down */
		{bfd_fsm_ignore, BFD_STA_ADMINDOWN},				/* Received_Init */
		{bfd_fsm_ignore, BFD_STA_ADMINDOWN},				/* Received_Up */
		{bfd_fsm_ignore, BFD_STA_ADMINDOWN},				/* TimerExpired */
		{bfd_fsm_recv_admin_down, BFD_STA_ADMINDOWN},			/* Received_AdminDown */
	},
	{
		// down
		{bfd_fsm_start, BFD_STA_DOWN},						/* Start，初始化定时器，任务队列�?*/
		{bfd_fsm_rcvd_down, BFD_STA_INIT},					/* Received_Down */
		{bfd_fsm_rcvd_init, BFD_STA_UP},					/* Received_Init */
		{bfd_fsm_ignore, BFD_STA_DOWN},						/* Received_Up */
		{bfd_fsm_ignore, BFD_STA_DOWN},						/* TimerExpired */
		{bfd_fsm_recv_admin_down, BFD_STA_DOWN},		    /* Received_AdminDown */
	},
	{
		// init
		{bfd_fsm_ignore, BFD_STA_INIT},						/* Start */
		{bfd_fsm_ignore, BFD_STA_INIT},						/* Received_Down */
		{bfd_fsm_rcvd_init, BFD_STA_UP},					/* Received_Init */
		{bfd_fsm_rcvd_up, BFD_STA_UP},						/* Received_Up */
		{bfd_fsm_timer_expire, BFD_STA_DOWN},				/* TimerExpired */
		{bfd_fsm_recv_admin_down, BFD_STA_DOWN},		    /* Received_AdminDown */
	},
	{
		// Up
		{bfd_fsm_ignore, BFD_STA_UP},						/* Start */
		{bfd_fsm_rcvd_down, BFD_STA_DOWN},					/* Received_Down */
		{bfd_fsm_ignore, BFD_STA_UP},						/* Received_Init */
		{bfd_fsm_ignore, BFD_STA_UP},						/* Received_Up */
		{bfd_fsm_timer_expire, BFD_STA_DOWN},				/* TimerExpired */		
		{bfd_fsm_recv_admin_down, BFD_STA_DOWN},		    /* Received_AdminDown */
	},
};


/* bfd 状态机处理函数 */
int bfd_fsm_event(struct session *bfd_session, int bfd_event)
{
	int next_state, old_state;
	int del_flag = 0;

	old_state = bfd_session->bfdh.sta;
	next_state = (*(FSM[bfd_session->bfdh.sta][bfd_event].func))(bfd_session);
    
	if (!next_state)
		bfd_session->bfdh.sta = FSM[bfd_session->bfdh.sta][bfd_event].next_state;
	else
		bfd_session->bfdh.sta = next_state;

	if (bfd_session->bfdh.sta != old_state)
	{
        /* 如果会话建立，变更定时器间隔 */
        if (bfd_session->bfdh.sta == BFD_STA_UP && old_state != BFD_STA_UP)
        {
            bfd_change_interval_time(bfd_session, bfd_session->des_min_tx_time, bfd_session->req_min_rx_time);
            // 发送会话成功消息
            bfd_notify(bfd_session->key, "session up", HaBFDSessionCreateSuccRsp);
            log_debug("session up \r\n");
        }
                
	    /* 会话异常 */
		if ((bfd_session->bfdh.sta != BFD_STA_UP) && (old_state == BFD_STA_UP))
		{
		    del_flag = 1;
            switch(bfd_session->bfdh.diag) {
                case BFD_DIAG_CTRL_TIME_EXPIRED:
                    //发送会话异常消
                    bfd_notify(bfd_session->key, "timer expired", HaBFDSessionStateErrRsp);            
                    break;

                case BFD_DIAG_NBR_SESSION_DOWN:
                    //发送会话异常消
                    bfd_notify(bfd_session->key, "neighbour session down", HaBFDSessionStateErrRsp);            
                    break;

                case BFD_DIAG_ADMIN_DOWN  :
                    //发送会话异常消 
                    bfd_notify(bfd_session->key, "admin down", HaBFDSessionStateErrRsp);            
                    break;
                default:
                    //发送会话异常消息
                    bfd_notify(bfd_session->key, "default down", HaBFDSessionStateErrRsp);            
                    break;                
            }
			log_debug("session down \r\n");                
		}

		/* Reset Tx Timer */	
		if(bfd_session->bfdh.sta != BFD_STA_UP)
		{
			bfd_change_interval_time(bfd_session, BFD_DEFAULT_TX_INTERVAL, BFD_DEFAULT_RX_INTERVAL);
			/* Cancel Expire timer */
			bfd_stop_expire_timer(bfd_session);
		}

		/* set downtime */
		if(bfd_session->bfdh.sta == BFD_STA_DOWN)
		{
            /*
			bfd->last_down = get_sys_uptime();
			bfd->last_diag = bfd->cpkt.diag;
			*/
		}

		/* Reset Diagnostic Code */
		if (old_state == BFD_STA_DOWN) {
			bfd_session->bfdh.diag = BFD_DIAG_NO_DIAG;
		}
	}

	return del_flag;
}


// 会话查找
struct session *bfd_session_lookup(uint32_t my_disc, uint32_t dst, uint32_t src)
{
	int key;
	struct session *bfd_session = NULL;

    pthread_rwlock_rdlock(&bfd_rwlock);
	if (my_disc){
		key = hash_key(my_disc, 0);
		if (key == -1) {
		    log_debug("not found key \r\n");
            return NULL;
		}
		bfd_session = master.session_tbl[key];
		while (bfd_session) {
			if (bfd_session->bfdh.my_disc == my_disc)
				break;
			bfd_session = bfd_session->session_next;
		}
	}
	else {
		key = hash_key(0, dst);
		if (key == -1) {
		    log_debug("not found key \r\n");
            return NULL;
		}
		bfd_session = master.neigh_tbl[key];
		while (bfd_session) {            		    
			if (dst == bfd_session->raddr.sin_addr.s_addr && src == bfd_session->laddr.sin_addr.s_addr)
			    break;
			else
			{
    		    log_debug("addr not match, dst : %u:%u:%u:%u, raddr : %u:%u:%u:%u  \r\n",
    		        NIPQUAD(dst),NIPQUAD(bfd_session->raddr.sin_addr.s_addr));             
			}
			bfd_session = bfd_session->neigh_next;
		}
	}
    pthread_rwlock_unlock(&bfd_rwlock);
	
	return bfd_session;
}


/* bfd报文接收线程处理回调函数 */
int bfd_recv_ctrl_packet(struct sockaddr_in *client_addr, struct sockaddr_in *server_addr, char *buffer, int len) {
    struct bfdhdr *bfdh;
    struct session *bfd_session;
    unsigned char old_poll_bit;
    int poll_seq_end = 0;
    int del_flag = 0;
    
    bfdh = (struct bfdhdr *)buffer;
    
    /* If the Length field is greater than the payload of the */
    /* encapsulating protocol, the packet MUST be discarded. */    
    if(bfdh->len > len)
    {
        log_info("length is too short. Discarded. bfdh->len :%d > recv_len :%d \r\n", bfdh->len, len);
        return -1;
    }
    
    /* Section 6.7.6 check */    
    /* If the version number is not correct (1), the packet MUST be discarded. */    
    if(bfdh->version != BFD_VERSION)
    {
        log_debug("bfd packet wrong version : %u \r\n", bfdh->version);
        return -1;
    }
    
    /* If the Length field is less than the minimum correct value (24 if */
    /* the A bit is clear, or 26 if the A bit is set), the packet MUST be */
    /* discarded. */    
    if((!bfdh->auth && bfdh->len != BFD_CTRL_LEN) || (bfdh->auth && bfdh->len < BFD_CTRL_AUTH_LEN))
    {
        log_debug("bfd packet length (%d) not right. Discarded \r\n", bfdh->len);
        return -1;
    }
        
    /* If the Detect Mult field is zero, the packet MUST be discarded. */
    if(bfdh->detect_mult == 0)
    {
        log_debug("Detect Multi field is zero. Discarded \r\n");
        return -1;
    }
    
    /* If the My Discriminator field is zero, the packet MUST be discarded. */
    if(bfdh->my_disc == 0)
    {
        log_debug("My Discriminator field is zero. Discarded \r\n");
        return -1;
    }
    
    /* If the Your Discriminator field is nonzero, it MUST be used to */
    /* select the session with which this BFD packet is associated.  If */
    /* no session is found, the packet MUST be discarded. */
    if(bfdh->your_disc)
    {
        /* your_disc 不为0，查找对话表 */
        bfd_session = bfd_session_lookup(bfdh->your_disc, 0, 0);
        if(bfd_session == NULL)
        {
            log_debug("couldn't find session with Your Discriminator field (%x). Discarded \r\n", bfdh->your_disc);
            return -1;
        }
    }
    else
    {        
        /* If the Your Discriminator field is zero and the State field is not
        Down or AdminDown, the packet MUST be discarded. */
        if(bfdh->sta != BFD_STA_ADMINDOWN && bfdh->sta != BFD_STA_DOWN)
        {
            log_debug("Received your_disc = 0, while state is not Down or AdminDown. Discarded \r\n");
            return -1;
        }
    
        /* If the Your Discriminator field is zero, the session MUST be
           selected based on some combination of other fields, possibly
           including source addressing information, the My Discriminator
           field, and the interface over which the packet was received.  The
           exact method of selection is application-specific and is thus
           outside the scope of this specification.  If a matching session is
           not found, a new session may be created, or the packet may be
           discarded.  This choice is outside the scope of this
           specification. 
           如果your_disc=0,则根据来源地址来查找会话表
        */
        bfd_session = bfd_session_lookup(0, client_addr->sin_addr.s_addr, server_addr->sin_addr.s_addr);           
        if(bfd_session == NULL)
        {
            log_debug("couldn't find session with peer_addr: %u:%u:%u:%u, Discarded \r\n", 
                NIPQUAD(client_addr->sin_addr.s_addr));
            return -1;
        }        
    }
    
    /* mark our address 
        memcpy(bfd->src, dst, bfd->proto->namelen(dst));
    */
                
    /* If the A bit is set and no authentication is in use (bfd.AuthType is zero), the packet MUST be discarded.
      If the A bit is clear and authentication is in use (bfd.AuthType is nonzero), the packet MUST be discarded. 
      如果认证字段置位，discarded
    */
    if(bfdh->auth)
    {
        log_debug("Auth type is set. Discarded");
        return -1;
    }
    
    /* If the A bit is set, the packet MUST be authenticated under the
           rules of section 6.6, based on the authentication type in use
           (bfd.AuthType.)  This may cause the packet to be discarded. */    
    /* FIXME authentication process */
    
        
    /* Set bfd.RemoteDiscr to the value of My Discriminator. */
    bfd_session->bfdh.your_disc = bfdh->my_disc;
    
    /* If the Required Min Echo RX Interval field is zero, the
           transmission of Echo packets, if any, MUST cease. */
        /* FIXME */
    if (bfdh->req_min_echo_rx_intv != 0) {
        log_debug("echo_rx_intv not zero, Discarded, peer_req_echo_rx_intv : %x \r\n", bfdh->req_min_echo_rx_intv);
        return -1;
    }
    
    /* If Demand mode is active, a Poll Sequence is being transmitted by
       the local system, and the Final (F) bit in the received packet is
       set, the Poll Sequence MUST be terminated. */
       /* FIXME */        
        
    /* If Demand mode is not active, the Final (F) bit in the received
       packet is set, and the local system has been transmitting packets
       with the Poll (P) bit set, the Poll (P) bit MUST be set to zero in
       subsequent transmitted packets. */
       /* permit session from loopback interface */
    if(!bfd_session->bfdh.demand && bfdh->final && (bfd_session->bfdh.poll)) {
        bfd_session->bfdh.poll = 0;
        poll_seq_end = 1;        
        /* 停止poll seq,  更新时间     */
        log_debug("BFD Poll Sequence is done. \r\n");
    
        bfd_session->act_tx_intv = 
            // 更新发送时间
            ntohl(bfd_session->bfdh.des_min_tx_intv) < ntohl(bfdh->req_min_rx_intv) ?
            ntohl(bfdh->req_min_rx_intv) : ntohl(bfd_session->bfdh.des_min_tx_intv);
    
        /* 更新接收时间间隔 */
        bfd_session->act_rx_intv = ntohl(bfd_session->bfdh.req_min_rx_intv);
    }
    bfd_session->act_tx_intv = 
            // 更新发送时间
            ntohl(bfd_session->bfdh.des_min_tx_intv) < ntohl(bfdh->req_min_rx_intv) ?
            ntohl(bfdh->req_min_rx_intv) : ntohl(bfd_session->bfdh.des_min_tx_intv);
            
        /* Update the Detection Time as described in section 6.7.4. */
        bfd_session->detect_time = bfdh->detect_mult *
            (bfd_session->act_rx_intv > ntohl(bfdh->des_min_tx_intv) ?
             bfd_session->act_rx_intv : ntohl(bfdh->des_min_tx_intv));
    
        /* Update the transmit interval as described in section 6.7.2. */
        // 收到F标志置位的话，重置发送任务计时器
        if (poll_seq_end){
            bfd_reset_tx_timer(bfd_session);
        }
    
        /* If bfd.SessionState is AdminDown */
        if (bfd_session->bfdh.sta == BFD_STA_ADMINDOWN)
        {               
            log_debug("local sta admindown, discard all received packet \r\n");
            return -1;
        }
        
        /* If received state is AdminDown
            If bfd.SessionState is not Down
             Set bfd.LocalDiag to 3 (Neighbor signaled session down)
             Set bfd.SessionState to Down */
        if (bfdh->sta == BFD_STA_ADMINDOWN)
        {
            if (bfd_session->bfdh.sta != BFD_STA_DOWN)
            {
                bfd_session->bfdh.diag = BFD_DIAG_NBR_SESSION_DOWN;
            }
        }
        
        /* 状态机处理 */
        if (bfdh->sta == BFD_STA_DOWN){
            del_flag = bfd_fsm_event(bfd_session, BFD_EVENT_RECV_DOWN);
        }
        else if (bfdh->sta == BFD_STA_INIT){
            del_flag = bfd_fsm_event(bfd_session, BFD_EVENT_RECV_INIT);
        }
        else if (bfdh->sta == BFD_STA_UP){
            del_flag = bfd_fsm_event(bfd_session, BFD_EVENT_RECV_UP);
        }
    
        /* If the Demand (D) bit is set and bfd.DemandModeDesired is 1,
           and bfd.SessionState is Up, Demand mode is active. 
           if receive D bit set, Discarded */        
        /* FIXME */         
        if (bfdh->demand)
        {
            log_debug("receive demand mode set, discarded \r\n");
            return -1;
        }
        
        /* If the Demand (D) bit is clear or bfd.DemandModeDesired is 0,
           or bfd.SessionState is not Up, Demand mode is not
           active. */
        else
        {
            bfd_session->bfdh.demand = 0;
        }
    
        /* If the Poll (P) bit is set, send a BFD Control packet to the
           remote system with the Poll (P) bit clear, and the Final (F) bit
           set. */
        // 在此之前确保时间参数已更
        if (bfdh->poll)
        {
            /* Store old p-bit */
            old_poll_bit = bfd_session->bfdh.poll;    
            log_debug("BFD: recv Poll Sequence, send final flag \r\n");
    
            bfd_session->bfdh.poll = 0;
            bfd_session->bfdh.final = 1;
            
            bfd_start_xmit_timer(bfd_session);
            bfd_send_ctrl_packet(bfd_session);
            bfd_session->bfdh.final = 0;            
            bfd_session->bfdh.poll = old_poll_bit;
        }
    
        /* If the packet was not discarded, it has been received for purposes
           of the Detection Time expiration rules in section 6.7.4. */
        //    log_info("BFD: Detect Time is %d(usec)", bfd_session->detect_time);
    
        if (bfd_session->bfdh.sta == BFD_STA_UP || bfd_session->bfdh.sta == BFD_STA_INIT)
        {
            // 收到正常bfd控制报文后，取消接收超时计时器任务
			log_debug("call reset expire timer \r\n");
            bfd_reset_expire_timer(bfd_session);
        }

        // 会话异常
        if (del_flag)
            bfd_session_delete(bfd_session);
    
    return 0;
       
}
    
        
// 创建接收套接字，成功返回0，失败
int bfd_create_rx_sock(void) {
    int ret;
    int on = 1;      

    // 创建套接
    bfd_rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (bfd_rx_sock == -1) {
        log_info("create rx socket fail \r\n");
        return -1;
    }

    // 设置监听地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // 所有地址
    server_addr.sin_port = htons(BFD_LISTENING_PORT); // 3784

    // 设置SO_REUSEADDR
    if ((ret = setsockopt(bfd_rx_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on))) != 0) {
        log_info("setsockopt reuseaddr fail, ret : %d,error : %d \r\n", ret, errno);
        close(bfd_rx_sock);
        return -1;
    }

    // 设置IP_PKTINFO
    if (0 != setsockopt(bfd_rx_sock, IPPROTO_IP, IP_PKTINFO, (char *)&on, sizeof(on))) {
        log_info("setsockopt ip_pktinfo fail, errno : %d \r\n", errno);
        close(bfd_rx_sock);
        return -1;  
    }

    // 设置IP_RECVTTL
    if(0 != setsockopt(bfd_rx_sock, IPPROTO_IP, IP_RECVTTL, (char *)&on, sizeof(on))) {
        log_info("setsockopt ip_recvttl fail，errno : %d \r\n", errno);
        close(bfd_rx_sock);
        return -1;    
    }    

    // 绑定本地监听地址
    if (0 != bind(bfd_rx_sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in))) {
        log_info("bind local listening addr fail，errno : %d \r\n", errno);
        close(bfd_rx_sock);
        return -1;
    }

   return 0;
}


/* bfd 接收线程 */
void *bfd_recv_thread(void *data)
{
    int ret ;
    int recv_len;
    int recv_ttl;
    int addr_len;
	char buffer[512] = {0};        /* 接收缓存 */
	struct in_pktinfo *pktinfo = NULL;	 /* 用于指向获取的本地地址信息 */	
	struct msghdr msg;
	struct iovec iov[1];    
	struct sockaddr_in client_addr; /* 来源地址 */
	struct sockaddr_in server_addr; /* 本地地址 */	
	struct cmsghdr *cmhp;           
	char buff[CMSG_SPACE(sizeof(struct in_pktinfo) + CMSG_SPACE(sizeof(int)))] = {0};   /* 控制信息 */
	struct cmsghdr *cmh = (struct cmsghdr *)buff;   /* 控制信息 */
    addr_len = sizeof(struct sockaddr_in);

    memset(&buffer, 0, sizeof(buffer));
    memset(&msg, 0, sizeof(struct msghdr));
    memset(&iov, 0, sizeof(struct iovec));
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    memset(&client_addr, 0, sizeof(struct sockaddr_in));    


    // 设置允许线程取消
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    // 设置延迟取消
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    // 接收bfd报文
    while(1) {
        pthread_testcancel(); 
        msg.msg_name = &client_addr;
        msg.msg_namelen = addr_len;
        msg.msg_iov = &iov[0];
        msg.msg_iovlen = 1;
        msg.msg_control = cmh;
        msg.msg_controllen = sizeof(buff);    
        iov[0].iov_base = &buffer;
        iov[0].iov_len = sizeof(buffer);                     

        // 超时退出
        recv_len = recvmsg(bfd_rx_sock, &msg, 0);
        if (recv_len > 0)
        {
            /* 辅助信息 */
            msg.msg_control = cmh;
            msg.msg_controllen = sizeof(buff);
            for (cmhp = CMSG_FIRSTHDR(&msg); cmhp; cmhp = CMSG_NXTHDR(&msg, cmhp)) {
                if (cmhp->cmsg_level == IPPROTO_IP) {
                    if (cmhp->cmsg_type == IP_PKTINFO) {
                        pktinfo = (struct in_pktinfo *)CMSG_DATA(cmhp);
                        server_addr.sin_family = AF_INET;
                        /* 获取头标识目的地址信息 */
                        server_addr.sin_addr = pktinfo->ipi_addr;
                        //log_info("packet daddr : %u:%u:%u:%u \r\n", NIPQUAD(server_addr.sin_addr));
                        //log_info("packet daddr : %u:%u:%u:%u \r\n", NIPQUAD(client_addr.sin_addr));                        
                    }
                    else if(cmhp->cmsg_type == IP_TTL) {
                        // 获取ttl
                        recv_ttl = *(int *)CMSG_DATA(cmhp);
                        //log_info("recv ttl %d \r\n", recv_ttl);
                    }
                }
            }
            // bfd报文处理、报文检查、状态机、定时器转换
            bfd_recv_ctrl_packet(&client_addr, &server_addr, buffer, recv_len);
        }
        memset(buffer, 0, sizeof(buffer));
        memset(buff, 0, sizeof(buff));
        memset(&server_addr, 0, sizeof(struct sockaddr_in));
        memset(&client_addr, 0, sizeof(struct sockaddr_in));        
        recv_ttl = 0;
    }
    return NULL;
}


// 定时器检测线程主函数
void *bfd_timing_monitor_thread(void *data) {
    int i;
    int ret;
    int fds;
    uint64_t value;
    struct time_callback_arg *arg;

    // 设置允许线程取消
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    // 设置延迟取消
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    while(1) {
        // 设置线程取消
        pthread_testcancel(); 
        fds = epoll_wait(efd, g_events, BFD_SESSION_HASH_SIZE, -1);
        for (i = 0; i<fds; i++)
        {    
            arg = (struct time_callback_arg *)g_events[i].data.ptr;
            if (g_events[i].events & EPOLLIN)
            {   
                ret = read(arg->fd, &value, sizeof(uint64_t));
                if (ret == -1) 
                    log_info("read return -1, errno :%d \r\n", errno);
                if (!arg->flag) {
                    // 超时定时器处
                    bfd_detect_timeout(arg->bfd_session);                    
                }
                else {
                    // 发送定时器处理
                    bfd_xmit_timeout(arg->bfd_session);
                }
            }            
        }        
    }
    
    return NULL;
}

// 设置回调函数
void bfd_setCallback(CALLBACK_FUNC pfunc) {
		callbackSendMsg = pfunc;	
}

// 发送
void bfd_notify(char *msgkey, char *msginfo, int msgtype){
    BFD_RSP rsp;
    strcpy(rsp.msgkey, msgkey);
    strcpy(rsp.msginfo, msginfo);
    rsp.msgtype = msgtype;

    callbackSendMsg(&rsp);
    log_debug("send %s msg \r\n", msginfo);

    return ;
}

void bfd_session_cfg_dump(struct session_cfg *session_cfg) {
    log_debug("local_ip_type : %u \r\n"
              "local_port :%hu \r\n"
              "local_ip : %u:%u:%u:%u \r\n"
              "remote_ip_type : %u \r\n"
              "remote_port : %hu \r\n"
              "remote_ip : %u:%u:%u:%u \r\n"
              "detect_mult : %u \r\n"
              "des_min_tx : %u \r\n"
              "req_min_rx : %u \r\n"
              "req_min_echo_rx : %u \r\n"
              "key : %s \r\n",
              session_cfg->local_ip_type, session_cfg->local_port, NIPQUAD(session_cfg->local_ip),
              session_cfg->remote_ip_type, session_cfg->remote_port, NIPQUAD(session_cfg->remote_ip),
              session_cfg->detect_multi, session_cfg->des_min_tx_interval, 
              session_cfg->req_min_rx_interval, session_cfg->req_min_echo_rx, session_cfg->key);
}

void bfd_add(BFD_CFG *cfg) {
	uint32_t src, dst;
	int ret = 0;
	src = inet_addr(cfg->localIP);
	dst = inet_addr(cfg->remoteIP);

    struct session_cfg  val;
    val.local_ip_type = cfg->localIPType;
    val.local_port = cfg->localPort;
    val.remote_ip_type = cfg->remoteIPType;
    val.remote_port = cfg->remotePort;
    val.detect_multi = cfg->detectMult;
    val.des_min_tx_interval = cfg->desMinTx;
    val.req_min_rx_interval = cfg->reqMinRx;
    val.req_min_echo_rx = cfg->reqMinEchoRx;
    val.local_ip.ip = src;
    val.remote_ip.ip = dst;    
    strcpy(val.key, cfg->key);
    bfd_session_cfg_dump(&val);
    if (val.remote_port != 3784) {
        bfd_notify(val.key, "bfd session port wrong, remote port != 3784", HaBFDSessionCreateFailRsp);
        log_debug("wrong remote port :%hu, should be 3784 \r\n");
        return ;
    }
    ret = bfd_session_add(&val);
    if ( ret != 0) {
        bfd_notify(val.key, "bfd add session fail", HaBFDSessionCreateFailRsp);
    }
    return;    
}

// bfd 初始化，成功返回0
int bfd_init(void) {
    int ret;   

    // 创建 efd 文件描述
    efd = epoll_create1(0);
    if (efd == -1) {
        log_info("create epoll fail \r\n");
        return -1;
    }
    log_debug("create epoll done \r\n");
    
    g_events = (struct epoll_event *)calloc(BFD_SESSION_HASH_SIZE, sizeof(struct epoll_event));
    if (g_events == NULL) {
        log_info("calloc fail \r\n");
        close(efd);
        return -1;
    }
    
    // 会话表初始化
    memset(&master, 0, sizeof(struct bfd_master));    
    
    //接收套接字初始化
    ret = bfd_create_rx_sock();
    if (ret != 0) {
        log_info("create rx_sock fail \r\n");
        return ret;
    }

    // 读写锁属性初始化
    pthread_rwlockattr_init(&bfd_rwlock_attr);
    // 设置写者优先属
    pthread_rwlockattr_setkind_np(&bfd_rwlock_attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    // 读写锁初始化
    ret = pthread_rwlock_init(&bfd_rwlock, &bfd_rwlock_attr);
    
    // 接收线程初始
    ret = pthread_create(&bfd_rx_thread, NULL, bfd_recv_thread, NULL);
    if (ret != 0 ) {
        log_info("thread create fail \r\n");   
        goto err;
    }

    // 定时器检测线程初始化
    ret = pthread_create(&bfd_timing_thread, NULL, bfd_timing_monitor_thread, NULL);
    if (ret != 0 ) {
        log_info("bfd timing thread create fail \r\n");        
        goto err;
    }
    return 0;
err:    
    pthread_rwlockattr_destroy(&bfd_rwlock_attr);    // 读写锁属性注销
    pthread_rwlock_destroy(&bfd_rwlock);            // 读写锁注销
    
    pthread_join(bfd_rx_thread, NULL);              // 线程取消
    pthread_join(bfd_timing_thread, NULL);    
    
    close(bfd_rx_sock);                            
    close(efd);                                    

    return ret;
}


// bfd退出，资源释放
int bfd_exit() {
    int i, ret = 0;
    unsigned int disc, addr, key;
    
    struct session *bfd_session;
    struct session *session_next;
    struct session *neigh;
    struct session *neigh_priv;

    // 释放bfd会话
    pthread_rwlock_wrlock(&bfd_rwlock);
    for (i = 0; i < BFD_SESSION_HASH_SIZE; i++) {
        bfd_session = master.session_tbl[i];
        while (bfd_session != NULL) {
            master.session_tbl[i] = bfd_session->session_next;           
                
            key = hash_key(0, bfd_session->raddr.sin_addr.s_addr);
            neigh_priv = NULL;
            neigh = master.neigh_tbl[key];
            while (neigh) {
                if (neigh->bfdh.my_disc == bfd_session->bfdh.my_disc) {
                    if (neigh_priv != NULL) 
                        neigh_priv->neigh_next = neigh->neigh_next;
                    else
                        master.neigh_tbl[key] = neigh->neigh_next;                
                    break;                        
                }
                else {
                    neigh_priv = neigh;
                    neigh = neigh->neigh_next;
                }
            }
            // 关掉定时
            bfd_stop_xmit_timer(bfd_session);
            bfd_stop_expire_timer(bfd_session);
            
            // 取消epoll 对应描述
            epoll_ctl(efd, EPOLL_CTL_DEL, bfd_session->rx_fd.fd, NULL);      
            epoll_ctl(efd, EPOLL_CTL_DEL, bfd_session->tx_fd.fd, NULL);      
            
            // 关闭定时器描述符
            close(bfd_session->rx_fd.fd);
            close(bfd_session->tx_fd.fd);
            
            // 关闭发送套接字
            close(bfd_session->tx_sock);
                           
            bfd_session->session_next = NULL;
            bfd_session->neigh_next = NULL;
            free(bfd_session);            
            bfd_session = master.session_tbl[i];
        }       
    }
    pthread_rwlock_unlock(&bfd_rwlock);
    
    log_debug("pthread cancel bfd_rx_thread \r\n");
    log_debug("pthread destroy rwlock and rwlock_attr \r\n");
    pthread_cancel(bfd_rx_thread);
    pthread_cancel(bfd_timing_thread);
    pthread_join(bfd_rx_thread, NULL);
    pthread_join(bfd_timing_thread, NULL);       
    pthread_rwlock_destroy(&bfd_rwlock);    // 读写锁注销
    pthread_rwlockattr_destroy(&bfd_rwlock_attr);    // 读写锁属性注销

    // 关闭文件描述
    close(efd);
    close(bfd_rx_sock);
    log_debug("bfd exit \r\n");
    return ret;
}


int bfd_test(int opt) {
    int ret;

    struct session_cfg test;
    test.local_ip_type = AF_INET;
    test.remote_ip_type = AF_INET;
    test.des_min_tx_interval = 31000;	// 500us
    test.req_min_rx_interval = 31000;	// 500us,
    test.req_min_echo_rx = 0;    
    test.remote_port = 3784;
    test.detect_multi = 2;
    if (opt == 1) {
	 	// 配置A
		log_info("10.251.254.2 --> 10.251.254.1 \r\n");
		test.local_ip.ip = inet_addr("10.251.254.2");
     	test.remote_ip.ip = inet_addr("10.251.254.1");
	    test.local_port = 4002;   		
	}   
	else {
		log_info("10.251.254.1 --> 10.251.254.2 \r\n");	
		test.local_ip.ip = inet_addr("10.251.254.1");
   		test.remote_ip.ip = inet_addr("10.251.254.2");
   		test.local_port = 4000;
    	
	}
    ret = bfd_session_add(&test);     
	if (ret != 0)
		log_info("add session fail \r\n");

	return ret;
}

/*
void main()
{
    int ret;
    struct session_cfg test;
    test.local_ip_type = AF_INET;
    test.remote_ip_type = AF_INET;
    test.local_ip.ip = inet_addr("10.251.254.2");
    test.remote_ip.ip = inet_addr("10.251.254.1");
    test.des_min_tx_interval = 3000000;
    test.req_min_rx_interval = 3000000;
    test.req_min_echo_rx = 0;

    test.remote_port = 3784;
    test.local_port = 4000;
    test.detect_multi = 2;
    
    // bfd初始�?
    ret = bfd_init();
    if (ret != 0) {
        log_info("bfd init fail \r\n");
        return ;
    }
    ret = bfd_session_add(&test);
    if (ret == 0)
        log_debug("add session done. \r\n");
    while(1);


    bfd_exit();
    return ;
}

*/
