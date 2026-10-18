#pragma once

namespace ShaderLab
{
	class McpRouter;
}
namespace ShaderLab::Mcp
{
	class IEngineCommandSink;
}

namespace ShaderLab::Support
{
	// /graph/load-file and /graph/save-file: .effectgraph packages and plain
	// JSON graphs on disk. Registered by ShaderLab's hosts next to the
	// engine routes.
	void RegisterPackageRoutes(McpRouter& router, Mcp::IEngineCommandSink& sink);
}
