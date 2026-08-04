#include "API/Model/BlendSpace.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace nuke {

namespace bfs = boost::filesystem;

void BlendSpace::Weights(float x, float y, std::vector<float>& out) const
{
	out.assign(points.size(), 0.0f);
	if (points.empty()) return;
	if (points.size() == 1) { out[0] = 1.0f; return; }

	if (dims <= 1)
	{
		// Sorted-neighbour segment lerp on x (clamped at the ends).
		std::vector<int> order(points.size());
		for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
		std::sort(order.begin(), order.end(), [&](int a, int b) { return points[a].x < points[b].x; });
		if (x <= points[order.front()].x) { out[order.front()] = 1.0f; return; }
		if (x >= points[order.back()].x)  { out[order.back()]  = 1.0f; return; }
		for (size_t i = 1; i < order.size(); ++i)
		{
			const float a = points[order[i - 1]].x, b = points[order[i]].x;
			if (x <= b)
			{
				const float f = (b > a) ? (x - a) / (b - a) : 0.0f;
				out[order[i - 1]] = 1.0f - f;
				out[order[i]]     = f;
				return;
			}
		}
		out[order.back()] = 1.0f;
		return;
	}

	// 2D gradient-band interpolation: per point i, the influence is the minimum over every
	// other point j of 1 - clamp(dot(p - pi, pj - pi) / |pj - pi|^2), then normalized.
	float total = 0.0f;
	for (size_t i = 0; i < points.size(); ++i)
	{
		float infl = 1.0f;
		for (size_t j = 0; j < points.size(); ++j)
		{
			if (j == i) continue;
			const float ex = points[j].x - points[i].x;
			const float ey = points[j].y - points[i].y;
			const float len2 = ex * ex + ey * ey;
			if (len2 < 1e-12f) continue;
			const float h = 1.0f - ((x - points[i].x) * ex + (y - points[i].y) * ey) / len2;
			infl = std::min(infl, h);
		}
		out[i] = std::max(infl, 0.0f);
		total += out[i];
	}
	if (total > 1e-8f)
		for (float& w : out) w /= total;
	else
	{
		// Outside every band: nearest point wins.
		size_t best = 0;
		float bd = 1e30f;
		for (size_t i = 0; i < points.size(); ++i)
		{
			const float dx = x - points[i].x, dy = y - points[i].y;
			const float d = dx * dx + dy * dy;
			if (d < bd) { bd = d; best = i; }
		}
		out[best] = 1.0f;
	}
}

std::string BlendSpace::ToString() const
{
	nlohmann::json j;
	j["type"] = "BlendSpace";
	j["guid"] = guid;
	j["name"] = name;
	j["dims"] = dims;
	j["paramX"] = paramX;
	j["paramY"] = paramY;
	nlohmann::json pts = nlohmann::json::array();
	for (const Point& p : points)
	{
		nlohmann::json pj = { { "clip", p.clip }, { "x", p.x }, { "speed", p.speed } };
		if (dims >= 2)  pj["y"] = p.y;
		if (p.mirror)   pj["mirror"] = true;
		pts.push_back(pj);
	}
	j["points"] = pts;
	return j.dump(1);
}

BlendSpace* BlendSpace::FromString(const std::string& json)
{
	try
	{
		nlohmann::json j = nlohmann::json::parse(json);
		BlendSpace* b = new BlendSpace();
		b->guid   = j.value("guid", "");
		b->name   = j.value("name", "");
		b->dims   = j.value("dims", 1);
		b->paramX = j.value("paramX", "");
		b->paramY = j.value("paramY", "");
		if (j.contains("points"))
			for (const auto& pj : j["points"])
			{
				Point p;
				p.clip   = pj.value("clip", "");
				p.x      = pj.value("x", 0.0f);
				p.y      = pj.value("y", 0.0f);
				p.speed  = pj.value("speed", 1.0f);
				p.mirror = pj.value("mirror", false);
				b->points.push_back(p);
			}
		return b;
	}
	catch (const std::exception&) { return nullptr; }
}

bool BlendSpace::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	const std::string s = ToString();
	o.write(s.data(), (std::streamsize)s.size());
	return (bool)o;
}

BlendSpace* BlendSpace::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	std::stringstream ss;
	ss << i.rdbuf();
	return FromString(ss.str());
}

BlendSpace* BlendSpace::LoadFromMemory(const std::string& data) { return FromString(data); }

}  // namespace nuke
