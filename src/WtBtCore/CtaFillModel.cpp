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
