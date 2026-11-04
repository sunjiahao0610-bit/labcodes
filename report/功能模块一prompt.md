[PROMPT]
1. 任务：补全 kern/trap/trapentry.S 中的 RESTORE_ALL 和 __alltraps，形成“trap 进入 → 保存现场 → C 语言处理 → 恢复现场 → trap 返回”的完整流程。
2. 操作要求：直接修改项目实际文件中的对应实验标记处；不得修改 SAVE_ALL、struct trapframe、trap() 的函数签名及其他现有逻辑。
3. 输出要求：依据以下上下文和规格实现，简述关键设计并报告验证结果。

[RELY]
1. kern/trap/trapentry.S：SAVE_ALL 提供实际保存关系，__trapret 已提供 RESTORE_ALL 与 sret 的返回流程。
2. kern/trap/trap.h：struct trapframe 包含 gpr、status、epc、badvaddr、cause；gpr 按 x0～x31 顺序组织。具体布局以工程定义为准。
3. SAVE_ALL 完成后，当前 sp 指向trapframe；其中 x2 对应槽位保存的是分配 trapframe 之前的旧 sp，因此有特殊的处理逻辑
4. libs/riscv.h：提供 REGBYTES、LOAD、STORE 及相关 CSR 定义。
5. C 处理接口：void trap(struct trapframe *tf)。
6. RISC-V 调用约定：a0 传递第一个指针参数，ra 保存过程调用返回地址。
7. Trap 返回机制：sret 根据 sepc 和 sstatus 恢复执行位置、返回特权级及中断状态。

[GUARANTEE]
1. RESTORE_ALL：实现从当前 trapframe 恢复处理器执行状态的汇编宏。
2. __alltraps：完成现场保存及 trap(tf) 调用，返回后衔接已有 __trapret。

[SPECIFICATION]
RESTORE_ALL

Pre-Condition：
1. sp 指向 SAVE_ALL 建立的有效的trapframe。
2. trap() 已返回，trapframe包含处理后的最终上下文。

Post-Condition：
1. 通用寄存器及返回所需的 sstatus、sepc 按 trapframe 的最终内容恢复。
2. 后续 sret 能返回该上下文指定的执行流。

Requirements：
1. 逐项核对 SAVE_ALL 与结构体布局，保持恢复槽位、寄存器对应关系及访存宽度一致。
2. x2 单独处理：恢复保存的旧 sp，并安排在所有依赖当前trapframe基址的读取之后；x0 保持恒零。
3. 借用通用寄存器搬运 CSR 时，先完成搬运，再恢复该寄存器的最终值。
4. cause、badvaddr 用于诊断，无需写回对应 CSR。
5. 使用最终 epc，不在恢复阶段增加指令偏移；返回状态遵循最终 status 和 sret 语义。

__alltraps

Pre-Condition：
1. 处理器已进入 S 模式 trap 入口，当前内核栈可用。

Post-Condition：
1. 通过 SAVE_ALL 建立完整trapframe，将其地址作为 struct trapframe * 参数传入 trap()。
2. trap() 返回后进入 __trapret，完成上下文恢复并通过 sret 返回。

Requirements：
1. 先保存现场，再设置参数并调用 C 函数，保证原有寄存器值可供恢复。
2. 按 RISC-V 调用约定传参和调用，保持栈对齐及trapframe完整。
3. C 函数返回后，恢复流程仍能正确定位trapframe，不依赖 a0 保留参数地址。
4. 正确衔接过程调用返回与trap返回，保留被打断程序原 ra 的恢复路径。

验证要求：
1. 检查保存与恢复映射、临时寄存器使用顺序及栈对齐，并完成编译。
2. 运行条件具备时，验证trap可反复进入和返回；并比较入口保存前与 sret 执行前的通用寄存器。
3. 报告实际完成的检查及未完成项。