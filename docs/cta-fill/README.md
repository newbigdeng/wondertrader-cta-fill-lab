# CTA 回测成交模型

本目录介绍 CTA 回测成交规则的改造、实现契约、测试和已知边界。这里的“成交”是回测内部记账。

## 改造目的与职责

原有 CTA 回测基本按目标与当前仓位的差额直接成交。改造把成交判断从 `CtaMocker` 中抽为 `ICtaFillModel::decide`，让策略目标不变时可以比较不同执行假设。

```text
历史行情 → 行情快照与事件钟 → 最新目标 / 实际仓位
        → 成交模型决策 → 最终成交价格 → apply_fill 记账
        → 按实际成交扣事件预算 → 成交与目标审计
```

模型只返回状态、方向、数量和价格信息。持仓明细、开平拆分、费用和盈亏仍由 `CtaMocker` 与 `HisDataReplayer` 处理。

| 模型 | 时序与价格 | 数量规则 |
| --- | --- | --- |
| `legacy_cta` | 保持原调用顺序、基准价与滑点路径 | 一次处理全部目标差额 |
| `causal_touch` | 目标创建后的合格事件，买取 ask1、卖取 bid1 | 全部目标差额 |
| `volume_limited` | 复用因果门槛和 touch 取价 | 按已核验 Tick 增量与参与率限制数量，允许部分成交 |

待成交目标按合约保存最新值。剩余量每次由“目标 − 实际仓位”重算；同目标重申不重置延迟，目标覆盖不会叠加旧剩余量。同一 Tick 的前后两次撮合共用事件预算。

## 文档与代码

- [事件时序、价格与数量规则](next-event-spec.md)
- [待成交目标与配置契约](pending-target-and-config.md)
- [测试结果、手续费与已知边界](testing-and-limits.md)
- [`CtaFillModel.h`](../../src/WtBtCore/CtaFillModel.h) / [`CtaFillModel.cpp`](../../src/WtBtCore/CtaFillModel.cpp)：纯决策、价格策略、量源与状态组件。
- [`CtaMocker.cpp`](../../src/WtBtCore/CtaMocker.cpp)：事件处理、目标管理、会计和审计输出。
- [`test_cta_fill_model.cpp`](../../src/TestUnits/test_cta_fill_model.cpp)：模型与状态组件测试。

最近整理日期：2026-10-01。当前模型已实现部分成交与审计，但增量恢复、数量精度及输入过滤仍有待解决的边界，详见测试文档。

上游通用参考：[WonderTrader 官方文档](https://wondertrader.github.io/#/)。
