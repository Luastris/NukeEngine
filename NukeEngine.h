#ifndef NUKEENGINE_H
#define NUKEENGINE_H
#include <cmath>
#include <algorithm>
#include <iostream>
#include <boost/function.hpp>
#include <boost/container/list.hpp>
#include <string.h>

namespace bst = boost;
namespace bc = bst::container;

#include "NukeAPI.h"   // single source of truth for NUKEENGINE_API (keyed on NUKEENGINE_EXPORTS)

#ifdef _WINDOWS
#define strdup _strdup
#endif

// This class is exported from the dll
class NUKEENGINE_API CNukeEngine {
public:
	CNukeEngine(void);
	// TODO: add your methods here.
};

extern NUKEENGINE_API int nNukeEngine;

NUKEENGINE_API int fnNukeEngine(void);
#endif