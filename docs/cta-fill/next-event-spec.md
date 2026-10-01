# 事件时序、价格与数量规则

## 一条 Tick 中的处理顺序

`CtaMocker::handle_tick` 保留前置 `proc_tick → on_tick_updated → 后置 proc_tick` 的结构；后置调用受 `pxType != 3` 条件控制。前后两次撮合属于同一输入事件，不能各领取一次完整预算。

在 Bar 回放中，调度通常发生在本轮模拟 Tick 后，目标由下一轮消费；策略 Tick 回调或条件触发产生的目标，在 Legacy 路径可能由本轮后置撮合消费。因此不能把所有原版信号都描述成同事件成交。

新增模型按本合约接受的 Tick 推进事件钟，其他合约不会替它推进延迟。事件序号不是时间戳，`event_delay` 也不是毫秒延迟。

## 因果门槛

`causal_touch` 和 `volume_limited` 要求：

```text
current_sequence > created_sequence
current_sequence - created_sequence > event_delay
```

实现使用差值比较，避免计算创建序号加延迟时溢出。目标创建于事件 100 时，delay=0 从 101 尝试，delay=2 从 103 尝试。缺报价或零预算不会重新启动目标时钟。

同值目标重申保持创建序号；实质不同的新目标覆盖旧目标并重新开始等待。

## 价格规则

因果模型只接受真实 Tick 的完整一档报价。缺档或零价返回 `NoQuote`，非有限、负价或交叉盘口返回 `InvalidMarketData`，均不会回退到 last。Bar 模拟 Tick 的报价不作为真实盘口。

买入取 ask1，卖出取 bid1；后续统一执行：touch → 额外静态滑点 → 买向上/卖向下对齐价格 tick → 涨跌停检查。越界返回 `PriceOutOfBounds`，不钳到涨跌停制造成交。Legacy 保留旧价格路径以维持基线兼容。

## 限量规则

`CtaTickVolumeSource` 用同交易日累计量差分核对当前 `volume`。首 Tick 建立基线，预算为零；累计量或增量不合格时不给流动性预算。

```text
事件预算 = floor(核验后的 volume_delta × participation_rate)
可用预算 = 事件预算 − 同事件已实际成交量
本次绝对成交量 = min(abs(target − actual), 可用预算)
```

`CtaEventVolumeLedger` 在新事件开始时重置，用记账后的真实持仓变化扣量。部分成交后目标保留，例如目标 10、本次成交 7，下次从实际 7 重算剩余 3。

## 状态与假设边界

| 状态 | 行为 |
| --- | --- |
| `WaitingLatency` | 未满足事件门槛，保留目标 |
| `NoQuote` / `InvalidMarketData` | 等待后续合格行情 |
| `NoLiquidity` | 本事件无可用预算，保留目标 |
| `PriceOutOfBounds` | 最终价越界，不记账，保留目标 |
| `Filled` | 按本次增量记账；部分成交继续等待，目标完成后释放 |
| `NoChange` | 无仓位差，不生成交易 |
| `InvalidInput` | 记录错误，当前状态机结束该目标 |

市场增量是已经发生的交易量，用它配合当前 touch 是容量近似。模型不限制一档挂量、不模拟队列、成交方向、隐藏流动性或自身冲击。逐 Tick 取整也会使预算依赖行情切分方式。已知精度和输入事件问题见 [测试与边界](testing-and-limits.md)。
