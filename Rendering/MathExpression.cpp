// MathExpression: thin wrapper over ExprTk for the Numeric Expression
// parameter node. ExprTk is a heavy single-header so we isolate it in
// this translation unit (with PCH disabled in the .vcxproj entry).
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

// Strip down ExprTk to the math-only feature set. The disabled subsystems
// pull in <regex> and other heavy machinery that have known crash bugs in
// MSVC Release. We don't use any of them; numeric expressions only need
// the arithmetic, trig, and conditional operators.
#define exprtk_disable_string_capabilities
#define exprtk_disable_rtl_io
#define exprtk_disable_rtl_io_file
#define exprtk_disable_rtl_vecops
#define exprtk_disable_enhanced_features
#define exprtk_disable_caseinsensitivity

// ExprTk pulls in <cmath>, etc. Suppress its own warnings.
#pragma warning(push)
#pragma warning(disable: 4100 4127 4244 4267 4456 4457 4458 4459 4505 4701 4702 4715 4996 26439 26451 26495 26498 26800 26819)
#include "exprtk.hpp"
#pragma warning(pop)

#include "MathExpression.h"

namespace ShaderLab::Rendering
{
	namespace
	{
		// Simple per-thread cache: parsed exprtk expressions are reusable
		// across evaluations, but ExprTk's symbol_table binds variables by
		// address, so we recompile when the variable set changes.
		struct CompiledKey
		{
			std::string utf8;
			std::vector<std::string> vars;
			bool operator==(const CompiledKey&) const = default;
		};
		struct CompiledKeyHash
		{
			size_t operator()(const CompiledKey& k) const noexcept
			{
				size_t h = std::hash<std::string>{}(k.utf8);
				for (const auto& v : k.vars)
					h ^= std::hash<std::string>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};

		std::string Narrow(const std::wstring& w)
		{
			std::string out;
			out.reserve(w.size());
			for (wchar_t c : w)
				out.push_back(static_cast<char>(c < 0x80 ? c : '?'));
			return out;
		}
	}

	bool EvaluateMathExpression(
		const std::wstring& expression,
		const wchar_t* const* varNames,
		const float* varValues,
		size_t varCount,
		float& outResult,
		std::wstring* outError)
	{
		if (expression.empty())
		{
			outResult = 0.0f;
			return true;
		}

		// ExprTk expressions bind variables by reference, so we need stable
		// storage for the current call's variable values.
		std::vector<double> values(varCount);
		std::vector<std::string> names(varCount);
		for (size_t i = 0; i < varCount; ++i)
		{
			values[i] = static_cast<double>(varValues[i]);
			names[i]  = Narrow(varNames[i]);
		}

		exprtk::symbol_table<double> symbols;
		for (size_t i = 0; i < varCount; ++i)
			symbols.add_variable(names[i], values[i]);
		symbols.add_constants();

		exprtk::expression<double> expr;
		expr.register_symbol_table(symbols);

		exprtk::parser<double> parser;
		std::string utf8 = Narrow(expression);
		if (!parser.compile(utf8, expr))
		{
			if (outError)
			{
				std::wstring err = L"Parse error: ";
				std::string e = parser.error();
				err.reserve(err.size() + e.size());
				for (char c : e) err.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
				*outError = std::move(err);
			}
			outResult = 0.0f;
			return false;
		}

		double result = expr.value();
		if (!std::isfinite(result))
		{
			if (outError) *outError = L"Result is not finite";
			outResult = 0.0f;
			return false;
		}
		outResult = static_cast<float>(result);
		return true;
	}
}
