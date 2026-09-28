# WonderTrader CTA 回测成交模型实验

> 这是基于 [WonderTrader 原版](https://github.com/wondertrader/wondertrader) 的个人二次开发仓库，**不是 WonderTrader 官方项目**。原版框架、主要架构和绝大多数源码由原项目作者贡献；我在此基础上修改的是 CTA 回测的成交环节。Python 侧的配套修改见 [wtpy-cta-fill-lab](https://github.com/newbigdeng/wtpy-cta-fill-lab)。原项目及本仓库的许可见 [LICENSE](LICENSE)。

## 原版框架做什么

WonderTrader 是以 C++ 为核心的量化交易框架。原版已经提供行情接入与存储、CTA/SEL/HFT/UFT 等策略引擎、目标仓位到执行与交易接口的链路，以及历史数据回放和回测能力；[wtpy 原版](https://github.com/wondertrader/wtpy)提供 Python 策略接口和应用层组件。这些能力不是本仓库从零实现的。

按源码目录看，主要模块可以这样理解：

| 原版模块 | 职责 |
| --- | --- |
| `src/Parser*`、`src/WtDtCore`、`src/WtDataStorage` | 行情接入、数据管理与存储 |
| `src/WtCore`、`src/WtRunner`、`src/WtUftCore` | 策略上下文与交易引擎运行 |
| `src/WtExecMon`、`src/WtExeFact`、`src/Trader*` | 目标仓位执行与交易通道对接 |
| `src/WtBtCore`、`src/WtBtPorter` | 历史行情回放、回测记账及对外接口 |

CTA 回测的一条主要路径是：历史行情由 `HisDataReplayer` 回放，`CtaMocker` 驱动策略、处理目标仓位并记录模拟成交和盈亏，`WtBtPorter` 向应用层提供接口。它与原版实盘的执行器、交易适配器、柜台委托回报链路不同。

## 我的二次开发：先简述

**我没有重写 WonderTrader；我把 CTA 回测中“目标仓位如何变成实际成交”这一段抽成可替换的成交模型，再逐步加入行情约束、部分成交和回归验证。**当前修改范围是 CTA 回测，不应把实验成交模型等同于真实市场成交。

- **Day11–13：抽出成交环节。**`LegacyCtaFill` 保留原版“信号直接按目标全量成交”的基线行为；`CausalTouchFill` 引入事件顺序和对手盘报价约束，并接入模型配置与待成交目标状态。这个阶段的因果模型仍是全量成交。
- **Day14：建立动态检测环境。**用 Debug 构建和 ASan/UBSan 辅助检查成交模型改动。
- **Day15–16：加入成交量约束。**`VolumeLimitedFill` 按真实 Tick 成交量与参与率限制单次可成交量，支持部分成交；事件预算和目标状态分别负责避免同一 Tick 成交量重复使用、避免把旧目标的差额叠加到新目标。
- **Day17–21：验证功能和边界。**增加单元测试、真实 Tick 会计与目标覆盖验收，并在固定目标和策略反馈两层做单日成交假设敏感性实验。实验结果只说明该样本对成交假设敏感，不代表实盘收益。

实现入口可从 [`CtaFillModel.h`](src/WtBtCore/CtaFillModel.h)、[`CtaMocker.cpp`](src/WtBtCore/CtaMocker.cpp) 和 [`test_cta_fill_model.cpp`](src/TestUnits/test_cta_fill_model.cpp) 开始阅读。Python 参数入口与实验脚本在配套的 [wtpy 仓库](https://github.com/newbigdeng/wtpy-cta-fill-lab)。

## 原版与归属

- 原版 C++ 仓库：[wondertrader/wondertrader](https://github.com/wondertrader/wondertrader)
- 原版 Python 子框架：[wondertrader/wtpy](https://github.com/wondertrader/wtpy)
- 本仓库保留上游许可和 Git 历史；上面列出的二次开发内容才是我在本项目中的工作。
