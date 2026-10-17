#include "pch_engine.h"
#include "ShaderLabSupport.h"
#include "ExprTkEvaluator.h"
#include "Rendering/MathExpression.h"

namespace ShaderLab::Support
{
	void RegisterShaderLabSupport()
	{
		Rendering::SetMathExpressionEvaluator(&EvaluateWithExprTk);
	}
}
