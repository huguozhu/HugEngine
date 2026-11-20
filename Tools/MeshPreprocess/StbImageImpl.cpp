// ============================================================
//  StbImageImpl.cpp —— stb_image 的唯一实现单元（T8 纹理回退）
//
//  【为什么单独一个 TU】stb_image 是 header-only，`STB_IMAGE_IMPLEMENTATION` 只能定义一次；
//  放在 main.cpp 里会让"只 include 头"的其它单元与它重复定义。这里只负责实现，
//  不导出任何接口。
//
//  【用途】预处理工具读 glTF 的 baseColor 贴图、算平均色，烘进材质记录的
//  `baseColorFactor`：光栅端暂时无法绑 bindless 纹理（写描述符数组会让驱动崩），
//  至少让 Nanite 接管档不再是纯白模。
// ============================================================

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
