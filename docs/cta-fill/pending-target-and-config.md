# Day13：待成交目标状态机与完整配置入口

本文描述 CTA **回测**内部状态，不代表真实委托、交易所回报或部分成交实现。

## 状态图与真值

```text
Idle -- 新目标 != 实际仓位 --> WaitingLatency
WaitingLatency -- 时间已到但缺报价/坏盘口 --> WaitingLiquidity
WaitingLatency -- 完整成交 --> Idle
WaitingLiquidity -- 继续缺报价 --> WaitingLiquidity
WaitingLiquidity -- 完整成交或目标取消 --> Idle
任一等待状态 -- 新的不同目标 --> WaitingLatency（覆盖旧目标，重置创建时钟）
```

每个合约的 `CtaPendingTarget` 保存 `code`、最新 `target`、`created_sequence`、`activation_sequence`、目标版本和阶段。`activation_sequence` 是创建事件之后第一个理论可尝试的事件；`event_delay` 的额外等待仍由 `CausalTouchFill` 判定。剩余量**不存储**，每次都由 `target - actual_position` 重算。现在 `apply_fill` 仍整笔成交；Day13 的部分成交用例测试状态机在未来实际仓位只变动一部分时不会沿用旧剩余量，并不代表回测已经会部分成交。

| before | event | after | 不变量 |
| --- | --- | --- | --- |
| `Idle, actual=2` | 目标 8 | `WaitingLatency, target=8` | 剩余 `8-2=6` |
| `Waiting*, target=8` | 目标缩为 3 | `WaitingLatency, target=3` | 剩余 `3-2=1`，不叠加旧 6 |
| `Waiting*, actual=2` | 目标回到 2 | `Idle` | 旧待成交被取消 |
| `Waiting*, actual=2` | 目标反向到 -4 | `WaitingLatency, target=-4` | 剩余 `-4-2=-6` |
| `Waiting*, target=8` | 再报目标 8 | 阶段不变 | 创建序号与版本不变，防止每 Tick 重设后永远等不到成交 |
| `Waiting*, target=8, actual=5` | 部分成交后再报 8 | 阶段不变 | 剩余重算为 `8-5=3` |

同一事件内多次不同目标以最后一次为准；每个合约有独立的 `CtaPendingTarget`。只改 `userTag`（或同目标的指定价）会更新信号元数据，但不作为新交易意图重置时钟。若已回到 `Idle`，之后实际仓位发生变化再发出相同数值目标，则是新意图，应重新激活。CTA 回放由同一事件循环调用这些方法，本阶段没有证据表明目标表被多个线程并发修改，因此未引入锁。

`CtaMocker::proc_tick` 在调用成交模型后重新查找信号并比较版本：若成交过程中的回调写入了更新的目标，就不擦掉新目标。结果为 `WaitingLatency`、`NoQuote`、`InvalidMarketData` 时保留待成交目标；整笔成交或无仓位差时回到 `Idle`。增量状态新增 `activation_seq`、`target_version`、`pending_phase`，并对旧文件缺失字段给出默认值；增量续跑尚未做端到端验证。

## 配置入口

Python 程序化入口现在支持：

```python
engine.set_cta_strategy(
    strategy,
    slippage=0,
    fill_model="causal_touch",
    event_delay=1,
)
```

`legacy_cta` 为默认值，调用原 `init_cta_mocker`；`causal_touch` 调用新增的 `init_cta_mocker_v2`。旧 C ABI 保留，v2 在**加载增量状态和注册 sink 之前**校验模型，失败返回 0。Python 拒绝未知模型、负数/非整数/超 64 位延迟，以及 Legacy 搭配非零延迟；旧动态库缺 v2 符号时报告清晰错误，不会静默改用 Legacy。C++/YAML 工厂入口同样支持 `model: causal_touch` 和 64 位 `event_delay`；它按纯数字字符串严格解析，拒绝负数、浮点数和溢出值，避免 `strtoull("-1")` 变成巨大正数。

py39 日常安装从 `/work/wtpy` 导入 Python 源码，但自带 `.so` 仍是旧版。开发时显式指定新编译的 Debug 库，避免覆盖仓库二进制：

```bash
export WTPY_BT_PORTER_LIB=/work/build/gcc8-debug/build_x64/Debug/bin/WtBtPorter/libWtBtPorter.so
/root/miniconda3/envs/py39/bin/python run_day13.py \
  --slippage 0 --fill-model causal_touch --event-delay 1 \
  --experiment my_causal_01 --begin 201909100930 --end 201909111500 --no-report
```

完整的隔离示例位于 `/work/records/day13/config-regression/run/run_day13.py`。已有 Bar 数据没有真实买卖一档，选择 `causal_touch` 后只会保留目标并返回 `NoQuote`，不会伪造成交；要验证 touch 成交还需要真实逐 Tick 盘口数据。

## 本次验证

- Debug `TestUnits`、`WtBtPorter` 均编译成功；模型相关 23/23 通过。
- `PendingTarget` 六个测试重复 20 次全部通过，日志 `/work/records/day13/pending-repeat20.log`。
- 排除原有两个无关失败项后，完整 TestUnits 为 37/37。
- 新库和 Python 绑定运行默认 Legacy：五份业务 CSV 与 Day10 golden 逐字节相同。
- Python + v2 的 causal Bar 冒烟测试通过；`trades.csv` 只有表头，符合无真实盘口时不成交的语义。
- Python 参数错误和 C++ 未知模型均被拒绝。真实 Tick 端到端、增量恢复仍待验证。
