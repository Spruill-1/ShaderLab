#pragma once
#include <string>

namespace ShaderLab::Rendering
{
	// Evaluates a math expression string using ExprTk. The expression may
	// reference scalar variables A,B,C,D,E via the parallel arrays. Returns
	// true on success and writes the result; on parse/eval failure returns
	// false with an optional error message.
	bool EvaluateMathExpression(
		const std::wstring& expression,
		const wchar_t* const* varNames,
		const float* varValues,
		size_t varCount,
		float& outResult,
		std::wstring* outError = nullptr);
}
