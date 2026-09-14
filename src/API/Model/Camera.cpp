#include "API/Model/Physics.h"
#include "API/Model/Camera.h"
#include "API/Model/Layers.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "API/Model/Texture.h"
#include "API/Model/Screen.h"   // screen->world rays use game-screen pixel space
#include "API/Model/Time.h"     // shake impulses advance on game time
#include "API/Model/Game.h"     // camera utilities gate on play mode
#include "API/Model/Collider.h" // spring-arm boom ignores the camera's own body
#include "API/Model/World.h"    // blend target resolve
#include "interface/Services.h"
#include "service/iPhysics.h"   // spring-arm boom collision (swept sphere)
#include "interface/AppInstance.h"
#include <cmath>
#include <algorithm>

namespace nuke {

Camera::Camera() : Component("Camera") {}

Camera::Camera(iRender* renderer) : Component("Camera")
{
	this->renderer = renderer;
	this->renderer->transform = transform;
	renderer->width = this->r_width;
	renderer->height = this->r_height;
	renderer->fov = fov;
	renderer->Far = _far;
	renderer->Near = _near;
	// Secondary (render-target) cameras own their renderer and must init it; the main one is
	// held by AppInstance and initialized by the bootstrap.
	if (renderer != AppInstance::GetSingleton()->render)
		{ WindowDesc _wd; _wd.w = r_width; _wd.h = r_height; renderer->init(_wd); }
	else
		cout << "[Camera]\t\t" << "[!] Camera of main renderer" << endl;
}

Camera::Camera(Atom* parent, iRender* renderer) : Component("Camera")
{
	this->renderer = renderer;
	Init(parent);
}

Vector3 Camera::ScreenPosToWorldRay(
	int mouseX, int mouseY,             // pixels, bottom-left window origin
	int screenWidth, int screenHeight,  // window size in pixels
	glm::mat4 ViewMatrix,               // camera position and orientation
	glm::mat4 ProjectionMatrix         // ratio, fov, near/far
	, glm::vec3& out_origin
) {

	// Ray start/end in Normalized Device Coordinates.
	glm::vec4 RayStart_NDC(
		(2.0f * mouseX) / screenWidth - 1.0f,
		1.0f - (2.0f * mouseY) / screenHeight,
		-1.0, // the near plane maps to Z=-1 in NDC
		1.0f
	);
	glm::vec4 RayEnd_NDC(
		(2.0f * mouseX) / screenWidth - 1.0f,
		1.0f - (2.0f * mouseY) / screenHeight,
		0.0,
		1.0f
	);


	glm::mat4 InverseProjectionMatrix = glm::inverse(ProjectionMatrix);

	glm::mat4 InverseViewMatrix = glm::inverse(ViewMatrix);

	glm::vec4 RayStart_camera = InverseProjectionMatrix * RayStart_NDC;
	RayStart_camera /= RayStart_camera.w;
	glm::vec4 RayStart_world = InverseViewMatrix * RayStart_camera;
	RayStart_world /= RayStart_world.w;
	glm::vec4 RayEnd_camera = InverseProjectionMatrix * RayEnd_NDC;
	RayEnd_camera /= RayEnd_camera.w;
	glm::vec4 RayEnd_world = InverseViewMatrix * RayEnd_camera;
	RayEnd_world /= RayEnd_world.w;


	// Faster way (just one inverse)
	//glm::mat4 M = glm::inverse(ProjectionMatrix * ViewMatrix);
	//glm::vec4 RayStart_world = M * RayStart_NDC; RayStart_world/=RayStart_world.w;
	//glm::vec4 RayEnd_world   = M * RayEnd_NDC  ; RayEnd_world  /=RayEnd_world.w;


	glm::vec3 RayDir_world(RayEnd_world - RayStart_world);
	RayDir_world = glm::normalize(RayDir_world);


	out_origin = glm::vec3(RayStart_world);
	auto out = glm::normalize(RayDir_world);
	return { out.x, out.y, out.z };
}

bool  Camera::RayOBBIntersection(
	glm::vec3 ray_origin,        // Ray origin, in world space
	glm::vec3 ray_direction,     // world space, must be normalized (direction, NOT a target position)
	glm::vec3 aabb_min,          // untransformed mesh min X,Y,Z
	glm::vec3 aabb_max,          // untransformed mesh max X,Y,Z
	glm::mat4 ModelMatrix,       // mesh transform (applied to the box too)
	float& intersection_distance // out: distance from ray_origin to the OBB hit
) {

	// Intersection method from Real-Time Rendering and Essential Mathematics for Games

	float tMin = 0.0f;
	float tMax = 100000.0f;

	glm::vec3 OBBposition_worldspace(ModelMatrix[3].x, ModelMatrix[3].y, ModelMatrix[3].z);

	glm::vec3 delta = OBBposition_worldspace - ray_origin;

	// Test intersection with the 2 planes perpendicular to the OBB's X axis
	{
		glm::vec3 xaxis(ModelMatrix[0].x, ModelMatrix[0].y, ModelMatrix[0].z);
		float e = glm::dot(xaxis, delta);
		float f = glm::dot(ray_direction, xaxis);

		if (fabs(f) > 0.001f) { // Standard case

			float t1 = (e + aabb_min.x) / f; // "left" plane
			float t2 = (e + aabb_max.x) / f; // "right" plane

			if (t1 > t2) {
				float w = t1; t1 = t2; t2 = w;
			}

			// tMax is the nearest "far" intersection (amongst the X,Y and Z planes pairs)
			if (t2 < tMax)
				tMax = t2;
			// tMin is the farthest "near" intersection (amongst the X,Y and Z planes pairs)
			if (t1 > tMin)
				tMin = t1;

			// "far" closer than "near" = no intersection
			if (tMax < tMin)
				return false;

		}
		else { // rare case: the ray is almost parallel to the planes
			if (-e + aabb_min.x > 0.0f || -e + aabb_max.x < 0.0f)
				return false;
		}
	}


	// Test intersection with the 2 planes perpendicular to the OBB's Y axis
	{
		glm::vec3 yaxis(ModelMatrix[1].x, ModelMatrix[1].y, ModelMatrix[1].z);
		float e = glm::dot(yaxis, delta);
		float f = glm::dot(ray_direction, yaxis);

		if (fabs(f) > 0.001f) {

			float t1 = (e + aabb_min.y) / f;
			float t2 = (e + aabb_max.y) / f;

			if (t1 > t2) { float w = t1; t1 = t2; t2 = w; }

			if (t2 < tMax)
				tMax = t2;
			if (t1 > tMin)
				tMin = t1;
			if (tMin > tMax)
				return false;

		}
		else {
			if (-e + aabb_min.y > 0.0f || -e + aabb_max.y < 0.0f)
				return false;
		}
	}


	// Test intersection with the 2 planes perpendicular to the OBB's Z axis
	{
		glm::vec3 zaxis(ModelMatrix[2].x, ModelMatrix[2].y, ModelMatrix[2].z);
		float e = glm::dot(zaxis, delta);
		float f = glm::dot(ray_direction, zaxis);

		if (fabs(f) > 0.001f) {

			float t1 = (e + aabb_min.z) / f;
			float t2 = (e + aabb_max.z) / f;

			if (t1 > t2) { float w = t1; t1 = t2; t2 = w; }

			if (t2 < tMax)
				tMax = t2;
			if (t1 > tMin)
				tMin = t1;
			if (tMin > tMax)
				return false;

		}
		else {
			if (-e + aabb_min.z > 0.0f || -e + aabb_max.z < 0.0f)
				return false;
		}
	}

	intersection_distance = tMin;
	return true;

}

void Camera::ProcessKeyboard() {
	if (!freeMode)
		return;

	if (KeyBoard::getSingleton()->getKeyPressed('w'))
		transform->position += transform->direction() * 3;
	if (KeyBoard::getSingleton()->getKeyPressed('a'))
		transform->position += transform->right() * -3;
	if (KeyBoard::getSingleton()->getKeyPressed('s'))
		transform->position += transform->direction() * -3;
	if (KeyBoard::getSingleton()->getKeyPressed('d'))
		transform->position += transform->right() * 3;

	//cout << "CAM MOV [ " << transform->position.toStringA() << " ]" << endl;
}


void Camera::Init(Atom* parent)
{
	if (!renderer) renderer = AppInstance::GetSingleton()->render;   // e.g. cameras loaded from a scene
	transform = &parent->GetTransform();
	atom = parent;   // the back-reference keys the renderer's per-camera state (NukeCameraDesc::cameraId): unset, every camera was id 0
	if (this->renderer)
		this->renderer->transform = transform;
	parent->components.push_back(this);
	if (renderer != AppInstance::GetSingleton()->render)
		{ WindowDesc _wd; _wd.w = r_width; _wd.h = r_height; renderer->init(_wd); }
	else
		cout << "[Camera]\t\t" << "[!] Camera of main renderer" << endl;
	//*KeyBoard::getSingleton() += bst::function<void(unsigned char, int, int)>(b::bind(&Camera::ProcessKeyboard, bst::ref(*this), _1, _2, _3));
//        *Mouse::getSingleton() += bst::function<void(int, int, int, int)>(b::bind(&Camera::ProcessMouse, bst::ref(*this), _1, _2, _3, _4));
//        *Mouse::getSingleton() &= bst::function<void(int, int)>(b::bind(&Camera::ProcessMouseMove, bst::ref(*this), _1, _2));
//        *Mouse::getSingleton() *= bst::function<void(int, int, int, int)>(b::bind(&Camera::mouseScroll, bst::ref(*this), _1, _2, _3, _4));
}
void Camera::FixedUpdate() {}
void Camera::Update() {
	//		renderer->width = this->r_width;
	//		renderer->height = this->r_height;
	renderer->fov = fov;
	renderer->Far = _far;
	renderer->Near = _near;
	//renderer->_crosshair = crosshair;


	// TODO: It crashes app on MacOS. Fix it.
#ifndef __APPLE__
	renderer->update();
#endif
	ProcessKeyboard();

	// Look-at constraint (opt-in): face the target, optionally smoothed.
	if (lookAtTarget && transform && Game::IsPlaying())
	{
		Vector3 p = transform->globalPosition();
		Vector3 to = lookAtTarget->GetTransform().globalPosition() - p;
		const double len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
		if (len > 1e-4)
		{
			Quaternion want = Quaternion::LookRotation(Vector3(to.x / len, to.y / len, to.z / len));
			Quaternion cur = transform->globalRotation();
			const double a = lookLag > 0.0f
			               ? 1.0 - std::pow((double)lookLag, Time::getSingleton()->gameDelta * 60.0)
			               : 1.0;
			transform->SetGlobal(p, Quaternion::Slerp(cur, want, a), transform->globalScale());
		}
	}
}

// ---- camera utilities (spring arm / blend) ------------------------------------------------

void Camera::BlendTo(Atom* targetCamera, double seconds, double easing)
{
	if (!targetCamera) return;
	blendTarget = targetCamera->id.id;
	blendDur = std::max(0.01, seconds);
	blendT = 0.0;
	blendEase = (int)easing;
	blendActive = true;
}

bool Camera::Blending() { return blendActive; }
Vector3 Camera::ViewPos() { return viewValid ? viewPos : (transform ? transform->globalPosition() : Vector3(0, 0, 0)); }
Vector3 Camera::ViewDir() { return viewValid ? viewFwd : (transform ? transform->direction() : Vector3(0, 0, 1)); }

static Vector3 VLerp(Vector3 a, Vector3 b, double t)
{ return Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }

static Vector3 VNorm(Vector3 v)
{
	const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return l > 1e-12 ? Vector3(v.x / l, v.y / l, v.z / l) : Vector3(0, 0, 1);
}

void Camera::ComposeView(Vector3& pos, Vector3& fwd, Vector3& up)
{
	pos = transform->globalPosition();
	fwd = transform->direction();
	up = transform->up();
	const double dt = Time::getSingleton()->gameDelta;

	if (rig == 1 && boomLength > 1e-4f)
	{
		double reach = boomLength;
		if (boomCollision)
		{
			// Swept SPHERE, not a ray: a thin ray slips past anything a boomRadius-wide camera
			// would clip through.
			iPhysics* ph = Physics::Scene();
			Collider* own = atom ? atom->GetComponent<Collider>() : nullptr;
			if (ph)
			{
				const float from[3] = { (float)pos.x, (float)pos.y, (float)pos.z };
				const float back[3] = { (float)-fwd.x, (float)-fwd.y, (float)-fwd.z };
				float dist = 0.0f;
				if (ph->sphereCastDist(boomRadius, from, back, boomLength,
				                       own ? own->bodyId : 0, dist))
					reach = std::max(0.0, (double)dist - 0.01);
			}
		}
		Vector3 want(pos.x - fwd.x * reach, pos.y - fwd.y * reach, pos.z - fwd.z * reach);
		if (boomLag > 0.0f && boomInit)
			boomEye = VLerp(boomEye, want, 1.0 - std::pow((double)boomLag, dt * 60.0));
		else
			boomEye = want;
		boomInit = true;
		pos = boomEye;
	}

	if (blendActive)
	{
		World* w = Game::GetWorld();
		Atom* ta = w ? w->GetById(blendTarget) : nullptr;
		Camera* tc = ta ? ta->GetComponent<Camera>() : nullptr;
		if (!tc || !tc->transform)
			blendActive = false;
		else
		{
			blendT += dt / blendDur;
			double t = std::min(1.0, blendT);
			switch (blendEase)
			{
				case 1: t = t * t * (3.0 - 2.0 * t); break;   // smoothstep
				case 2: t = t * t; break;                     // ease-in
				case 3: t = 1.0 - (1.0 - t) * (1.0 - t); break;   // ease-out
			}
			pos = VLerp(pos, tc->transform->globalPosition(), t);
			fwd = VNorm(VLerp(fwd, tc->transform->direction(), t));
			up = VNorm(VLerp(up, tc->transform->up(), t));
			if (blendT >= 1.0)
			{
				blendActive = false;
				mainCamera = false;
				tc->mainCamera = true;   // the hand-off: the target renders from here on
			}
		}
	}

	viewPos = pos; viewFwd = fwd; viewUp = up;
	viewValid = true;
}

void Camera::SetProjection(Projection p) { projection = p; }   // World::Render eases projBlend toward it
Projection Camera::GetProjection()       { return projection; }
void   Camera::SetOrthoSize(double size) { orthoSize = (float)size; }
double Camera::GetOrthoSize()            { return orthoSize; }
void   Camera::SetLayerMask(double mask) { layerMask = (int)(long long)mask; }
double Camera::GetLayerMask()            { return (double)(unsigned int)layerMask; }

// --- screen -> world -------------------------------------------------------------------------
// Must mirror the renderer's projection (incl. the live persp<->ortho blend).

Vector3 Camera::ScreenRayOrigin(double px, double py)
{
	if (!transform) return Vector3(0, 0, 0);
	const double w = std::max(1.0, Screen::Width()), h = std::max(1.0, Screen::Height());
	const double ndcx = px / w * 2.0 - 1.0;
	const double ndcy = 1.0 - py / h * 2.0;   // top-left pixel origin -> +y up NDC
	// Rays must leave the RENDERED eye (boom/blend applied), or picking skews off-screen.
	Vector3 p = viewValid ? viewPos : transform->globalPosition();
	if (projBlend >= 0.5f)   // orthographic: parallel rays, origin slides over the view rect
	{
		const double oh = (orthoSize > 1e-4f) ? orthoSize : 1.0;
		const double ow = oh * (w / h);
		Vector3 f0 = viewValid ? viewFwd : transform->direction();
		Vector3 u = viewValid ? viewUp : transform->up();
		Vector3 r(f0.y * u.z - f0.z * u.y, f0.z * u.x - f0.x * u.z, f0.x * u.y - f0.y * u.x);
		return Vector3(p.x + ndcx * ow * r.x + ndcy * oh * u.x,
		               p.y + ndcx * ow * r.y + ndcy * oh * u.y,
		               p.z + ndcx * ow * r.z + ndcy * oh * u.z);
	}
	return p;
}

Vector3 Camera::ScreenRayDir(double px, double py)
{
	if (!transform) return Vector3(0, 0, 1);
	Vector3 f = viewValid ? viewFwd : transform->direction();
	if (projBlend >= 0.5f) return f;   // orthographic: fixed direction
	const double w = std::max(1.0, Screen::Width()), h = std::max(1.0, Screen::Height());
	const double ndcx = px / w * 2.0 - 1.0;
	const double ndcy = 1.0 - py / h * 2.0;
	const double thf = std::tan((double)fov * 0.5 * 0.017453292519943295);
	Vector3 u = viewValid ? viewUp : transform->up();
	Vector3 r(f.y * u.z - f.z * u.y, f.z * u.x - f.x * u.z, f.x * u.y - f.y * u.x);
	Vector3 d(f.x + ndcx * thf * (w / h) * r.x + ndcy * thf * u.x,
	          f.y + ndcx * thf * (w / h) * r.y + ndcy * thf * u.y,
	          f.z + ndcx * thf * (w / h) * r.z + ndcy * thf * u.z);
	const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len > 1e-12) { d.x /= len; d.y /= len; d.z /= len; }
	return d;
}

Vector3 Camera::ScreenToWorldPoint(double px, double py, double depth)
{
	Vector3 o = ScreenRayOrigin(px, py), d = ScreenRayDir(px, py);
	return Vector3(o.x + d.x * depth, o.y + d.y * depth, o.z + d.z * depth);
}

void Camera::AddShake(double amplitude, double frequency, double duration)
{
	if (amplitude <= 0.0 || duration <= 0.0) return;
	ShakeImp s;
	s.amp  = (float)amplitude;
	s.freq = (frequency > 0.0) ? (float)frequency : 12.0f;
	s.dur  = (float)duration;
	s.t    = 0.0f;
	s.seed = (float)((shakes.size() * 37 + 11) % 97);   // decorrelate stacked impulses
	shakes.push_back(s);
}

void Camera::ShakeOffset(float out[3])
{
	out[0] = out[1] = out[2] = 0.0f;
	if (shakes.empty()) return;
	const float dt = (float)Time::getSingleton()->gameDelta;   // pauses with the game
	for (size_t i = 0; i < shakes.size(); )
	{
		ShakeImp& s = shakes[i];
		s.t += dt;
		if (s.t >= s.dur) { shakes.erase(shakes.begin() + i); continue; }
		const float fade = 1.0f - s.t / s.dur;                 // linear decay to zero
		const float w = s.t * s.freq * 6.2831853f;
		out[0] += s.amp * fade * sinf(w + s.seed);
		out[1] += s.amp * fade * sinf(w * 1.31f + s.seed * 2.17f);
		out[2] += s.amp * fade * 0.25f * sinf(w * 0.77f + s.seed * 3.03f);
		++i;
	}
}

void Camera::Reset() {}
void Camera::Pause() {}

void Camera::Destroy()
{
	// NEVER deinit the MAIN renderer: only a camera-OWNED secondary renderer is ours.
	if (renderer && renderer != AppInstance::GetSingleton()->render)
		renderer->deinit();
	renderer = nullptr;
}

}  // namespace nuke