#pragma once
#ifdef SHADERLAB_ENGINE_EXPORTS
#define SHADERLAB_API __declspec(dllexport)
#else
#define SHADERLAB_API __declspec(dllimport)
#endif
