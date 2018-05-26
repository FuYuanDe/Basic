#ifndef _UDPTEST_H
#define _UDPTEST_H

#define log(fmt, arg...) printf("[udptest] %s:%d "fmt, __FUNCTION__, __LINE__, ##arg)

#ifndef NIPQUAD
#define NIPQUAD(addr) \
	((unsigned char *)&addr)[0], \
	((unsigned char *)&addr)[1], \
	((unsigned char *)&addr)[2], \
	((unsigned char *)&addr)[3]
#endif

#endif