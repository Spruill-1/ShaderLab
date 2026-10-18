# Architecture Overview

The engine is [EffectGraph](https://github.com/Spruill-1/EffectGraph), a separate repository that ShaderLab consumes as the `EffectGraph.Engine` package (`EffectGraphEngine.lib`, linked statically into each program; decisions #231, #232). Everything else below is this repository.

```mermaid
graph TB
    subgraph App["ShaderLab.exe (WinUI 3 app)"]
        MW[MainWindow / App]
        UI[Controls + GuiEngineCommandSink + XAML]
        RE[RenderEngine<br/>SwapChainPanel binding]
    end

    subgraph Engine["EffectGraph (EffectGraph.Engine package)"]
        EG[Graph/*]
        EV[GraphEvaluator]
        FX[EffectRegistry / ShaderLabEffects / SourceNodeFactory]
        IO[ImageLoader / VideoSourceProvider / capture providers / ShaderCompiler]
        MON[DisplayMonitor / ICC]
    end

    subgraph Support["ShaderLabSupport.lib (static, ShaderLab only)"]
        MCP[Mcp/: McpRouter + Routes/]
        SUP[Support/: ExprTk, .effectgraph files, plugin discovery]
        SRC[Sources/: desktop and window capture]
    end

    subgraph Tests["ShaderLabTests.exe"]
        TR[TestRunner, self-registering cases<br/>Mcp / Hosts / persistence / expressions / host loop / UI snapshot + layout / video export + playback]
    end

    subgraph Headless["ShaderLabHeadless.exe"]
        HC[Console host:<br/>render PNG / FP32 pixels / --script batch]
    end

    MW --> UI
    UI --> RE
    UI --> MCP
    RE --> EV
    UI --> EG
    UI --> FX
    UI --> MON
    EV --> EG
    EV --> FX
    FX --> IO
    MCP --> EG
    MCP --> EV
    TR --> EG
    TR --> EV
    TR --> FX
    TR --> IO
    TR --> MON
    HC --> EG
    HC --> EV
    HC --> FX
    HC --> MCP
```


---

Back to [docs/](../README.md) • [Repo root](../../README.md)