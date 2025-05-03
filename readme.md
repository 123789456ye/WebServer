# WebServer
用C++实现的高性能WEB服务器，经过webbenchh压力测试可以实现上万的QPS

## 修改
将部分 const char* 换为 string_view
引入 jthread 和 stop_token 重写线程池，并使用匿名函数进行绑定

## 致谢
Linux高性能服务器编程，游双著.

[TinyWebServer](https://github.com/qinguoyi/TinyWebServer)
