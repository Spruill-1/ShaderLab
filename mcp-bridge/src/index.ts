/**
 * ShaderLab MCP Bridge
 *
 * Translates MCP JSON-RPC (STDIO transport) to HTTP calls against ShaderLab's
 * embedded HTTP API at localhost:PORT.
 *
 * Usage:
 *   SHADERLAB_PORT=47808 node dist/index.js
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const PORT = parseInt(process.env.SHADERLAB_PORT || "47808", 10);
const BASE = `http://localhost:${PORT}`;

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

async function httpGet(path: string): Promise<any> {
  const res = await fetch(`${BASE}${path}`);
  return res.json();
}

async function httpPost(path: string, body: any): Promise<any> {
  const res = await fetch(`${BASE}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return res.json();
}

// ---------------------------------------------------------------------------
// MCP Server
// ---------------------------------------------------------------------------

const server = new McpServer({
  name: "shaderlab",
  version: "1.0.0",
});

// ========================= RESOURCES ========================================

server.resource("context", "shaderlab://context", async (uri) => ({
  contents: [
    {
      uri: uri.href,
      mimeType: "application/json",
      text: JSON.stringify(await httpGet("/context"), null, 2),
    },
  ],
}));

server.resource("graph", "shaderlab://graph", async (uri) => ({
  contents: [
    {
      uri: uri.href,
      mimeType: "application/json",
      text: JSON.stringify(await httpGet("/graph"), null, 2),
    },
  ],
}));

server.resource("registry-effects", "shaderlab://registry/effects", async (uri) => ({
  contents: [
    {
      uri: uri.href,
      mimeType: "application/json",
      text: JSON.stringify(await httpGet("/registry/effects"), null, 2),
    },
  ],
}));

server.resource("custom-effects", "shaderlab://custom-effects", async (uri) => ({
  contents: [
    {
      uri: uri.href,
      mimeType: "application/json",
      text: JSON.stringify(await httpGet("/custom-effects"), null, 2),
    },
  ],
}));

server.resource("output-image", "shaderlab://output/image", async (uri) => ({
  contents: [
    {
      uri: uri.href,
      mimeType: "application/json",
      text: JSON.stringify(await httpGet("/render/capture"), null, 2),
    },
  ],
}));

// ========================= TOOLS ============================================

server.tool(
  "graph_add_node",
  "Add a built-in D2D effect node to the graph by name",
  { effectName: z.string().describe("Name of the built-in effect (e.g. 'Gaussian Blur', 'Color Matrix')") },
  async ({ effectName }) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/add-node", { effectName })) }],
  })
);

server.tool(
  "graph_remove_node",
  "Remove a node from the graph by ID",
  { nodeId: z.number().describe("Node ID to remove") },
  async ({ nodeId }) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/remove-node", { nodeId })) }],
  })
);

server.tool(
  "graph_connect",
  "Connect an output pin to an input pin",
  {
    srcId: z.number().describe("Source node ID"),
    srcPin: z.number().describe("Source output pin index"),
    dstId: z.number().describe("Destination node ID"),
    dstPin: z.number().describe("Destination input pin index"),
  },
  async (args) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/connect", args)) }],
  })
);

server.tool(
  "graph_disconnect",
  "Disconnect an edge between two pins",
  {
    srcId: z.number(),
    srcPin: z.number(),
    dstId: z.number(),
    dstPin: z.number(),
  },
  async (args) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/disconnect", args)) }],
  })
);

server.tool(
  "graph_set_property",
  "Set a property on a node (float, bool, string, or array for vectors)",
  {
    nodeId: z.number().describe("Node ID"),
    key: z.string().describe("Property name"),
    value: z.union([z.number(), z.boolean(), z.string(), z.array(z.number())]).describe("Property value"),
  },
  async (args) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/set-property", args)) }],
  })
);

server.tool(
  "graph_load_json",
  "Load a complete graph from JSON string",
  { json: z.string().describe("Full graph JSON") },
  async ({ json }) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/load", JSON.parse(json))) }],
  })
);

server.tool(
  "graph_save_json",
  "Serialize the current graph to JSON",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await httpGet("/graph/save")) }],
  })
);

server.tool(
  "graph_clear",
  "Clear the entire graph (keeps Output node)",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/graph/clear", {})) }],
  })
);

server.tool(
  "graph_get_node",
  "Get detailed info about a specific node",
  { nodeId: z.number().describe("Node ID") },
  async ({ nodeId }) => ({
    content: [{ type: "text", text: JSON.stringify(await httpGet(`/graph/node/${nodeId}`), null, 2) }],
  })
);

server.tool(
  "effect_compile",
  "Compile HLSL source for a custom effect node",
  {
    nodeId: z.number().describe("Custom effect node ID"),
    hlsl: z.string().describe("HLSL source code"),
  },
  async (args) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/effect/compile", args)) }],
  })
);

server.tool(
  "set_preview_node",
  "Change which node is shown in the preview",
  { nodeId: z.number().describe("Node ID to preview") },
  async (args) => ({
    content: [{ type: "text", text: JSON.stringify(await httpPost("/render/preview-node", args)) }],
  })
);

server.tool(
  "render_capture",
  "Capture the current preview output (SDR tone-mapped PNG). Note: HDR values are clipped. Use render_pixel for true scRGB values.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await httpGet("/render/capture")) }],
  })
);

server.tool(
  "render_pixel",
  "Read the true scRGB FP16 pixel value at coordinates. Values > 1.0 are HDR.",
  {
    x: z.number().describe("X coordinate in image space"),
    y: z.number().describe("Y coordinate in image space"),
  },
  async ({ x, y }) => ({
    content: [{ type: "text", text: JSON.stringify(await httpGet(`/render/pixel/${x}/${y}`)) }],
  })
);

server.tool(
  "registry_get_effect",
  "Get metadata for a built-in D2D effect (properties, ranges, enum labels)",
  { name: z.string().describe("Effect name") },
  async ({ name }) => ({
    content: [{ type: "text", text: JSON.stringify(await httpGet(`/registry/effect/${encodeURIComponent(name)}`), null, 2) }],
  })
);

// ========================= START ============================================

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error("ShaderLab MCP bridge error:", err);
  process.exit(1);
});
