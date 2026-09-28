/*!
 * \file CtaMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "CtaMocker.h"
#include "WtHelper.h"
#include "EventNotifier.h"

#include <exception>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <boost/filesystem.hpp>

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include "rapidjson/filereadstream.h"
#include <fstream>
namespace rj = rapidjson;

const char* CMP_ALG_NAMES[] =
{
	"＝",
	">",
	"<",
	">=",
	"<="
};

const char* ACTION_NAMES[] =
{
	"OL",
	"CL",
	"OS",
	"CS",
	"SYN"
};


inline uint32_t makeCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 1 };
	return _auto_context_id.fetch_add(1);
}


// 每个 CTA 回测上下文只持有一个默认 Legacy 模型；模型在构造时创建，
// 后续行情与信号复用同一实例，不会在每个 Tick 上重复分配。
// 因此旧配置或完全不指定 fill_model 的回测仍沿用“目标差额一次成交”。
CtaMocker::CtaMocker(HisDataReplayer* replayer, const char* name, int32_t slippage /* = 0 */, bool persistData /* = true */, EventNotifier* notifier /* = NULL */, bool isRatioSlp /* = false */)
	: ICtaStraCtx(name)
	, _replayer(replayer)
	, _total_calc_time(0)
	, _emit_times(0)
	, _is_in_schedule(false)
	, _ud_modified(false)
	, _strategy(NULL)
	, _slippage(slippage)
	, _ratio_slippage(isRatioSlp)
	, _fill_model(new LegacyCtaFill())
	, _schedule_times(0)
	, _total_closeprofit(0)
	, _notifier(notifier)
	, _has_hook(false)
	, _hook_valid(true)
	, _cur_step(0)
	, _wait_calc(false)
	, _in_backtest(false)
	, _persist_data(persistData)
{
	_context_id = makeCtxId();
}


CtaMocker::~CtaMocker()
{
}

uint64_t CtaMocker::current_event_sequence(const char* stdCode) const
{
	if (_fill_model_name == "legacy_cta")
		return _event_sequence;
	const auto it = _contract_event_sequences.find(stdCode);
	return it == _contract_event_sequences.end() ? 0 : it->second;
}

void CtaMocker::dump_stradata()
{
	rj::Document root(rj::kObjectType);
	// 增量回测续跑时恢复逻辑事件钟，避免 pending 信号永远等不到保存的序号。
	// 注意：目前 Day16 的 Tick 成交量基线与事件预算账本尚未序列化；
	// 任意事件中途恢复 volume_limited 的一致性还不能仅靠此字段保证。
	root.AddMember("event_sequence", _event_sequence, root.GetAllocator());
	// Day17：新增模型需要每合约时钟；全局旧字段仍保留以兼容已有快照。
	{
		rj::Value clocks(rj::kObjectType);
		for (const auto& item : _contract_event_sequences)
		{
			rj::Value sequence;
			sequence.SetUint64(item.second);
			clocks.AddMember(rj::Value(item.first.c_str(), root.GetAllocator()),
				sequence, root.GetAllocator());
		}
		root.AddMember("contract_event_sequences", clocks, root.GetAllocator());
	}

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosInfo& pInfo = it->second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("code", rj::Value(stdCode, allocator), allocator);
			pItem.AddMember("volume", pInfo._volume, allocator);
			pItem.AddMember("closeprofit", pInfo._closeprofit, allocator);
			pItem.AddMember("dynprofit", pInfo._dynprofit, allocator);
			pItem.AddMember("lastentertime", pInfo._last_entertime, allocator);
			pItem.AddMember("lastexittime", pInfo._last_exittime, allocator);

			rj::Value details(rj::kArrayType);
			for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
			{
				const DetailInfo& dInfo = *dit;
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("long", dInfo._long, allocator);
				dItem.AddMember("price", dInfo._price, allocator);
				dItem.AddMember("maxprice", dInfo._max_price, allocator);
				dItem.AddMember("minprice", dInfo._min_price, allocator);
				dItem.AddMember("volume", dInfo._volume, allocator);
				dItem.AddMember("opentime", dInfo._opentime, allocator);
				dItem.AddMember("opentdate", dInfo._opentdate, allocator);

				dItem.AddMember("profit", dInfo._profit, allocator);
				dItem.AddMember("maxprofit", dInfo._max_profit, allocator);
				dItem.AddMember("maxloss", dInfo._max_loss, allocator);
				dItem.AddMember("opentag", rj::Value(dInfo._opentag, allocator), allocator);

				details.PushBack(dItem, allocator);
			}

			pItem.AddMember("details", details, allocator);

			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{//资金保存
		rj::Value jFund(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		jFund.AddMember("total_profit", _fund_info._total_profit, allocator);
		jFund.AddMember("total_dynprofit", _fund_info._total_dynprofit, allocator);
		jFund.AddMember("total_fees", _fund_info._total_fees, allocator);
		jFund.AddMember("tdate", _cur_tdate, allocator);

		root.AddMember("fund", jFund, allocator);
	}

	{//信号保存
		// 保留的是“尚待成交的最新目标”及其创建事件，而不单是最后一次成交量。
		// 部分成交后，目标例如仍为 10，实际仓位可以暂时只有 7。
		rj::Value jSigs(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto& m : _sig_map)
		{
			const char* stdCode = m.first.c_str();
			const SigInfo& sInfo = m.second;

			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("usertag", rj::Value(sInfo._usertag.c_str(), allocator), allocator);

			// 兼容旧 JSON 键名 volume；这里实际保存目标仓位，不是本 Tick 市场 volume。
			jItem.AddMember("volume", sInfo._pending.target(), allocator);
			jItem.AddMember("sigprice", sInfo._sigprice, allocator);
			jItem.AddMember("gentime", sInfo._gentime, allocator);
			// 恢复后仍能按原信号的因果时钟等待，不把旧信号当作新创建。
			jItem.AddMember("created_seq", sInfo._pending.created_sequence(), allocator);
			jItem.AddMember("activation_seq", sInfo._pending.activation_sequence(), allocator);
			jItem.AddMember("target_version", sInfo._pending.version(), allocator);
			jItem.AddMember("pending_phase", static_cast<uint32_t>(sInfo._pending.phase()), allocator);
			jItem.AddMember("source", static_cast<uint32_t>(sInfo._source), allocator);

			jSigs.AddMember(rj::Value(stdCode, allocator), jItem, allocator);
		}

		root.AddMember("signals", jSigs, allocator);
	}

	{//条件单保存
		rj::Value jCond(rj::kObjectType);
		rj::Value jItems(rj::kObjectType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto it = _condtions.begin(); it != _condtions.end(); it++)
		{
			const char* code = it->first.c_str();
			const CondList& condList = it->second;

			rj::Value cArray(rj::kArrayType);
			for (auto& condInfo : condList)
			{
				rj::Value cItem(rj::kObjectType);
				cItem.AddMember("code", rj::Value(code, allocator), allocator);
				cItem.AddMember("usertag", rj::Value(condInfo._usertag, allocator), allocator);

				cItem.AddMember("field", (uint32_t)condInfo._field, allocator);
				cItem.AddMember("alg", (uint32_t)condInfo._alg, allocator);
				cItem.AddMember("target", condInfo._target, allocator);
				cItem.AddMember("qty", condInfo._qty, allocator);
				cItem.AddMember("action", (uint32_t)condInfo._action, allocator);

				cArray.PushBack(cItem, allocator);
			}

			jItems.AddMember(rj::Value(code, allocator), cArray, allocator);
		}
		jCond.AddMember("settime", _last_cond_min, allocator);
		jCond.AddMember("items", jItems, allocator);

		root.AddMember("conditions", jCond, allocator);
	}

	if(_persist_data)
	{
		std::string folder = WtHelper::getOutputDir();
		folder += _name;
		folder += "/";

		if (!StdFile::exists(folder.c_str()))
			boost::filesystem::create_directories(folder.c_str());

		std::string filename = folder;
		filename += _name;
		filename += ".json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}
}

void CtaMocker::dump_chartdata()
{
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	rj::Value klineItem(rj::kObjectType);
	if(_chart_code.empty())
	{
		//如果没有设置主K线，就用主K线落地
		klineItem.AddMember("code", rj::Value(_main_code.c_str(), allocator), allocator);
		klineItem.AddMember("period", rj::Value(_main_period.c_str(), allocator), allocator);
	}
	else
	{
		klineItem.AddMember("code", rj::Value(_chart_code.c_str(), allocator), allocator);
		klineItem.AddMember("period", rj::Value(_chart_period.c_str(), allocator), allocator);
	}

	root.AddMember("kline", klineItem, allocator);

	if (!_chart_indice.empty())
	{
		rj::Value jIndice(rj::kArrayType);
		for (const auto& v : _chart_indice)
		{
			const ChartIndex& cIndex = v.second;
			rj::Value jIndex(rj::kObjectType);
			jIndex.AddMember("name", rj::Value(cIndex._name.c_str(), allocator), allocator);
			jIndex.AddMember("index_type", cIndex._indexType, allocator);

			rj::Value jLines(rj::kArrayType);
			for(const auto& v2 : cIndex._lines)
			{
				const ChartLine& cLine = v2.second;
				rj::Value jLine(rj::kObjectType);
				jLine.AddMember("name", rj::Value(cLine._name.c_str(), allocator), allocator);
				jLine.AddMember("line_type", cLine._lineType, allocator);

				//rj::Value jVals(rj::kArrayType);
				//for(const double& val : cLine._values)
				//{
				//	jVals.PushBack(val, allocator);
				//}

				//jLine.AddMember("values", jVals, allocator);

				jLines.PushBack(jLine, allocator);
			}

			jIndex.AddMember("lines", jLines, allocator);

			rj::Value jBaseLines(rj::kObjectType);
			for (const auto& v3 : cIndex._base_lines)
			{
				jBaseLines.AddMember(rj::Value(v3.first.c_str(), allocator), rj::Value(v3.second), allocator);
			}

			jIndex.AddMember("baselines", jBaseLines, allocator);

			jIndice.PushBack(jIndex, allocator);
		}

		root.AddMember("index", jIndice, allocator);
	}

	//if(!_chart_marks.empty())
	//{
	//	rj::Value jMarks(rj::kArrayType);
	//	for(const ChartMark& mark : _chart_marks)
	//	{
	//		rj::Value jMark(rj::kObjectType);
	//		jMark.AddMember("bartime", mark._bartime, allocator);
	//		jMark.AddMember("price", mark._price, allocator);
	//		jMark.AddMember("icon", rj::Value(mark._icon.c_str(), allocator), allocator);
	//		jMark.AddMember("tag", rj::Value(mark._tag.c_str(), allocator), allocator);

	//		jMarks.PushBack(jMark, allocator);
	//	}

	//	root.AddMember("marks", jMarks, allocator);
	//}

	if(_persist_data)
	{
		std::string folder = WtHelper::getOutputDir();
		folder += _name;
		folder += "/";

		if(!StdFile::exists(folder.c_str()))
			boost::filesystem::create_directories(folder.c_str());

		std::string filename = folder;
		filename += "btchart.json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());

		filename = folder;
		filename += "indice.csv";
		std::string content = "bartime,index_name,line_name,value\n";
		if (!_index_logs.str().empty()) content += _index_logs.str();
		StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

		filename = folder;
		filename += "marks.csv";
		content = "bartime,price,icon,tag\n";
		if (!_mark_logs.str().empty()) content += _mark_logs.str();
		StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());
	}
}

void CtaMocker::dump_outputs()
{
	if (!_persist_data)
		return;

	std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "/";
	boost::filesystem::create_directories(folder.c_str());

	std::string filename = folder + "trades.csv";
	std::string content = "code,time,direct,action,price,qty,tag,fee,barno\n";
	if(!_trade_logs.str().empty()) content += _trade_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "closes.csv";
	content = "code,direct,opentime,openprice,closetime,closeprice,qty,profit,maxprofit,maxloss,totalprofit,entertag,exittag,openbarno,closebarno\n";
	if (!_close_logs.str().empty()) content += _close_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "funds.csv";
	content = "date,closeprofit,positionprofit,dynbalance,fee\n";
	if (!_fund_logs.str().empty()) content += _fund_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "signals.csv";
	content = "code,target,sigprice,gentime,usertag\n";
	if (!_sig_logs.str().empty()) content += _sig_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "positions.csv";
	content = "date,code,volume,closeprofit,dynprofit\n";
	if (!_pos_logs.str().empty()) content += _pos_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	// 独立审计文件不改变原版五份 CSV 的字段或顺序。decision 包括等待/拒绝；
	// audit 只包括真正经过 apply_fill 的数量，因此可核对会计与模型决策。
	filename = folder + "fill_decisions.csv";
	content = "code,model,event_seq,created_seq,activation_seq,status,target,actual_before,volume_delta,volume_used,touch,execution_price\n";
	if (!_fill_decision_logs.str().empty()) content += _fill_decision_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());
	filename = folder + "fill_audit.csv";
	content = "code,model,time,event_seq,created_seq,target,actual_before,actual_after,signed_qty,reference_last,reference_mid,touch_price,execution_price,spread_cost,extra_slippage\n";
	if (!_fill_audit_logs.str().empty()) content += _fill_audit_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());
	filename = folder + "equity_events.csv";
	content = "code,action_date,action_time,event_seq,net_pnl,target,actual,abs_gap\n";
	if (!_equity_event_logs.str().empty()) content += _equity_event_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());
	filename = folder + "target_replacements.csv";
	content = "code,event_seq,signal_time,old_target,new_target,actual_before,old_remaining\n";
	if (!_target_replacement_logs.str().empty()) content += _target_replacement_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();
		for (auto it = _user_datas.begin(); it != _user_datas.end(); it++)
		{
			root.AddMember(rj::Value(it->first.c_str(), allocator), rj::Value(it->second.c_str(), allocator), allocator);
		}

		filename = folder;
		filename += "ud_";
		filename += _name;
		filename += ".json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}
}

void CtaMocker::log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag /* = "" */)
{
	_sig_logs << stdCode << "," << target << "," << price << "," << gentime << "," << usertag << "\n";
}

void CtaMocker::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag, double fee, uint32_t barNo)
{
	_trade_logs << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << (isOpen ? "OPEN" : "CLOSE") 
		<< "," << price << "," << qty << "," << userTag << "," << fee << "," << barNo << "\n";
}

void CtaMocker::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double maxprofit, double maxloss, 
	double totalprofit /* = 0 */, const char* enterTag /* = "" */, const char* exitTag /* = "" */, uint32_t openBarNo /* = 0 */, uint32_t closeBarNo /* = 0 */)
{
	_close_logs << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
		<< "," << closeTime << "," << closepx << "," << qty << "," << profit << "," << maxprofit << "," << maxloss << ","
		<< totalprofit << "," << enterTag << "," << exitTag << "," << openBarNo << "," << closeBarNo << "\n";
}

bool CtaMocker::init_cta_factory(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;
	// YAML 的 model/event_delay/participation_rate 与 Python v3 最终都进入
	// configure_fill_model；这里负责把 YAML 文本先转换为合法的参数。
	// WTSVariant::asUInt64 对 "-1" 会按 strtoull 转成一个极大正数；
	// 在这里按十进制无符号整数严格解析，不能让坏配置静默等待到回测结束。
	uint64_t delay_events = 0;
	if (cfg->get("event_delay") != NULL)
	{
		const char* raw = cfg->getCString("event_delay");
		if (raw == NULL || *raw == '\0')
		{
			log_error("event_delay must be an unsigned integer");
			return false;
		}
		for (const char* p = raw; *p != '\0'; ++p)
		{
			if (*p < '0' || *p > '9')
			{
				log_error("event_delay must be an unsigned integer: {}", raw);
				return false;
			}
		}
		errno = 0;
		char* end = NULL;
		const unsigned long long parsed = std::strtoull(raw, &end, 10);
		if (errno == ERANGE || end == raw || *end != '\0')
		{
			log_error("event_delay is out of range: {}", raw);
			return false;
		}
		delay_events = static_cast<uint64_t>(parsed);
	}
	// Day16 的参与率只允许 volume_limited 使用；缺省值为 0，不影响旧配置。
	// 示例：model: volume_limited, participation_rate: 0.1；这里的 0.1
	// 是每 Tick 市场增量的 10%，不是盘口买一/卖一挂量的 10%。
	const double participation_rate = cfg->get("participation_rate") == NULL
		? 0.0 : cfg->getDouble("participation_rate");
	if (!configure_fill_model(cfg->getCString("model"), delay_events, participation_rate))
		return false;

	const char* module = cfg->getCString("module");

	DllHandle hInst = DLLHelper::load_library(module);
	if (hInst == NULL)
		return false;

	FuncCreateStraFact creator = (FuncCreateStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
	if (creator == NULL)
	{
		DLLHelper::free_library(hInst);
		return false;
	}

	_factory._module_inst = hInst;
	_factory._module_path = module;
	_factory._creator = creator;
	_factory._remover = (FuncDeleteStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
	_factory._fact = _factory._creator();

	WTSVariant* cfgStra = cfg->get("strategy");
	if (cfgStra)
	{
		_strategy = _factory._fact->createStrategy(cfgStra->getCString("name"), cfgStra->getCString("id"));
		if(_strategy)
		{
			WTSLogger::info("Strategy {}.{} is created,strategy ID: {}", _factory._fact->getName(), _strategy->getName(), _strategy->id());
		}
		_strategy->init(cfgStra->get("params"));
		_name = _strategy->id();
	}

	return true;
}

bool CtaMocker::configure_fill_model(const char* model, uint64_t delay_events,
	double participation_rate)
{
	// 每次只装一个模型：Legacy 全量立即成交；Causal 只约束延迟/报价；
	// VolumeLimited 在 Causal 之上再约束单 Tick 成交数量。
	// 先验证参数、后替换模型，避免坏配置把原模型改成半初始化状态。
	const std::string name = model == NULL ? "" : model;
	if (!std::isfinite(participation_rate))
	{
		log_error("participation_rate must be finite");
		return false;
	}
	if (name.empty() || name == "legacy_cta")
	{
		// 兼容历史回测：不给旧模型偷偷加事件延迟或参与率。
		if (delay_events != 0 || participation_rate != 0.0)
		{
			log_error("legacy_cta does not accept event_delay or participation_rate");
			return false;
		}
		_fill_model.reset(new LegacyCtaFill());
		_fill_model_name = "legacy_cta";
		_fill_delay_events = 0;
		// 新实验不能继承上一模型的日内量源基线或已用 Tick 预算。
		_volume_ledgers.clear();
		_tick_volume_sources.clear();
		_tick_input_guards.clear();
		_contract_event_sequences.clear();
		_event_volumes.clear();
		return true;
	}
	if (name == "causal_touch")
	{
		// 此模型仍是全额目标差额成交，只有信号时序与 touch 价变化。
		if (participation_rate != 0.0)
		{
			log_error("participation_rate requires volume_limited model");
			return false;
		}
		_fill_model.reset(new CausalTouchFill(delay_events));
		_fill_model_name = "causal_touch";
		_fill_delay_events = delay_events;
		_volume_ledgers.clear();
		_tick_volume_sources.clear();
		_tick_input_guards.clear();
		_contract_event_sequences.clear();
		_event_volumes.clear();
		return true;
	}
	if (name == "volume_limited")
	{
		// 小于等于零会让预算始终为零；大于一则超过本 Tick 的市场成交量。
		if (participation_rate <= 0.0 || participation_rate > 1.0)
		{
			log_error("volume_limited requires 0 < participation_rate <= 1");
			return false;
		}
		_fill_model.reset(new VolumeLimitedFill(participation_rate, delay_events));
		_fill_model_name = "volume_limited";
		_fill_delay_events = delay_events;
		// 此后 handle_tick 才会启用 TickVolumeSource 和 EventVolumeLedger。
		_volume_ledgers.clear();
		_tick_volume_sources.clear();
		_tick_input_guards.clear();
		_contract_event_sequences.clear();
		_event_volumes.clear();
		return true;
	}
	log_error("unknown CTA fill model: {}", name);
	return false;
}

void CtaMocker::load_incremental_data(const char* incremental_backtest_base)
{
	std::string folder = WtHelper::getOutputDir();
	folder += incremental_backtest_base;
	folder += "/";
	WTSLogger::info("loading incremental data from: {}", folder);

	std::string tradesFilename = folder + "trades.csv";
	if (boost::filesystem::exists(tradesFilename))
	{
		std::ifstream tradesFile(tradesFilename);
		std::string str;
		// 跳过标题行
		std::getline(tradesFile, str);
		while (std::getline(tradesFile, str))
		{
			_trade_logs << str << "\n";
		}
	}

	std::string closesFilename = folder + "closes.csv";
	if (boost::filesystem::exists(closesFilename))
	{
		std::ifstream closesFile(closesFilename);
		std::string str;
		// 跳过标题行
		std::getline(closesFile, str);
		while (std::getline(closesFile, str))
		{
			_close_logs << str << "\n";
		}
	}

	std::string fundsFilename = folder + "funds.csv";
	if (boost::filesystem::exists(fundsFilename))
	{
		std::ifstream fundsFile(fundsFilename);
		std::string str;
		// 跳过标题行
		std::getline(fundsFile, str);
		while (std::getline(fundsFile, str))
		{
			_fund_logs << str << "\n";
		}
	}

	std::string positionsFilename = folder + "positions.csv";
	if (boost::filesystem::exists(positionsFilename))
	{
		std::ifstream positionsFile(positionsFilename);
		std::string str;
		// 跳过标题行
		std::getline(positionsFile, str);
		while (std::getline(positionsFile, str))
		{
			_pos_logs << str << "\n";
		}
	}

	std::string signalsFilename = folder + "signals.csv";
	if (boost::filesystem::exists(signalsFilename))
	{
		std::ifstream signalsFile(signalsFilename);
		std::string str;
		// 跳过标题行
		std::getline(signalsFile, str);
		while (std::getline(signalsFile, str))
		{
			_sig_logs << str << "\n";
		}
	}

	std::string strategyDumpFilename = folder + fmtutil::format("{}.json", incremental_backtest_base);
	if (boost::filesystem::exists(strategyDumpFilename))
	{
		WTSLogger::info("load incremental data json: {}", strategyDumpFilename);
		FILE* fp = fopen(strategyDumpFilename.c_str(), "rb");
		char readBuffer[65536];
		rj::FileReadStream strategyDumpFile(fp, readBuffer, sizeof(readBuffer));
		rj::Document d;
		d.ParseStream(strategyDumpFile);
		fclose(fp);
		// 老版本 JSON 可能没有该键；存在时才恢复因果事件钟。
		if (d.HasMember("event_sequence"))
			_event_sequence = d["event_sequence"].GetUint64();
		if (d.HasMember("contract_event_sequences")
			&& d["contract_event_sequences"].IsObject())
		{
			for (rj::Value::ConstMemberIterator it = d["contract_event_sequences"].MemberBegin();
				it != d["contract_event_sequences"].MemberEnd(); ++it)
				if (it->value.IsUint64())
					_contract_event_sequences[it->name.GetString()] = it->value.GetUint64();
		}
		if (d.HasMember("positions"))
		{
			const rj::Value& positions = d["positions"];
			for (rj::SizeType i = 0; i < positions.Size(); i++)
			{
				const rj::Value& positionEntry = positions[i];
				const char* positionEntry_code = positionEntry["code"].GetString();
				PosInfo& pInfo = _pos_map[positionEntry_code];
				pInfo._volume = positionEntry["volume"].GetDouble();
				pInfo._closeprofit = positionEntry["closeprofit"].GetDouble();
				pInfo._dynprofit = positionEntry["dynprofit"].GetDouble();
				pInfo._last_entertime = positionEntry["lastentertime"].GetUint64();
				pInfo._last_exittime = positionEntry["lastexittime"].GetUint64();

				if (positionEntry.HasMember("details"))
				{
					const rj::Value& details = positionEntry["details"];
					for (rj::SizeType j = 0; j < details.Size(); j++)
					{
						const rj::Value& positionDetailEntry = details[j];
						DetailInfo curPosDetail;
						curPosDetail._long = positionDetailEntry["long"].GetBool();
						curPosDetail._price = positionDetailEntry["price"].GetDouble();
						curPosDetail._max_price = positionDetailEntry["maxprice"].GetDouble();
						curPosDetail._min_price = positionDetailEntry["minprice"].GetDouble();
						curPosDetail._volume = positionDetailEntry["volume"].GetDouble();
						curPosDetail._opentime = positionDetailEntry["opentime"].GetUint64();
						curPosDetail._opentdate = positionDetailEntry["opentdate"].GetInt();
						curPosDetail._profit = positionDetailEntry["profit"].GetDouble();
						curPosDetail._max_profit = positionDetailEntry["maxprofit"].GetDouble();
						curPosDetail._max_loss = positionDetailEntry["maxloss"].GetDouble();
						strcpy(curPosDetail._opentag, positionDetailEntry["opentag"].GetString());
						pInfo._details.push_back(curPosDetail);
					}
				}
			}
		}

		if (d.HasMember("fund"))
		{
			_fund_info._total_profit = d["fund"]["total_profit"].GetDouble();
			_fund_info._total_dynprofit = d["fund"]["total_dynprofit"].GetDouble();
			_fund_info._total_fees = d["fund"]["total_fees"].GetDouble();
		}

		if (d.HasMember("signals"))
		{
			// JSON 键 volume 是“目标仓位”，不能误认为 Tick 的 volume。
			// 旧文件可能缺少 Day13 增加的状态字段，下方使用保守默认值。
			for (rj::Value::ConstMemberIterator itr = d["signals"].MemberBegin(); itr != d["signals"].MemberEnd(); ++itr)
			{
				std::string stkCode = itr->name.GetString();
				SigInfo& sInfo = _sig_map[stkCode];
				sInfo._usertag = itr->value["usertag"].GetString();
				const double target = itr->value["volume"].GetDouble();
				sInfo._sigprice = itr->value["sigprice"].GetDouble();
				sInfo._gentime = itr->value["gentime"].GetUint64();
				// 兼容旧状态文件：缺失的因果字段采用保守默认值。
				const uint64_t created = itr->value.HasMember("created_seq")
					? itr->value["created_seq"].GetUint64() : 0;
				const uint64_t activation = itr->value.HasMember("activation_seq")
					? itr->value["activation_seq"].GetUint64()
					: (created == std::numeric_limits<uint64_t>::max() ? created : created + 1);
				const uint64_t version = itr->value.HasMember("target_version")
					? itr->value["target_version"].GetUint64() : 1;
				// 对损坏或未来版本的状态值做边界限制，避免恢复成未知状态。
				const uint32_t phase_value = itr->value.HasMember("pending_phase")
					? itr->value["pending_phase"].GetUint() : 1;
				const PendingPhase phase = phase_value <= static_cast<uint32_t>(PendingPhase::WaitingLiquidity)
					? static_cast<PendingPhase>(phase_value) : PendingPhase::WaitingLatency;
				sInfo._pending.restore(stkCode.c_str(), target, created, activation,
					version, phase);
				// 老快照没有分合约计数时，用原全局计数作为保守起点，
				// 至少避免待成交目标永远等不到 created_seq。
				if (_fill_model_name != "legacy_cta"
					&& _contract_event_sequences.find(stkCode) == _contract_event_sequences.end())
					_contract_event_sequences[stkCode] = _event_sequence;
				if (itr->value.HasMember("source"))
					sInfo._source = static_cast<SignalSource>(itr->value["source"].GetUint());
			}
		}

		if (d.HasMember("conditions") && d["conditions"].HasMember("items"))
		{
			// conditions -> items 下面的内容是两层嵌套   items[CODE] is a list
			rj::Value& conditionItemsEntry = d["conditions"]["items"];

			for (rj::Value::ConstMemberIterator itr = conditionItemsEntry.MemberBegin(); itr != conditionItemsEntry.MemberEnd(); ++itr)
			{
				std::string stkCode = itr->name.GetString();
				for (rj::SizeType i = 0; i < itr->value.Size(); i++)
				{
					const rj::Value& conditionItemStkCondEntry = itr->value[i];
					CondEntrust condEntrust;
					strcpy(condEntrust._usertag, conditionItemStkCondEntry["usertag"].GetString());
					condEntrust._field = (WTSCompareField)conditionItemStkCondEntry["field"].GetInt();
					condEntrust._alg = (WTSCompareType)conditionItemStkCondEntry["alg"].GetInt();
					condEntrust._target = conditionItemStkCondEntry["target"].GetDouble();
					condEntrust._qty = conditionItemStkCondEntry["qty"].GetDouble();
					condEntrust._action = (char)conditionItemStkCondEntry["action"].GetUint();

					_condtions[stkCode].push_back(condEntrust);
				}
			}
		}
	}
	else
	{
		WTSLogger::warn("fail load incremental data json: {}", strategyDumpFilename);
	}
}

//////////////////////////////////////////////////////////////////////////
//IDataSink
void CtaMocker::handle_init()
{
	this->on_init();
}

void CtaMocker::handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	this->on_bar(stdCode, period, times, newBar);
}

void CtaMocker::handle_schedule(uint32_t uDate, uint32_t uTime)
{
	this->on_schedule(uDate, uTime);
}

void CtaMocker::handle_session_begin(uint32_t curTDate)
{
	this->on_session_begin(curTDate);
}

void CtaMocker::handle_session_end(uint32_t curTDate)
{
	this->on_session_end(curTDate);
}

void CtaMocker::handle_section_end(uint32_t curTDate, uint32_t curTime)
{
	/*
	 *	By Wesley @ 2022.05.16
	 *	如果小节结束，也需要清理掉价格缓存，防止小节跳空
	 *	这种主要是针对夜盘交易
	 */
	_price_map.clear();
}

void CtaMocker::handle_replay_done()
{
	_in_backtest = false;

	if(_emit_times > 0)
	{
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_INFO, 
			"Strategy has been scheduled {} times, totally taking {} us, {:.3f} us each time",
			_emit_times, _total_calc_time, _total_calc_time*1.0 / _emit_times);
	}
	else
	{
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_INFO, 
			"Strategy has been scheduled for {} times", _emit_times);
	}

	dump_outputs();

	dump_stradata();

	dump_chartdata();

	if (_has_hook && _hook_valid)
	{
		WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, "Replay done, notify control thread");
		while(_wait_calc)
			_cond_calc.notify_all();
		WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, "Notify control thread the end done");
	}

	WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Notify strategy the end of backtest");
	this->on_bactest_end();
}

void CtaMocker::proc_tick(const char* stdCode, double last_px, double cur_px)
{
	// 此函数既可能在 on_tick_updated 之前，也可能在其之后被同一 handle_tick 调用。
	// 它负责“尝试消费”信号；是否成交由模型决定，未成交的目标留在 _sig_map。
	{
		auto it = _sig_map.find(stdCode);
		if (it != _sig_map.end())
		{
			//if (sInfo->isInTradingTime(_replayer->get_raw_time(), true))
			{
				// 回调可能创建新信号；先复制旧目标，避免被覆盖后的引用失效。
				// 一个合约只保存最新目标；这里处理的是进入本次调用时的快照版本。
				const SigInfo sInfo = it->second;
				double price;
				if (decimal::eq(sInfo._desprice, 0.0))
					price = cur_px;
				else
					price = sInfo._desprice;
				// do_set_position 可返回 WaitingLatency/NoQuote/NoLiquidity 等等待状态，
				// 也可部分成交。它不会自行删掉未完成的目标。
				const FillStatus status = do_set_position(stdCode, sInfo._pending.target(), price,
					sInfo._usertag.c_str(), sInfo._pending.created_sequence(), sInfo._source);
				// 成交回调可能改写同合约目标；只更新仍属于快照版本的状态。
				// 若版本已变，旧决策绝不能把新目标误删或标成已完成。
				auto current = _sig_map.find(stdCode);
				if (current != _sig_map.end()
					&& current->second._pending.version() == sInfo._pending.version())
				{
					const auto pos = _pos_map.find(stdCode);
					const double actual = pos == _pos_map.end() ? 0.0 : pos->second._volume;
					// 部分成交时 actual 仍未到目标，on_decision 会继续等待下一 Tick。
					current->second._pending.on_decision(status, actual);
					if (current->second._pending.phase() == PendingPhase::Idle)
					{
						// 仅状态机判为 Idle 才释放；NoLiquidity/部分成交不会走到这里。
						// 当前 InvalidInput 等非等待状态也可能变 Idle，应结合日志诊断。
						_sig_map.erase(current);
						// 保持原条件回调时机：目标完成处理后才通知。
						if (sInfo._sigtype == 2 && status != FillStatus::InvalidInput)
							on_condition_triggered(stdCode, sInfo._pending.target(), cur_px,
								sInfo._usertag.c_str());
					}
				}
			}
		}
	}

	update_dyn_profit(stdCode, cur_px);

	//////////////////////////////////////////////////////////////////////////
	//检查条件单
	if (!_condtions.empty())
	{
		auto it = _condtions.find(stdCode);
		if (it == _condtions.end())
			return;

		const CondList& condList = it->second;
		double curPrice = cur_px;
		const CondEntrust* matchedEntrust = NULL;
		for (const CondEntrust& entrust : condList)
		{
			/*
			 * 如果开启了tick模式，就正常比较
			 * 但是如果没有开启tick模式，逻辑就非常复杂
			 * 因为不开回测的时候tick是用开高低收模拟出来的，如果直接按照目标价格触发，可能是有问题的
			 * 首先要拿到上一笔价格，和当前最新价格做一个比价，得到左边界和右边界
			 * 这里只能假设前后两笔价格之间是连续的，这样需要将两笔价格都加入判断
			 * 当条件是等于时，如果目标价格在左右边界之间，说明目标价格在这期间是出现过的，则认为价格匹配
			 * 当条件是大于的时候，我们需要判断右边界，即稍大的值是否满足条件，并取左边界与目标价中稍大的作为当前价
			 * 当条件是小于的时候，我们需要判断左边界，即稍小的值是否满足条件，并取右边界与目标价中稍小的作为当前价
			 */

			double left_px = min(last_px, cur_px);
			double right_px = max(last_px, cur_px);

			bool isMatched = false;
			if (!_replayer->is_tick_simulated())
			{
				//如果tick数据不是模拟的，则使用最新价格
				switch (entrust._alg)
				{
				case WCT_Equal:
					isMatched = decimal::eq(curPrice, entrust._target);
					break;
				case WCT_Larger:
					isMatched = decimal::gt(curPrice, entrust._target);
					break;
				case WCT_LargerOrEqual:
					isMatched = decimal::ge(curPrice, entrust._target);
					break;
				case WCT_Smaller:
					isMatched = decimal::lt(curPrice, entrust._target);
					break;
				case WCT_SmallerOrEqual:
					isMatched = decimal::le(curPrice, entrust._target);
					break;
				default:
					break;
				}

				if (isMatched)
				{
					matchedEntrust = &entrust;
					break;
				}
			}
			else
			{
				//如果tick数据是模拟的，则要处理一下
				switch (entrust._alg)
				{
				case WCT_Equal:
					isMatched = decimal::le(left_px, entrust._target) && decimal::ge(right_px, entrust._target);
					break;
				case WCT_Larger:
					isMatched = decimal::gt(right_px, entrust._target);
					break;
				case WCT_LargerOrEqual:
					isMatched = decimal::ge(right_px, entrust._target);
					break;
				case WCT_Smaller:
					isMatched = decimal::lt(left_px, entrust._target);
					break;
				case WCT_SmallerOrEqual:
					isMatched = decimal::le(left_px, entrust._target);
					break;
				default:
					break;
				}

				if (isMatched)
				{
					/*
					* By HeJ @ 2023.02.27
					* 在bar回测中，经常会出现同一个价格触发了多个条件单时，要选出一个作为最终的触发价，遵循以下规则：
					* 1 alg不同的条件单，或者alg为WCT_Equal，以最先设置的那个为准
					* 2 alg一样的调价单，如果是WCT_Larger与WCT_LargerOrEqual，取触发价较小的，WCT_Smaller与WCT_SmallerOrEqual，取触发价较大的
					*/
					if (matchedEntrust == NULL)
					{
						matchedEntrust = &entrust;
						if (entrust._alg == WCT_Larger || entrust._alg == WCT_LargerOrEqual)
							curPrice = max(left_px, entrust._target);
						else if (entrust._alg == WCT_Smaller || entrust._alg == WCT_SmallerOrEqual)
							curPrice = min(right_px, entrust._target);
						else
							curPrice = entrust._target;
					}
					else if (matchedEntrust->_alg == entrust._alg)
					{
						if (entrust._alg == WCT_Larger || entrust._alg == WCT_LargerOrEqual)
						{
							if (entrust._target < matchedEntrust->_target)
							{
								matchedEntrust = &entrust;
								curPrice = max(left_px, entrust._target);
							}
						}
						else if (entrust._alg == WCT_Smaller || entrust._alg == WCT_SmallerOrEqual)
						{
							if (entrust._target > matchedEntrust->_target)
							{
								matchedEntrust = &entrust;
								curPrice = min(right_px, entrust._target);
							}
						}
					}
				}
			}
		}

		if (matchedEntrust != NULL)
		{
			const CondEntrust& entrust = *matchedEntrust;
			double price = curPrice;
			double curQty = stra_get_position(stdCode);
			//_replayer->is_tick_enabled() ? newTick->price() : entrust._target;	//如果开启了tick回测,则用tick数据的价格,如果没有开启,则只能用条件单价格
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_INFO,
				"Condition order triggered[newprice: {}{}{}], instrument: {}, {} {}",
				curPrice, CMP_ALG_NAMES[entrust._alg], entrust._target, stdCode, ACTION_NAMES[entrust._action], entrust._qty);
			switch (entrust._action)
			{
			case COND_ACTION_OL:
			{
				if (decimal::lt(curQty, 0))
					append_signal(stdCode, entrust._qty, entrust._usertag, price, 2);
				else
					append_signal(stdCode, curQty + entrust._qty, entrust._usertag, price, 2);
			}
			break;
			case COND_ACTION_CL:
			{
				double maxQty = min(curQty, entrust._qty);
				append_signal(stdCode, curQty - maxQty, entrust._usertag, price, 2);
			}
			break;
			case COND_ACTION_OS:
			{
				if (decimal::gt(curQty, 0))
					append_signal(stdCode, -entrust._qty, entrust._usertag, price, 2);
				else
					append_signal(stdCode, curQty - entrust._qty, entrust._usertag, price, 2);
			}
			break;
			case COND_ACTION_CS:
			{
				double maxQty = min(abs(curQty), entrust._qty);
				append_signal(stdCode, curQty + maxQty, entrust._usertag, price, 2);
			}
			break;
			case COND_ACTION_SP:
			{
				append_signal(stdCode, entrust._qty, entrust._usertag, price, 2);
			}
			default: break;
			}

			//同一个bar设置针对同一个合约的条件单,只可能触发一条
			//所以这里直接清理掉即可
			_condtions.erase(it);
		}
	}
}


void CtaMocker::handle_tick(const char* stdCode, WTSTickData* newTick, uint32_t pxType /* = 0 */)
{
	if (_fill_model_name != "legacy_cta")
	{
		const WTSTickStruct& input = newTick->getTickStruct();
		if (!_tick_input_guards[stdCode].accept(input.trading_date, input.action_date,
			input.action_time, input.total_volume, input.price,
			input.bid_prices[0], input.ask_prices[0],
			input.bid_qty[0], input.ask_qty[0]))
		{
			// 丢弃的重复/乱序输入不应消耗 Tick 延迟、预算或触发策略回调。
			log_debug("skip duplicate/out-of-order CTA Tick for {} at {} {}",
				stdCode, input.action_date, input.action_time);
			return;
		}
	}
	// 每收到一条回放 Tick，逻辑事件序号只加一次；它不是 action_time 毫秒值。
	// 同一 handle_tick 前后可能两次 proc_tick，但都属于这一条输入事件。
	++_event_sequence;
	if (_fill_model_name != "legacy_cta")
		++_contract_event_sequences[stdCode];
	double cur_px = newTick->price();

	/*
	 *	By Wesley @ 2022.04.19
	 *	这里的逻辑改了一下
	 *	如果缓存的价格不存在，则上一笔价格就用最新价
	 *	这里主要是为了应对跨日价格跳空的情况
	 */
	double last_px = cur_px;
	if(pxType != 0)
	{
		auto it = _price_map.find(stdCode);
		if (it != _price_map.end())
			last_px = it->second;
		else
			last_px = cur_px;
	}
	
	
	_price_map[stdCode] = cur_px;
	// 决策模型稍后从 _ticks 读取当前盘口；先更新缓存，不能读上一条报价。
	_ticks[stdCode] = newTick->getTickStruct();
	if (dynamic_cast<VolumeLimitedFill*>(_fill_model.get()) != nullptr)
	{
		// 只有显式启用 volume_limited 才做成交量校验；Legacy/Causal 不增添开销。
		// 每条输入 Tick 只验证一次；同一事件的前后两次 proc_tick 共用结果。
		const WTSTickStruct& tick = _ticks[stdCode];
		// WT 样本中 total_volume 是截至当前的交易日内累计量；volume 是从上一条
		// Tick 到当前 Tick 新增的量。observe 用累计量差分核对它，不合格返回 0。
		// 模拟 Bar 生成的 Tick 不能被当成真实盘口/真实市场增量，直接给 0。
		_event_volumes[stdCode] = _replayer->is_tick_simulated() ? 0.0
			: _tick_volume_sources[stdCode].observe(tick.trading_date,
				tick.total_volume, tick.volume);
	}

	// 第一轮先检查旧目标/条件：例如上一 Tick 剩余的 3 手可在本 Tick 尝试。
	//先检查是否要信号要触发
	//By Wesley @ 2022.04.19
	//虽然这段逻辑下面也根据isBarEnd复制了一段
	//但是这一段还是要保留
	proc_tick(stdCode, last_px, cur_px);

	_in_tick_callback = true;
	// 策略回调可以创建或覆盖目标；_in_tick_callback 仅标记它的来源。
	on_tick_updated(stdCode, newTick);
	_in_tick_callback = false;

	/*
	 *	By Wesley @ 2022.04.19
	 *	isBarEnd，如果是逐tick回放，这个永远都是true，永远也不会触发下面这段逻辑
	 *	如果是模拟的tick数据，用收盘价模拟tick的时候，isBarEnd才会为true
	 *	如果不是收盘价模拟的tick，那么直接在当前tick触发撮合逻辑
	 *	这样做的目的是为了让在模拟tick触发的ontick中下单的信号能够正常处理
	 *	而不至于在回测的时候成交价偏离太远
	 */
	// 第二轮沿用同一个事件序号和已用预算：不能把一条 Tick 当成两条行情。
	// 新在本轮策略回调中创建的目标，因果模型要求等下一条 Tick 才能成交；
	// 旧目标若尚未完成，也只能使用本 Tick 尚未消费的预算。
	if(pxType != 3)
		proc_tick(stdCode, last_px, cur_px);
	// 两轮 proc_tick 和策略回调均完成后才取权益快照。net_pnl 不含任意假设的
	// 初始资金：已实现盈亏 + 当前浮盈 - 累计费用。Day21 报告另加固定分母。
	const WTSTickStruct& input = newTick->getTickStruct();
	const double net_pnl = _fund_info._total_profit + _fund_info._total_dynprofit
		- _fund_info._total_fees;
	const auto pos = _pos_map.find(stdCode);
	const double actual = pos == _pos_map.end() ? 0.0 : pos->second._volume;
	const auto pending = _sig_map.find(stdCode);
	const double target = pending == _sig_map.end() ? actual : pending->second._pending.target();
	// 事件末尾同时记录目标与实际仓位，Day21 可直接计算每 Tick 的跟踪偏差。
	// 没有待成交信号时，已实现的实际仓位就是最近目标，不虚构缺口。
	_equity_event_logs << fmt::format("{},{},{},{},{:.2f},{},{},{}\n", stdCode,
		input.action_date, input.action_time, current_event_sequence(stdCode), net_pnl,
		target, actual, abs(target - actual));
}


//////////////////////////////////////////////////////////////////////////
//回调函数
void CtaMocker::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (newBar == NULL)
		return;

	thread_local static char realPeriod[8] = { 0 };
	fmtutil::format_to(realPeriod, "{}{}", period, times);

	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, realPeriod);

	KlineTag& tag = _kline_tags[key];
	tag._closed = true;

	if(tag._notify)
		on_bar_close(stdCode, realPeriod, newBar);
}

void CtaMocker::on_init()
{
	_ticks.clear();
	// 每次新回测从空的行情基线、预算账本开始，不继承上一轮实验的日内状态。
	_volume_ledgers.clear();
	_tick_volume_sources.clear();
	_event_volumes.clear();
	_in_backtest = true;
	if (_strategy)
		_strategy->on_init(this);

	WTSLogger::info("CTA Strategy initialized with {} slippage: {}", _ratio_slippage?"ratio":"absolute", _slippage);
}

void CtaMocker::update_dyn_profit(const char* stdCode, double price)
{
	auto it = _pos_map.find(stdCode);
	if (it != _pos_map.end())
	{
		PosInfo& pInfo = (PosInfo&)it->second;
		if (pInfo._volume == 0)
		{
			pInfo._dynprofit = 0;
		}
		else
		{
			WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
			double dynprofit = 0;
			for (auto pit = pInfo._details.begin(); pit != pInfo._details.end(); pit++)
			{
				DetailInfo& dInfo = *pit;
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				if (dInfo._profit > 0)
					dInfo._max_profit = max(dInfo._profit, dInfo._max_profit);
				else if (dInfo._profit < 0)
					dInfo._max_loss = min(dInfo._profit, dInfo._max_loss);

				dInfo._max_price = std::max(dInfo._max_price, price);
				dInfo._min_price = std::min(dInfo._min_price, price);

				dynprofit += dInfo._profit;
			}

			pInfo._dynprofit = dynprofit;
		}
	}

	double total_dynprofit = 0;
	for (auto& v : _pos_map)
	{
		const PosInfo& pInfo = v.second;
		total_dynprofit += pInfo._dynprofit;
	}

	_fund_info._total_dynprofit = total_dynprofit;
}

void CtaMocker::on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy /* = true */)
{
	//这个逻辑全部迁移到handle_tick里去了
}

void CtaMocker::on_bar_close(const char* code, const char* period, WTSBarStruct* newBar)
{
	if (_strategy)
		_strategy->on_bar(this, code, period, newBar);
}

void CtaMocker::on_tick_updated(const char* code, WTSTickData* newTick)
{
	auto it = _tick_subs.find(code);
	if (it == _tick_subs.end())
		return;

	if (_strategy)
		_strategy->on_tick(this, code, newTick);
}

void CtaMocker::on_calculate(uint32_t curDate, uint32_t curTime)
{
	if (_strategy)
		_strategy->on_schedule(this, curDate, curTime);
}

void CtaMocker::enable_hook(bool bEnabled /* = true */)
{
	_hook_valid = bEnabled;

	WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calculating hook {}", bEnabled?"enabled":"disabled");
}

void CtaMocker::install_hook()
{
	_has_hook = true;

	WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "CTA hook installed");
}

bool CtaMocker::step_calc()
{
	if (!_has_hook)
	{
		return false;
	}

	//总共分为4个状态
	//0-初始状态，1-oncalc，2-oncalc结束，3-oncalcdone
	//所以，如果出于0/2，则说明没有在执行中，需要notify
	bool bNotify = false;
	while (_in_backtest && (_cur_step == 0 || _cur_step == 2))
	{
		_cond_calc.notify_all();
		bNotify = true;
	}

	if(bNotify)
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Notify calc thread, wait for calc done");

	if(_in_backtest)
	{
		_wait_calc = true;
		StdUniqueLock lock(_mtx_calc);
		_cond_calc.wait(_mtx_calc);
		_wait_calc = false;
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc done notified");
		_cur_step = (_cur_step + 1) % 4;

		return true;
	}
	else
	{
		_hook_valid = false;
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Backtest exit automatically");
		return false;
	}
}

bool CtaMocker::on_schedule(uint32_t curDate, uint32_t curTime)
{
	_is_in_schedule = true;//开始调度,修改标记

	_schedule_times++;

	bool isMainUdt = false;
	bool emmited = false;

	for (auto it = _kline_tags.begin(); it != _kline_tags.end(); it++)
	{
		const std::string& key = it->first;
		KlineTag& marker = (KlineTag&)it->second;

		StringVector ay = StrUtil::split(key, "#");
		const char* stdCode = ay[0].c_str();

		if (key == _main_key)
		{
			if (marker._closed)
			{
				isMainUdt = true;
				marker._closed = false;
			}
			else
			{
				isMainUdt = false;
				break;
			}
		}

		WTSSessionInfo* sInfo = _replayer->get_session_info(stdCode, true);

		if (isMainUdt || _kline_tags.empty())
		{
			TimeUtils::Ticker ticker;

			uint32_t offTime = sInfo->offsetTime(curTime, true);
			if (offTime <= sInfo->getCloseTime(true))
			{
				_condtions.clear();
				if(_has_hook && _hook_valid)
				{
					WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Waiting for resume notify");
					StdUniqueLock lock(_mtx_calc);
					_cond_calc.wait(_mtx_calc);
					WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc resumed");
					_cur_step = 1;
				}

				on_calculate(curDate, curTime);

				if (_has_hook && _hook_valid)
				{
					WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc done, notify control thread");
					while (_cur_step==1)
						_cond_calc.notify_all();

					WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Waiting for resume notify");
					StdUniqueLock lock(_mtx_calc);
					_cond_calc.wait(_mtx_calc);
					WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc resumed");
					_cur_step = 3;
				}

				if(_has_hook)
					on_calculate_done(curDate, curTime);
				emmited = true;

				if (_condtions.empty())
					_last_cond_min = (uint64_t)curDate * 10000 + curTime;

				_emit_times++;
				_total_calc_time += ticker.micro_seconds();


				/*
				 *	By Wesley @ 2022.07.16
				 *	策略计算完成，需要把指标数据做一个检查
				 *	如果策略在本轮没有设置指标值，则用上一个数据补齐
				 *	如果是开始，则用默认值补齐
				 */
				//for(auto& v : _chart_indice)
				//{
				//	ChartIndex& cIndex = v.second;
				//	for(auto& line : cIndex._lines)
				//	{
				//		ChartLine& cLine = line.second;
				//		if(cLine._values.size() < _emit_times)
				//		{
				//			double lastVal = DBL_MAX;
				//			if (!cLine._values.empty())
				//				lastVal = cLine._values.back();

				//			cLine._values.emplace_back(lastVal);
				//		}
				//	}
				//}

				if (_has_hook && _hook_valid)
				{
					WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc done, notify control thread");
					while(_cur_step == 3)
						_cond_calc.notify_all();
				}
			}
			else
			{
				WTSLogger::log_dyn("strategy", _name.c_str(), LL_INFO, "{} is not trading time,strategy will not be scheduled", curTime);
			}
			break;
		}
	}

	_is_in_schedule = false;//调度结束,修改标记
	return emmited;
}


void CtaMocker::on_session_begin(uint32_t curTDate)
{
	_cur_tdate = curTDate;

	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if (!decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
			pInfo._frozen = 0;
		}
	}

	/*
	 *	By Wesley @ 2022.04.19
	 *	新交易日开始的时候，价格缓存清掉，要重新处理
	 */
	_price_map.clear();

	if (_strategy)
		_strategy->on_session_begin(this, curTDate);
}

void CtaMocker::enum_position(FuncEnumCtaPosCallBack cb, bool bForExecute)
{
	wt_hashmap<std::string, double> desPos;
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		const PosInfo& pInfo = it.second;
		desPos[stdCode] = pInfo._volume;
	}

	for (auto sit : _sig_map)
	{
		const char* stdCode = sit.first.c_str();
		const SigInfo& sInfo = sit.second;
		desPos[stdCode] = sInfo._pending.target();
	}

	for (auto v : desPos)
	{
		cb(v.first.c_str(), v.second);
	}
}

void CtaMocker::on_session_end(uint32_t curTDate)
{
	if (_strategy)
		_strategy->on_session_end(this, curTDate);

	uint32_t curDate = curTDate;//_replayer->get_trading_date();

	double total_profit = 0;
	double total_dynprofit = 0;

	for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		const PosInfo& pInfo = it->second;
		total_profit += pInfo._closeprofit;
		total_dynprofit += pInfo._dynprofit;

		if(decimal::eq(pInfo._volume, 0.0))
			continue;

		_pos_logs << fmt::format("{},{},{},{:.2f},{:.2f}\n", curDate, stdCode,
			pInfo._volume, pInfo._closeprofit, pInfo._dynprofit);
	}

	_fund_logs << fmt::format("{},{:.2f},{:.2f},{:.2f},{:.2f}\n", curDate,
		_fund_info._total_profit, _fund_info._total_dynprofit,
		_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees);
	
	if (_notifier)
		_notifier->notifyFund("BT_FUND", curDate, _fund_info._total_profit, _fund_info._total_dynprofit,
			_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees);
}

CondList& CtaMocker::get_cond_entrusts(const char* stdCode)
{
	CondList& ce = _condtions[stdCode];
	return ce;
}

//////////////////////////////////////////////////////////////////////////
//策略接口
void CtaMocker::stra_enter_long(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if(commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	_replayer->sub_tick(_context_id, stdCode);
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式,则直接触发
	{
		double curQty = stra_get_position(stdCode);
		if(decimal::lt(curQty, 0))
			append_signal(stdCode, qty, userTag, 0.0, _is_in_schedule ? 0 : 1);
		else
			append_signal(stdCode, curQty + qty, userTag, 0.0, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		strcpy(entrust._code, stdCode);
		strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_SmallerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_LargerOrEqual;
		}

		entrust._action = COND_ACTION_OL;

		condList.emplace_back(entrust);
	}
}

void CtaMocker::stra_enter_short(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	if(!commInfo->canShort())
	{
		log_error("Cannot short on {}", stdCode);
		return;
	}

	_replayer->sub_tick(_context_id, stdCode);
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式,则直接触发
	{
		double curQty = stra_get_position(stdCode);
		if(decimal::gt(curQty, 0))
			append_signal(stdCode, -qty, userTag, 0.0, _is_in_schedule ? 0 : 1);
		else
			append_signal(stdCode, curQty - qty, userTag, 0.0, _is_in_schedule ? 0 : 1);

	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		strcpy(entrust._code, stdCode);
		strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_LargerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_SmallerOrEqual;
		}

		entrust._action = COND_ACTION_OS;

		condList.emplace_back(entrust);
	}
}

void CtaMocker::stra_exit_long(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	WTSSessionInfo* sInfo = commInfo->getSessionInfo();
	uint32_t offTime = sInfo->offsetTime(_replayer->get_min_time(), true);
	bool isLastBarOfDay = (offTime == sInfo->getCloseTime(true));

	//读取可平持仓,如果是收盘那根bar，则直接读取全部持仓
	double curQty = stra_get_position(stdCode, !isLastBarOfDay);
	if (decimal::le(curQty, 0))
		return;

	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式,则直接触发
	{
		double maxQty = min(curQty, qty);
		double totalQty = stra_get_position(stdCode, false);
		append_signal(stdCode, totalQty - maxQty, userTag, 0.0, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		strcpy(entrust._code, stdCode);
		strcpy(entrust._usertag, userTag);

		entrust._qty = min(curQty, qty);
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_LargerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_SmallerOrEqual;
		}

		entrust._action = COND_ACTION_CL;

		condList.emplace_back(entrust);
	}
}

void CtaMocker::stra_exit_short(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	if (!commInfo->canShort())
	{
		log_error("Cannot short on {}", stdCode);
		return;
	}

	double curQty = stra_get_position(stdCode);
	if (decimal::ge(curQty, 0))
		return;

	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式,则直接触发
	{
		double maxQty = min(abs(curQty), qty);
		append_signal(stdCode, curQty + maxQty, userTag, 0.0, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		strcpy(entrust._code, stdCode);
		strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_SmallerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_LargerOrEqual;
		}

		entrust._action = COND_ACTION_CS;

		condList.emplace_back(entrust);
	}
}

double CtaMocker::stra_get_price(const char* stdCode)
{
	if (_replayer)
		return _replayer->get_cur_price(stdCode);

	return 0.0;
}

double CtaMocker::stra_get_day_price(const char* stdCode, int flag /* = 0 */)
{
	if (_replayer)
		return _replayer->get_day_price(stdCode, flag);

	return 0.0;
}

void CtaMocker::stra_set_position(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice /* = 0.0 */, double stopprice /* = 0.0 */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	//如果不能做空，则目标仓位不能设置负数
	if (!commInfo->canShort() && decimal::lt(qty, 0))
	{
		log_error("Cannot short on {}", stdCode);
		return;
	}

	double total = stra_get_position(stdCode, false);
	// 只有“不存在不同的待成交目标”时，同值设置才可忽略。部分成交后，
	// 实际仓位可能是 7、旧目标仍为 10；此时 set_position(7) 必须进入
	// append_signal 取消剩余 3 手，不能因 qty==actual 提前返回。
	if (decimal::eq(total, qty))
	{
		const auto pending = _sig_map.find(stdCode);
		if (pending == _sig_map.end()
			|| decimal::eq(pending->second._pending.target(), qty))
			return;
	}

	if(commInfo->isT1())
	{
		double valid = stra_get_position(stdCode, true);
		double frozen = total - valid;
		//如果是T+1规则，则目标仓位不能小于冻结仓位
		if(decimal::lt(qty, frozen))
		{
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_ERROR, "New position of {} cannot be set to {} due to {} being frozen", stdCode, qty, frozen);
			return;
		}
	}

	_replayer->sub_tick(_context_id, stdCode);
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//没有设置触发条件，则直接添加信号
	{
		append_signal(stdCode, qty, userTag, 0.0, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		bool isBuy = decimal::gt(qty, total);

		CondEntrust entrust;
		strcpy(entrust._code, stdCode);
		strcpy(entrust._usertag, userTag);
		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = isBuy ? WCT_SmallerOrEqual : WCT_LargerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = isBuy ? WCT_LargerOrEqual : WCT_SmallerOrEqual;
		}

		entrust._action = COND_ACTION_SP;

		condList.emplace_back(entrust);
	}
}

void CtaMocker::append_signal(const char* stdCode, double qty, const char* userTag /* = "" */, double price /* = 0.0 */, uint32_t sigType /* = 0 */)
{
	double curPx = _price_map[stdCode];

	// _sig_map 以合约为键，不建立一串独立订单；再次设置同合约目标会更新最新意图。
	const auto previous = _sig_map.find(stdCode);
	const bool replaces_pending = previous != _sig_map.end()
		&& !decimal::eq(previous->second._pending.target(), qty);
	const double old_target = replaces_pending ? previous->second._pending.target() : 0.0;
	SigInfo& sInfo = _sig_map[stdCode];
	const auto pos = _pos_map.find(stdCode);
	const double actual = pos == _pos_map.end() ? 0.0 : pos->second._volume;
	if (replaces_pending)
	{
		// 旧目标尚未成交的量在覆盖时失效；这里只记录事实，不重复计为成交。
		// 例如目标 10 已成交 7，改目标 7，则 old_remaining=3。
		_target_replacement_logs << fmt::format("{},{},{},{},{},{},{}\n", stdCode,
			current_event_sequence(stdCode),
			(uint64_t)_replayer->get_date() * 1000000000
				+ (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs(),
			old_target, qty, actual, abs(old_target - actual));
	}
	// 真值始终是“最新目标”与“实际仓位”；例如目标 10 已成交 7，剩余是 3。
	// 同值重复信号保留激活时钟；tag-only 变化仅更新标签，
	// 不视为新的交易意图，也不能重新领取当下 Tick 的预算。
	sInfo._pending.on_target(stdCode, qty, actual, current_event_sequence(stdCode));
	sInfo._sigprice = curPx;
	sInfo._desprice = price;
	sInfo._usertag = userTag;
	sInfo._gentime = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs();
	sInfo._sigtype = sigType;
	// 同事件多次不同目标只保留最后一个；不同合约各有独立状态。
	// 来源只用于解释目标是在调度、策略 Tick 回调还是条件触发时产生。
	sInfo._source = sigType == 2 ? SignalSource::Condition
		: (_is_in_schedule ? SignalSource::Schedule
		: (_in_tick_callback ? SignalSource::StrategyTick : SignalSource::Other));

	log_signal(stdCode, qty, curPx, sInfo._gentime, userTag);

	//save_data();
}

// 消费已生成的目标仓位信号时调用。可以把本函数理解为“决策与记账的桥”：
// 先从实际仓位、最新目标、事件/盘口/量源组成 FillRequest，再交给模型判断。
// 模型只决定是否成交、本次有符号成交量和 touch 基准价；
// 原版交易明细、费用、静态滑点和资金处理仍留在 apply_fill。
FillStatus CtaMocker::do_set_position(const char* stdCode, double qty, double price,
	const char* userTag, uint64_t created_sequence, SignalSource source)
{
	// operator[] 在该合约第一次交易时建立默认 PosInfo，与原版调用顺序相同。
	PosInfo& pInfo = _pos_map[stdCode];
	// 非零 price 通常来自信号保存的指定价；零价则回退到上下文价格表。
	// curPx 只是未加滑点的基准价，不等同于最终成交价。
	double curPx = price;
	if (decimal::eq(price, 0.0))
		curPx = _price_map[stdCode];
	uint64_t curTm = (uint64_t)_replayer->get_date() * 10000 + _replayer->get_min_time();
	uint32_t curTDate = _replayer->get_trading_date();

	// 把最新目标和本次回放事件传给模型；时间使用逻辑序号，不用墙钟。
	// qty 是目标总仓位，不是本次一定要成交的量；pInfo._volume 才是实际仓位。
	FillRequest request;
	request.actual_position = pInfo._volume;
	request.target_position = qty;
	request.base_price = curPx;
	request.created_sequence = created_sequence;
	request.source = source;
	request.market.code = stdCode;
	const uint64_t event_sequence = current_event_sequence(stdCode);
	request.market.event_sequence = event_sequence;
	request.market.trading_date = curTDate;
	// 同一合约、交易日、事件的预算账本只初始化一次；两次 proc_tick 共用。
	// 对 Legacy/Causal，volume_ledger 始终为空，旧模型不受参与率机制影响。
	CtaEventVolumeLedger* volume_ledger = nullptr;
	if (dynamic_cast<VolumeLimitedFill*>(_fill_model.get()) != nullptr)
	{
		volume_ledger = &_volume_ledgers[stdCode];
		volume_ledger->begin(curTDate, event_sequence);
		// 第一次尝试通常为 0；同 Tick 第二次尝试会看到第一次实际用掉的量。
		request.market.volume_used = volume_ledger->used();
	}
	// _ticks 在 handle_tick 开头更新。模拟 Bar 的 Tick 即使带了合成报价，
	// 也不能把它当成真实盘口；因果模型将返回 NoQuote，绝不回退 last。
	// bidqty1/askqty1 虽传给快照，目前不参与 volume_limited 的预算计算。
	auto tick_it = _ticks.find(stdCode);
	if (tick_it != _ticks.end())
	{
		const WTSTickStruct& tick = tick_it->second;
		request.market.last = tick.price;
		if (!_replayer->is_tick_simulated())
		{
			// 因果模型买取 ask1、卖取 bid1；若真实盘口缺档则继续等待。
			request.market.bid1 = tick.bid_prices[0];
			request.market.ask1 = tick.ask_prices[0];
			request.market.bidqty1 = tick.bid_qty[0];
			request.market.askqty1 = tick.ask_qty[0];
			if (volume_ledger != nullptr)
				// 这是经累计量核对的“当前 Tick 增量”，不是 total_volume 全日累计量。
				request.market.volume_delta = _event_volumes[stdCode];
		}
	}
	FillResult fill = _fill_model->decide(request);
	// Legacy 保持原 apply_fill 的价格路径；新增模型则在会计记账前统一完成
	// touch -> 滑点 -> tick 对齐 -> 涨跌停检查。拒绝价绝不能先改仓位。
	if (fill.status == FillStatus::Filled && _fill_model_name != "legacy_cta")
	{
		WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
		if (commInfo == NULL)
		{
			fill.status = FillStatus::InvalidInput;
			fill.signed_delta = 0.0;
		}
		else
		{
			CtaPriceRequest price_request;
			price_request.touch_price = fill.base_price;
			price_request.price_tick = commInfo->getPriceTick();
			price_request.slippage = static_cast<double>(_slippage);
			price_request.ratio_slippage = _ratio_slippage;
			price_request.is_buy = fill.signed_delta > 0.0;
			if (tick_it != _ticks.end())
			{
				price_request.lower_limit = tick_it->second.lower_limit;
				price_request.upper_limit = tick_it->second.upper_limit;
			}
			const CtaPriceResult priced = CtaPricePolicy::calculate(price_request);
			if (priced.status == FillStatus::Filled)
			{
				fill.execution_price = priced.execution_price;
				fill.execution_price_ready = true;
			}
			else
			{
				fill.status = priced.status;
				fill.signed_delta = 0.0;
			}
		}
	}
	// 饱和计算只用于审计；真正的门槛在 CausalTouchFill::decide 中用减法比较，
	// 不会因 created_seq + 1 + delay 的 uint64 溢出而提前激活。
	const uint64_t max_seq = std::numeric_limits<uint64_t>::max();
	const uint64_t activation_seq = created_sequence >= max_seq - _fill_delay_events
		? max_seq : created_sequence + 1 + _fill_delay_events;
	_fill_decision_logs << stdCode << "," << _fill_model_name << "," << event_sequence
		<< "," << created_sequence << "," << activation_seq << ","
		<< cta_fill_status_name(fill.status) << "," << qty << "," << pInfo._volume
		<< "," << request.market.volume_delta << "," << request.market.volume_used
		<< "," << fill.base_price << ","
		<< (fill.execution_price_ready ? fill.execution_price : 0.0) << "\n";

	// 仓位在 decimal::eq 容差内不变，沿用原版“直接返回、不记账”的处理。
	if (fill.status == FillStatus::NoChange)
		return fill.status;
	// 非有限数输入会被新模型拒绝；这是相对未校验的原版唯一明确的边界变化。
	if (fill.status == FillStatus::InvalidInput)
	{
		log_error("invalid CTA fill input for {}", stdCode);
		return fill.status;
	}
	// 延迟、缺报价、零预算等都不调用 apply_fill：不产生成交与手续费。
	// 外层 proc_tick 将保留待成交目标，等下一事件重新计算 remaining。
	if (fill.status != FillStatus::Filled)
		return fill.status;

	// 决策只给出有符号差额和基准价；真正的成交、滑点和会计副作用在下方。
	// 只把本次有符号成交增量交给旧会计路径；Legacy/Causal 的增量仍是完整差额。
	// 例：实际 0、目标 10、预算 7 -> signed_delta=+7，传给 apply_fill 的 qty=7。
	// 若这里误传目标 10，费用、交易 CSV 与仓位都会虚增 3 手。
	const double actual_before = pInfo._volume;
	const double actual_after = actual_before + fill.signed_delta;
	const double final_price = apply_fill(stdCode, actual_after, userTag, curTm, curTDate, fill);
	const double applied_delta = pInfo._volume - actual_before;
	if (!decimal::eq(applied_delta, 0.0) && std::isfinite(final_price))
	{
		// 成本仅按实际成交数量统计。spread 以当前买卖一档中点为基准，
		// extra 以 touch 为基准；last 单列作观察价，不能冒充盘口中点。
		const double direction = applied_delta > 0.0 ? 1.0 : -1.0;
		const double scale = _replayer->get_commodity_info(stdCode)->getVolScale();
		const double magnitude = std::abs(applied_delta) * scale;
		const bool has_mid = _fill_model_name != "legacy_cta"
			&& request.market.bid1 > 0.0 && request.market.ask1 > 0.0;
		const double mid = has_mid ? (request.market.bid1 + request.market.ask1) / 2.0 : 0.0;
		const double spread_cost = has_mid
			? direction * (fill.base_price - mid) * magnitude : 0.0;
		const double extra_cost = direction * (final_price - fill.base_price) * magnitude;
		_fill_audit_logs << stdCode << "," << _fill_model_name << "," << curTm
			<< "," << event_sequence << "," << created_sequence << "," << qty
			<< "," << actual_before << "," << pInfo._volume << "," << applied_delta
			<< "," << request.market.last << ",";
		if (has_mid)
			_fill_audit_logs << mid;
		_fill_audit_logs << "," << fill.base_price << "," << final_price << ",";
		if (has_mid)
			_fill_audit_logs << spread_cost;
		_fill_audit_logs << "," << extra_cost << "\n";
	}
	if (volume_ledger != nullptr)
		// 必须以记账后的真实仓位差额扣预算；不能只按模型建议量扣。
		volume_ledger->consume(std::abs(pInfo._volume - actual_before));
	return fill.status;
}
#pragma region apply_fill
// 这一段由原 do_set_position 的记账主体抽出，仍维持原分支和计算顺序。
// Filled 表示“回测中准备按此数量立即记账”，不是外部交易通道的成交回报。
// qty 是本次成交后的仓位而非策略最终目标；部分成交后 pending 仍保存原目标。
// 这里沿用原版会计实现，Day16 不另外写一套资金/手续费/开平仓逻辑。
double CtaMocker::apply_fill(const char* stdCode, double qty, const char* userTag,
	uint64_t curTm, uint32_t curTDate, const FillResult& fill)
{
	PosInfo& pInfo = _pos_map[stdCode];
	// 与原版相同：找不到合约品种信息就无法获得价格跳动、乘数和费率，直接返回。
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
		return std::numeric_limits<double>::quiet_NaN();

	// 从模型传回的基准价起算；下面按买卖方向叠加滑点得到最终记账价。
	double trdPx = fill.execution_price_ready ? fill.execution_price : fill.base_price;

	// 差额为正表示买入，为负表示卖出；它尚未区分“开”还是“平”。
	// volume_limited 已将 abs(diff) 裁剪到本 Tick 剩余预算；之后所有记录
	// 都必须使用这个 diff，而不能重新使用策略目标与旧仓位的完整差额。
	double diff = fill.signed_delta;
	bool isBuy = decimal::gt(diff, 0.0);
	// 仅旧仓非零且增量与旧仓同向时走加仓分支。
	// 旧仓为零时乘积为零，会进入下方 else 并在“剩余量”分支新开仓。
	if (decimal::gt(pInfo._volume*diff, 0))//当前持仓和仓位变化方向一致, 增加一条明细, 增加数量即可
	{
		pInfo._volume = qty;

		//如果T+1，则冻结仓位要增加
		if (commInfo->isT1())
		{
			//ASSERT(diff>0);
			pInfo._frozen += diff;
			log_debug("{} frozen position up to {}", stdCode, pInfo._frozen);
		}
		
		// 固定滑点按价格跳动数计算；比例滑点按万分比计算后修正到价格跳动。
		// 买入抬高记账价、卖出压低记账价；模型自身尚未处理滑点。
		if (_slippage != 0 && !fill.execution_price_ready)
		{
			if (_ratio_slippage)
			{
				//By Wesley @ 2023.05.05
				//如果是比率滑点，则要根据目标成交价计算
				//得到滑点以后，再根据pricetick做一个修正
				double slp = (_slippage * trdPx / 10000.0);
				slp = round(slp / commInfo->getPriceTick())*commInfo->getPriceTick();

				trdPx += slp * (isBuy ? 1 : -1);
			}
			else
				trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);
		}

		// 加仓产生新的持仓明细，并保存本次开仓价、数量、时间与策略标签。
		DetailInfo dInfo;
		dInfo._long = decimal::gt(qty, 0);
		dInfo._price = trdPx;
		dInfo._max_price = trdPx;
		dInfo._min_price = trdPx;
		dInfo._volume = abs(diff);
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		strcpy(dInfo._opentag, userTag);
		dInfo._open_barno = _schedule_times;
		pInfo._details.emplace_back(dInfo);
		pInfo._last_entertime = curTm;

		// 开仓费由回放器根据品种费率计算；成交记录和资金累计仍在旧路径中写入。
		double fee = _replayer->calc_fee(stdCode, trdPx, abs(diff), 0);
		_fund_info._total_fees += fee;

		log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(diff), userTag, fee, _schedule_times);
	}
	else
	{// 旧仓与增量不同向，或旧仓为零：先消耗旧明细，仍有余量再开新方向。
		// left 是尚未分配的绝对成交量；空仓开仓时没有旧明细可平。
		double left = abs(diff);
		// 减仓、平仓、反手也只对同一个基准价应用一次原版滑点。
		if (_slippage != 0 && !fill.execution_price_ready)
		{
			if (_ratio_slippage)
			{
				//By Wesley @ 2023.05.05
				//如果是比率滑点，则要根据目标成交价计算
				//得到滑点以后，再根据pricetick做一个修正
				double slp = (_slippage * trdPx / 10000.0);
				slp = round(slp / commInfo->getPriceTick())*commInfo->getPriceTick();

				trdPx += slp * (isBuy ? 1 : -1);
			}
			else
				trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);
		}

		pInfo._volume = qty;
		if (decimal::eq(pInfo._volume, 0))
			pInfo._dynprofit = 0;
		// 按已有持仓明细的顺序逐条抵扣；一笔目标变化可能产生多条平仓记录。
		uint32_t count = 0;
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			double maxQty = min(dInfo._volume, left);
			if (decimal::eq(maxQty, 0))
				continue;

			double maxProf = dInfo._max_profit * maxQty / dInfo._volume;
			double maxLoss = dInfo._max_loss * maxQty / dInfo._volume;

			dInfo._volume -= maxQty;
			left -= maxQty;

			if (decimal::eq(dInfo._volume, 0))
				count++;

			// 平仓盈亏按成交价差、平仓量和合约乘数计算；空头方向取反。
			// 同时更新明细、策略已实现盈亏和资金累计，不由 FillResult 承担。
			double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!dInfo._long)
				profit *= -1;
			pInfo._closeprofit += profit;
			_total_closeprofit += profit;
			pInfo._dynprofit = pInfo._dynprofit*dInfo._volume / (dInfo._volume + maxQty);//浮盈也要做等比缩放
			pInfo._last_exittime = curTm;
			_fund_info._total_profit += profit;

			// 原版用费率标志 2/1 区分同交易日与非同交易日平仓。
			// log_trade 与 log_close 分别形成成交记录和平仓盈亏记录。
			double fee = _replayer->calc_fee(stdCode, trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;
			//这里写成交记录
			log_trade(stdCode, dInfo._long, false, curTm, trdPx, maxQty, userTag, fee, _schedule_times);
			//这里写平仓记录
			log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, maxProf, maxLoss, 
				_total_closeprofit - _fund_info._total_fees, dInfo._opentag, userTag, dInfo._open_barno, _schedule_times);

			if (left == 0)
				break;
		}

		//需要清理掉已经平仓完的明细
		while (count > 0)
		{
			auto it = pInfo._details.begin();
			pInfo._details.erase(it);
			count--;
		}

		// 抵扣旧明细后仍有余量时开新仓：既覆盖反手，也覆盖从空仓开仓。
		// 例如旧仓 +3、目标 -4、本次只能卖 3：left 在平完旧多仓后为 0，
		// 当前事件不会越过零仓直接开空；下一事件再由最新 actual 重算差额。
		// 新仓继续使用上面已经加过滑点的 trdPx，不会再次叠加滑点。
		if (left > 0)
		{
			left = left * qty / abs(qty);

			//如果T+1，则冻结仓位要增加
			if (commInfo->isT1())
			{
				pInfo._frozen += left;
				log_debug("{} frozen position up to {}", stdCode, pInfo._frozen);
			}

			DetailInfo dInfo;
			dInfo._long = decimal::gt(qty, 0);
			dInfo._price = trdPx;
			dInfo._max_price = trdPx;
			dInfo._min_price = trdPx;
			dInfo._volume = abs(left);
			dInfo._opentime = curTm;
			dInfo._opentdate = curTDate;
			dInfo._open_barno = _schedule_times;
			strcpy(dInfo._opentag, userTag);
			pInfo._details.emplace_back(dInfo);
 
			//这里还需要写一笔成交记录
			double fee = _replayer->calc_fee(stdCode, trdPx, abs(left), 0);
			_fund_info._total_fees += fee;
			log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(left), userTag, fee, _schedule_times);

			pInfo._last_entertime = curTm;
		}
	}
	return trdPx;
}

WTSKlineSlice* CtaMocker::stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain /* = false */)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, period);

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);
	else
		strcat(key, "1");

	if (isMain)
	{
		if (_main_key.empty())
			_main_key = key;
		else if (_main_key != key)
		{
			WTSLogger::error("Main k bars can only be setup once");
			return NULL;
		}

		/*
		 *	By Wesley @ 2022.07.16
		 */
		_main_code = stdCode;
		_main_period = period;
	}

	WTSKlineSlice* kline = _replayer->get_kline_slice(stdCode, basePeriod, count, times, isMain);

	KlineTag& tag = _kline_tags[key];
	tag._closed = false;

	if (kline)
	{
		//double lastClose = kline->close(-1);
		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _replayer->get_hot_mgr());
		WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
		std::string realCode = stdCode;
		if(cInfo.isExright())
			realCode = realCode.substr(0, realCode.size()-1);
		_replayer->sub_tick(id(), realCode.c_str());
	}

	return kline;
}

WTSTickSlice* CtaMocker::stra_get_ticks(const char* stdCode, uint32_t count)
{
	return _replayer->get_tick_slice(stdCode, count);
}

WTSTickData* CtaMocker::stra_get_last_tick(const char* stdCode)
{
	auto it = _ticks.find(stdCode);
	if (it != _ticks.end())
	{
		WTSTickData* lastTick = WTSTickData::create((WTSTickStruct&)it->second);
		return lastTick;
	}

	return _replayer->get_last_tick(stdCode);
}

void CtaMocker::stra_sub_ticks(const char* code)
{
	/*
	 *	By Wesley @ 2022.03.01
	 *	主动订阅tick会在本地记一下
	 *	tick数据回调的时候先检查一下
	 */
	_tick_subs.insert(code);

	_replayer->sub_tick(_context_id, code);
}

void CtaMocker::stra_sub_bar_events(const char* stdCode, const char* period)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, period);

	KlineTag& tag = _kline_tags[key];
	tag._notify = true;
}

WTSCommodityInfo* CtaMocker::stra_get_comminfo(const char* stdCode)
{
	return _replayer->get_commodity_info(stdCode);
}

std::string CtaMocker::stra_get_rawcode(const char* stdCode)
{
	return _replayer->get_rawcode(stdCode);
}

uint32_t CtaMocker::stra_get_tdate()
{
	return _replayer->get_trading_date();
}

uint32_t CtaMocker::stra_get_date()
{
	return _replayer->get_date();
}

uint32_t CtaMocker::stra_get_time()
{
	return _replayer->get_min_time();
}

double CtaMocker::stra_get_fund_data(int flag)
{
	switch (flag)
	{
	case 0:
		return _fund_info._total_profit - _fund_info._total_fees + _fund_info._total_dynprofit;
	case 1:
		return _fund_info._total_profit;
	case 2:
		return _fund_info._total_dynprofit;
	case 3:
		return _fund_info._total_fees;
	default:
		return 0.0;
	}
}

void CtaMocker::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

void CtaMocker::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

void CtaMocker::stra_log_warn(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message);
}

void CtaMocker::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}

const char* CtaMocker::stra_load_user_data(const char* key, const char* defVal /*= ""*/)
{
	auto it = _user_datas.find(key);
	if (it != _user_datas.end())
		return it->second.c_str();

	return defVal;
}

void CtaMocker::stra_save_user_data(const char* key, const char* val)
{
	_user_datas[key] = val;
	_ud_modified = true;
}

uint64_t CtaMocker::stra_get_first_entertime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return 0;

	return pInfo._details[0]._opentime;
}

uint64_t CtaMocker::stra_get_last_entertime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return 0;

	return pInfo._details[pInfo._details.size() - 1]._opentime;
}

const char* CtaMocker::stra_get_last_entertag(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return "";

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return "";

	return pInfo._details[pInfo._details.size() - 1]._opentag;
}

uint64_t CtaMocker::stra_get_last_exittime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._last_exittime;
}

double CtaMocker::stra_get_last_enterprice(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return 0;

	return pInfo._details[pInfo._details.size() - 1]._price;
}

double CtaMocker::stra_get_position(const char* stdCode, bool bOnlyValid /* = false */, const char* userTag /* = "" */)
{
	//By Wesley @ 2022.05.22
	//如果有信号，说明刚下了指令，还没等到下一个tick进来，用户就在读取仓位
	// 注意：如果还没有建立该合约的 PosInfo，此接口会先返回待成交目标，
	// 这是旧接口的即时读数语义，不能据此认定目标已真正成交。
	// 一旦 _pos_map 已建立，下方返回 pInfo._volume，实际成交应以 trades.csv 核实。
	double totalPos = 0;
	auto sit = _sig_map.find(stdCode);
	if (sit != _sig_map.end())
	{
		totalPos = sit->second._pending.target();
	}

	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return totalPos;

	const PosInfo& pInfo = it->second;
	totalPos = pInfo._volume;

	if (strlen(userTag) == 0)
	{
		if (bOnlyValid)
		{
			//只有userTag为空的时候时候，才会用bOnlyValid
			//这里理论上，只有多头才会进到这里
			//其他地方要保证，空头持仓的话，_frozen要为0
			return totalPos - pInfo._frozen;
		}
		else
			return totalPos;
	}
	else
	{
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
		{
			const DetailInfo& dInfo = (*it);
			if (strcmp(dInfo._opentag, userTag) != 0)
				continue;

			return dInfo._volume;
		}
	}

	return 0;
}

double CtaMocker::stra_get_position_avgpx(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._volume == 0)
		return 0.0;

	double amount = 0.0;
	for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
	{
		const DetailInfo& dInfo = *dit;
		amount += dInfo._price*dInfo._volume;
	}

	return amount / pInfo._volume;
}

double CtaMocker::stra_get_position_profit(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._dynprofit;
}

uint64_t CtaMocker::stra_get_detail_entertime(const char* stdCode, const char* userTag)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		return dInfo._opentime;
	}

	return 0;
}

double CtaMocker::stra_get_detail_cost(const char* stdCode, const char* userTag)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		return dInfo._price;
	}

	return 0.0;
}

double CtaMocker::stra_get_detail_profit(const char* stdCode, const char* userTag, int flag /* = 0 */)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		switch (flag)
		{
		case 0:
			return dInfo._profit;
		case 1:
			return dInfo._max_profit;
		case -1:
			return dInfo._max_loss;
		case 2:
			return dInfo._max_price;
		case -2:
			return dInfo._min_price;
		}
	}

	return 0.0;
}

void CtaMocker::set_chart_kline(const char* stdCode, const char* period)
{
	_chart_code = stdCode;
	_chart_period = period;
}

void CtaMocker::add_chart_mark(double price, const char* icon, const char* tag)
{
	if (!_is_in_schedule)
	{
		WTSLogger::error("Marks can be added only during schedule");
		return;
	}

	uint64_t curTime = _replayer->get_date();
	curTime = curTime*10000 + _replayer->get_min_time();

	_mark_logs << curTime << "," << price << "," << icon << "," << tag << std::endl;
}

void CtaMocker::register_index(const char* idxName, uint32_t indexType)
{
	ChartIndex& cIndex = _chart_indice[idxName];
	cIndex._name = idxName;
	cIndex._indexType = indexType;
}

bool CtaMocker::register_index_line(const char* idxName, const char* lineName, uint32_t lineType)
{
	auto it = _chart_indice.find(idxName);
	if (it == _chart_indice.end())
	{
		WTSLogger::error("Index {} not registered", idxName);
		return false;
	}

	ChartIndex& cIndex = it->second;
	ChartLine& cLine = cIndex._lines[lineName];
	cLine._name = lineName;
	cLine._lineType = lineType;
	return true;
}

bool CtaMocker::add_index_baseline(const char* idxName, const char* lineName, double val)
{
	auto it = _chart_indice.find(idxName);
	if (it == _chart_indice.end())
	{
		WTSLogger::error("Index {} not registered", idxName);
		return false;
	}

	ChartIndex& cIndex = it->second;
	cIndex._base_lines[lineName] = val;
	return true;
}

bool CtaMocker::set_index_value(const char* idxName, const char* lineName, double val)
{
	if (!_is_in_schedule)
	{
		WTSLogger::error("Marks can be added only during schedule");
		return false;
	}

	auto ait = _chart_indice.find(idxName);
	if (ait == _chart_indice.end())
	{
		WTSLogger::error("Index {} not registered", idxName);
		return false;
	}

	ChartIndex& cIndex = ait->second;
	auto bit = cIndex._lines.find(lineName);
	if (bit == cIndex._lines.end())
	{
		WTSLogger::error("Line {} of index {} not registered", lineName, idxName);
		return false;
	}

	uint64_t curTime = _replayer->get_date();
	curTime = curTime * 10000 + _replayer->get_min_time();
	_index_logs << curTime << "," << idxName << "," << lineName << "," << val << std::endl;
	return true;
}

