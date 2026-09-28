/*!
 * \file CtaMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <sstream>
#include <atomic>
#include <memory>
#include <unordered_map>
#include "HisDataReplayer.h"
#include "CtaFillModel.h"  // CTA 回测内部成交决策接口；不属于对外 Porter ABI

#include "../Includes/FasterDefs.h"
#include "../Includes/ICtaStraCtx.h"
#include "../Includes/CtaStrategyDefs.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSCollection.hpp"

#include "../Share/DLLHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/fmtlib.h"

NS_WTP_BEGIN
class EventNotifier;
NS_WTP_END

USING_NS_WTP;

class HisDataReplayer;
class CtaStrategy;

const char COND_ACTION_OL = 0;	//开多
const char COND_ACTION_CL = 1;	//平多
const char COND_ACTION_OS = 2;	//开空
const char COND_ACTION_CS = 3;	//平空
const char COND_ACTION_SP = 4;	//直接设置仓位

typedef struct _CondEntrust
{
	WTSCompareField _field;
	WTSCompareType	_alg;
	double			_target;

	double			_qty;

	char			_action;	//0-开多,1-平多,2-开空,3-平空

	char			_code[MAX_INSTRUMENT_LENGTH];
	char			_usertag[32];


	_CondEntrust()
	{
		memset(this, 0, sizeof(_CondEntrust));
	}

} CondEntrust;

typedef std::vector<CondEntrust>	CondList;
typedef wt_hashmap<std::string, CondList>	CondEntrustMap;


class CtaMocker : public ICtaStraCtx, public IDataSink
{
// CtaMocker 是回测中的策略上下文和会计执行者：保存最新目标、实际持仓，
// 接收回放 Tick，调用成交模型后再写成交、费用、盈亏等。
// 成交模型只回答“此刻能成交多少、以哪个未加滑点的基准价”，不能自行改账。
public:
	CtaMocker(HisDataReplayer* replayer, const char* name, int32_t slippage = 0, bool persistData = true, EventNotifier* notifier = NULL, bool isRatioSlp = false);
	virtual ~CtaMocker();

private:
	void	dump_outputs();
	void	dump_stradata();
	void	dump_chartdata();
	inline void log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "");
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0, uint32_t barNo = 0);
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double maxprofit, double maxloss, double totalprofit = 0, const char* enterTag = "", const char* exitTag = "", uint32_t openBarNo = 0, uint32_t closeBarNo = 0);

	void	update_dyn_profit(const char* stdCode, double price);

	// 每次 proc_tick 尝试消费一个目标时进入此函数：
	// 1. 从 _pos_map 取实际仓位，从 _sig_map 取待完成目标；
	// 2. 组织 FillRequest，包括本 Tick 盘口、经核验的市场增量和已用预算；
	// 3. 调用 _fill_model->decide；Filled 时只按 signed_delta 记本次成交。
	// WaitingLatency/NoQuote/NoLiquidity/InvalidMarketData 不记账，调用者保留信号继续等待。
	FillStatus do_set_position(const char* stdCode, double qty, double price,
		const char* userTag, uint64_t created_sequence, SignalSource source);
	// 保留旧 do_set_position 的会计流程：开平拆分、滑点、费用、盈亏、持仓与日志。
	// qty 是“实际旧仓位 + 本次成交增量”，不是策略期望的最终目标。
	// 例如当前 0、目标 10、本次成交 7：这里必须传 7，不能传 10。
	double	apply_fill(const char* stdCode, double qty, const char* userTag,
		uint64_t curTm, uint32_t curTDate, const FillResult& fill);
	void	append_signal(const char* stdCode, double qty, const char* userTag, double price, uint32_t sigType);

	inline CondList& get_cond_entrusts(const char* stdCode);

	void	proc_tick(const char* stdCode, double last_px, double cur_px);
	uint64_t current_event_sequence(const char* stdCode) const;

public:
	bool	init_cta_factory(WTSVariant* cfg);
	// 仅供 C++ 内部配置和 YAML 的 cta.model 使用；默认仍为 legacy_cta。
	// volume_limited 需要 (0,1] 的 participation_rate；旧模型不接受非零 rate。
	// 切换模型时清空量源和预算账本，防止上一种模型的状态渗入新实验。
	bool	configure_fill_model(const char* model, uint64_t delay_events = 0,
		double participation_rate = 0.0);
	void	load_incremental_data(const char* lastBacktestName);
	void	install_hook();
	void	enable_hook(bool bEnabled = true);
	bool	step_calc();

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataSink
	virtual void	handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType = 0) override;
	virtual void	handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	virtual void	handle_schedule(uint32_t uDate, uint32_t uTime) override;

	virtual void	handle_init() override;
	virtual void	handle_session_begin(uint32_t curTDate) override;
	virtual void	handle_session_end(uint32_t curTDate) override;

	virtual void	handle_section_end(uint32_t curTDate, uint32_t curTime) override;

	virtual void	handle_replay_done() override;

	//////////////////////////////////////////////////////////////////////////
	//ICtaStraCtx
	virtual uint32_t id() { return _context_id; }

	//回调函数
	virtual void on_init() override;
	virtual void on_session_begin(uint32_t curTDate) override;
	virtual void on_session_end(uint32_t curTDate) override;
	virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override;
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	virtual bool on_schedule(uint32_t curDate, uint32_t curTime) override;
	virtual void enum_position(FuncEnumCtaPosCallBack cb, bool bForExecute) override;

	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override;
	virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override;
	virtual void on_calculate(uint32_t curDate, uint32_t curTime) override;


	//////////////////////////////////////////////////////////////////////////
	//策略接口
	virtual void stra_enter_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	virtual void stra_enter_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	virtual void stra_exit_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	virtual void stra_exit_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;

	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override;
	virtual void stra_set_position(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override;
	virtual double stra_get_price(const char* stdCode) override;

	/*
	 *	读取当日价格
	 */
	virtual double stra_get_day_price(const char* stdCode, int flag = 0) override;

	virtual uint32_t stra_get_tdate() override;
	virtual uint32_t stra_get_date() override;
	virtual uint32_t stra_get_time() override;

	virtual double stra_get_fund_data(int flag = 0) override;

	virtual uint64_t stra_get_first_entertime(const char* stdCode) override;
	virtual uint64_t stra_get_last_entertime(const char* stdCode) override;
	virtual uint64_t stra_get_last_exittime(const char* stdCode) override;
	virtual double stra_get_last_enterprice(const char* stdCode) override;
	virtual const char* stra_get_last_entertag(const char* stdCode) override;
	virtual double stra_get_position_avgpx(const char* stdCode) override;
	virtual double stra_get_position_profit(const char* stdCode) override;

	virtual uint64_t stra_get_detail_entertime(const char* stdCode, const char* userTag) override;
	virtual double stra_get_detail_cost(const char* stdCode, const char* userTag) override;
	virtual double stra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override;

	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain = false) override;
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) override;
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) override;

	virtual void stra_sub_ticks(const char* stdCode) override;
	virtual void stra_sub_bar_events(const char* stdCode, const char* period) override;

	/*
	 *	获取分月合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	virtual void stra_log_info(const char* message) override;
	virtual void stra_log_debug(const char* message) override;
	virtual void stra_log_warn(const char* message) override;
	virtual void stra_log_error(const char* message) override;

	virtual void stra_save_user_data(const char* key, const char* val) override;
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

	/*
	 *	设置图表K线
	 */
	virtual void set_chart_kline(const char* stdCode, const char* period) override;

	/*
	 *	添加信号
	 */
	virtual void add_chart_mark(double price, const char* icon, const char* tag) override;

	/*
	 *	添加指标
	 */
	virtual void register_index(const char* idxName, uint32_t indexType) override;

	/*
	 *	添加指标线
	 */
	virtual bool register_index_line(const char* idxName, const char* lineName, uint32_t lineType) override;

	/*
	 *	添加基准线
	 *	@idxName	指标名称
	 *	@lineName	线条名称
	 *	@val		数值
	 */
	virtual bool add_index_baseline(const char* idxName, const char* lineName, double val) override;

	/*
	 *	设置指标值
	 */
	virtual bool set_index_value(const char* idxName, const char* lineName, double val) override;

private:
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

protected:
	uint32_t			_context_id;
	HisDataReplayer*	_replayer;

	uint64_t		_total_calc_time;	//总计算时间
	uint32_t		_emit_times;		//总计算次数

	int32_t			_slippage;			//成交滑点， 如果是比例滑点，则为万分比
	bool			_ratio_slippage;	//是否比例滑点
	// 模型由上下文独占，且只在配置时构造，不会在每个 Tick 上分配。
	// 默认 Legacy；只有显式选 volume_limited，以下三个按合约分组的表才会参与撮合。
	std::unique_ptr<ICtaFillModel> _fill_model;
	std::string _fill_model_name = "legacy_cta"; // 用于可审计输出，避免根据类型名猜配置
	uint64_t _fill_delay_events = 0;
	uint64_t _event_sequence = 0; // 兼容旧状态文件的全局计数；Legacy 仍沿用
	// 新增模型的延迟按合约自己的输入 Tick 计数，其他合约不能替它推进时钟。
	std::unordered_map<std::string, uint64_t> _contract_event_sequences;
	// stdCode -> 当前 Tick 已实际成交的手数；同 Tick 双 proc_tick 不会重复领预算。
	std::unordered_map<std::string, CtaEventVolumeLedger> _volume_ledgers;
	// stdCode -> 上条真实 Tick 的日内累计量及交易日；用于校验 volume 字段。
	std::unordered_map<std::string, CtaTickVolumeSource> _tick_volume_sources;
	std::unordered_map<std::string, CtaTickInputGuard> _tick_input_guards;
	// stdCode -> 当前 Tick 校验后的新增市场量；一次 handle_tick 只计算一次。
	std::unordered_map<std::string, double> _event_volumes;
	bool _in_tick_callback = false; // 标记目标是否由 on_tick_updated 生成，以审计因果时序

	uint32_t		_schedule_times;	//调度次数

	std::string		_main_key;

	std::string		_main_code;
	std::string		_main_period;

	typedef struct _KlineTag
	{
		bool	_closed;
		bool	_notify;

		_KlineTag() :_closed(false), _notify(false){}

	} KlineTag;
	typedef wt_hashmap<std::string, KlineTag> KlineTags;
	KlineTags	_kline_tags;

	typedef wt_hashmap<std::string, double> PriceMap;
	PriceMap		_price_map;

	typedef struct _DetailInfo
	{
		bool		_long;
		double		_price;
		double		_volume;
		uint64_t	_opentime;
		uint32_t	_opentdate;
		double		_max_profit;
		double		_max_loss;
		double		_max_price;
		double		_min_price;
		double		_profit;
		char		_opentag[32];
		uint32_t	_open_barno;

		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	typedef struct _PosInfo
	{
		double		_volume;
		double		_closeprofit;
		double		_dynprofit;
		uint64_t	_last_entertime;
		uint64_t	_last_exittime;
		double		_frozen;

		std::vector<DetailInfo> _details;

		_PosInfo()
		{
			_volume = 0;
			_closeprofit = 0;
			_dynprofit = 0;
			_frozen = 0;
			_last_entertime = 0;
			_last_exittime = 0;
		}

		inline double valid() const { return _volume - _frozen; }
	} PosInfo;
	typedef wt_hashmap<std::string, PosInfo> PositionMap;
	PositionMap		_pos_map;
	double	_total_closeprofit;

	typedef struct _SigInfo
	{
		// 同合约只保留最新目标，覆盖旧目标；部分成交后的剩余量随实际仓位重算。
		CtaPendingTarget _pending;
		std::string	_usertag;
		double		_sigprice; // 记录信号生成时的行情价，用于信号输出/审计
		double		_desprice; // 非零时作为指定触发基准价；因果模型仍用真实 touch 价
		uint32_t	_sigtype;
		uint64_t	_gentime;
		SignalSource _source;        // 产生目标的回调/条件来源

		_SigInfo()
		{
			_sigprice = 0;
			_desprice = 0;
			_sigtype = 0;
			_gentime = 0;
			_source = SignalSource::Unknown;
		}
	}SigInfo;
	typedef wt_hashmap<std::string, SigInfo>	SignalMap;
	SignalMap		_sig_map;

	std::stringstream	_trade_logs;
	std::stringstream	_close_logs;
	std::stringstream	_fund_logs;
	std::stringstream	_sig_logs;
	std::stringstream	_pos_logs;
	std::stringstream _fill_decision_logs; // 每次目标尝试的因果/预算/状态
	std::stringstream _fill_audit_logs;    // 只记录实际记账的成交增量与成本分解
	std::stringstream _equity_event_logs;  // 每条有效 Tick 后的模拟净盈亏，用于窗口回撤
	std::stringstream _target_replacement_logs; // 旧目标被新目标覆盖时尚未成交的数量
	std::stringstream	_index_logs;
	std::stringstream	_mark_logs;

	CondEntrustMap		_condtions;

	//是否处于调度中的标记
	bool			_is_in_schedule;	//是否在自动调度中

	//用户数据
	typedef wt_hashmap<std::string, std::string> StringHashMap;
	StringHashMap	_user_datas;
	bool			_ud_modified;

	typedef struct _StraFundInfo
	{
		double	_total_profit;
		double	_total_dynprofit;
		double	_total_fees;

		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	StraFundInfo		_fund_info;

	typedef struct _StraFactInfo
	{
		std::string		_module_path;
		DllHandle		_module_inst;
		ICtaStrategyFact*	_fact;
		FuncCreateStraFact	_creator;
		FuncDeleteStraFact	_remover;

		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		~_StraFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} StraFactInfo;
	StraFactInfo	_factory;

	CtaStrategy*	_strategy;
	EventNotifier*	_notifier;

	StdUniqueMutex	_mtx_calc;
	StdCondVariable	_cond_calc;
	bool			_has_hook;		//这是人为控制是否启用钩子
	bool			_hook_valid;	//这是根据是否是异步回测模式而确定钩子是否可用
	std::atomic<uint32_t>		_cur_step;	//临时变量，用于控制状态

	bool			_in_backtest;
	bool			_wait_calc;

	//是否对回测结果持久化
	bool			_persist_data;

	uint32_t		_cur_tdate;
	uint32_t		_cur_bartime;
	uint64_t		_last_cond_min;

	//tick订阅列表
	wt_hashset<std::string> _tick_subs;

	std::string		_chart_code;
	std::string		_chart_period;

	typedef struct _ChartLine
	{
		std::string	_name;
		uint32_t	_lineType;
	} ChartLine;

	typedef struct _ChartIndex
	{
		std::string	_name;
		uint32_t	_indexType;
		std::unordered_map<std::string, ChartLine> _lines;
		std::unordered_map<std::string, double> _base_lines;
	} ChartIndex;

	std::unordered_map<std::string, ChartIndex>	_chart_indice;

	typedef wt_hashmap<std::string, WTSTickStruct>	TickCache;
	TickCache	_ticks;
};
