#pragma once

#include <cstdint>

// 本文件只定义 CTA 回测内部的“成交决策”数据与接口。
// 不修改 WtBtPorter 的对外 ABI，也不直接读写持仓、资金或输出文件。

// 一次决策可见的行情快照。event_sequence 每个回放 Tick 只增加一次，
// 即使同一 Tick 内调用两次 proc_tick，也仍是同一个事件。
// CausalTouchFill 只使用真实 Tick 的买卖一档价格，不使用 last 冒充盘口。
// code 是借用的指针，仅在本次同步 decide() 调用期间使用，模型不得长期保存。
struct MarketSnapshot
{
	const char* code = nullptr;          // WT 标准合约代码；不拥有这段内存
	uint64_t event_sequence = 0;       // 单调递增的回放事件序号，不是墙钟时间
	uint32_t trading_date = 0;         // 当前交易日，不等同于自然日
	double last = 0.0;                  // 本事件最新价；因果模型不以它替代盘口
	double bid1 = 0.0;                  // 买一价；模拟 Bar Tick 或缺档时为 0
	double ask1 = 0.0;                  // 卖一价；模拟 Bar Tick 或缺档时为 0
	double bidqty1 = 0.0;               // 买一量；Day12 不用它决定成交量
	double askqty1 = 0.0;               // 卖一量；Day12 不用它决定成交量
	double volume_delta = 0.0;          // 本事件成交量；Day12 不读取容量
};

// 信号来源用于审计时间语义；不同来源可能在 Tick 回放的不同阶段创建。
enum class SignalSource : uint8_t
{
	Unknown,
	Schedule,       // Bar 收盘后的调度信号；天然由下一轮 Tick 消费
	StrategyTick,   // on_tick_updated 中生成；原版可能在同一 Tick 后半段成交
	Condition,      // proc_tick 检查条件单时生成
	Other           // 初始化、会话回调等其他策略入口
};

// 传给模型的一次目标仓位变更。这里的 actual_position 是回测上下文记录的
// 策略持仓，不是交易账户的真实成交持仓。
struct FillRequest
{
	double actual_position = 0.0;        // 调整前持仓：CtaMocker::PosInfo::_volume
	double target_position = 0.0;        // 策略要求的新目标仓位
	double base_price = 0.0;             // CtaMocker 已选好的基准价，尚未加滑点
	uint64_t created_sequence = 0;       // 最新目标产生时的回放事件序号
	SignalSource source = SignalSource::Unknown;
	MarketSnapshot market;               // Legacy 忽略盘口；因果模型按 touch 价决策
};

// 状态只描述模型本次的决策；Filled 不代表交易所成交回报。
enum class FillStatus : uint8_t
{
	NoChange,      // 新旧仓位按 decimal::eq 的容差视为相等
	Filled,        // 建议按 signed_delta 整笔调整，后续还要由 apply_fill 记账
	InvalidInput,  // 实际仓位、目标仓位或基准价不是有限数
	WaitingLatency,   // 同事件或尚未度过配置的额外事件延迟
	NoQuote,          // 本事件没有完整的买卖一档，继续等待
	InvalidMarketData // 非有限、负价或交叉盘口；不回退 last
};

// 纯决策结果；不携带手续费、平仓盈亏和资金副作用。
struct FillResult
{
	FillStatus status = FillStatus::NoChange;
	double signed_delta = 0.0;          // 有符号仓位差：目标－当前；买正、卖负
	double base_price = 0.0;            // 原样返回请求的基准价；最终价在记账时加滑点
};

// 可替换的内部决策接口。虚析构保证经基类指针销毁派生模型安全；
// const decide() 应仅依据输入计算结果，不直接改 CtaMocker 的状态。
class ICtaFillModel
{
public:
	virtual ~ICtaFillModel() = default;
	virtual FillResult decide(const FillRequest& request) const = 0;
};

// 兼容原版 CTA 的决策：对有限数输入，按目标与当前仓位的全部差额成交，
// 沿用调用方选出的基准价；不模拟排队、部分成交、盘口容量或成交延迟。
// NaN/Inf 会返回 InvalidInput，这个边界行为与未经校验的原版不同。
// 此类只实现决策，开平拆分、滑点、费用、持仓、资金仍在 CtaMocker::apply_fill。
class LegacyCtaFill final : public ICtaFillModel
{
public:
	FillResult decide(const FillRequest& request) const override;
};

// 下一合格事件的乐观 touch 模型：买取 ask1、卖取 bid1，再由 CtaMocker
// 独立施加现有静态滑点。不会读取盘口量，也不模拟排队、撤单或部分成交。
// delay_events=0 表示目标创建后的下一事件即可尝试；2 表示再多等两事件。
class CausalTouchFill final : public ICtaFillModel
{
public:
	explicit CausalTouchFill(uint64_t delay_events = 0) : _delay_events(delay_events) {}
	FillResult decide(const FillRequest& request) const override;

private:
	uint64_t _delay_events;
};
