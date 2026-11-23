# 实验报告

## 实验基本信息

| 项目 | 内容 |
|------|------|
| **实验名称** | Lab 5: 用户程序 |
| **小组成员** | 2513628-乐嘉文、2511676-孙家昊、2510777-朱万旭 |
| **完成日期** | 待定 |

### 小组分工

| 成员 | 负责的练习/模块 | 负责的实验报告部分 |
|------|----------------|-----------|
| 2513628-乐嘉文 |分支任务：gdb 调试系统调用以及返回  | 实验目的、逻辑分析、内容实现、总结|
| 2511676-孙家昊 | 功能模块三、四、Challenge |  |
| 2510777-朱万旭 |功能模块一、二  | |

---

## 一、实验目的

- 了解第一个用户进程创建过程
- 了解系统调用框架的实现机制
- 了解ucore如何实现系统调用sys_fork/sys_exec/sys_exit/sys_wait来进行进程管理

---

## 二、实验环境

| 成员 | AI 编程工具 | 底层模型 | 备注 |
|------|------------|---------|------|
| 2513628-乐嘉文 | DeepSeek Harness | DeepSeek-V4.1-Flash | |
| 2511676-孙家昊 | Codex | GPT6.1-sol | |
| 2510777-朱万旭 | Deepseek Harness| Deepsee-V4.1-Flash | |

**说明：**
- **AI 编程工具**：指具体使用的终端工具、编辑器插件、桌面应用或浏览器界面
- **底层模型**：指该工具使用的大语言模型及版本

---

## 三、实验整体逻辑分析

### 2.1 本章节的逻辑主线

实验5开始构造用户进程，让用户程序真正运行在用户态，并在需要 ucore 支持时，通过系统调用进入内核获得服务。

### 2.2 功能的逐步实现

本实验需要完成系统调用入口、首个用户进程的创建与装载、用户进程的复制、以及退出、让出和终止等进程控制功能，从而把“启动内核线程”推进到“运行和管理用户程序”的完整阶段。

用户程序先通过系统调用进入内核，内核完成进程管理与虚拟内存管理后，再通过调度把结果送回用户态继续执行。

---

## 四、实验内容与实现

<!-- 
说明：本部分按照 exercises 文档中的练习顺序组织
每个功能模块或者练习中可能包含多项任务，请根据实际情况调整
-->

### 功能模块一：用户态系统调用功能

**负责人：** 2510777-朱万旭


#### 模块功能描述

用户程序通过 `ecall`把请求号和参数放入约定寄存器，`exception_handler` 识别出 `CAUSE_USER_ECALL` 后交给 `syscall`；`syscall` 从当前 `trapframe` 中取出调用号和参数并分发到各 sys_*，由它们完成进程管理、程序替换、等待回收、让出 CPU 等内核语义，最后把结果写回用户寄存器，经异常返回路径回到用户态。最终实现用户态系统调用功能。

**需要实现/修改的函数**

```c
kern/syscall/syscall.c
void syscall(void)
static int sys_exit(uint64_t arg[])
static int sys_fork(uint64_t arg[])
static int sys_wait(uint64_t arg[])
static int sys_exec(uint64_t arg[])
static int sys_yield(uint64_t arg[]) 
static int sys_kill(uint64_t arg[]) 
static int sys_getpid(uint64_t arg[]) 
static int sys_putc(uint64_t arg[])
static int sys_pgdir(uint64_t arg[])

kern/trap/trap.c
void exception_handler(struct trapframe *tf)
```

#### 最终提示词
本模块最终使用的提示词请见[模块一最终提示词.md](模块一最终提示词.md)
---

### 功能模块二：创建加载第一个用户进程功能

**负责人：** 2510777-朱万旭

#### 模块功能描述

本模块实现把系统从内核线程推进到第一个用户程序：`init_main` 创建 `user_main` 并等待其退出，`user_main` 通过 `kernel_execve` 把自身从内核线程转成用户进程；`kernel_execve` 经 ebreak 进入 trap 返回路径，之后由 `do_execve` 回收旧地址空间、`load_icode` 装载 ELF、建立用户页表和栈并配置 trapframe，最后由 sret 回到 U 模式，并由`do_wait` 进行回收。


**需要实现/修改的函数**

```c
kern/process/proc.c
static int init_main(void *arg)  
static struct proc_struct *alloc_proc(void)  
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
int do_wait(int pid, int *code_store)  
static int user_main(void *arg)  
static int kernel_execve(const char *name, unsigned char *binary, size_t size)  
int do_execve(const char *name, size_t len, unsigned char *binary, size_t size)
static int load_icode(unsigned char *binary, size_t size)
```

#### 最终提示词
本模块最终使用的提示词请见[模块二最终提示词.md](模块二最终提示词.md)

---

### 功能模块三：复制用户进程功能

**负责人：** 2511676-孙家昊

---

### 功能模块四：其余系统调用功能

**负责人：** 2511676-孙家昊

---

### 扩展练习 Challenge

**负责人：** 2511676-孙家昊

---
### 分支任务：gdb 调试系统调用以及返回

**负责人：** 2513628-乐嘉文

---

## 五、测试与验证

<!-- 根据该实验的具体情况，提供完整的测试运行截图，应包含：
- 编译及运行成功的输出（make qemu），可能有多个
- 测试通过的结果（make grade），只有一个
-->

## 六、实验总结与收获

### 对操作系统的理解

了解到第一个用户进程创建的过程，系统调用框架的实现机制，以及ucore如何实现系统调用sys_fork/sys_exec/sys_exit/sys_wait来进行进程管理。

---

