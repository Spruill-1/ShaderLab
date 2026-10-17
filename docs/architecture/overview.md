# Architecture Overview

```mermaid
graph TB
    subgraph App["ShaderLab.exe (WinUI 3 app)"]
        MW[MainWindow / App]
        UI[Controls + GuiEngineCommandSink + XAML]
        RE[RenderEngine<br/>SwapChainPanel binding]
    end

    subgraph Engine["ShaderLabEngine.lib (static)"]
        EG[Graph/*]
        EV[GraphEvaluator]
        FX[EffectRegistry / ShaderLabEffects / SourceNodeFactory]
        IO[ImageLoader / VideoSourceProvider / capture providers / ShaderCompiler]
        MON[DisplayMonitor / ICC]
    end

    subgraph Support["ShaderLabSupport.lib (static, ShaderLab only)"]
        MCP[Mcp/: McpRouter + Routes/]
        SUP[Support/: ExprTk, .effectgraph files]
    end

    subgraph Tests["EffectGraphTests.exe / ShaderLabTests.exe"]
        TR[One TestRunner, self-registering cases<br/>engine: Graph / Effects / Rendering / Sources / Video / Api / Math<br/>ShaderLab: Mcp / Hosts / expressions / host loop / playback]
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