// The engine's single stb_image / stb_dxt implementation (Texture decode + BC encode, Cursor,
// the image importer). Every other TU includes the headers without the IMPLEMENTATION defines.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>
