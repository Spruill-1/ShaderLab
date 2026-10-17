#pragma once

namespace ShaderLab::Support
{
	// Plugs ShaderLab's own pieces into the engine: ExprTk for Numeric
	// Expression nodes. Every ShaderLab program calls it once at startup,
	// before any graph evaluates; EffectGraph users do not have it.
	void RegisterShaderLabSupport();
}
