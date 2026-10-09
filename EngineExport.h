#pragma once
#ifdef SHADERLAB_ENGINE_EXPORTS
#define SHADERLAB_API __declspec(dllexport)
#else
#define SHADERLAB_API __declspec(dllimport)
#endif

// Engine ABI version. Bumped manually whenever a public engine symbol's
// signature or behavior changes in a way that would break a host built
// against an older header. Independent of `Version.h::VersionMajor` (the
// app version) and `Version.h::GraphFormatVersion` (the JSON schema).
//
// Hosts (the WinUI app, ShaderLabHeadless when it lands, third-party
// consumers if any) should call `ShaderLab_GetAbiVersion()` at startup
// and compare against this constant. Mismatch means the DLL is from a
// different build than the headers and is unsafe to use.
//
// **History** (compatibility-breaking changes):
//   1: Initial. Phase 6.
//   2: stdio-migration Step 2 — McpHttpServer renamed McpRouter,
//      Response extracted to ShaderLab::Mcp::Response (McpTypes.h) with
//      a noReply discriminator, Handler gains a query argument,
//      IEngineCommandSink::Dispatch re-typed accordingly, EngineContext
//      gains getPipelineFormatName.
//   3: stdio-migration Step 9 — the HTTP transport is deleted. McpRouter
//      loses Start/Stop/Port/IsRunning + the Winsock listener (keeps
//      AddRoute/RouteRequest/HasRoute); the broker (shim → hub → session)
//      is the only transport. ActivityCallback's peer arg becomes clientId.
//   4: Layout changes to exported types, accumulated over the GPU-timing and
//      tiling work: EffectNode gains lastGpuMs / gpuState / clockTickBucket,
//      CustomEffectDefinition gains lookupInputCount, GraphEvaluator gains the
//      GPU timer, dispatch counter and pull-propagation set, and
//      D3D11ComputeRunner is newly exported. A host built against v3 headers
//      reads these at the wrong offsets -- the exact access violation inside
//      the engine DLL that a stale incremental build produced mid-session.
//   5: Performance pass. GraphEvaluator drops the Evaluate-time pre-render
//      and gains content-addressed variant/reflection memos, split analysis
//      targets and async-readback bookkeeping; D3D11ComputeRunner's single
//      staging buffer becomes a ring and its readback flag an enum.
//   6: User effects and option variants. Layout changes to
//      ShaderLabEffectDescriptor (sourcePath), ShaderLabEffects (load report),
//      VideoSourceProvider (m_convertCtx), ParameterDefinition (specialize),
//      EffectNode (variantsCompiling), BytecodeCompileKey (optionKey) and
//      GraphEvaluator (variant state); a new IEngineCommandSink virtual
//      (OnGraphMediaDirChanged), EffectGraphFile::LoadResult (package) and
//      Mcp::EngineContext (renderFrameFor).
//   7: ShaderLabEffectDescriptor gains deriveImageOutputSize; D3D11ComputeRunner
//      gains the two-pass image contract (accumulator, pass constants);
//      VideoSourceProvider's decode-ahead queue, PlayTo and learned
//      keyframe spans, and SourceNodeFactory without its paused-clock map
//      and with its past-the-end black images.
#define SHADERLAB_ENGINE_ABI_VERSION 7

// C-linkage entry so it can be GetProcAddress'd if a host wants to do a
// version check before dynamically loading the DLL.
extern "C" SHADERLAB_API uint32_t ShaderLab_GetAbiVersion();

