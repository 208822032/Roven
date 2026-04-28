# KVLite 嵌入向量服务

这是 KVLite V 族命令后续接入真实嵌入向量模型的独立 gRPC 服务原型。

## 文件说明

```text
embedding_server/
  embedding.proto        gRPC 协议定义
  requirements.txt       Python 依赖
  generate.sh            根据 proto 生成 Python gRPC 代码
  server.py              嵌入向量服务端
  client_test.py         简单客户端测试
  README.md              说明文档