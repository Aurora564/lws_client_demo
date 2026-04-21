# clangd 爆红排查手册

## 排查流程

```
nvim 爆红
  │
  ├─ 1. 确认是哪个文件/哪一行报错
  │       → 优先看 "file not found" 类错误，它是下游所有报错的根源
  │
  ├─ 2. 用 gcc 在命令行复现
  │       echo '#include <libwebsockets.h>' | gcc -I<path> -x c - -fsyntax-only
  │       → 命令行能复现 = 路径/依赖问题；不能复现 = clangd 配置问题
  │
  ├─ 3. 检查 clangd 配置
  │       .clangd / compile_commands.json
  │
  └─ 4. 检查系统依赖
          头文件的 #include 可能引入其他系统头文件
```

---

## 本次遇到的问题与原因

### 问题一：`<cstring>` 在 C 文件中报错

**现象**：`ws_client.h` 里写了 `#include <cstring>`，clangd 报错。  
**原因**：`<cstring>` 是 C++ 标准库头文件，C 文件只能用 `<string.h>`。  
**修复**：
```c
// 错误
#include <cstring>

// 正确（C 文件）
#include <string.h>
```

---

### 问题二：`#include <ws_client.h>` 找不到本地头文件

**现象**：`ws_client.c` 用尖括号引用本地头文件，clangd 和编译器都报 not found。  
**原因**：尖括号 `<>` 只搜索系统路径和 `-I` 指定的路径；引号 `""` 优先搜索当前文件所在目录。  
**修复**：
```c
// 错误：本地头文件用尖括号
#include <ws_client.h>

// 正确：本地头文件用引号
#include "ws_client.h"
```

---

### 问题三：`.clangd` 相对路径对 clangd 不可靠

**现象**：`.clangd` 配置了 `-I./output/include`，但 clangd 仍找不到头文件。  
**原因**：clangd 对 `CompileFlags.Add` 中相对路径的解析行为不稳定，在某些版本中
相对路径基准不是 `.clangd` 所在目录。  
**修复**：改用绝对路径。

```yaml
# 不可靠
CompileFlags:
  Add:
    - -I./output/include

# 可靠
CompileFlags:
  Add:
    - -I/home/aurora/project/lws_client_demo/output/include
```

---

### 问题四：`.clangd` 对无构建系统的文件不够可靠

**现象**：`.clangd` 路径正确，但新建的 `reference/ws_client.c` 仍然爆红。  
**原因**：clangd 优先读取 `compile_commands.json`；对没有出现在构建数据库里的文件，
`CompileFlags` 有时不会生效。  
**修复**：创建 `compile_commands.json` 明确列出每个文件的编译命令。

```json
[
  {
    "directory": "/home/aurora/project/lws_client_demo",
    "file": "src/ws_client.c",
    "command": "gcc -std=c99 -I/home/aurora/.../output/include -c src/ws_client.c"
  }
]
```

> **优先级**：`compile_commands.json` > `.clangd` CompileFlags

---

### 问题五：libwebsockets.h 的传递依赖缺失

**现象**：`compile_commands.json` 和路径都正确，clangd / gcc 仍报  
`sys/capability.h: No such file or directory`。  
**原因**：`libwebsockets.h` 内部 `#include <sys/capability.h>`，该文件由
`libcap-devel` 包提供，未安装时整条 include 链断掉。  
**诊断命令**：
```bash
echo '#include <libwebsockets.h>' | gcc -I<path> -x c - -fsyntax-only
# 看报错是 libwebsockets.h 本身，还是它 include 的某个系统头
```
**修复**：
```bash
sudo dnf install libcap-devel   # Fedora / RHEL
# sudo apt install libcap-dev   # Debian / Ubuntu
```

---

## clangd 生效步骤

修改 `.clangd` 或 `compile_commands.json` 后，必须重启 clangd：

```
# nvim 内
:LspRestart

# 或直接退出重新打开
```

---

## 总结：排查优先级

| 步骤 | 检查项 |
|------|--------|
| 1 | 代码本身：C/C++ 头文件区分、引号 vs 尖括号 |
| 2 | 系统依赖：`gcc -fsyntax-only` 复现，看缺什么系统包 |
| 3 | clangd 配置：优先用 `compile_commands.json`，路径用绝对路径 |
| 4 | clangd 重启：改完配置必须 `:LspRestart` |
