# ha-ra

`ha-ra` 是一个面向 Renesas RA 项目的本地 `stdio` MCP 服务器。

它的目标很直接：让 Codex 这类 MCP 客户端能直接接入你的 e2 Studio RA 工程，完成项目识别、构建、产物分析、内存占用分析、刷写，以及调试后端启动。

当前版本已经按本地真实 RA 工程验证过 `pyOCD` 路径。`J-Link` 和 `E2 / E2 Lite` 的后端类型已经能识别，但还没有做完自动化刷写。

## 这个 MCP 能做什么

- 扫描 e2 Studio 工作区里的 RA 工程
- 解析 `.cproject`、`.project`、`configuration.xml`、`.launch`
- 识别 Eclipse 项目名和目录名不一致的情况
- 构建工程
  - 支持 `e2studioc`
  - 支持 `make`
- 解析 GNU Arm 编译报错和警告
- 找出当前构建产物
  - 例如 `.elf`、`.axf`、`.hex`、`.bin`、`.srec`、`.map`
- 分析内存占用
  - 使用 `memory_regions.ld`
  - 使用 `arm-none-eabi-objdump -h`
  - 计算 `ROM / RAM / Data Flash` 占用
- 按 `.launch` 配置刷写固件
  - 当前已实现并验证 `pyOCD`
- 启动、停止、查询调试后端
  - 当前已实现 `pyocd-gdbserver`

## 当前适合谁用

如果你满足下面这些条件，这个 MCP 基本就是为你准备的：

- 你在 Windows 上开发 Renesas RA 工程
- 你用 e2 Studio 管理工作区和 `.launch`
- 你希望 Codex 直接参与固件开发流程，而不是只做静态代码分析
- 你愿意让 MCP 读取本机的 e2 Studio、GNU Arm、`pyOCD`、`DebugComp/RA` 环境

## 当前限制

- `pyOCD` 已经打通并做过本机验证
- `J-Link` 和 `E2 / E2 Lite` 目前只做到后端识别，还没有完成自动化刷写
- `ADM` / 虚拟串口日志目前没有做 RA 支持
- 还不是完整的“单步调试控制器”，当前更偏向“启动调试后端 + 交给后续工具接管”

## MCP 工具

目前对外暴露的主要工具有：

- `list_projects`
- `get_project_config`
- `build_project`
- `clean_project`
- `rebuild_project`
- `get_build_status`
- `get_build_artifacts`
- `get_build_size`
- `get_map_summary`
- `get_linker_sections`
- `flash_firmware`
- `debug_start`
- `debug_stop`
- `debug_status`

## 推荐使用流程

推荐按下面顺序使用：

1. 先用 `list_projects` 确认工作区里有哪些工程
2. 用 `get_project_config` 看项目名、设备、配置、产物路径
3. 用 `build_project` 构建
4. 用 `get_build_artifacts` 确认当前可用产物
5. 用 `get_build_size` 或 `get_map_summary` 看内存占用
6. 用 `flash_firmware` 刷写
7. 需要时用 `debug_start` 启动调试后端

## 工作原理

这个项目现在主要走 RA 路径：

- 从 `.cproject` 读取构建配置、工具链、包含路径、宏定义
- 从 `.project` 读取 Eclipse 项目名
- 从 `configuration.xml` 和 FSP 生成头文件补足 RA 设备信息
- 从 `.launch` 读取调试后端、程序路径、目标芯片、`pyOCD` 参数
- 从构建目录里的 `memory_regions.ld` 和 ELF 节区头计算内存占用

这意味着：

- 你不需要手工重复告诉 MCP 工程配置
- MCP 会尽量跟着 e2 Studio 的项目状态走
- 如果 `.launch` 里写了机器相关绝对路径，这些路径也会被继承

## 环境要求

- Windows
- Python 3.11 或更高版本
- Renesas e2 Studio for RA
- GNU Arm Embedded toolchain
- `pyOCD`
- 一个包含 RA 工程的 e2 Studio 工作区

## 本机安装

### 1. 克隆仓库

```powershell
git clone <your-private-repo-url> ha-ra
cd ha-ra
```

### 2. 安装 Python 包

推荐直接装成 editable：

```powershell
python -m pip install -e .
```

如果你只想本地直接跑，也可以不安装包，而是用：

```powershell
$env:PYTHONPATH = "src"
python -m e2studio_mcp.server
```

### 3. 检查本机关键路径

你至少需要确认这些路径：

- e2 Studio `eclipse` 目录
- GNU Arm `bin` 目录
- `DebugComp/RA` 目录
- `pyocd.exe` 所在目录
- e2 Studio 工作区目录

### 4. 启动 MCP 服务

```powershell
$env:PYTHONPATH = "src"
$env:E2MCP_PLATFORM = "ra"
$env:E2MCP_WORKSPACE = "C:\path\to\your\e2studio\workspace"
$env:E2MCP_BUILD_CONFIG = "Debug"
$env:E2MCP_E2STUDIO_PATH = "C:\Renesas\RA\...\eclipse"
$env:E2MCP_GCC_ARM_PATH = "C:\Renesas\RA\...\toolchains\gcc_arm\...\bin"
$env:E2MCP_RA_DEBUGCOMP_PATH = "C:\Users\<you>\.eclipse\com.renesas.platform_xxx\DebugComp\RA"
$env:E2MCP_PYOCD_PATH = "C:\Users\<you>\AppData\Roaming\Python\Python311\Scripts"
python -m e2studio_mcp.server
```

如果你想给一个默认工程：

```powershell
$env:E2MCP_PROJECT = "your_project_dir_name"
```

## 在 Codex 里配置

如果你用 Codex 本地配置文件，可以加一个类似下面的段落：

```toml
[mcp_servers."ha-ra"]
type = "stdio"
command = 'C:\Path\To\Python\python.exe'
args = ["-m", "e2studio_mcp.server"]
cwd = 'C:\Path\To\ha-ra'
startup_timeout_sec = 90.0
tool_timeout_sec = 300.0

[mcp_servers."ha-ra".env]
PYTHONPATH = "src"
E2MCP_PLATFORM = "ra"
E2MCP_WORKSPACE = 'C:\Path\To\e2studio\workspace'
E2MCP_BUILD_CONFIG = "Debug"
E2MCP_E2STUDIO_PATH = 'C:\Renesas\RA\...\eclipse'
E2MCP_GCC_ARM_PATH = 'C:\Renesas\RA\...\toolchains\gcc_arm\...\bin'
E2MCP_RA_DEBUGCOMP_PATH = 'C:\Users\<you>\.eclipse\com.renesas.platform_xxx\DebugComp\RA'
E2MCP_PYOCD_PATH = 'C:\Users\<you>\AppData\Roaming\Python\Python311\Scripts'
```

如果你希望默认进入某个工程，再加：

```toml
E2MCP_PROJECT = "your_project_dir_name"
```

## 在别的电脑上怎么部署

把下面这套流程照着做就行。

### 1. 装基础环境

新电脑上先准备好：

- e2 Studio RA
- Python
- `pyOCD`
- 你的 RA 工程工作区

### 2. 克隆仓库

```powershell
git clone <your-private-repo-url> ha-ra
cd ha-ra
python -m pip install -e .
```

### 3. 找到新电脑对应的本地路径

最常改的是这几个：

| 变量 | 你要换成什么 |
|---|---|
| `E2MCP_WORKSPACE` | 新电脑上的 e2 Studio 工作区路径 |
| `E2MCP_E2STUDIO_PATH` | 新电脑上的 `...\eclipse` 路径 |
| `E2MCP_GCC_ARM_PATH` | 新电脑上的 GNU Arm `bin` 路径 |
| `E2MCP_RA_DEBUGCOMP_PATH` | 新电脑上的 `DebugComp\RA` 路径 |
| `E2MCP_PYOCD_PATH` | 新电脑上的 `pyocd.exe` 所在目录 |
| `E2MCP_PROJECT` | 你希望默认打开的工程目录名，可选 |
| `E2MCP_BUILD_CONFIG` | 通常是 `Debug` 或 `Release` |

### 4. 检查 `.launch` 里的绝对路径

这是跨电脑部署时最容易踩坑的地方。

有些 RA 工程的 `.launch` 文件里会直接写死本机绝对路径，例如：

```xml
<stringAttribute
  key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerOther"
  value="--config C:\Users\user\RA_PYOCD\pyocd.yaml --connect under-reset"/>
```

如果你把工程拿到另一台电脑，这个路径通常会失效。

你需要做其中一种：

1. 在新电脑上把这个路径改成新机器自己的 `pyocd.yaml`
2. 在 e2 Studio 里重新保存对应的 Debug 配置
3. 如果你的项目不依赖这个额外配置，删掉 `--config ...`

### 5. 验证部署是否成功

新电脑上建议先做这几个检查：

```powershell
$env:PYTHONPATH = "src"
$env:E2MCP_PLATFORM = "ra"
$env:E2MCP_WORKSPACE = "C:\Path\To\e2studio\workspace"
python -c "from e2studio_mcp.config import load_config; c = load_config(); print(c.platform); print(c.toolchain.e2studio_path); print(c.toolchain.gcc_arm_path)"
```

然后再让 MCP 客户端依次调用：

1. `list_projects`
2. `get_project_config`
3. `build_project`
4. `get_build_artifacts`

如果这四步都通了，再继续刷写。

## 典型问题

### 找不到工程

先检查：

- `E2MCP_WORKSPACE` 是否指向正确工作区
- 工程目录下是否存在 `.cproject`
- 工程目录名和 `.project` 里的 Eclipse 项目名是否一致

### 找不到构建产物

先检查：

- 是否真的已经构建成功
- `.launch` 的 `PROGRAM_NAME` 是否指向当前存在的 `.elf`
- `.cproject` 里的 `buildPath` 是否还是旧工程残留值

### 刷写时报 `pyOCD` 相关错误

先检查：

- `pyocd.exe` 是否可执行
- `.launch` 里 `gdbServerOther` 的 `--config` 路径是否有效
- `gdbServerTargetName` 是否和目标芯片匹配
- 板卡 / probe 是否被系统正确识别

### 内存分析结果不对

先检查：

- 当前分析的是不是正确的 `.elf`
- 构建目录里是否存在 `memory_regions.ld`
- 工程是否确实使用 FSP 生成的链接脚本

## 和当前本机环境对应的已验证配置

这套路径在当前机器上已验证可用：

- `E2MCP_E2STUDIO_PATH = C:\Renesas\RA\e2studio_v2023-04_fsp_v4.5.0\eclipse`
- `E2MCP_WORKSPACE = C:\Users\user\e2_studio\workspace`
- 示例工程目录：`ra6m5_eeg_imu_y2`
- 示例 launch：`ra6m5_eeg_imu_y2 Debug.launch`
- 已验证后端：`pyOCD`

## 相关文档

- [docs/CODEX_RA_SETUP.md](docs/CODEX_RA_SETUP.md)
- [docs/plans/2026-04-23-ra-codex-design.md](docs/plans/2026-04-23-ra-codex-design.md)

## 当前状态总结

如果你问“这个 MCP 现在最适合做什么”，答案是：

- 让 Codex 理解你的 RA 工程
- 让 Codex 直接构建和分析固件
- 让 Codex 按 `.launch` 走 `pyOCD` 刷写链路
- 为后续补齐 `J-Link / E2 Lite` 打基础
