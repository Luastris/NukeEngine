#pragma once
#ifndef NUKEE_TEXTURE_H
#define NUKEE_TEXTURE_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

class NUKEENGINE_API Texture
{
public:
    unsigned int id = 0;
    char name[64] = { 0 };
    char path[1024] = { 0 };

    std::string guid;                      // asset id
    int width = 0, height = 0;             // decoded RGBA8 dimensions
    std::vector<unsigned char> pixels;     // width*height*4, RGBA8

	Texture(char* path);
	Texture();

    // Native asset format (.nutex, binary): header + decoded RGBA8 pixels. Import decodes
    // source images (PNG/JPG/... via stb_image, incl. embedded) into this so nothing
    // references the source at runtime.
    bool            SaveToFile(const std::string& path) const;
    static Texture* LoadFromFile(const std::string& path);
};
}  // namespace nuke

#endif // !NUKEE_TEXTURE_H
