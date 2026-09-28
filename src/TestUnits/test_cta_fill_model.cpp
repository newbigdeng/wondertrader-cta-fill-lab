#include "gtest/gtest/gtest.h"

#include <limits>
#include <cmath>

#include "../WtBtCore/CtaFillModel.h"

// 这里测试的是 LegacyCtaFill::decide 的纯决策，不构造 CtaMocker。
// 因此不覆盖滑点计算、手续费、开平记录、资金或 CSV 输出；这些需要完整回测对照。

namespace
{
// 每个用例都通过同一组确定性行情字段构造请求，避免测试输入互相影响。
// 盘口和成交量设为非零，使请求形状接近未来模型；本组用例并未枚举所有盘口变化。
FillRequest request_for(double actual, double target, double base_price)
{
	FillRequest request;
	request.actual_position = actual;
	request.target_position = target;
	request.base_price = base_price;
	request.market.code = "CFFEX.IF.HOT";
	request.market.event_sequence = 42;
	request.market.trading_date = 20190910;
	request.market.last = 100.0;
	request.market.bid1 = 99.8;
	request.market.ask1 = 100.2;
	request.market.bidqty1 = 10.0;
	request.market.askqty1 = 12.0;
	request.market.volume_delta = 3.0;
	return request;
}
}

// 空仓到多 2：通过基类接口调用，验证多态接线、正向增量及基准价透传。
TEST(LegacyCtaFill, OpenLongFillsEntireDelta)
{
	LegacyCtaFill model;
	const ICtaFillModel& interface = model;
	const FillResult result = interface.decide(request_for(0.0, 2.0, 100.0));
	EXPECT_TRUE(result.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(2.0, result.signed_delta);
	EXPECT_DOUBLE_EQ(100.0, result.base_price);
}

// 空仓到空 2：卖出方向以负增量表示，模型本身不生成“开空”成交记录。
TEST(LegacyCtaFill, OpenShortFillsEntireDelta)
{
	LegacyCtaFill model;
	const FillResult result = model.decide(request_for(0.0, -2.0, 100.0));
	EXPECT_TRUE(result.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(-2.0, result.signed_delta);
}

// 覆盖多头加/减仓与空头加/减仓，确认四种变化都只按目标－当前计算。
TEST(LegacyCtaFill, IncreaseAndReducePreserveSignedDelta)
{
	LegacyCtaFill model;
	EXPECT_DOUBLE_EQ(1.0, model.decide(request_for(2.0, 3.0, 100.0)).signed_delta);
	EXPECT_DOUBLE_EQ(-2.0, model.decide(request_for(3.0, 1.0, 100.0)).signed_delta);
	EXPECT_DOUBLE_EQ(-1.0, model.decide(request_for(-2.0, -3.0, 100.0)).signed_delta);
	EXPECT_DOUBLE_EQ(2.0, model.decide(request_for(-3.0, -1.0, 100.0)).signed_delta);
}

// 多 1 到空 1 的差额为 -2；反方向为 +2。
// 这里只验证总增量，旧持仓先平、新方向再开的明细由 apply_fill 负责。
TEST(LegacyCtaFill, ReversePositionFillsFullDifference)
{
	LegacyCtaFill model;
	const FillResult long_to_short = model.decide(request_for(1.0, -1.0, 100.0));
	const FillResult short_to_long = model.decide(request_for(-1.0, 1.0, 100.0));
	EXPECT_TRUE(long_to_short.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(-2.0, long_to_short.signed_delta);
	EXPECT_DOUBLE_EQ(2.0, short_to_long.signed_delta);
}

// 目标与当前相同：保持默认 NoChange，不能凭基准价为 0 推断需要成交。
// 注意：price==0 时从价格表取价属于 CtaMocker 的职责，不是此单测所测。
TEST(LegacyCtaFill, EqualPositionDoesNotFill)
{
	LegacyCtaFill model;
	const FillResult result = model.decide(request_for(1.0, 1.0, 0.0));
	EXPECT_TRUE(result.status == FillStatus::NoChange);
	EXPECT_DOUBLE_EQ(0.0, result.signed_delta);
}

// 模型不重新选价：调用方传入 101.2，就把 101.2 作为未加滑点的基准价返回。
TEST(LegacyCtaFill, KeepsCallerSelectedBasePrice)
{
	LegacyCtaFill model;
	// CtaMocker 可能传入信号保存的指定价格，而不是当前 Tick 价格。
	const FillResult result = model.decide(request_for(0.0, 1.0, 101.2));
	EXPECT_TRUE(result.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(101.2, result.base_price);
}

// 仅选取目标仓位 NaN、基准价 +Inf 两个代表性坏输入，验证返回 InvalidInput。
// 实际仓位及其他非有限数组合由相同 isfinite 条件处理，但本用例未逐一枚举。
TEST(LegacyCtaFill, RejectsNonFiniteInputs)
{
	LegacyCtaFill model;
	FillRequest request = request_for(0.0, 1.0, 100.0);
	request.target_position = std::numeric_limits<double>::quiet_NaN();
	EXPECT_TRUE(model.decide(request).status == FillStatus::InvalidInput);
	request = request_for(0.0, 1.0, 100.0);
	request.base_price = std::numeric_limits<double>::infinity();
	EXPECT_TRUE(model.decide(request).status == FillStatus::InvalidInput);
}

// Day12 的模型测试只验证因果门槛与 touch 取价，不把 Filled 解释为交易所回报。
// 创建序号和行情序号由回放器推进；同一 Tick 内两次 proc_tick 不应推进两次。
namespace
{
FillRequest causal_request(double actual, double target, uint64_t created, uint64_t market)
{
	FillRequest request = request_for(actual, target, 100.0);
	request.created_sequence = created;
	request.market.event_sequence = market;
	request.source = SignalSource::StrategyTick;
	request.market.bid1 = 99.0;
	request.market.ask1 = 101.0;
	return request;
}
}

TEST(CausalTouchFill, SameEventMustWait)
{
	CausalTouchFill model;
	const FillResult result = model.decide(causal_request(0.0, 2.0, 100, 100));
	EXPECT_TRUE(result.status == FillStatus::WaitingLatency);
	EXPECT_DOUBLE_EQ(0.0, result.signed_delta);
}

TEST(CausalTouchFill, NextEventBuyUsesAskTouch)
{
	CausalTouchFill model;
	const FillResult result = model.decide(causal_request(0.0, 2.0, 100, 101));
	EXPECT_TRUE(result.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(2.0, result.signed_delta);
	EXPECT_DOUBLE_EQ(101.0, result.base_price);
}

TEST(CausalTouchFill, NextEventSellUsesBidTouch)
{
	CausalTouchFill model;
	const FillResult result = model.decide(causal_request(0.0, -2.0, 100, 101));
	EXPECT_TRUE(result.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(-2.0, result.signed_delta);
	EXPECT_DOUBLE_EQ(99.0, result.base_price);
}

TEST(CausalTouchFill, ConfiguredDelayCountsAdditionalEvents)
{
	CausalTouchFill model(2);
	EXPECT_TRUE(model.decide(causal_request(0.0, 1.0, 100, 102)).status == FillStatus::WaitingLatency);
	EXPECT_TRUE(model.decide(causal_request(0.0, 1.0, 100, 103)).status == FillStatus::Filled);
}

TEST(CausalTouchFill, MissingEitherSideIsNoQuote)
{
	CausalTouchFill model;
	FillRequest request = causal_request(0.0, 1.0, 100, 101);
	request.market.ask1 = 0.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::NoQuote);
	request = causal_request(0.0, -1.0, 100, 101);
	request.market.bid1 = 0.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::NoQuote);
}

TEST(CausalTouchFill, BadOrCrossedBookIsRejected)
{
	CausalTouchFill model;
	FillRequest request = causal_request(0.0, 1.0, 100, 101);
	request.market.bid1 = 102.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::InvalidMarketData);
	request = causal_request(0.0, 1.0, 100, 101);
	request.market.ask1 = std::numeric_limits<double>::quiet_NaN();
	EXPECT_TRUE(model.decide(request).status == FillStatus::InvalidMarketData);
}

TEST(CausalTouchFill, NoQuoteCanFillOnLaterValidEvent)
{
	CausalTouchFill model;
	FillRequest request = causal_request(0.0, 1.0, 100, 101);
	request.market.ask1 = 0.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::NoQuote);
	request = causal_request(0.0, 1.0, 100, 102);
	EXPECT_TRUE(model.decide(request).status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(101.0, model.decide(request).base_price);
}

TEST(CausalTouchFill, ReplacedTargetResetsCausalClock)
{
	CausalTouchFill model;
	// 新目标在事件 101 才产生，不能沿用被覆盖目标的创建序号 100。
	EXPECT_TRUE(model.decide(causal_request(0.0, 3.0, 101, 101)).status == FillStatus::WaitingLatency);
	const FillResult result = model.decide(causal_request(0.0, 3.0, 101, 102));
	EXPECT_TRUE(result.status == FillStatus::Filled);
	EXPECT_DOUBLE_EQ(3.0, result.signed_delta);
}

// run_by_bars 先模拟 Tick，再执行 onMinuteEnd -> handle_schedule。
// 因此调度目标记在事件 100 后，事件 101 是它遇到的第一笔可处理行情。
TEST(CausalTouchFill, BarCloseScheduleUsesNextReplayEvent)
{
	CausalTouchFill model;
	FillRequest request = causal_request(0.0, 1.0, 100, 101);
	request.source = SignalSource::Schedule;
	EXPECT_TRUE(model.decide(request).status == FillStatus::Filled);
}

// on_tick_updated 在前后两次 proc_tick 之间；后一次仍是同一个事件 100。
// 仅事件 101 才能使回调生成的目标满足因果等待门槛。
TEST(CausalTouchFill, PostCallbackProcTickDoesNotAdvanceEvent)
{
	CausalTouchFill model;
	FillRequest request = causal_request(0.0, 1.0, 100, 100);
	request.source = SignalSource::StrategyTick;
	EXPECT_TRUE(model.decide(request).status == FillStatus::WaitingLatency);
	request.market.event_sequence = 101;
	EXPECT_TRUE(model.decide(request).status == FillStatus::Filled);
}

// Day13 真值表：actual 固定为 2，等待期间的扩大、缩小、取消和反向
// 都用最新 target 覆盖；remaining 每次从 target - actual 重新计算。
TEST(PendingTarget, OverwriteTruthTable)
{
	CtaPendingTarget pending;
	struct Row { double target; double remaining; PendingPhase phase; };
	const Row rows[] = {
		{8.0, 6.0, PendingPhase::WaitingLatency},
		{3.0, 1.0, PendingPhase::WaitingLatency},
		{2.0, 0.0, PendingPhase::Idle},
		{-4.0, -6.0, PendingPhase::WaitingLatency}
	};
	for (uint64_t i = 0; i < 4; ++i)
	{
		EXPECT_TRUE(pending.on_target("CFFEX.IF.HOT", rows[i].target, 2.0, 100 + i));
		EXPECT_DOUBLE_EQ(rows[i].target, pending.target());
		EXPECT_DOUBLE_EQ(rows[i].remaining, pending.remaining(2.0));
		EXPECT_TRUE(pending.phase() == rows[i].phase);
		EXPECT_EQ(100 + i, pending.created_sequence());
		EXPECT_EQ(101 + i, pending.activation_sequence());
		EXPECT_EQ(i + 1, pending.version());
	}
}

TEST(PendingTarget, SameTargetIsIdempotentDuringWait)
{
	CtaPendingTarget pending;
	EXPECT_TRUE(pending.on_target("CFFEX.IF.HOT", 8.0, 2.0, 100));
	pending.on_decision(FillStatus::NoQuote, 2.0);
	EXPECT_TRUE(pending.phase() == PendingPhase::WaitingLiquidity);
	EXPECT_FALSE(pending.on_target("CFFEX.IF.HOT", 8.0, 2.0, 101));
	EXPECT_EQ(100, pending.created_sequence());
	EXPECT_EQ(101, pending.activation_sequence());
	EXPECT_EQ(1, pending.version());
}

TEST(PendingTarget, PartialFillRecomputesRemainingBeforeNewTarget)
{
	CtaPendingTarget pending;
	pending.on_target("CFFEX.IF.HOT", 8.0, 2.0, 100);
	pending.on_decision(FillStatus::Filled, 5.0); // 将来部分成交后，真实持仓变为 5。
	EXPECT_TRUE(pending.phase() == PendingPhase::WaitingLiquidity);
	EXPECT_DOUBLE_EQ(3.0, pending.remaining(5.0));
	EXPECT_FALSE(pending.on_target("CFFEX.IF.HOT", 8.0, 5.0, 101));
	EXPECT_EQ(100, pending.created_sequence());
	EXPECT_TRUE(pending.on_target("CFFEX.IF.HOT", 3.0, 5.0, 102));
	EXPECT_DOUBLE_EQ(-2.0, pending.remaining(5.0));
	EXPECT_EQ(2, pending.version());
}

TEST(PendingTarget, SameEventKeepsOnlyLastTarget)
{
	CtaPendingTarget pending;
	pending.on_target("CFFEX.IF.HOT", 8.0, 2.0, 100);
	pending.on_target("CFFEX.IF.HOT", -4.0, 2.0, 100);
	EXPECT_DOUBLE_EQ(-6.0, pending.remaining(2.0));
	EXPECT_EQ(100, pending.created_sequence());
	EXPECT_EQ(2, pending.version());
}

TEST(PendingTarget, ContractsHaveIndependentClocks)
{
	CtaPendingTarget first, second;
	first.on_target("CFFEX.IF.HOT", 8.0, 2.0, 100);
	second.on_target("CFFEX.IC.HOT", -4.0, 0.0, 105);
	first.on_decision(FillStatus::NoQuote, 2.0);
	EXPECT_EQ(100, first.created_sequence());
	EXPECT_EQ(105, second.created_sequence());
	EXPECT_TRUE(first.phase() == PendingPhase::WaitingLiquidity);
	EXPECT_TRUE(second.phase() == PendingPhase::WaitingLatency);
	EXPECT_DOUBLE_EQ(-4.0, second.remaining(0.0));
}

TEST(PendingTarget, FilledTargetReturnsToIdle)
{
	CtaPendingTarget pending;
	pending.on_target("CFFEX.IF.HOT", 8.0, 2.0, 100);
	pending.on_decision(FillStatus::Filled, 8.0);
	EXPECT_TRUE(pending.phase() == PendingPhase::Idle);
	EXPECT_DOUBLE_EQ(0.0, pending.remaining(8.0));
}

// Day16 全部使用明确的合成事件预算；这里不声称任何真实 .dsb Tick 的量已审计。
// 把模型决策和事件账本串起来：actual 只按本次增量变化，而非直接跳到 target。
TEST(VolumeLimitedFill, SequentialBudgetsConserveQuantity)
{
	VolumeLimitedFill model(1.0);
	CtaEventVolumeLedger ledger;
	double actual = 0.0;
	const double budgets[] = {3.0, 4.0, 10.0};
	const double fills[] = {3.0, 4.0, 3.0};
	const double remainings[] = {7.0, 3.0, 0.0};
	for (uint64_t i = 0; i < 3; ++i)
	{
		FillRequest request = causal_request(actual, 10.0, 100, 101 + i);
		request.market.volume_delta = budgets[i];
		ledger.begin(20190910, request.market.event_sequence);
		request.market.volume_used = ledger.used();
		const FillResult fill = model.decide(request);
		EXPECT_TRUE(fill.status == FillStatus::Filled);
		EXPECT_DOUBLE_EQ(fills[i], fill.signed_delta);
		EXPECT_TRUE(std::abs(fill.signed_delta) <= budgets[i]);
		actual += fill.signed_delta;
		ledger.consume(std::abs(fill.signed_delta));
		EXPECT_DOUBLE_EQ(actual, request.actual_position + fill.signed_delta);
		EXPECT_DOUBLE_EQ(remainings[i], 10.0 - actual);
	}
	EXPECT_DOUBLE_EQ(10.0, actual);
}

TEST(VolumeLimitedFill, ZeroBudgetDoesNotFill)
{
	VolumeLimitedFill model(1.0);
	FillRequest request = causal_request(0.0, 5.0, 100, 101);
	request.market.volume_delta = 0.0;
	const FillResult fill = model.decide(request);
	EXPECT_TRUE(fill.status == FillStatus::NoLiquidity);
	EXPECT_DOUBLE_EQ(0.0, fill.signed_delta);
	CtaPendingTarget pending;
	pending.on_target(request.market.code, 5.0, 0.0, 100);
	pending.on_decision(fill.status, 0.0);
	EXPECT_TRUE(pending.phase() == PendingPhase::WaitingLiquidity);
}

TEST(VolumeLimitedFill, RateFloorsBudgetAndCapsAtRemaining)
{
	VolumeLimitedFill model(0.25);
	FillRequest request = causal_request(0.0, 9.0, 100, 101);
	request.market.volume_delta = 15.0; // floor(15 * 0.25) == 3
	EXPECT_DOUBLE_EQ(3.0, model.decide(request).signed_delta);
	request = causal_request(7.0, 9.0, 100, 102);
	request.market.volume_delta = 100.0;
	EXPECT_DOUBLE_EQ(2.0, model.decide(request).signed_delta);
}

TEST(VolumeLimitedFill, SellsAndReversesWithoutSkippingZero)
{
	VolumeLimitedFill model(1.0);
	FillRequest request = causal_request(3.0, -4.0, 100, 101);
	request.market.volume_delta = 3.0;
	const FillResult close = model.decide(request);
	EXPECT_DOUBLE_EQ(-3.0, close.signed_delta);
	EXPECT_DOUBLE_EQ(0.0, request.actual_position + close.signed_delta);
	request = causal_request(0.0, -4.0, 100, 102);
	request.market.volume_delta = 2.0;
	const FillResult open = model.decide(request);
	EXPECT_DOUBLE_EQ(-2.0, open.signed_delta);
	EXPECT_DOUBLE_EQ(-2.0, request.actual_position + open.signed_delta);
}

TEST(VolumeLimitedFill, SameEventSharesBudgetAfterTargetOverwrite)
{
	VolumeLimitedFill model(1.0);
	CtaEventVolumeLedger ledger;
	ledger.begin(20190910, 101);
	FillRequest request = causal_request(0.0, 10.0, 100, 101);
	request.market.volume_delta = 4.0;
	request.market.volume_used = ledger.used();
	const FillResult first = model.decide(request);
	EXPECT_DOUBLE_EQ(4.0, first.signed_delta);
	ledger.consume(std::abs(first.signed_delta));
	ledger.begin(20190910, 101); // 第二次 proc_tick 不得重置已用预算。
	request = causal_request(4.0, 8.0, 100, 101);
	request.market.volume_delta = 4.0;
	request.market.volume_used = ledger.used();
	const FillResult second = model.decide(request);
	EXPECT_TRUE(second.status == FillStatus::NoLiquidity);
	EXPECT_DOUBLE_EQ(4.0, ledger.used());
	ledger.begin(20190910, 102);
	EXPECT_DOUBLE_EQ(0.0, ledger.used());
}

TEST(VolumeLimitedFill, ContractsHaveIndependentBudgets)
{
	CtaEventVolumeLedger first, second;
	first.begin(20190910, 101);
	second.begin(20190910, 101);
	first.consume(3.0);
	EXPECT_DOUBLE_EQ(3.0, first.used());
	EXPECT_DOUBLE_EQ(0.0, second.used());
	first.begin(20190911, 101);
	EXPECT_DOUBLE_EQ(0.0, first.used());
}

TEST(VolumeLimitedFill, RejectsInvalidVolumeAndWaitsForQuote)
{
	VolumeLimitedFill model(0.5);
	FillRequest request = causal_request(0.0, 3.0, 100, 101);
	request.market.volume_delta = -1.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::InvalidMarketData);
	request.market.volume_delta = std::numeric_limits<double>::quiet_NaN();
	EXPECT_TRUE(model.decide(request).status == FillStatus::InvalidMarketData);
	request.market.volume_delta = 10.0;
	request.market.ask1 = 0.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::NoQuote);
	request.market.ask1 = 101.0;
	request.market.event_sequence = 100;
	EXPECT_TRUE(model.decide(request).status == FillStatus::WaitingLatency);
}

TEST(TickVolumeSource, FirstTickBuildsBaselineAndNextTickUsesVerifiedDelta)
{
	CtaTickVolumeSource source;
	// 实样本 20210104 前两笔：240 -> 311，对应 volume 240、71。
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 240.0, 240.0));
	EXPECT_DOUBLE_EQ(71.0, source.observe(20210104, 311.0, 71.0));
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 311.0, 0.0));
}

TEST(TickVolumeSource, TradingDateResetAndBackwardTotalDoNotCreateBudget)
{
	CtaTickVolumeSource source;
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 100.0, 100.0));
	EXPECT_DOUBLE_EQ(5.0, source.observe(20210104, 105.0, 5.0));
	// 异常回退的当前笔为零预算；下一笔只从新基线 80 算到 83。
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 80.0, 0.0));
	EXPECT_DOUBLE_EQ(3.0, source.observe(20210104, 83.0, 3.0));
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210105, 160.0, 160.0));
	EXPECT_DOUBLE_EQ(36.0, source.observe(20210105, 196.0, 36.0));
}

TEST(TickVolumeSource, InvalidOrMismatchedFieldsGetZeroBudget)
{
	CtaTickVolumeSource source;
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 100.0, 100.0));
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 105.0, 6.0));
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 110.0,
		std::numeric_limits<double>::quiet_NaN()));
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104,
		std::numeric_limits<double>::infinity(), 1.0));
	EXPECT_DOUBLE_EQ(2.0, source.observe(20210104, 112.0, 2.0));
}

// Day17：created=100 时三种配置首次可尝试事件分别为 101/102/104。
// 每一行也验证“前一事件不能抢跑”，而不是只验证最终能成交。
TEST(Latency, FirstEligibleEventTable)
{
	struct Row { uint64_t delay; uint64_t first; };
	const Row rows[] = {{0, 101}, {1, 102}, {3, 104}};
	for (const Row& row : rows)
	{
		CausalTouchFill model(row.delay);
		EXPECT_TRUE(model.decide(causal_request(0.0, 1.0, 100, row.first - 1)).status
			== FillStatus::WaitingLatency);
		EXPECT_TRUE(model.decide(causal_request(0.0, 1.0, 100, row.first)).status
			== FillStatus::Filled);
	}
}

TEST(Latency, MissingQuoteAfterActivationDoesNotRestartClock)
{
	CausalTouchFill model(1);
	FillRequest request = causal_request(0.0, 2.0, 100, 102);
	request.market.ask1 = 0.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::NoQuote);
	request.market.event_sequence = 103;
	request.market.ask1 = 101.0;
	EXPECT_DOUBLE_EQ(2.0, model.decide(request).signed_delta);
}

TEST(Latency, NearSequenceOverflowNeverActivatesEarly)
{
	const uint64_t max_seq = std::numeric_limits<uint64_t>::max();
	CausalTouchFill model(3);
	EXPECT_TRUE(model.decide(causal_request(0.0, 1.0, max_seq - 2, max_seq)).status
		== FillStatus::WaitingLatency);
	EXPECT_TRUE(model.decide(causal_request(0.0, 1.0, max_seq - 4, max_seq)).status
		== FillStatus::Filled);
}

TEST(Latency, VolumeModelUsesSameCausalGate)
{
	VolumeLimitedFill model(0.5, 1);
	FillRequest request = causal_request(0.0, 10.0, 100, 101);
	request.market.volume_delta = 20.0;
	EXPECT_TRUE(model.decide(request).status == FillStatus::WaitingLatency);
	request.market.event_sequence = 102;
	EXPECT_DOUBLE_EQ(10.0, model.decide(request).signed_delta);
}

// Day18：价格策略是纯函数。Legacy 的旧滑点分支由端到端 golden 单独验证。
TEST(PricePolicy, BuyAndSellFollowTouchSlippageAndDirectionalTickAlignment)
{
	CtaPriceRequest request;
	request.touch_price = 100.1;
	request.price_tick = 0.2;
	request.slippage = 1.0;
	request.is_buy = true;
	EXPECT_DOUBLE_EQ(100.4, CtaPricePolicy::calculate(request).execution_price);
	request.is_buy = false;
	EXPECT_DOUBLE_EQ(99.8, CtaPricePolicy::calculate(request).execution_price);
}

TEST(PricePolicy, RatioSlippageAndNoSlippageAreDeterministic)
{
	CtaPriceRequest request;
	request.touch_price = 100.0;
	request.price_tick = 0.2;
	request.slippage = 10.0; // 10 bps = 0.1 元，买方对齐到 100.2。
	request.ratio_slippage = true;
	EXPECT_DOUBLE_EQ(100.2, CtaPricePolicy::calculate(request).execution_price);
	request.slippage = 0.0;
	request.touch_price = 100.1;
	EXPECT_DOUBLE_EQ(100.2, CtaPricePolicy::calculate(request).execution_price);
	request.is_buy = false;
	EXPECT_DOUBLE_EQ(100.0, CtaPricePolicy::calculate(request).execution_price);
}

TEST(PricePolicy, LimitBoundaryIsInclusiveAndBeyondItWaits)
{
	CtaPriceRequest request;
	request.touch_price = 100.0;
	request.price_tick = 0.2;
	request.slippage = 1.0;
	request.upper_limit = 100.2;
	EXPECT_TRUE(CtaPricePolicy::calculate(request).status == FillStatus::Filled);
	request.upper_limit = 100.0;
	EXPECT_TRUE(CtaPricePolicy::calculate(request).status == FillStatus::PriceOutOfBounds);
	request.is_buy = false;
	request.lower_limit = 99.8;
	request.upper_limit = 0.0;
	EXPECT_TRUE(CtaPricePolicy::calculate(request).status == FillStatus::Filled);
	request.lower_limit = 100.0;
	EXPECT_TRUE(CtaPricePolicy::calculate(request).status == FillStatus::PriceOutOfBounds);
}

TEST(PricePolicy, BadQuoteTickSlipAndLimitsNeverYieldExecutionPrice)
{
	CtaPriceRequest request;
	request.touch_price = 100.0;
	request.price_tick = 0.2;
	struct Row { double touch; double tick; double slip; double lower; double upper; FillStatus status; };
	const Row rows[] = {
		{0.0, 0.2, 0.0, 0.0, 0.0, FillStatus::NoQuote},
		{-1.0, 0.2, 0.0, 0.0, 0.0, FillStatus::InvalidMarketData},
		{100.0, 0.0, 0.0, 0.0, 0.0, FillStatus::InvalidMarketData},
		{100.0, 0.2, -1.0, 0.0, 0.0, FillStatus::InvalidMarketData},
		{100.0, 0.2, 0.0, 101.0, 100.0, FillStatus::InvalidMarketData},
		{100.0, 0.2, 0.0, 0.0, std::numeric_limits<double>::infinity(), FillStatus::InvalidMarketData}
	};
	for (const Row& row : rows)
	{
		request.touch_price = row.touch;
		request.price_tick = row.tick;
		request.slippage = row.slip;
		request.lower_limit = row.lower;
		request.upper_limit = row.upper;
		const CtaPriceResult result = CtaPricePolicy::calculate(request);
		EXPECT_TRUE(result.status == row.status);
		EXPECT_DOUBLE_EQ(0.0, result.execution_price);
	}
}

// Day19/20：零成交状态不能被误当作 Filled；已用预算、反手和目标覆盖
// 均必须守住数量守恒。真实费用/CSV 另由回测回放验证。
TEST(AccountingBoundary, NoFillNeverCarriesSignedQuantity)
{
	VolumeLimitedFill model(0.5);
	FillRequest request = causal_request(0.0, 10.0, 100, 100);
	request.market.volume_delta = 20.0;
	EXPECT_DOUBLE_EQ(0.0, model.decide(request).signed_delta);
	request.market.event_sequence = 101;
	request.market.volume_delta = 0.0;
	EXPECT_DOUBLE_EQ(0.0, model.decide(request).signed_delta);
	request.market.volume_delta = 20.0;
	request.market.ask1 = 0.0;
	EXPECT_DOUBLE_EQ(0.0, model.decide(request).signed_delta);
}

TEST(Adversarial, RepeatedTickAndBackwardTotalCannotReissueBudget)
{
	CtaTickVolumeSource source;
	CtaEventVolumeLedger ledger;
	VolumeLimitedFill model(1.0);
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 100.0, 100.0));
	const double delta = source.observe(20210104, 104.0, 4.0);
	ledger.begin(20210104, 101);
	FillRequest request = causal_request(0.0, 10.0, 100, 101);
	request.market.volume_delta = delta;
	const FillResult first = model.decide(request);
	EXPECT_DOUBLE_EQ(4.0, first.signed_delta);
	ledger.consume(4.0);
	ledger.begin(20210104, 101);
	request.actual_position = 4.0;
	request.market.volume_used = ledger.used();
	EXPECT_TRUE(model.decide(request).status == FillStatus::NoLiquidity);
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 104.0, 0.0));
	EXPECT_DOUBLE_EQ(0.0, source.observe(20210104, 90.0, 0.0));
	EXPECT_DOUBLE_EQ(2.0, source.observe(20210104, 92.0, 2.0));
}

TEST(Adversarial, PendingTargetCancellationAndReversalNeverReuseOldIntent)
{
	CtaPendingTarget pending;
	pending.on_target("CFFEX.IF.HOT", 10.0, 2.0, 100);
	pending.on_decision(FillStatus::Filled, 5.0);
	EXPECT_DOUBLE_EQ(5.0, pending.remaining(5.0));
	pending.on_target("CFFEX.IF.HOT", 5.0, 5.0, 101);
	EXPECT_TRUE(pending.phase() == PendingPhase::Idle);
	pending.on_target("CFFEX.IF.HOT", -2.0, 5.0, 102);
	EXPECT_DOUBLE_EQ(-7.0, pending.remaining(5.0));
	EXPECT_EQ(102, pending.created_sequence());
}

TEST(Adversarial, ExactDuplicateDoesNotAdvanceCausalInputClock)
{
	CtaTickInputGuard guard;
	EXPECT_TRUE(guard.accept(20210104, 20210104, 93000400,
		311.0, 5230.8, 5231.4, 5231.6, 2.0, 1.0));
	EXPECT_FALSE(guard.accept(20210104, 20210104, 93000400,
		311.0, 5230.8, 5231.4, 5231.6, 2.0, 1.0));
	// 同毫秒但盘口确实变化，不是完全相同的行情输入。
	EXPECT_TRUE(guard.accept(20210104, 20210104, 93000400,
		311.0, 5230.8, 5231.4, 5231.8, 2.0, 1.0));
}

TEST(Adversarial, BackwardTimestampAndCumulativeVolumeAreRejected)
{
	CtaTickInputGuard guard;
	EXPECT_TRUE(guard.accept(20210104, 20210104, 93000900,
		398.0, 5231.8, 5232.8, 5234.8, 3.0, 1.0));
	EXPECT_FALSE(guard.accept(20210104, 20210104, 93000400,
		400.0, 5232.0, 5232.8, 5234.8, 3.0, 1.0));
	EXPECT_FALSE(guard.accept(20210104, 20210104, 93001400,
		390.0, 5234.0, 5233.0, 5234.4, 1.0, 21.0));
	EXPECT_TRUE(guard.accept(20210104, 20210104, 93001400,
		511.0, 5234.0, 5233.0, 5234.4, 1.0, 21.0));
}

TEST(Adversarial, TradingDateResetAllowsNewDailyBaseline)
{
	CtaTickInputGuard guard;
	EXPECT_TRUE(guard.accept(20210104, 20210104, 150000400,
		84288.0, 5260.0, 5259.2, 5262.0, 5.0, 14.0));
	EXPECT_TRUE(guard.accept(20210105, 20210105, 92900200,
		160.0, 5240.0, 5239.8, 5240.2, 2.0, 3.0));
	EXPECT_FALSE(guard.accept(20210104, 20210104, 150000400,
		84288.0, 5260.0, 5259.2, 5262.0, 5.0, 14.0));
}

TEST(Adversarial, PendingTargetCanWaitAcrossSessionBoundary)
{
	CtaPendingTarget pending;
	CtaEventVolumeLedger ledger;
	VolumeLimitedFill model(1.0);
	pending.on_target("CFFEX.IF.HOT", 2.0, 0.0, 100);
	FillRequest request = causal_request(0.0, pending.target(), 100, 101);
	request.market.volume_delta = 0.0;
	pending.on_decision(model.decide(request).status, 0.0);
	EXPECT_TRUE(pending.phase() == PendingPhase::WaitingLiquidity);
	// 会话结束本身没有隐式成交或取消；下一交易日首 Tick 重建量基线后，
	// 有可信增量的后续事件可继续尝试。
	ledger.begin(20210105, 103);
	request.market.trading_date = 20210105;
	request.market.event_sequence = 103;
	request.market.volume_delta = 2.0;
	request.market.volume_used = ledger.used();
	const FillResult result = model.decide(request);
	EXPECT_DOUBLE_EQ(2.0, result.signed_delta);
	pending.on_decision(result.status, 2.0);
	EXPECT_TRUE(pending.phase() == PendingPhase::Idle);
}
