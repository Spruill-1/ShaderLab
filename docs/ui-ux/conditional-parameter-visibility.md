# Conditional Parameter Visibility

Effect parameters support conditional visibility via the `visibleWhen` field on `ParameterDefinition`.

- **Format**: `"ParamName == value"` (e.g., `"Mode == 1"`, `"EnableHDR == true"`)
- **Hidden from UI**: When the condition is false, the parameter is hidden from both the Properties panel and data pins on the graph canvas. Both ask the same engine function, `Graph::IsParameterShown` (`Graph/ParameterVisibility.h`), which also hides properties that are not declared parameters (host-fed values such as Working Space's).
- **Dynamic**: Visibility re-evaluates whenever the controlling parameter changes.


---

Back to [docs/](../README.md) • [Repo root](../../README.md)