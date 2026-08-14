// PhysicalSky.frag.slang — Preetham 解析大气散射天空
// 输入 worldDir（Skybox.vert 反算），输出线性 HDR 天空辐射度

static const float SKY_PI = 3.14159265359;

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldDir;
};

[[vk::push_constant]] cbuffer PhysicalSkyPC {
    float4x4 invViewProjMatrix;  // [0..64]（VS 使用，与 Skybox.vert 对齐）
    float    intensity;          // [64..68]
    float3   sunDirection;       // [68..80]
    float    turbidity;          // [80..84]
    float    groundAlbedo;       // [84..88]
    float    sunIntensity;       // [88..92]
    float    _pad;               // [92..96]
};

// ── Perez 天空分布函数 ──
// A/B/C/D/E 为系数，cosTheta=视线天顶角余弦，gamma=视线与太阳夹角
float Perez(float A, float B, float C, float D, float E, float cosTheta, float gamma, float cosGamma) {
    float t = 1.0 + A * exp(B / max(cosTheta, 0.0001));
    return t * (1.0 + C * exp(D * gamma) + E * cosGamma * cosGamma);
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float3 dir = normalize(input.worldDir);
    float3 sun = normalize(sunDirection);

    // 仅渲染上半球；dir.y<=0 为地面，输出简化地面色（SkyboxPass 深度 EQUAL 只填无几何处）
    if (dir.y <= 0.0) {
        return float4(float3(groundAlbedo) * 0.5 * intensity, 1.0);
    }

    float cosTheta = clamp(dir.y, 0.0, 1.0);         // 视线天顶角余弦
    float thetaSun = acos(clamp(sun.y, 0.0, 1.0));   // 太阳天顶角
    float cosSun   = clamp(sun.y, 0.0, 1.0);
    float cosGamma = clamp(dot(dir, sun), -1.0, 1.0);
    float gamma    = acos(cosGamma);

    float T = turbidity;
    float T2 = T * T;

    // ── 天顶 xyY（Preetham 系数拟合）──
    float chi  = (4.0 / 9.0 - T / 120.0) * SKY_PI;
    float chi2 = chi * chi, chi3 = chi2 * chi;

    float Yz = (4.0453 * T - 4.9710) * tan(chi) - 0.2155 * T + 2.4192;

    float xz = T2 * (0.00166 * chi3 - 0.00375 * chi2 + 0.00209 * chi)
             + T  * (-0.02903 * chi3 + 0.06377 * chi2 - 0.03202 * chi + 0.00394)
             + (0.11693 * chi3 - 0.21196 * chi2 + 0.06052 * chi + 0.25886);
    float yz = T2 * (0.00275 * chi3 - 0.00610 * chi2 + 0.00317 * chi)
             + T  * (-0.04214 * chi3 + 0.08970 * chi2 - 0.04153 * chi + 0.00516)
             + (0.15346 * chi3 - 0.26756 * chi2 + 0.06670 * chi + 0.26688);

    // ── 亮度（luminance）分布系数 ──
    float Al =  0.1787 * T - 1.4630;
    float Bl = -0.3554 * T + 0.4275;
    float Cl = -0.0227 * T + 5.3251;
    float Dl =  0.1206 * T - 2.5771;
    float El = -0.0670 * T + 0.3703;

    // ── x 色度分布系数 ──
    float Ax = -0.0193 * T - 0.2592, Bx = -0.0665 * T + 0.0008,
          Cx = -0.0004 * T + 0.2125, Dx = -0.0641 * T - 0.8989, Ex = -0.0033 * T + 0.0452;
    // ── y 色度分布系数 ──
    float Ayc = -0.0167 * T - 0.2608, Byc = -0.0950 * T + 0.0092,
          Cyc = -0.0079 * T + 0.2102, Dyc = -0.0441 * T - 1.6537, Eyc = -0.0109 * T + 0.0529;

    // ── 天顶处 Perez 分布（分母归一化）──
    float fYz = Perez(Al, Bl, Cl, Dl, El, 1.0, thetaSun, cosSun);
    float fxz = Perez(Ax, Bx, Cx, Dx, Ex, 1.0, thetaSun, cosSun);
    float fyz = Perez(Ayc, Byc, Cyc, Dyc, Eyc, 1.0, thetaSun, cosSun);

    // ── 视线方向 Perez 分布 ──
    float fY = Perez(Al, Bl, Cl, Dl, El, cosTheta, gamma, cosGamma);
    float fx = Perez(Ax, Bx, Cx, Dx, Ex, cosTheta, gamma, cosGamma);
    float fy = Perez(Ayc, Byc, Cyc, Dyc, Eyc, cosTheta, gamma, cosGamma);

    // ── 视线方向 xyY ──
    float Y = Yz * fY / fYz;
    float x = xz * fx / fxz;
    float y = yz * fy / fyz;

    // ── xyY → XYZ → 线性 RGB ──
    float X = (x * Y) / max(y, 0.0001);
    float Z = ((1.0 - x - y) * Y) / max(y, 0.0001);

    // Rec.709 色彩匹配矩阵（XYZ → linear RGB）
    float3 linearRGB;
    linearRGB.r =  3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    linearRGB.g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    linearRGB.b =  0.0557 * X - 0.2040 * Y + 1.0570 * Z;

    linearRGB *= intensity;

    // ── 太阳盘：视线接近太阳方向时叠加（~2° 视直径）──
    float sunDot  = dot(dir, sun);
    float sunDisc = smoothstep(0.99935, 0.99990, sunDot);
    linearRGB += float3(1.0, 0.95, 0.85) * sunDisc * sunIntensity * 2.0;

    return float4(linearRGB, 1.0);
}
