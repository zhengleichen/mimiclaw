# ESP32 结合大模型实现自然语言控制硬件教程

## 一、 文档目的
本教程旨在指导开发者使用 ESP32 开发板，通过接入大语言模型（LLM）和飞书（Feishu），实现自然语言控制硬件的功能。
例如：在飞书中发送“帮我开灯”或“将舵机旋转90度”，大模型理解意图后，自动调用 ESP32 上的底层工具（Tool）控制相应的 GPIO 引脚，从而完成特定的物理操作。

## 二、 详细技术原理
1. **消息接收 (Feishu WebSocket)**: ESP32 通过 WebSocket 长连接监听飞书机器人的消息事件，无需公网 IP 即可接收用户发送的自然语言指令。
2. **大模型代理循环 (Agent Loop)**: ESP32 收到消息后，会将其发送给大语言模型（如 Anthropic Claude 或 字节豆包等）。MimiClaw 实现了 ReAct 代理循环协议（Tool Use Protocol）。
3. **工具注册与意图识别 (Tools & Skills)**: 
   - **Tool (工具)**：在 ESP32 的 C 代码中，将底层的 GPIO 操作（如高低电平控制、PWM 脉冲等）封装成带有 JSON Schema 描述的函数，并注册到大模型可用的工具列表中。
   - **Skill (技能)**：在 MimiClaw 中，Skill 是“软定义”的，通过在系统提示词文件（如 `SOUL.md`）中定义特定的“技能”，赋予大模型理解特定自然语言并将其映射到具体 Tool 参数的能力。
4. **工具执行与反馈**: 大模型分析出意图后，返回一个 `tool_use` 请求。ESP32 解析该请求，调用本地 C 函数执行 GPIO 操作，将结果作为 `tool_result` 返回给大模型。大模型最终生成一段友好的回复（例如“灯已经为您打开啦”），通过飞书发送给用户。

## 三、 硬件与软件准备
### 1. 硬件准备
- **电脑**：Windows 系统，用于编译和烧录代码。
- **硬件开发板**：ESP32-S3 开发板（建议 16MB Flash + 8MB PSRAM，例如小智 AI 开发板等）。
- **外设与数据线**：Type-C 数据线（用于供电和烧录），LED 灯、舵机等外接设备（连接至特定的 GPIO 引脚）。

### 2. 软件准备
- **ESP-IDF**：乐鑫官方开发框架（建议 v5.5+ 版本）。
- **MimiClaw 项目源码**：专为 ESP32 打造的纯 C 语言大模型 Agent 框架。
- **飞书开发者账号**：用于创建企业自建机器人应用。
- **大模型 API Key**：例如 Anthropic API Key 或 OpenAI API Key（MimiClaw 默认支持）,可以自行拓展其他大模型，更换大模型的BASE_URL即可，我用的是火山引擎的大模型。

## 四、 具体实现步骤

### 步骤 1：配置与烧录 MimiClaw
1. **获取代码并设置环境**：
   ```bash
   git clone https://github.com/memovai/mimiclaw.git
   cd mimiclaw
   idf.py set-target esp32s3
   ```
2. **配置密钥**：
   复制配置文件模板：
   ```bash
   cp main/mimi_secrets.h.example main/mimi_secrets.h
   ```
   在 `main/mimi_secrets.h` 中填入你的 WiFi 账号、大模型 API 密钥以及飞书的 App ID 和 App Secret。
3. **编译并烧录**：
   连接开发板到电脑，运行以下命令编译、烧录并打开串口监视器：
   ```bash
   idf.py build flash monitor
   ```

### 步骤 2：在程序中编写 Tool 工具调用 GPIO
MimiClaw 已经内置了非常强大的 `gpio` 工具（详见 `main/tools/tool_gpio.c`）。如果你需要自定义其他硬件控制（如专用的舵机 PWM 控制），可以参考以下步骤：
1. **定义执行函数**：在 `main/tools/` 目录下创建你的 C 函数，解析大模型传入的 JSON 参数并调用 ESP-IDF 的底层 API（如 `gpio_set_level`）。
   ```c
   // 示例：执行 GPIO 控制的核心逻辑
   esp_err_t tool_gpio_execute(const char *input_json, char *output, size_t output_size) {
       // 1. 解析 input_json 获取 action, pin, level
       // 2. 调用 esp-idf API: gpio_set_level((gpio_num_t)pin, level);
       // 3. 将执行结果写入 output 缓冲返回
   }
   ```
2. **注册工具与 Schema**：在 `main/tools/tool_registry.c` 中，定义工具的名称、描述和 JSON Schema，以便大模型知道该如何调用它：
   ```c
   mimi_tool_t gpio = {
       .name = "gpio",
       .description = "Control GPIO pins. Supports config_output, set_level, toggle, pulse...",
       .input_schema_json = "{\"type\":\"object\", \"properties\":{\"action\":{\"type\":\"string\"},\"pin\":{\"type\":\"integer\"},\"level\":{\"type\":\"integer\"}},\"required\":[\"action\",\"pin\"]}",
       .execute = tool_gpio_execute,
   };
   register_tool(&gpio);
   ```

### 步骤 3：编写 Skill 调用 Tool
在当前的纯 C 架构下，**Skill 主要是通过系统提示词（System Prompt）来实现的**。
MimiClaw 会在开发板的 Flash（SPIFFS）中存放一个 `/spiffs/config/SOUL.md` 文件作为大模型的人设和技能设定。你可以向大模型传授特定的“技能”映射：
1. 通过串口 CLI 或预置文件修改 `SOUL.md`。
2. 增加如下自然语言指令与工具调用的映射（Skill 设定）：
   ```markdown
   # 你的硬件控制技能 (Skills)
   当用户让你“开灯”或“关灯”时，你知道连接灯的 GPIO 引脚是 2。
   - 开灯：调用 `gpio` tool，action 为 `config_output`，然后调用 `set_level`，pin 为 2，level 为 1。
   - 关灯：调用 `gpio` tool，action 为 `set_level`，pin 为 2，level 为 0。
   当用户让你“旋转舵机”时，根据角度换算成脉冲时间，并调用对应的 tool。
   ```
   有了这个 Skill 说明，大模型就能精准地将用户的口语转化为特定的工具参数。

### 步骤 4：连接飞书
1. 登录 **飞书开放平台** (open.feishu.cn/app)，创建企业自建应用。
2. **启用机器人能力**：在“添加应用能力”中开启机器人。
3. **添加权限**：在权限管理中添加 `im:message` (发送消息) 和 `im:message.receive_v1` (接收消息) 权限。
4. **启用长连接**：在“事件订阅”中，将订阅方式选为 **长连接 (WebSocket)**，并添加接收消息事件。这一步至关重要，它使得 ESP32 即使在内网也能实时接收飞书消息。
5. **发布应用** 并获取 `App ID` 与 `App Secret`，将其填入 `mimi_secrets.h` 或通过 ESP32 串口运行配置指令：
   ```text
   mimi> set_feishu_id cli_xxxxxxxxxxxxxxxx
   mimi> set_feishu_secret xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
   mimi> restart
   ```

### 步骤 5：通过飞书发自然语言实现特定意图 (端到端流程)
1. 在飞书客户端中找到你配置好的机器人。
2. 发送自然语言指令：“**帮我把开发板上的灯打开**”。
3. **系统处理流**：
   - 飞书通过 WebSocket 将该文本推送给 ESP32 开发板。
   - ESP32 提取文本，连同 `SOUL.md` 里的 Skill 提示词一起打包成请求发送给大模型 API。
   - 大模型经过推理，匹配到“开灯”技能，通过 Tool Use 协议返回一段 JSON：`{"name": "gpio", "input": {"action": "set_level", "pin": 2, "level": 1}}`。
   - ESP32 的 Agent Loop 捕获到工具调用，触发 `tool_gpio_execute`，拉高 GPIO 2 的电平，开发板上的 LED 灯随即亮起。
   - ESP32 将执行成功的状态返回给大模型。
   - 大模型根据成功结果，生成回复：“好的，已经为您打开了灯！”
   - ESP32 收到最终回复，通过飞书 WebSocket 发回给用户。
   
至此，一个从云端自然语言到底层硬件控制的完整 AI 代理闭环就实现了！
