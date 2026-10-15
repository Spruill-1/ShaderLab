# Architecture Overview

```mermaid
graph TB
    subgraph App["ShaderLab.exe (WinUI 3 app)"]
        MW[MainWindow / App]
        UI[Controls + GuiEngineCommandSink + XAML]
        RE[RenderEngine<br/>SwapChainPanel binding]
    end

    subgraph Engine["ShaderLabEngine.dll"]
        EG[Graph/*]
        EV[GraphEvaluator]
        FX[EffectRegistry / ShaderLabEffects / SourceNodeFactory]
        IO[ImageLoader / VideoSourceProvider / capture providers / ShaderCompiler]
        MON[DisplayMonitor / ICC]
        MCP[Engine/Mcp: McpRouter + Routes/]
    end

    subgraph Tests["ShaderLabTests.exe"]
        TR[TestRunner main + self-registering cases<br/>Graph / Effects / Rendering / Sources / Video / Mcp / Math]
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