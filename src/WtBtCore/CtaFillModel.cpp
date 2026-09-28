#include "CtaFillModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../Share/decimal.h"

const char* cta_fill_status_name(FillStatus status)
{
	switch (status)
	{
	case FillStatus::NoChange: return "NoChange";
	case FillStatus::Filled: return "Filled";
	case FillStatus::InvalidInput: return "InvalidInput";
	case FillStatus::WaitingLatency: return "WaitingLatency";
	case FillStatus::NoQuote: return "NoQuote";
	case FillStatus::InvalidMarketData: return "InvalidMarketData";
	case FillStatus::NoLiquidity: return "NoLiquidity";
	case FillStatus::PriceOutOfBounds: return "PriceOutOfBounds";
	}
	return "Unknown";
}

CtaPriceResult CtaPricePolicy::calculate(const CtaPriceRequest& request)
{
	CtaPriceResult result;
	const double touch = request.touch_price;
	const double tick = request.price_tick;
	const double slip = request.slippage;
	const double lower = request.lower_limit;
	const double upper = request.upper_limit;
	// 零报价是无对手盘；负价、坏 tick、负滑点和损坏的涨跌停字段是无效行情/配置。
	if (touch == 0.0)
	{
		result.status = FillStatus::NoQuote;
		return result;
	}
	if (!std::isfinite(touch) || touch < 0.0 || !std::isfinite(tick) || tick <= 0.0
		|| !std::isfinite(slip) || slip < 0.0 || !std::isfinite(lower) || lower < 0.0
		|| !std::isfinite(upper) || upper < 0.0 || (lower > 0.0 && upper > 0.0 && lower > upper))
	{
		result.status = FillStatus::InvalidMarketData;
		return result;
	}
	const double extra = request.ratio_slippage ? touch * slip / 10000.0 : slip * tick;
	const double raw = touch + (request.is_buy ? extra : -extra);
	const double units = raw / tick;
	if (!std::isfinite(extra) || !std::isfinite(raw) || !std::isfinite(units) || raw <= 0.0)
	{
		result.status = FillStatus::InvalidMarketData;
		return result;
	}
	// 小容差只抵消二进制浮点在整格附近的表示误差，不改变买向上/卖向下的原则。
	const double aligned_units = request.is_buy ? std::ceil(units - 1e-9)
		: std::floor(units + 1e-9);
	const double price = aligned_units * tick;
	if (!std::isfinite(price) || price <= 0.0)
	{
		result.status = FillStatus::InvalidMarketData;
		return result;
	}
	const double boundary_epsilon = tick * 1e-9;
	if ((lower > 0.0 && price < lower - boundary_epsilon)
		|| (upper > 0.0 && price > upper + boundary_epsilon))
	{
		result.status = FillStatus::PriceOutOfBounds;
		return result;
	}
	result.status = FillStatus::Filled;
	result.execution_price = price;
	return result;
}

// 纯函数式决策：不查询行情、不修改持仓，也不生成成交或资金记录。
// CtaMocker 每次将“当前实际仓位”和“最新目标”传入；原版模型一次返回全部差额。
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
	// 这里只给出拟成交差额；后续仍由 CtaMocker::apply_fill 拆开平、计费并写日志。
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
	// 这样目标已完成后不会因为盘口暂时缺失而重新进入等待状态。
	if (decimal::eq(request.actual_position, request.target_position))
		return result;

	const double delta = request.target_position - request.actual_position;
	if (!std::isfinite(delta))
	{
		result.status = FillStatus::InvalidInput;
		return result;
	}

	// created_sequence 是信号生成时的逻辑事件序号，event_sequence 是当前输入 Tick。
	// delay_events=0 也要求 current > created，即“至少下一条 Tick”才能撮合；
	// delay_events=2 则要求两者之差大于 2。用减法比较避免加法溢出。
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

	// 只选对手盘报价，不按 askqty/bidqty 限量。买入要打卖一，卖出要打买一；
	// 这是乐观 touch 价，不模拟排队顺序、挂单撤销或冲击成本。
	// 最终的固定/比例滑点由 apply_fill 处理，这里不重复叠加。
	result.status = FillStatus::Filled;
	result.signed_delta = delta;
	result.base_price = delta > 0.0 ? ask : bid;
	return result;
}

FillResult VolumeLimitedFill::decide(const FillRequest& request) const
{
	// 先判因果与盘口：没有合格事件或真实报价时，不读取所谓“流动性”。
	// 复用 CausalTouchFill 会先得到完整的 target-actual 差额和 touch 基准价；
	// 本函数只负责把可成交数量裁剪到本 Tick 剩余预算以内。
	FillResult result = CausalTouchFill(_delay_events).decide(request);
	if (result.status != FillStatus::Filled)
		return result;

	const double volume = request.market.volume_delta;
	const double used = request.market.volume_used;
	// volume 由 TickVolumeSource 核验；used 由 EventVolumeLedger 提供。
	// 两者不能混淆：volume 是市场这条 Tick 新增了多少，used 是本策略已用多少。
	// 非有限或负量不参与预算，避免 NaN/Inf、异常供应商数据进入持仓记账。
	if (!std::isfinite(_participation_rate) || _participation_rate <= 0.0
		|| _participation_rate > 1.0 || !std::isfinite(volume) || volume < 0.0
		|| !std::isfinite(used) || used < 0.0)
	{
		result.status = FillStatus::InvalidMarketData;
		result.signed_delta = 0.0;
		return result;
	}

	// 参与率是简化的“市场增量份额”上限，不是交易所承诺的可成交量。
	// 例如 volume=71、rate=0.1：原始额度 7.1，按整手向下取整为 7。
	const double raw_budget = volume * _participation_rate;
	if (!std::isfinite(raw_budget))
	{
		result.status = FillStatus::InvalidMarketData;
		result.signed_delta = 0.0;
		return result;
	}
	// 同一 Tick 可能两次调用 proc_tick；若第一次已成交 7 手，第二次的 used=7，
	// available 就是 7-7=0，不能把同一条市场成交量再用一次。
	const double available = std::floor(raw_budget) - used;
	if (available <= 0.0)
	{
		result.status = FillStatus::NoLiquidity;
		result.signed_delta = 0.0;
		return result;
	}

	// 还必须受目标剩余量约束：目标只剩 3 手、预算有 8 手，也只能成交 3。
	// copysign 保留方向：买入为正，卖出为负；不在这里判断开仓还是平仓。
	const double magnitude = std::min(std::abs(result.signed_delta), available);
	result.signed_delta = std::copysign(magnitude, result.signed_delta);
	return result;
}

void CtaEventVolumeLedger::begin(uint32_t trading_date, uint64_t event_sequence)
{
	// begin 可以在同一 Tick 的前后两次 proc_tick 中重复调用；相同键不能清零。
	// 只有新事件或新交易日才打开一页新账本，避免同事件预算重复领取。
	if (!_initialized || _trading_date != trading_date || _event_sequence != event_sequence)
	{
		_trading_date = trading_date;
		_event_sequence = event_sequence;
		_used = 0.0;
		_initialized = true;
	}
}

void CtaEventVolumeLedger::consume(double quantity)
{
	// 调用者仅在 apply_fill 改变实际仓位后记账；模型虽然可能建议 Filled，
	// 但若会计路径未产生实际持仓变化，就不应凭“建议量”扣预算。
	// 本类不自行计算每 Tick 总预算，只安全累计已成交量。
	if (std::isfinite(quantity) && quantity > 0.0
		&& std::isfinite(_used + quantity))
		_used += quantity;
}

double CtaTickVolumeSource::observe(uint32_t trading_date, double total_volume, double volume)
{
	// total_volume 在本样本中是所属 trading_date 截至当前 Tick 的累计量；
	// volume 是“上一条 Tick 之后至当前 Tick”的新增量，不是上一条 Tick 的量。
	// 无效的累计量无法充当基线；直接把本 Tick 当作零预算。
	if (!std::isfinite(total_volume) || total_volume < 0.0)
		return 0.0;
	if (!_initialized || _trading_date != trading_date)
	{
		// 每日首条记录可能已经累计了开盘以来一段成交，但没有可信的上一笔
		// total_volume 可验证它；因此只记住当前累计值，不给策略分配预算。
		_trading_date = trading_date;
		_last_total = total_volume;
		_initialized = true;
		return 0.0;
	}

	// 对同一个交易日的相邻两条 Tick：市场新增量 = 当前累计量 - 上条累计量。
	// 如 240 -> 311，本条应为 71；下一条 311 -> 398，本条应为 87。
	const double delta = total_volume - _last_total;
	// 即使本条 volume 不匹配或累计量倒退，也用“有效的当前累计值”重建基线；
	// 下一条只和本条比较，不能把异常期间的量一次性补进下一条预算。
	_last_total = total_volume;
	if (!std::isfinite(delta) || delta < 0.0
		|| !std::isfinite(volume) || volume < 0.0
		|| std::abs(delta - volume) > 1e-8)
		// 对本次模型而言，异常数据的可用量为 0；不是改写原始行情文件。
		return 0.0;
	return volume;
}

bool CtaTickInputGuard::accept(uint32_t trading_date, uint32_t action_date,
	uint32_t action_time, double total_volume, double last, double bid1,
	double ask1, double bidqty1, double askqty1)
{
	// 这些字段参与模型判断或用于识别同一输入，坏数值不能推进因果时钟。
	if (!std::isfinite(total_volume) || total_volume < 0.0 || !std::isfinite(last)
		|| !std::isfinite(bid1) || !std::isfinite(ask1)
		|| !std::isfinite(bidqty1) || !std::isfinite(askqty1))
		return false;
	if (_initialized)
	{
		if (trading_date < _trading_date)
			return false;
		if (trading_date == _trading_date)
		{
			if (action_date < _action_date
				|| (action_date == _action_date && action_time < _action_time)
				|| total_volume < _total_volume)
				return false;
			if (action_date == _action_date && action_time == _action_time
				&& total_volume == _total_volume && last == _last
				&& bid1 == _bid1 && ask1 == _ask1
				&& bidqty1 == _bidqty1 && askqty1 == _askqty1)
				return false;
		}
	}
	_trading_date = trading_date;
	_action_date = action_date;
	_action_time = action_time;
	_total_volume = total_volume;
	_last = last;
	_bid1 = bid1;
	_ask1 = ask1;
	_bidqty1 = bidqty1;
	_askqty1 = askqty1;
	_initialized = true;
	return true;
}

bool CtaPendingTarget::on_target(const char* code, double target,
	double actual_position, uint64_t sequence)
{
	_code = code == nullptr ? "" : code;
	// 同一目标是幂等重申；部分成交后重复设置也不能让延迟重新开始。
	// 例如目标 10 已成交 7，又调用 set_position(10)，仍等待剩余 3，
	// 不重新创建一个 10 手的新订单，也不重置 created_sequence。
	if (_phase != PendingPhase::Idle && decimal::eq(_target, target))
		return false;

	// Idle 后重新发出同数值目标，若当前实际仓位不同，仍属于一个新的待成交意图。
	const bool changed = !decimal::eq(_target, target) || _version == 0
		|| (_phase == PendingPhase::Idle && !decimal::eq(target, actual_position));
	_target = target;
	if (changed)
	{
		++_version;
		_created_sequence = sequence;
		_activation_sequence = sequence == std::numeric_limits<uint64_t>::max()
			? sequence : sequence + 1;
	}
	// 新目标正好等于实际仓位时取消待成交目标，不叠加旧剩余。
	// 目标覆盖以最新目标为准；已成交仓位和已用 Tick 预算不能撤回。
	_phase = decimal::eq(_target, actual_position)
		? PendingPhase::Idle : PendingPhase::WaitingLatency;
	return changed;
}

void CtaPendingTarget::on_decision(FillStatus status, double actual_position)
{
	// 延迟和缺流动性都保留目标；只有目标已完成/无变化等情形才结束等待。
	// Filled 可以是部分成交，故不能仅凭 Filled 就把状态设为 Idle。
	if (status == FillStatus::WaitingLatency)
		_phase = PendingPhase::WaitingLatency;
	else if (status == FillStatus::NoQuote || status == FillStatus::InvalidMarketData
		|| status == FillStatus::NoLiquidity || status == FillStatus::PriceOutOfBounds)
		_phase = PendingPhase::WaitingLiquidity;
	else if (status == FillStatus::Filled)
		// remaining() 每次用“最新目标 - 实际仓位”重算，避免缓存剩余量失真。
		_phase = decimal::eq(remaining(actual_position), 0.0)
			? PendingPhase::Idle : PendingPhase::WaitingLiquidity;
	else
		_phase = PendingPhase::Idle;
}

void CtaPendingTarget::restore(const char* code, double target,
	uint64_t created_sequence, uint64_t activation_sequence,
	uint64_t version, PendingPhase phase)
{
	_code = code == nullptr ? "" : code;
	_target = target;
	_created_sequence = created_sequence;
	_activation_sequence = activation_sequence;
	_version = version;
	_phase = phase;
}

double CtaPendingTarget::remaining(double actual_position) const
{
	return _target - actual_position;
}
