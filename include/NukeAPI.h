#pragma once
#ifndef NUKE_API_H
#define NUKE_API_H

// Export macro for the NukeEngine DLL.
//
// The engine is built with NUKEENGINE_EXPORTS defined (see NukeEngine.vcxproj) so its
// public symbols are exported; consumers (the editor, gameplay/render plugins, the
// scripting module) see dllimport and link against NukeEngine.lib (the import lib).
//
// Annotate the PUBLIC API surface only:
//   class NUKEENGINE_API World { ... };          // exports all of World's members
//   NUKEENGINE_API void SomeFreeFunction();      // exports a free function
//   extern NUKEENGINE_API int someGlobal;        // exports a global
// Internal/implementation-only types stay unannotated and are not exported.
#ifdef _WIN32
  #ifdef NUKEENGINE_EXPORTS
    #define NUKEENGINE_API __declspec(dllexport)
  #else
    #define NUKEENGINE_API __declspec(dllimport)
  #endif
  // 4251: exported class has a std::/boost:: member without a dll-interface.
  // 4275: exported class derives from a non-dll-interface base.
  // Both are benign here: the engine DLL and all consumers use the same /MDd CRT
  // and the same STL/boost, so the layouts match across the boundary.
  #pragma warning(disable: 4251)
  #pragma warning(disable: 4275)
#else
  #define NUKEENGINE_API
#endif

#endif // NUKE_API_H
