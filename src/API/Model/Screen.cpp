#include "API/Model/Screen.h"

namespace nuke {

static int g_w = 0, g_h = 0;

void   Screen::Set(int w, int h) { if (w > 0 && h > 0) { g_w = w; g_h = h; } }
double Screen::Width()  { return (double)g_w; }
double Screen::Height() { return (double)g_h; }
double Screen::Aspect() { return g_h > 0 ? (double)g_w / (double)g_h : 0.0; }

}  // namespace nuke
