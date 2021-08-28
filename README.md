# learn-io_uring
学习io_uring

编译过程<br/>
1 git clone https://github.com/axboe/liburing.git 。执行./configure && make -j2 && sudo make install（make j2开启两个线程编译，根据自己的核数定）。<br/>
2 git clone https://github.com/libuv/libuv.git 。执行./autogen.sh && ./configure && make -j2 && sudo make install。<br/>
3 安装完依赖后新建test.cc。然后编译 gcc -xc  test2.cc -luring -luv（xc指定按c语言编译，c++的话限制不一样，会报错）。<br/>
4 新建两个测试文件hello.cc和world.cc 。执行 ./a.out hello.cc  world.cc。<br/>
5 输出
```c
读取的大小：6997，文件信息：hello.cc => 6997
读取的大小：11019，文件信息：world.cc => 11019
```
