default:
	@/opt/toolchains/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/bin/arm-linux-gnueabihf-gcc -c bfdd.c -lrt -lpthread -fPIC 
	@/opt/toolchains/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux/bin/arm-linux-gnueabihf-gcc -shared -o libbfdd.so bfdd.o -fPIC 			
	@cp -Rf libbfdd.so ../../../../sbc/lib/	
clear:
	@rm -rf bfdd.o libbfdd.so	