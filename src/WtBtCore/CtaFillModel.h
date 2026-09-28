#pragma once

#include <cstdint>
#include <string>

// 本文件只定义 CTA 回测内部的“成交决策”数据与接口。
// 一条真实 Tick 的限量成交链路是：
//   CtaTickVolumeSource 核验本 Tick 的新增市场成交量
//   -> CtaEventVolumeLedger 查询本 Tick 已使用的额度
//   -> VolumeLimitedFill 决定本次最多成交多少
//   -> CtaMocker::apply_fill 改持仓、费用和成交记录
//   -> CtaEventVolumeLedger 按实际持仓变化扣减额度。
// 这些类不直接读写持仓、资金或输出文件；它们也不是交易所真实成交回报。

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
	double bidqty1 = 0.0;               // 买一挂量：当前盘口快照，不等于已经发生的市场成交量
	double askqty1 = 0.0;               // 卖一挂量；当前 VolumeLimitedFill 不用挂量限制成交
	// 当前 Tick 相对上一条 Tick 新增的市场成交量，不是“上一条 Tick 自己的量”。
	// CtaTickVolumeSource 核对后才填入；每日首 Tick 无前序基线，保守地填 0。
	double volume_delta = 0.0;
	// 同一合约、同一输入 Tick 内本策略已经实际成交的数量；不跨 Tick 累计。
	// 因 handle_tick 可能两次调用 proc_tick，第二次必须扣除第一次已用量。
	double volume_used = 0.0;
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
	SignalSource source = SignalSource::Unknown; // 用于记录目标来源，不改变当前预算公式
	MarketSnapshot market;               // Legacy 忽略盘口；因果模型按 touch 价决策
};

// 状态只描述模型本次的决策；Filled 不代表交易所成交回报。
enum class FillStatus : uint8_t
{
	NoChange,      // 新旧仓位按 decimal::eq 的容差视为相等
	Filled,        // 建议按 signed_delta 调整；它可以只是目标差额的一部分
	InvalidInput,  // 实际仓位、目标仓位或基准价不是有限数
	WaitingLatency,   // 同事件或尚未度过配置的额外事件延迟
	NoQuote,          // 本事件没有完整的买卖一档，继续等待
	InvalidMarketData, // 非有限、负价或交叉盘口；不回退 last
	NoLiquidity,      // 报价有效，但本 Tick 剩余预算为零；保留目标等待后续 Tick
	PriceOutOfBounds  // 加滑点/对齐后的成交价越过有效涨跌停；等待后续行情
};

// 日志和实验输出使用稳定的状态名称，不依赖 enum 的整数顺序。
const char* cta_fill_status_name(FillStatus status);

// 纯决策结果；不携带手续费、平仓盈亏和资金副作用。
struct FillResult
{
	FillStatus status = FillStatus::NoChange;
	double signed_delta = 0.0;          // 本次拟成交的有符号数量；限量模型可小于目标－当前
	double base_price = 0.0;            // 原样返回请求的基准价；最终价在记账时加滑点
	// Day18 非 Legacy 价格策略计算出最终价后设置；Legacy 仍走原会计滑点分支。
	double execution_price = 0.0;
	bool execution_price_ready = false;
};

// Day18 的纯价格计算输入。涨跌停为 0 表示行情未提供该侧边界，而不是价格 0。
// 这里不决定成交量，不查询品种、资金、持仓，也不修改已有 Legacy 路径。
struct CtaPriceRequest
{
	double touch_price = 0.0;
	double price_tick = 0.0;
	double slippage = 0.0; // 固定模式为 tick 数；比例模式为万分比
	double lower_limit = 0.0;
	double upper_limit = 0.0;
	bool is_buy = true;
	bool ratio_slippage = false;
};

struct CtaPriceResult
{
	FillStatus status = FillStatus::InvalidInput;
	double execution_price = 0.0;
};

// 顺序固定：touch 报价 -> 额外静态滑点 -> 按交易方向对齐 tick -> 涨跌停检查。
// 买价向上、卖价向下对齐；越界直接拒绝，不钳到涨跌停制造虚假成交。
class CtaPricePolicy
{
public:
	static CtaPriceResult calculate(const CtaPriceRequest& request);
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

// Day16：事件成交量约束模型。先沿用 CausalTouchFill 的事件延迟和对手盘报价：
// 买单用卖一价，卖单用买一价；本模型只进一步限制“数量”。
// 本 Tick 总预算 = floor(volume_delta * participation_rate)，可用预算再减 volume_used；
// 本次成交绝对量 = min(abs(target_position - actual_position), 可用预算)。
// 例如目标 10、当前 0、volume_delta=71、rate=0.1，则本次至多买 7 手，
// 剩余 3 手留给后续事件，不会直接把仓位改成 10。
// volume_delta 源于 WT Tick 的 volume（本 Tick 新增量），并用 total_volume
// （交易日内累计量）差分核对；一档挂量不是这个预算的来源。
class VolumeLimitedFill final : public ICtaFillModel
{
public:
	VolumeLimitedFill(double participation_rate, uint64_t delay_events = 0)
		: _participation_rate(participation_rate), _delay_events(delay_events) {}
	FillResult decide(const FillRequest& request) const override;

private:
	double _participation_rate; // 假设最多参与市场本 Tick 成交量的比例，配置范围 (0,1]
	uint64_t _delay_events;      // 目标创建后额外等待的输入事件数
};

// “预算账本”只记本合约在当前输入事件中已成交多少，不记录市场原始成交量、
// 策略目标仓位或跨日累计量。CtaMocker 以 stdCode 为键为每个合约保存一份。
// begin() 遇到新交易日或新事件才清零；同一 Tick 中两次 proc_tick、
// 同一事件内目标覆盖，都不能重新领取一次完整预算。
// consume() 在 apply_fill 之后按真实仓位变化调用，而不是按拟成交量调用。
class CtaEventVolumeLedger//记录已使用量
{
public:
	// 开始检查某个事件：同一 (交易日, 事件序号) 幂等，换事件才重置 _used。
	void begin(uint32_t trading_date, uint64_t event_sequence);
	// 确认已经实际成交的绝对量；拒绝非有限、非正数，不做撤销/回滚。
	void consume(double quantity);
	// 提供给 VolumeLimitedFill 的 volume_used，单位与 Tick volume/持仓手数一致。
	double used() const { return _used; }

private:
	uint32_t _trading_date = 0;
	uint64_t _event_sequence = 0;
	double _used = 0.0;
	bool _initialized = false;
};

// “Tick 量源校验器”只负责判断输入行情中的 volume 能否作为本次市场增量，
// 尚不乘参与率，也不扣本策略的已用预算；这与上面的 Ledger 分工不同。
// 对已审计的 WT 样本，total_volume 是本交易日截至当前 Tick 的累计成交量；
// volume 是从上一条 Tick 到当前 Tick 新增的量，不是上一条 Tick 自身的成交量。
// 例如 total_volume 从 240 变为 311，当前 Tick 的 volume 应为 71。
// 每个合约各持有一份：首 Tick/跨交易日首 Tick 因没有可信前序基线返回 0；
// 同日后续要求 volume == current_total - previous_total 且数值非负有限。
// 累计量倒退或字段不一致的该笔返回 0；有效累计量作为新基线，防止下一笔虚增。
// 这是对本数据源的保守契约；不保证任何供应商的 Tick 都遵循同一字段语义。
class CtaTickVolumeSource//检验tick里的数量是否正常
{
public:
	// 返回经校验后可供模型使用的“本 Tick 市场新增量”；失败返回 0。
	double observe(uint32_t trading_date, double total_volume, double volume);

private:
	uint32_t _trading_date = 0;
	double _last_total = 0.0;
	bool _initialized = false;
};

// Day20：只保护新增因果模型的输入逻辑时钟。完全重复的可见行情、同交易日
// 时间倒退或累计量倒退都不生成新的事件；同毫秒但盘口/成交量确有变化仍允许。
// Legacy 为保持原版兼容，不启用这道门。拒绝输入后不推进序号、不回调策略。
class CtaTickInputGuard//预防数据出现错误，比如说时间倒退或者成交量倒退
{
public:
	bool accept(uint32_t trading_date, uint32_t action_date, uint32_t action_time,
		double total_volume, double last, double bid1, double ask1,
		double bidqty1, double askqty1);

private:
	uint32_t _trading_date = 0;
	uint32_t _action_date = 0;
	uint32_t _action_time = 0;
	double _total_volume = 0.0;
	double _last = 0.0;
	double _bid1 = 0.0;
	double _ask1 = 0.0;
	double _bidqty1 = 0.0;
	double _askqty1 = 0.0;
	bool _initialized = false;
};

// Day13：每个合约的最新待成交目标。实际持仓仍由 CtaMocker 持有，不能在这里
// 缓存“剩余量”；例如目标 10、已成交 7，下次应从实际仓位重算剩余 3，
// 而不是沿用第一次提交目标时记录的 10。
enum class PendingPhase : uint8_t
{
	Idle,
	WaitingLatency,
	WaitingLiquidity // 盘口缺失/无预算，或部分成交尚未完成；继续等后续 Tick
};

class CtaPendingTarget
{
public:
	// 返回 true 表示创建新意图；等待中的同值重复信号只更新外层标签，不重置时钟。
	bool on_target(const char* code, double target, double actual_position, uint64_t sequence);
	void on_decision(FillStatus status, double actual_position);
	void restore(const char* code, double target, uint64_t created_sequence,
		uint64_t activation_sequence, uint64_t version, PendingPhase phase);
	double remaining(double actual_position) const;

	const std::string& code() const { return _code; }
	double target() const { return _target; }
	uint64_t created_sequence() const { return _created_sequence; }
	uint64_t activation_sequence() const { return _activation_sequence; }
	uint64_t version() const { return _version; }
	PendingPhase phase() const { return _phase; }

private:
	std::string _code;
	double _target = 0.0;
	uint64_t _created_sequence = 0;
	uint64_t _activation_sequence = 0; // 最早可尝试事件；额外延迟仍由成交模型判断
	uint64_t _version = 0; // 目标发生实质变化才增加，用来识别回调期间被覆盖的旧信号
	PendingPhase _phase = PendingPhase::Idle;
};
