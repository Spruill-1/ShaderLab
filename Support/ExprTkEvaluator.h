#pragma once
#include <string>

namespace ShaderLab::Support
{
	// Numeric Expression evaluation with ExprTk; the signature of
	// Rendering::MathExpressionEvaluator.
	bool EvaluateWithExprTk(
		const std::wstring& expression,
		const wchar_t* const* varNames,
		const float* varValues,
		size_t varCount,
		float& outResult,
		std::wstring* outError);
}
