# 待成交目标与配置契约

## 最新目标状态机

每个合约保存一份 `CtaPendingTarget`，包含最新 target、创建事件、版本与阶段。实际仓位由 `CtaMocker` 管理，剩余量不缓存。

```text
Idle → 不同于实际仓位的新目标 → WaitingLatency
WaitingLatency → 无报价/预算 → WaitingLiquidity
Waiting* → 部分成交 → WaitingLiquidity
Waiting* → 目标完成/取消 → Idle
Waiting* → 不同的新目标 → WaitingLatency
```

| 情形 | 当前语义 |
| --- | --- |
| 实际 2，设置目标 8 | 剩余 6 |
| 旧目标 8，缩为 3，实际仍为 2 | 新剩余 1，不叠加旧剩余 |
| 目标 10 已成交 7，再设置 10 | 保留创建时钟，继续剩余 3 |
| 目标 10 已成交 7，再设置 7 | 取消尚未成交的 3 |
| 实际 5，新目标 -2 | 重算差额 -7，实际开平受本次预算限制 |

只变标签等元数据不重置同值目标时钟。同事件多次不同目标以最新值为准。撮合后重新查找目标并比较版本，避免旧决策删除已被覆盖的新目标。

当前未完成目标可跨会话继续等待，没有默认当日失效规则。额外延迟由模型计算；pending 中的 `activation_sequence` 是创建后下一事件的派生信息，不等于加额外延迟后的完整门槛。

## Python 配置

配套 [wtpy 仓库](https://github.com/newbigdeng/wtpy-cta-fill-lab) 的程序化接口支持：

```python
engine.set_cta_strategy(
    strategy,
    slippage=1,
    fill_model="volume_limited",
    event_delay=1,
    participation_rate=0.1,
)
```

| 参数 | 契约 |
| --- | --- |
| `fill_model` | `legacy_cta`、`causal_touch`、`volume_limited`；默认 Legacy |
| `event_delay` | 非负 64 位整数，表示下一事件之外额外等待的事件数；Legacy 必须为 0 |
| `participation_rate` | 限量模型要求有限数且 `0 < rate <= 1`；其他模型必须为 0 |

Legacy 走原 `init_cta_mocker`；causal 走 v2；volume 走 v3。旧 C ABI 保留，新增模型需要配套动态库；缺符号或配置错误不会静默回退。`WTPY_BT_PORTER_LIB` 可指定与 Python 接口匹配的构建库。

## C++ / YAML 配置

策略工厂读取 `model`、`event_delay` 和 `participation_rate`，最终进入 `configure_fill_model`。YAML 延迟按十进制无符号整数严格解析，拒绝负数、浮点文本和溢出值；模型参数校验成功后才装配模型。

## 保存与恢复边界

状态保存事件钟、分合约事件钟、待成交目标及版本，并兼容旧文件缺失字段。当前没有完整持久化量源基线、输入过滤状态及同事件预算，也没有完成连续回放与分段恢复的逐笔等价验收。

因此不能把接口中的 `incremental=True` 理解为新增模型已经支持任意同日断点的等价恢复。模型配置指纹、恢复边界和审计日志续接还需要完善，见 [测试与边界](testing-and-limits.md)。
