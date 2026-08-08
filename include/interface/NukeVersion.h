#pragma once
// The engine's RELEASE NAME — the single place it is defined. Everything else derives:
// the About window, the boot banner, the mac bundle plists (the superbuild parses this
// header), and plugins that want to know what they were compiled against.
//
// This is the HUMAN identity of a release. Machine compatibility stays the job of
// NUKE_ENGINE_ABI (NUKEEInteface.h): the loader refuses a module on an ABI mismatch no
// matter what either side's version string says. A plugin comparing its compiled-against
// NUKE_ENGINE_VERSION with the host's runtime nuke::EngineVersion() (Modular.h) can tell
// "same ABI, older release" — useful for feature-gating, never for load gating.
#define NUKE_ENGINE_VERSION "Deuterium-2"
