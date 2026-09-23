#pragma once

#include <cstdint>

// 本文件只定义 CTA 回测内部的“成交决策”数据与接口。
// 不修改 WtBtPorter 的对外 ABI，也不直接读写持仓、资金或输出文件。

// 一次决策可见的行情快照，为后续模型预留显式输入。
// 当前 CtaMocker 仅填 code、trading_date、last；其中 last 暂存选出的基准价，
// 不一定是真实最新价；盘口和成交量字段仍为默认值，不能直接用于真实撮合假设。
// code 是借用的指针，仅在本次同步 decide() 调用期间使用，模型不得长期保存。
struct MarketSnapshot
{
	const char* code = nullptr;          // WT 标准合约代码；不拥有这段内存
	uint64_t event_sequence = 0;       // 预留：行情事件序号，当前调用方未填
	uint32_t trading_date = 0;         // 当前交易日，不等同于自然日
	double last = 0.0;                  // 当前暂填基准价，不能当成可靠的真实最新价
	double bid1 = 0.0;                  // 预留：买一价，当前调用方未填
	double ask1 = 0.0;                  // 预留：卖一价，当前调用方未填
	double bidqty1 = 0.0;               // 预留：买一量，当前调用方未填
	double askqty1 = 0.0;               // 预留：卖一量，当前调用方未填
	double volume_delta = 0.0;          // 预留：本事件成交量增量，当前调用方未填
};

// 传给模型的一次目标仓位变更。这里的 actual_position 是回测上下文记录的
// 策略持仓，不是交易账户的真实成交持仓。
struct FillRequest
{
	double actual_position = 0.0;        // 调整前持仓：CtaMocker::PosInfo::_volume
	double target_position = 0.0;        // 策略要求的新目标仓位
	double base_price = 0.0;             // CtaMocker 已选好的基准价，尚未加滑点
	MarketSnapshot market;               // 预留行情信息；Legacy 模型不据此撮合
};

// 状态只描述模型本次的决策；Filled 不代表交易所成交回报。
enum class FillStatus : uint8_t
{
	NoChange,      // 新旧仓位按 decimal::eq 的容差视为相等
	Filled,        // 建议按 signed_delta 整笔调整，后续还要由 apply_fill 记账
	InvalidInput   // 实际仓位、目标仓位或基准价不是有限数
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
