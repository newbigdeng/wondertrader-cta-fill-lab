#include "gtest/gtest/gtest.h"

#include <limits>

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
