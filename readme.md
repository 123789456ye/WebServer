# WebServer
基于C++23实现的高性能WEB服务器 ~~，经过webbenchh压力测试可以实现上万的QPS ~~

## 压测
上面是我fork的版本，下面是这一版
![](https://imgur.com/a/fgXlcev)

## 修改
改为基于execution模型调度的服务器
完成部分：将context修改为sender版，及其代码

debug中：keep-alive支持，perf疑似有许多空转
未完成：将IO全部换为async，真正的异步
TODO：日志；buffer；实现消息队列（把来不及的请求queue起来）；调优等

## 致谢

[uring_exec](https://github.com/Caturra000/uring_exec)

[TinyWebServer](https://github.com/markparticle/WebServer)
