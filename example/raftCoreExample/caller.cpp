//
// Created by swx on 23-6-4.
//
#include <iostream>
#include "clerk.h"
#include "util.h"
int main() {
  Clerk client;
  client.Init("test.conf"); //获取所有raft节点ip、port ，并进行连接
  auto start = now();
  int count = 500;
  int tmp = count;
  while (tmp--) {
    client.Put("x", std::to_string(tmp)); //Put 方法会封装成 RPC 请求，发送到当前认为的 leader 节点，最终在集群中达成一致并写入日志。
    std::string get1 = client.Get("x");
    std::printf("get return :{%s}\r\n", get1.c_str());
  }
  return 0;
}