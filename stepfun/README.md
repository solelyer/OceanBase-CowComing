# 近知：浏览器端侧知识库

把课程笔记、项目说明或文本资料导入浏览器，使用公开开源 BGE-small-zh 模型在浏览器内生成向量，由本机 seekdb 保存原文、向量和来源并执行全文与向量混合检索。无需模型 API Key，不调用云端推理服务。

## 环境与启动

复现环境为 Linux x86_64、Node.js 22.12 或更高版本、Chrome / Edge 现代版本。建议至少 6 GiB 可用内存、8 GiB 可用磁盘；需要 `curl`，默认数据库启动方式需要 Docker，首次安装需能访问 npm、Hugging Face 和官方 seekdb 镜像。安装过程中只下载模型与软件，实际推理始终在浏览器内执行。

在报名仓库的本题分支执行：

```bash
cd stepfun
node --version
npm ci
npm run prepare:model
npm run build
npm run db:start
```

保持数据库终端运行，在第二个终端进入 `stepfun/` 后执行：

```bash
npm run db:wait
npm start
```

打开 **http://127.0.0.1:18776**。`db:start` 使用固定 digest 的 OceanBase 官方 seekdb 1.4.0 镜像，SQL 只映射到本机 12891 端口，数据保存到 `.data/seekdb-server/`。`db:wait` 执行真实 SQL 确认就绪。应用通过 seekdb-js 的 MySQL server 模式连接，不加载 Node 原生数据库绑定。

如果已经安装了 seekdb 可执行文件，可使用同一启动脚本直接启动本地服务，无需 Docker：

```bash
SEEKDB_SERVER_BIN=/absolute/path/to/seekdb npm run db:start
```

该脚本明确设置 4 个 CPU、4G 内存预算、2G 数据文件和 2G 日志盘。本次实际验证使用该二进制模式；当前工具没有 Docker daemon 权限，未将固定镜像启动配置记为本机 Docker 实测。镜像 digest 来自官方 Docker Hub，启动参数与数据卷路径依据官方镜像文档。

`prepare:model` 固定模型 revision，校验文件大小和 ONNX SHA256。模型、依赖和数据库软件取得后，无需外网推理服务。
Hugging Face 原站无法访问时，可用固定 revision 的镜像下载，文件完整性校验相同：

```bash
MODEL_DOWNLOAD_ORIGIN=https://hf-mirror.com npm run prepare:model
```

`package-lock.json` 锁定安装结果。`.npmrc` 的 `legacy-peer-deps` 用来避开 seekdb 的可选自动 embedding provider 的 workspace peer 声明；该 provider 不参与本作品。`ignore-scripts` 避免安装只用于服务器推理的 ONNX Runtime Node 二进制及 CUDA 组件；本作品使用浏览器 ONNX Runtime Web，Vite/esbuild 使用平台预编译包。应用只使用 seekdb-js 的 server 模式，嵌入式绑定及默认 embedding provider 均不会加载。

## 验证样例

1. 从空数据目录启动，默认得到“我的资料”INT8 知识库。点击“导入示例资料”，等待提示“索引刷新完成”，数量为 16 段。
2. 输入“怎样让手冲咖啡不那么酸？”。首条应定位到“手冲笔记 · 酸味与萃取”，原文说明水温、研磨度和萃取时间，保留来源和段号。
3. 再次点击“导入示例资料”，段数仍为 16，不能产生重复记录。
4. 新建 `incremental.txt`，内容为“飞船补给柜的取货令牌是蓝鲸7319。离港前用此令牌领取三箱压缩食品。”通过文件选择框导入。数量变为 17；检索“飞船补给柜的取货令牌是什么？”应返回该文件的原文及来源。
5. 点击侧栏“＋”，创建另一知识库，仅导入其他内容。在两个知识库之间切换，结果应只来自当前库。每个知识库的模型精度固定，禁止把 FP32 与 INT8 向量混用。
6. 停止并重新启动服务、刷新页面，原知识库及新增文件仍可检索。首次进入的默认库和后续用户创建的库均存于 seekdb；页面仅用 localStorage 记住上次所选库。

示例语料及八个查询的人工相关性标注在 `sample.json`。内容为原创公开测试数据，产品不读取相关性标注来生成向量或排序。

要复现空状态，可停止服务后指定一个尚未使用的数据目录：

```bash
KB_SQL_DATABASE=near_knowledge_fresh KB_DATA_DIR="$PWD/.data/fresh-demo" npm start
```

应用采用追加导入：相同来源与内容去重；变更过的正文作为新段落追加保留，不提供文档版本替换。单文件最多 500 KiB，一次最多 400 个段落。按段落分割后使用 320 字符窗口、40 字符重叠；推理最大 256 tokens，超长文本按模型 tokenizer 截断，检索结果仍展示存入数据库的完整段落。

## 后端选择与状态

默认“自动：优先 WebGPU”调用真实 `navigator.gpu.requestAdapter()`，可用时显式使用 ONNX WebGPU execution provider，无法创建适配器或模型初始化失败时降级为 WASM。右上角和左侧状态显示实际后端，实验面板给出降级原因。WASM 使用最多四线程；非隔离环境使用单线程。应用发送 COOP/COEP 头，本机 localhost 可使用安全上下文能力；远程浏览器使用 WebGPU 时应提供 HTTPS 或通过 SSH 转发到本机 localhost。

首次模型加载与推理会有等待；后续请求复用同一 Worker 中的模型。INT8 与 FP32 使用相同模型 revision、tokenizer、CLS pooling 和 L2 normalization。查询附加 BGE 中文检索指令，文档不附加。

## 检查与性能复现

```bash
npm test
npx playwright install chromium
npm run test:browser -- --mode wasm
npm run test:browser -- --mode webgpu
npm run test:browser -- --mode auto --quick
```

三条浏览器命令依次执行，需要先保持 `db:start` 启动的真实 seekdb 服务运行。每条命令在该服务中建立独立 SQL 数据库，并自行启动、停止及重启 HTTP 应用服务。测试使用真实模型、浏览器推理、原文导入、数据库原生混合检索和实际服务重启，不使用模型或数据库 mock。

- `wasm` 禁用 WebGPU，比较 FP32 和 INT8；每项八问、三轮，记录 Recall@3 和逐次延迟。
- `webgpu` 显式启用真实 WebGPU API。本机无可用硬件图形适配器时，自动化命令使用 Chromium SwiftShader 软件适配器来验证 WebGPU 路径。报告保留 vendor/architecture，不能把该结果当作硬件 GPU 加速成绩。
- `auto` 在 WebGPU 被禁用的真实浏览器环境中检查自动降级提示和完整流程。
- 另固定 INT8、设备和相同文本，比较 batch size 1 与 8，各三次，保留吞吐和中位数；优化是否更快以实测为准。

产物保存在 `test-results/<时间>-<后端>/`：`report.json`、数据库审计、服务日志、桌面与手机截图。报告附源码及锁文件摘要、模型版本、实际适配器和浏览器版本。`evidence/` 保存本次交付的选定运行记录，性能汇总见 [BENCHMARK.md](BENCHMARK.md)。这组公开小样本不能代表其他语料、设备或比赛平台的正式分数。

## 运行边界

- 这是本机单用户应用，HTTP 默认只监听 loopback。原文和向量会发送到同机 HTTP 服务并存于 seekdb；“端侧”指浏览器推理，不表示不向本地数据库传输资料。
- 应用通过 seekdb-js 的单连接执行 SQL；完整多语句操作串行化，避免共享会话的 `@search_parm` 串扰。关闭应用会正常结束 MySQL 连接，数据库由独立服务管理。
- 数据库连接可用 `SEEKDB_HOST`、`SEEKDB_PORT`、`SEEKDB_USER`、`SEEKDB_PASSWORD` 配置，默认是本机 `127.0.0.1:12891`、用户 root、空密码。SQL 数据库默认为 `near_knowledge`，可以用 `KB_SQL_DATABASE` 指定合法的独立库名；首次启动需要创建该数据库的权限。审计日志位置由 `KB_DATA_DIR` 控制，它不替代 SQL 数据持久化。
- 当前硬件 GPU 性能未经验证；WebGPU 软件适配器只用于功能路径验证。部分浏览器或设备的 WebGPU 算子支持不同，默认自动降级保持可用。

## 技术来源

- [seekdb-js](https://github.com/oceanbase/seekdb-js)，npm `seekdb@1.4.0`，Apache-2.0；使用其 Collection 与原生 hybridSearch，无客户端融合。
- [Transformers.js](https://github.com/huggingface/transformers.js)，`@huggingface/transformers@4.3.1`，Apache-2.0；ONNX Runtime Web 版本由锁文件记录。
- [Xenova/bge-small-zh-v1.5](https://huggingface.co/Xenova/bge-small-zh-v1.5/tree/75c43b069aac4d136ba6bc1122f995fedcfd2781)，原始模型 [BAAI/bge-small-zh-v1.5](https://huggingface.co/BAAI/bge-small-zh-v1.5)，MIT；采用上游提供的动态 INT8 ONNX 权重，不宣称自行训练或量化。
- [官方赛题](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/kw9wfx4bu9pyvat9)。
