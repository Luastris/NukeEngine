#include "API/Model/Texture.h"
#include <memory>

namespace nuke {

Texture::Texture(char* path) {
	strcpy(this->path, path);

}

Texture::Texture() {}
}  // namespace nuke