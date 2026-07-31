#pragma once
#ifndef NUKE_API_H
#define NUKE_API_H

// Export macro for the NukeEngine DLL: the engine builds with NUKEENGINE_EXPORTS defined, so
// consumers see dllimport and link against NukeEngine.lib. Annotate the PUBLIC API surface only;
// implementation-only types stay unannotated.
#ifdef _WIN32
  #ifdef NUKEENGINE_EXPORTS
    #define NUKEENGINE_API __declspec(dllexport)
  #else
    #define NUKEENGINE_API __declspec(dllimport)
  #endif
  // 4251/4275 (std::/boost:: member or base without a dll-interface): benign only because the
  // engine DLL and every consumer share one CRT and one STL/boost, so layouts match.
  #pragma warning(disable: 4251)
  #pragma warning(disable: 4275)
#else
  #define NUKEENGINE_API
#endif

#endif // NUKE_API_H
