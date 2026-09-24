#include "CtaFillModel.h"

#include <cmath>

#include "../Share/decimal.h"

// 纯函数式决策：不查询行情、不修改持仓，也不生成成交或资金记录。
FillResult LegacyCtaFill::decide(const FillRequest& request) const
{
	FillResult result;
	// 即使最终为 NoChange/InvalidInput，也保留调用方传入的基准价供诊断。
	result.base_price = request.base_price;

	// 避免 NaN/Inf 进入仓位差计算。原版没有这一层校验，故这是唯一明确的边界语义变化。
	// market 中预留的盘口字段当前不参与 Legacy 模型判断，也不在此处校验。
	if (!std::isfinite(request.actual_position)
		|| !std::isfinite(request.target_position)
		|| !std::isfinite(request.base_price))
	{
		result.status = FillStatus::InvalidInput;
		return result;
	}

	// 沿用原 do_set_position 的 decimal::eq 容差；极小差额不产生交易。
	// 此时 result 仍保持默认 NoChange，signed_delta 为 0。
	if (decimal::eq(request.actual_position, request.target_position))
		return result;

	// 旧回测的整笔立即成交假设：不检查可成交量，也不拆成多个事件。
	// 正负号只表达买/卖方向；开平、反手拆分和最终滑点价由 apply_fill 完成。
	result.status = FillStatus::Filled;
	result.signed_delta = request.target_position - request.actual_position;
	return result;
}

// 只做因果与报价选择；它不持有订单状态，最新目标及其 created_sequence
// 由 CtaMocker 的信号表保存。每次 decide 都根据当前事件重新判断，
// 所以缺报价时可以自然等待下一笔有效 Tick，且新目标会重置创建序号。
FillResult CausalTouchFill::decide(const FillRequest& request) const
{
	FillResult result;
	if (!std::isfinite(request.actual_position)
		|| !std::isfinite(request.target_position))
	{
		result.status = FillStatus::InvalidInput;
		return result;
	}

	// 没有仓位差时不必等待行情；与 Legacy 一样不生成成交。
	if (decimal::eq(request.actual_position, request.target_position))
		return result;

	const double delta = request.target_position - request.actual_position;
	if (!std::isfinite(delta))
	{
		result.status = FillStatus::InvalidInput;
		return result;
	}

	// 用减法比较以免 created_sequence + delay_events 溢出。
	// 同一 Tick 内先后两次 proc_tick 的 event_sequence 不变，故不能同事件成交。
	if (request.market.event_sequence <= request.created_sequence
		|| request.market.event_sequence - request.created_sequence <= _delay_events)
	{
		result.status = FillStatus::WaitingLatency;
		return result;
	}

	const double bid = request.market.bid1;
	const double ask = request.market.ask1;
	if (!std::isfinite(bid) || !std::isfinite(ask) || bid < 0.0 || ask < 0.0)
	{
		result.status = FillStatus::InvalidMarketData;
		return result;
	}
	if (bid == 0.0 || ask == 0.0)
	{
		result.status = FillStatus::NoQuote;
		return result;
	}
	if (bid > ask)
	{
		result.status = FillStatus::InvalidMarketData;
		return result;
	}

	// 只选对手盘报价，不按 askqty/bidqty 限量。最终滑点由 apply_fill 处理。
	result.status = FillStatus::Filled;
	result.signed_delta = delta;
	result.base_price = delta > 0.0 ? ask : bid;
	return result;
}
