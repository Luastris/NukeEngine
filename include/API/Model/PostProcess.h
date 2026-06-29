#pragma once
#ifndef NUKEE_POSTPROCESS_H
#define NUKEE_POSTPROCESS_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"
#include <vector>
#include <map>
#include <array>
#include <string>

namespace nuke {

// One entry in a camera's post-process chain: a custom post-shader asset + its parameter overrides.
// `props` mirrors a material instance's prop map (name -> up to 4 floats), keyed by the shader's PostParams.
struct PostEffect
{
	std::string shaderGuid;     // post-shader asset (a "<name>.post.hlsl")
	bool        enabled = true;
	std::map<std::string, std::array<float, 4>> props;
};

// Per-camera post-process: an ordered chain of custom post-shader effects (NOT hardcoded). Lives as a sibling
// Component of Camera on the same atom; World::Render runs only the chain of the camera on that atom. The
// effect list is stored on disk in the hidden `effectsData` JSON field; `effects` is the runtime form.
class NUKEENGINE_API PostProcess : public Component
{
	NUKE_CLASS(PostProcess, Component)
public:
	[[nuke::prop(hidden)]] std::string effectsData;   // serialized chain (JSON); edited via the custom inspector

	std::vector<PostEffect> effects;   // runtime chain (parsed from effectsData)

	PostProcess();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	void EnsureParsed();   // parse effectsData -> effects when it changed (load / external edit)
	void Commit();         // serialize effects -> effectsData (after an inspector edit)

private:
	std::string parsedFrom;   // effectsData value `effects` was last parsed from
};
}  // namespace nuke

#endif // !NUKEE_POSTPROCESS_H
