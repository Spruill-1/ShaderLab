#include "pch_engine.h"
#include "EngineMcpRoutes.h"
#include "Routes/RouteSupport.h"

namespace ShaderLab::Mcp
{
    void RegisterEngineRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        Routes::RegisterCatalogRoutes(router);
        Routes::RegisterGraphRoutes(router, sink);
        Routes::RegisterPropertyRoutes(router, sink);
        Routes::RegisterEffectRoutes(router, sink);
        Routes::RegisterReadbackRoutes(router, sink);
        Routes::RegisterFileRoutes(router, sink);
        Routes::RegisterDisplayRoutes(router, sink);
    }
}
