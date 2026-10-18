# ShaderLab Documentation

The repo root [README.md](../README.md) covers identity, install, and getting-started. Everything below is the deeper technical reference, organized by audience.

> **Looking for the changelog?** [`CHANGELOG.md`](../CHANGELOG.md) at the repo root tracks every version. The [decision log](history/decision-log.md) below tracks architectural choices with rationale (independent of release boundaries).

ShaderLab's engine is [EffectGraph](https://github.com/Spruill-1/EffectGraph), which lives in its own repository with its own documentation. Its documents are listed [below](#effectgraph-engine--in-its-own-repository).

---

## Architecture

Reference for "how does ShaderLab work under the hood".

- [Architecture Overview](architecture/overview.md) — high-level diagram of the components (engine package, support library, UI controllers, hosts).
- [Threading Model](architecture/threading-model.md) — UI thread vs render worker, offscreen-blit composition, dispatcher, output-window sinks.
- [Engine / Host Split](architecture/engine-host-split.md) — EffectGraph's engine library, ShaderLab's support library, `IEngineCommandSink`, host event hooks, the retired engine ABI history.

## Effects

- [Effect Designer](effects/effect-designer.md) — the modal window for authoring custom pixel/compute shaders.
- [Numeric Expression Node (ExprTk)](effects/numeric-expression.md) — single-input math expression parameter node.

## UI / UX

- [Graph Editor UX](ui-ux/graph-editor.md) — keyboard / mouse shortcuts, color coding, inline data display.
- [Multi-Output Windows](ui-ux/multi-output-windows.md) — the secondary preview windows.
- [Animation System](ui-ux/animation-system.md) — Clock-driven dirty propagation.
- [Conditional Parameter Visibility](ui-ux/conditional-parameter-visibility.md) — `visibleWhen` expressions on parameters.

## Hosts

- [ShaderLabHeadless (Console Host)](hosts/headless.md) — the no-UI host for CI / batch scripting.
- [MCP Server (AI Agent Integration)](hosts/mcp-server.md) — JSON-RPC 2.0 server, tool catalog, route distribution.

## Development

- [Build Instructions](development/build.md) — prerequisites, EffectGraph packages, configurations, dependency map.
- [Releasing](development/releasing.md) — the tag-driven release, Azure Artifact Signing, the one-time setup, the private effects' related set.
- [Project Structure](development/project-structure.md) — full file tree with per-file descriptions.
- [MCP Migration: HTTP → stdio](development/mcp-stdio-migration.md) — **complete** (all 9 steps; decision #71, engine ABI 3). The implementation plan and rationale for replacing the embedded HTTP MCP server with stdio + a broker relay, kept as the reference for how the transport works.

## History

- [Decision Log](history/decision-log.md) — chronological architectural decisions with rationale.

## EffectGraph (engine) — in its own repository

The graph model, evaluator, effects and their documentation moved to [EffectGraph](https://github.com/Spruill-1/EffectGraph) (decision #231).

Architecture:

- [Pipeline Format Strategy](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/pipeline-format.md) — why the pipeline is always scRGB FP16 and how DWM/ACM handles the final display conversion.
- [Graph Model](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/effect-graph-model.md) — `GraphModel` / `EffectNode` / `EffectEdge` / `PropertyValue`, JSON serialization, dirty tracking.
- [Topological Evaluation](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/topological-evaluation.md) — Kahn's algorithm, evaluation order, cycle detection.
- [Display Monitoring](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/display-monitoring.md) — WinRT `AdvancedColorInfo` + `AdvancedColorInfoChanged` event, ICC profile parsing, SDR white level.
- [Display Profile Mocking](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/display-profile-mocking.md) — simulated SDR/HDR/WCG environments and the testing harness.
- [Compute Shader Analysis Pipeline](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/compute-analysis-pipeline.md) — D2D compute conventions, CPU readback, analysis output schema.
- [D2D / D3D11 Hybrid Compute System](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/d2d-d3d11-hybrid-compute.md) — `CustomComputeBridgeEffect`, `D3D11ComputeRunner`, GPU-binding routing, COM class hierarchy.
- [EffectGraph API](https://github.com/Spruill-1/EffectGraph/blob/main/docs/architecture/effectgraph-api.md) — the engine as a COM library for any D3D11 program: `IEffectGraphContext`, `IEffectGraphNode`, texture inputs, `Realize` / `RealizeTo`.

Effects:

- [Built-in Effect Catalog](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/builtin-catalog.md) — the 38 built-in effects (Analysis, Color, Source, Tone Mapping, Parameter).
- [User Effects](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/user-effects.md) — effects registered at runtime from saved-graph files, outside the build.
- [Effect Versioning System](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/effect-versioning.md) — how `effectVersion` bumps are detected on graph load.
- [Option Variants](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/option-variants.md) — compile one shader per option of an option parameter, all up front.
- [Parameter Nodes](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/parameter-nodes.md) — Float / Integer / Toggle / Gamut Parameter and Clock.
- [Property Bindings (Data Pins)](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/property-bindings.md) — wiring analysis fields to downstream parameters.
- [Working Space Integration](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/working-space.md) — host-driven node mirroring the active display profile.
- [New Effect Defaults](https://github.com/Spruill-1/EffectGraph/blob/main/docs/effects/new-effect-defaults.md) — default property values for every newly added D2D effect.

Development:

- [Building EffectGraph](https://github.com/Spruill-1/EffectGraph/blob/main/docs/development/build.md) — the engine projects, built-in effect header generation, its tests.
- [EffectGraph Packages](https://github.com/Spruill-1/EffectGraph/blob/main/docs/development/packaging.md) — `EffectGraph.Engine`, `EffectGraph.Native` and the C# `EffectGraph` package, how they are built.

---

Back to [Repo root](../README.md).
