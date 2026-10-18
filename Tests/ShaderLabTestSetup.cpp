#include "pch_engine.h"
#include "TestCommon.h"
#include "Support/ShaderLabSupport.h"

// ShaderLabTests runs with what ShaderLab's hosts register at start-up
// (ExprTk behind the engine's Numeric Expression hook).
SL_TEST_SETUP(RegisterShaderLabSupportForTests)
{
    ShaderLab::Support::RegisterShaderLabSupport();
}
