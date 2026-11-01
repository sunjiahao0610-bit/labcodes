[PROMPT]

任务：
修改 code/kern/trap/trap.c 中的 interrupt_handler() 和 exception_handler()，
完成中断分类、持续时钟中断处理、非法指令与断点异常处理，以及其他
Trap 类型的诊断输出，使处理结果能够正确衔接已有的 Trap 返回流程。

操作要求：
1. 必须修改项目中的实际文件，不能仅展示代码片段。
2. 唯一允许修改的文件是 code/kern/trap/trap.c。
3. 唯一允许修改的范围是 interrupt_handler() 和 exception_handler()
   两个函数的函数体，不得修改函数名称、参数或返回类型。
4. 保留全部原有注释，包括中文说明、英文说明、被注释的代码以及
   LAB3、YOUR CODE 等标记。不得删除、改写或移动原注释。
5. 不得修改这两个函数以外的任何内容，包括头文件包含、文件级宏、
   文件级变量、print_ticks()、print_trapframe()、trap_dispatch()、
   trap() 等。
6. 可以在这两个函数内部增加必要的局部变量、局部常量、局部静态变量
   和函数声明；不得新增辅助函数，不得新增文件级变量或宏。
7. 允许阅读其他文件以核对接口和测试要求，但不得修改其他文件。
8. 保持原文件的编码和换行格式，避免产生无关格式差异。

输出要求：
以真实工程代码为准，使用下列 [RELY]、[GUARANTEE] 和
[SPECIFICATION] 完成实现。完成后简要说明修改内容、验证结果及实际
未覆盖的验证项目，不得将未执行的测试描述为通过。


[RELY]

1. 运行环境与 Trap 流程

当前项目使用 RISC-V 64 位架构。

已有的 trap_dispatch() 根据 tf->cause 的最高位区分中断与异常：
最高位为 1 时调用 interrupt_handler()，否则调用 exception_handler()。

两个处理函数返回后，已有汇编代码从 Trap frame 恢复现场，并执行
sret。因此，本模块应通过修改 tf->epc 调整异常返回位置，不应自行
执行 sret，也不应直接操作当前栈来完成返回。

2. 数据结构

code/kern/trap/trap.h 中已有：

struct trapframe {
    struct pushregs gpr;
    uintptr_t status;
    uintptr_t epc;
    uintptr_t badvaddr;
    uintptr_t cause;
};

其中：
- cause：保存 scause，用于判断 Trap 类型。
- epc：保存异常或中断的返回位置；修改它会影响后续恢复的 sepc。
- gpr、status、badvaddr：本任务不要求修改。

uintptr_t 在当前项目中为 64 位无符号整数。
工程还提供 uint16_t 等整数类型。

3. 已有宏和变量

code/kern/trap/trap.c：
#define TICK_NUM 100

code/kern/driver/clock.h：
extern volatile size_t ticks;

code/libs/riscv.h：
#define IRQ_S_TIMER 5
#define CAUSE_ILLEGAL_INSTRUCTION 0x2
#define CAUSE_BREAKPOINT 0x3

其他 IRQ_*、CAUSE_* 常量以 riscv.h 中的真实定义为准，
使用已有常量，不要自行重定义。

当前版本没有定义 SHUTDOWN_AFTER。
原有实验注释要求累计打印 10 次后关机，因此在 interrupt_handler()
函数体内部定义值为 10 的局部常量 SHUTDOWN_AFTER，例如局部枚举常量；
不得在函数外增加宏。

4. 可依赖的函数

void clock_set_next_event(void);
void print_trapframe(struct trapframe *tf);
int cprintf(const char *fmt, ...);

trap.c 内已有静态函数 print_ticks()：
正常构建时输出：
    "%d ticks\n", TICK_NUM

定义 DEBUG_GRADE 时，该函数还包含已有的测试终止行为。
允许调用它，但不得修改它。

code/libs/sbi.h 中的关机接口为：
void sbi_shutdown(void);

当前 trap.c 没有包含 sbi.h。由于禁止修改函数外的内容，应在
interrupt_handler() 函数体内部增加与真实头文件一致的声明：
    extern void sbi_shutdown(void);
不得修改文件顶部的 include，也不得依赖隐式函数声明。

5. 现有测试约束

code/tools/grade.sh 会检查以下输出关键字：
- "Breakpoint at"
- "Illegal instruction at"
- "Supervisor software interrupt"
- "Unknown interrupt: cause 15"
- "100 ticks"

未知异常测试会通过 print_trapframe() 检查 cause 为 20 的现场输出。

code/tools/test_lab3.c 使用：
    .word 0x0000007b
触发一条 4 字节的非法指令异常。

断点处理必须能够正确处理本实验涉及的两种指令：
- 2 字节压缩断点指令 c.ebreak。
- 4 字节断点指令 ebreak。

测试还会直接调用 interrupt_handler()，并传入 cause 为 15、
没有最高位中断标志的测试现场。中断原因提取应兼容这种调用。

这些测试信息只用于确定行为和验收标准，不得修改测试文件或伪造
测试通过信息。


[GUARANTEE]

必须实现或修改的接口：

void interrupt_handler(struct trapframe *tf);
void exception_handler(struct trapframe *tf);

interrupt_handler() 必须完成：
- 中断原因提取与分类。
- 时钟中断的重新设置、累计计数、定期输出和达到阈值后的关机。
- 其他已知中断的类型提示。
- 未知中断的原因提示与完整 Trap frame 输出。

exception_handler() 必须完成：
- 非法指令和断点异常的识别。
- 按规定格式输出触发异常的原指令地址。
- 根据实际指令长度调整 tf->epc。
- 其他异常的完整 Trap frame 输出。

允许的新增内容仅限于两个函数体内必要的局部声明和处理逻辑。
不得新增辅助函数，不得修改其他接口。


[SPECIFICATION]

interrupt_handler(struct trapframe *tf)

Pre-Condition：
- tf 指向有效、可读取的 Trap frame。
- 正常调用时，tf->cause 表示一个中断。
- 允许测试代码直接传入已经不包含最高位中断标志的原因编号。
- 时钟及 Trap 入口已由现有初始化流程配置。
- ticks 已由 clock_init() 初始化为 0。

Post-Condition：
- 根据去除最高位中断标志后的原因编号执行对应处理。
- 除达到关机条件的路径外，函数正常返回已有的 Trap 返回流程。
- 不修改 tf->epc、tf->cause 或其他保存的现场字段。
- 只有时钟中断能够更新 ticks 和时钟统计输出次数。

Case 1：S 态时钟中断 IRQ_S_TIMER
每收到一次时钟中断，必须先设置下一次时钟事件，再将 ticks 增加 1。
不能只增加计数而不重新设置定时器。

当递增后的 ticks 为 TICK_NUM 的整数倍时，调用已有统计输出功能，
输出一行“100 ticks”，并将累计输出次数增加 1。

累计输出次数必须跨函数调用保存，不能在每次进入函数时重置。
当累计输出次数达到 SHUTDOWN_AFTER，即 10 次时，调用 sbi_shutdown()。

在正常构建下：
- 第 1～99 次时钟中断不打印统计行。
- 第 100 次打印第 1 行。
- 第 200 次打印第 2 行。
- 第 1000 次打印第 10 行，然后关机。

ticks 必须保持累计值，不得在每打印一次后清零。
重新设置时钟、增加 ticks、打印统计、累计输出次数和关机判断之间
不得发生 switch 穿透或重复执行。

Case 2：其他已知中断
对原 switch 已列出的软件中断、其他权限级时钟中断和外部中断，
输出与其权限级和类型一致的提示后返回。

保留正确的已有提示，例如：
    "Supervisor software interrupt\n"

修正明显不符合中断类型的提示，例如不能把时钟中断或外部中断
输出为 software interrupt。

这些分支不增加 ticks，不设置下一次时钟事件，也不触发统计关机。

Case 3：未知中断
先输出去除最高位标志后的原因编号，格式为：
    "Unknown interrupt: cause %lu\n"

传入与 %lu 匹配的 unsigned long 参数，使原因编号为 15 时输出：
    "Unknown interrupt: cause 15"

随后调用一次 print_trapframe(tf)，输出完整现场，再正常返回。
不得修改原始 tf->cause，也不得用简短提示替代完整现场输出。

Requirements：
- 复用现有中断原因提取方式，或者使用等价且不会引入符号扩展问题
  的无符号运算。
- 关机阈值、累计输出计数和关机接口声明只能放在函数内部。
- 保留 print_ticks() 的既有行为；上述周期输出和关机验收使用未定义
  DEBUG_GRADE 的正常构建。
- 不引入调度、设备控制器初始化或其他超出本实验范围的功能。


exception_handler(struct trapframe *tf)

Pre-Condition：
- tf 指向有效的 Trap frame。
- tf->cause 表示同步异常原因。
- 对本实验要求恢复执行的非法指令和断点异常，tf->epc 指向可读取的
  指令地址；本实验要求覆盖 16 位和 32 位指令。
- 对其他异常，不假定 tf->epc 一定可读取或非零。

Post-Condition：
- 非法指令和断点异常输出正确的类型及原始指令地址。
- 对本实验支持的指令长度，将 tf->epc 更新到该指令之后的位置，
  使正常 Trap 返回不会再次执行同一条异常指令。
- 其他异常输出完整现场，保持 tf->epc 不变，不擅自实现恢复策略。
- 除规定需要更新的 epc 外，不修改 Trap frame 的其他字段。

Case 1：非法指令异常 CAUSE_ILLEGAL_INSTRUCTION
先保存并输出更新前的 tf->epc，格式为：
    "Illegal instruction at 0x%016lx\n"

地址参数转换为 unsigned long，保留当前 RV64 地址的完整宽度。

根据异常位置的实际指令编码判断长度，将 tf->epc 前移对应字节数。
现有测试中的 .word 0x0000007b 必须按 4 字节处理，返回后能够继续
执行后续测试代码。

Case 2：断点异常 CAUSE_BREAKPOINT
先输出更新前的 tf->epc，格式为：
    "Breakpoint at 0x%016lx\n"

地址参数转换为 unsigned long。

根据实际断点指令编码确定长度：
- c.ebreak：将 tf->epc 增加 2。
- ebreak：将 tf->epc 增加 4。

不得将所有断点统一按 2 字节或统一按 4 字节处理。

Case 3：其他异常
除非法指令和断点异常外，所有其他异常均调用一次
print_trapframe(tf)，然后返回。

该要求既适用于 default 分支，也适用于当前 switch 中已经列出、
但原本只有 break 的异常分支，例如访存异常和各类 ECALL。

不修改这些异常的 tf->epc，不读取其指令地址，不增加本实验未要求
的缺页恢复、系统调用分发或其他恢复逻辑。

Requirements：
- 仅在确实需要识别指令长度的非法指令和断点分支读取指令内容，
  避免未知异常测试中 epc 为 0 时发生额外访问异常。
- 优先读取指令起始的 16 位半字，不要为了判断长度而直接进行
  可能未对齐或跨边界的 32 位读取。
- 按 RISC-V 编码识别本实验涉及的长度：
  起始半字最低两位不为二进制 11 时，为 16 位指令；
  最低两位为 11 且第 4～2 位不全为 1 时，为 32 位指令。
  本任务不要求扩展到 48 位及以上指令，不能声称实现了任意长度解码。
- 必须先使用原 epc 输出异常位置，再更新 epc。
- 修改的是 tf->epc，由既有返回汇编写回 sepc；不直接操作 sepc
  代替修改 Trap frame。
- 不修改两个函数的原注释，不修改任何函数外内容。

验证要求：
- 检查最终差异，确认只有 trap.c 中这两个函数体发生变化，
  原注释完整保留，文件其他部分未被改动。
- 在临时目录的项目副本中执行已有构建和 make grade，避免为验证
  改动其他项目文件。
- 验证断点返回、非法指令返回、未知类型兜底，以及时钟计数达到 200
  的现有测试。
- 使用正常启动验证连续输出 10 次“100 ticks”后由 SBI 正常关机。
- 如补充指令长度验证，应在临时副本中进行，不得向项目新增测试文件。
- 如工具链或运行环境缺失，准确说明无法执行的验证，不得伪报通过。