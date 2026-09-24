# Day12：CTA 下一事件 touch 成交规格

本文只描述本分支的 CTA **回测**语义，不代表真实交易所委托或成交回报。

## 为什么要按来源区分

- `HisDataReplayer::run_by_bars` 先回放本轮模拟 Tick，随后 `onMinuteEnd -> handle_schedule`。调度产生的目标本来就在下一轮 Tick 被 `proc_tick` 消费，不能笼统称原版所有信号都“同事件成交”。
- `CtaMocker::handle_tick` 的顺序是前置 `proc_tick -> on_tick_updated -> （pxType != 3 时）后置 proc_tick`。策略在 `on_tick_updated` 里生成的目标，原版有可能被后置调用在**同一个 Tick**消费。条件单也可能在前置调用中生成信号、由后置调用消费。
- 两次 `proc_tick` 有不同的语义位置，不能简单注释其中一次；Day12 不改变该控制流，只改变新模型接受成交的门槛。

## 事件钟与目标

`CtaMocker::_event_sequence` 每进入一次 `handle_tick` 增加一次；前后两次 `proc_tick` 共享序号。它不是系统时间，也不按分钟递增。`append_signal` 仍按合约保存最新目标，并同时保存 `created_sequence` 和 `SignalSource`（`Schedule`、`StrategyTick`、`Condition`、`Other`）。同一合约的新目标覆盖旧目标时，创建序号也重置。

`CausalTouchFill` 的资格条件是 `market_sequence > created_sequence + event_delay`；实现用差值比较避免加法溢出。默认 `event_delay=0`，因此创建事件本身不能成交，下一笔**合格**行情事件才可能成交。若延迟设为 2，则创建序号为 100 时，101 和 102 等待，从 103 开始尝试报价。

| 决策状态 | 含义 | CtaMocker 对目标的处理 |
| --- | --- | --- |
| `WaitingLatency` | 尚未跨过因果/额外事件门槛 | 保留，下一事件重试 |
| `NoQuote` | 买卖一档缺失或为零 | 保留，下一事件重试；绝不回退最新价 |
| `InvalidMarketData` | 盘口非有限、负价或买一高于卖一 | 保留，下一事件重试 |
| `Filled` | 整笔目标差额按 touch 价记账 | 删除已处理目标 |
| `NoChange` | 目标与当前仓位相同 | 删除已处理目标，不生成成交 |
| `InvalidInput` | 仓位输入无效 | 记录错误并删除无效目标 |

真实 Tick 才提供盘口。Bar 回测模拟 Tick 的盘口字段即使有合成值，也在传给模型时置空，结果是 `NoQuote`；没有真实盘口数据就不声称 touch 模型已经完成回测成交。买入基准价为 ask1，卖出为 bid1；本阶段不使用买卖一档数量或成交量限制成交。模型只返回基准价和完整有符号差额；现有 `apply_fill` 随后按原规则加静态滑点、计算手续费并写入 `trades.csv` 等输出。当前 `trades.csv` 记录的是加滑点后的价格，模型单测可检查未加滑点的 touch 基准价；本阶段尚未新增独立的逐事件决策 CSV。

## 配置入口与边界

构造 `CtaMocker` 时仍默认 `LegacyCtaFill`。使用 C++/YAML 策略工厂入口时，可在 `cta` 配置中增加 `model: causal_touch` 和可选的 `event_delay: 0`；不写 `model` 或写 `legacy_cta` 即保持旧行为。无效模型名使初始化失败，不静默回退。此项没有修改 Porter 的既有 C ABI。当前 Python `WtBtEngine.set_cta_strategy()` 使用程序化 `init_cta_mocker`，**不会读取该工厂配置项**；要从 Python 选择新模型，还需后续增加安全的绑定入口，不能仅在 `run_day10.py` 的 YAML 里填 `model` 就认为已经生效。

增量回测状态保存 `event_sequence`、待处理目标的 `created_seq` 与来源，并兼容缺少这些字段的旧文件。历史状态的兼容性尚未做完整端到端增量回测验证。

## 验证范围与局限

- Day12 新增十个模型测试，覆盖 schedule 与 on_tick 来源、同事件等待、下一事件双向 touch 取价、可配置额外延迟、缺盘口后继续等待、异常/交叉盘口、目标覆盖重置时钟。
- Day11 的七个 Legacy 模型测试仍须通过；默认 Legacy 的完整回测还须与 Day10 golden 的 `signals/trades/closes/funds/positions.csv` 逐字节比较。
- 这是“下一事件可见报价上的乐观整笔成交”假设，不包含订单到达时间、盘口队列、排队成交概率、撤单竞争、隐藏流动性、成交容量和价格限制边界。下一事件更符合因果，不等于真实可成交。
