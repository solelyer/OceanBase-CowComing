# 近知：一页技术说明

**输入与输出。** 用户创建知识库，上传 TXT/Markdown 或粘贴文本；应用显示分段数量、导入进度、实际推理后端，接受自然语言问题，输出排序后的原文段落及文件来源。默认 INT8，也可建立独立 FP32 对照库。

```mermaid
flowchart LR
  T[浏览器文本与问题] --> W[Web Worker\nTransformers.js + BGE]
  W --> E[512 维归一化向量]
  E --> A[本机 Node HTTP 服务]
  A --> S[seekdb-js Collection]
  S --> D[(seekdb\n原文、元数据、向量及索引)]
  D --> H[DBMS_HYBRID_SEARCH\n全文 + HNSW + 原生 RRF]
  H --> R[原文与来源卡片]
```

**模型与量化。** 固定 `Xenova/bge-small-zh-v1.5@75c43b069aac4d136ba6bc1122f995fedcfd2781`。FP32 为 `model.onnx`；优化版本使用上游 `model_quantized.onnx` 动态 INT8 权重。两者共享 tokenizer，采用 CLS pooling、L2 normalization，输出 512 维向量，最大 256 tokens；中文查询附加 BGE 检索指令。文件体积按一个精度对应的 ONNX 和四个公共模型配置文件合计，不能把缓存中的多个版本相加。大小、压缩比与同样本检索质量见性能报告。

**端侧运行。** 模型在 Worker 中加载，文档和问题均在实际浏览器里运行推理。自动模式优先请求 WebGPU，显式设置 execution provider；无适配器或初始化失败时切换 WASM，保留降级原因。WASM 使用最多四线程。禁用远程模型加载，只允许同源 HTTP；本地服务没有模型推理接口。真实 WebGPU 验证记录中的适配器是 SwiftShader 软件实现，不能代表 NVIDIA/AMD 硬件加速。

**数据与混合检索。** 后端经 `seekdb@1.4.0` 的 server 模式连接独立运行的本机 seekdb，不加载 Node 数据库原生绑定。每个知识库对应独立 Collection，配置 IK 全文索引、512 维 cosine HNSW 索引；embeddingFunction 显式为 null。浏览器生成的向量连同原文、来源 ID、来源名及段号写入数据库。检索使用 `Collection.hybridSearch`，由 SDK 调用 `DBMS_HYBRID_SEARCH` 生成并执行数据库端融合 SQL；全文候选、向量候选及 RRF 均在 seekdb 内完成，客户端只渲染结果。

**增量与状态。** 来源 ID 与正文的 SHA256 作为段落稳定身份，重复内容以 upsert 去重。每批最多 32 段，等待 `refresh_index()` 完成后才提示可检索。知识库注册信息、原文和向量持久化于 seekdb；精度保存在库元数据中并在请求时核对，切库不会混用数据。所有多语句数据库操作串行化，避免共享会话变量串扰。应用重启时正常关闭 MySQL 连接，数据库由独立服务管理；启动脚本提供固定官方 Docker 镜像和已安装 seekdb 可执行文件两种方式。

**优化与验证。** 缓存模型实例、使用 INT8 权重、批处理文档向量化、按批刷新索引。固定语料、查询、设备、模型 revision 和精度进行单批/多批对比；模型加载、文本向量化、写入、索引刷新、查询向量化、数据库检索及端到端延迟分别记录。FP32/WASM、INT8/WASM 和 INT8/WebGPU 使用同一组公开中文样例检查 Recall@3；测试记录每轮原始延迟和中位数，并验证文件导入、增量可见、去重、知识库隔离及服务重启后的真实检索。
