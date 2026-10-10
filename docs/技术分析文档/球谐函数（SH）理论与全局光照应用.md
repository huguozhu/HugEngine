# 球谐函数（SH）理论与全局光照应用

> **本文定位**：把 GI 计算里用到的球谐（Spherical Harmonics, SH）从头讲清楚——**为什么会出现 SH、
> 基函数怎么来的、公式怎么推、工程里怎么落地、容易踩哪些坑**。属于数学与实现原理教程，
> 不涉及某个具体 pass 的接线（那部分见 `docs/HugEngine引擎介绍/07.全局光照GI本质、实现与架构优化.md`）。
>
> **本文的公式与工程常数都对着当前源码核对过**：`Y_00 = 0.28209479177387814`、
> `Y_1 = 0.4886025119029199`、Lambert 卷积 `A_0 = π`、`A_1 = 2π/3`、白炉解析值 `√π` 等。
> 生成日期：2026-10-09　HugEngine 渲染引擎

---

## 一、为什么 GI 里会出现 SH

### 1.1 问题的来源：渲染方程里的方向积分

漫反射的渲染方程是**球面上的积分**：

$$L_o(x, \omega_o) = \frac{\rho(x)}{\pi}\int_{\Omega} L_i(x, \omega)\,(n\cdot\omega)\,\mathrm d\omega$$

关键困难在于被积函数不只是"数"，而是 **$L_i(x,\omega)$：一个定义在球面上的函数**——它对每个入射方向
$\omega$ 有一个值。要算这个积分，理论上需要知道入射光在整个半球上的分布。

三类做法：

| 做法 | 代表 | 代价 |
|---|---|---|
| 逐方向采样（把积分当蒙特卡洛求和） | 路径追踪、RTGI | 每个着色点几十~几千条射线 |
| **把方向函数压成少数几个系数** | **SH（本文）** | 一次点积（几个乘加），与采样数无关 |
| 预烘成与方向无关的标量 | 纯环境光常数项 | 丢失方向性 |

SH 属于中间那一类：**用几个实数近似一整个球面函数**，让"方向上的分布"变成"每组几个系数"。

### 1.2 降维的直觉：用"球面的傅里叶"近似

一维周期函数可以用正弦/余弦（傅里叶级数）展开；**球面**上对应的正交基就是 **球谐函数**。
只保留低频几项，就得到"模糊但便宜"的球面函数近似——而 GI 恰好天然低频（漫反射是强的低通滤波），
这就是 SH 在 GI 里成立的根本原因。

### 1.3 工程里的三种典型用法

| 用法 | 工程实例 |
|---|---|
| **探针存入射光分布** | DDGI：每探针 4 系数，沿 32 条 Fibonacci 方向取入射辐射度后投影（`GI/DDGI.comp.slang`） |
| **屏幕探针的 SH 投影** | Lumen：屏幕探针沿 SDF 追踪后投影为 4 系数（`Lumen/Lumen_ScreenProbe_SHProject.comp.slang`） |
| **环境光辐照度的解析近似** | Ramamoorthi & Hanrahan 2001 的经典结论（工程目前是"改进方向"，见 07 §2.2 的 SH-9） |

---

## 二、球面函数的数学准备

### 2.1 球坐标与立体角

方向用单位向量 $\omega = (x,y,z)$，或球坐标 $(\theta,\phi)$：

$$x = \sin\theta\cos\phi,\qquad y = \sin\theta\sin\phi,\qquad z = \cos\theta$$

（本文采用 **z 为极轴**的右手系；工程里 z 是"上"，见 §3.6 的轴序说明。）

立体角元：

$$\mathrm d\omega = \sin\theta\,\mathrm d\theta\,\mathrm d\phi$$

整个球面的立体角是 $\int_0^{2\pi}\!\!\int_0^{\pi}\sin\theta\,\mathrm d\theta\,\mathrm d\phi = 4\pi$；
半球是 $2\pi$。**这个 $4\pi$ / $2\pi$ 后面会反复出现**（蒙特卡洛的 pdf、白炉的解析值都来自它）。

### 2.2 球面上的内积与正交基

在球面上定义内积（把函数当向量）：

$$\langle f, g\rangle = \int_{\Omega} f(\omega)\,g(\omega)\,\mathrm d\omega$$

一组基 $\{Y_i\}$ 若满足 $\langle Y_i, Y_j\rangle = \delta_{ij}$ 就是**正交归一基**。
于是一个球面函数可以像向量一样展开：

$$f(\omega) = \sum_i c_i Y_i(\omega),\qquad c_i = \langle f, Y_i\rangle = \int_\Omega f(\omega)Y_i(\omega)\,\mathrm d\omega$$

**这两条式子是本文后面一切的骨架**：第一式是"重建（合成）"，第二式是"投影（分析）"。

### 2.3 为什么基函数是勒让德多项式

我们希望基函数能分离变量：$Y(\theta,\phi) = \Theta(\theta)\Phi(\phi)$，并让 $\Phi$ 是 $\phi$ 的周期函数。
把拉普拉斯方程在球坐标下分离变量，$\Theta$ 满足的方程解就是**连带勒让德函数** $P_l^m(\cos\theta)$。

- $l$ 称**带（band）/ 阶**，$m$ 称**次数**，取值 $l \ge 0$、$-l \le m \le l$；
- 前几个勒让德多项式：$P_0(t)=1$、$P_1(t)=t$、$P_2(t)=\frac{3t^2-1}{2}$、$P_3(t)=\frac{5t^3-3t}{2}$；
- 连带勒让德函数 $P_l^m(t) = (-1)^m(1-t^2)^{m/2}\dfrac{\mathrm d^m}{\mathrm dt^m}P_l(t)$（含 Condon–Shortley 相位）。

> 工程实践里**几乎不需要手算连带勒让德函数**：用到的 $l\le2$ 的显式形式在 §3.3 直接给出。

---

## 三、球谐基函数的定义

### 3.1 复数形式

$$Y_l^m(\theta,\phi) = K_l^m\,P_l^m(\cos\theta)\,e^{\mathrm i m\phi},\qquad
K_l^m = \sqrt{\frac{2l+1}{4\pi}\frac{(l-m)!}{(l+m)!}}$$

它天然正交归一：$\int Y_l^m \overline{Y_{l'}^{m'}}\,\mathrm d\omega = \delta_{ll'}\delta_{mm'}$。
但渲染里我们处理的是实值函数，复基用起来别扭（系数成对共轭），所以工程一律用**实数形式**。

### 3.2 实数形式（工程用这个）

把 $e^{\mathrm im\phi}$ 的实部/虚部拆开并吸收归一化因子，得到实球谐：

$$Y_l^{m} =
\begin{cases}
\sqrt{2}\,K_l^{|m|}\,P_l^{|m|}(\cos\theta)\cos(|m|\phi) & m > 0\\[2pt]
K_l^{0}\,P_l^{0}(\cos\theta) & m = 0\\[2pt]
\sqrt{2}\,K_l^{|m|}\,P_l^{|m|}(\cos\theta)\sin(|m|\phi) & m < 0
\end{cases}$$

它们同样正交归一，且都是**实值**。$l$ 固定时共有 $2l+1$ 个基函数；到 $l=L$ 为止共 $(L+1)^2$ 个。

| 最大带 $L$ | 系数个数 $(L+1)^2$ | 常见叫法 |
|---|---|---|
| 0 | 1 | 常数环境光 |
| **1** | **4** | **二阶 4 系数**（工程用；DDGI / Lumen 探针） |
| 2 | 9 | "SH-9"（经典环境光辐照度近似） |
| 3 | 16 | 少量反射类用途 |

> 注意命名混乱：带 $l$ 从 0 开始数，所以"$l\le1$（4 个系数）"常被叫**二阶**、"$l\le2$（9 个系数）"叫**三阶**。
> 本文一律写清楚"$l\le$ 几 / 几个系数"。

### 3.3 $l\le2$ 的显式基函数（工程范围内）

用 $(x,y,z)$ 直接写（$x=\sin\theta\cos\phi$、$y=\sin\theta\sin\phi$、$z=\cos\theta$）：

| $i$ | $l$ | $m$ | 实数基函数 | 数值常数 |
|---|---|---|---|---|
| 0 | 0 | 0 | $Y_{00} = \frac{1}{2}\sqrt{\frac{1}{\pi}}$ | 0.2820947918 |
| 1 | 1 | −1 | $Y_{1,-1} = \sqrt{\frac{3}{4\pi}}\,y$ | 0.4886025119 |
| 2 | 1 | 0 | $Y_{1,0} = \sqrt{\frac{3}{4\pi}}\,z$ | 0.4886025119 |
| 3 | 1 | 1 | $Y_{1,1} = \sqrt{\frac{3}{4\pi}}\,x$ | 0.4886025119 |
| 4 | 2 | −2 | $Y_{2,-2} = \sqrt{\frac{15}{4\pi}}\,xy$ | 1.0925484306 |
| 5 | 2 | −1 | $Y_{2,-1} = \sqrt{\frac{15}{4\pi}}\,yz$ | 1.0925484306 |
| 6 | 2 | 0 | $Y_{2,0} = \sqrt{\frac{5}{16\pi}}\,(3z^2-1)$ | 0.3153915653 |
| 7 | 2 | 1 | $Y_{2,1} = \sqrt{\frac{15}{4\pi}}\,xz$ | 1.0925484306 |
| 8 | 2 | 2 | $Y_{2,2} = \sqrt{\frac{15}{16\pi}}\,(x^2-y^2)$ | 0.5462742153 |

**怎么直观理解这些基函数**
**工程对照**：这两组常数在代码里就是上面两行。DDGI 的投影端（`GI/DDGI.comp.slang:59-77`）
与 Lumen 的共享库（`Lumen/ScreenProbeSampling.slang:66-78`）是**同一套约定的两份镜像**：

```hlsl
// Engine/Shader/Shaders/GI/DDGI.comp.slang:61-77
static const float kSH_Y00 = 0.28209479177387814;  // l=0: sqrt(1/(4π))

// Band 1 (l=1): 3 个系数
float3 SH_Band1(float3 dir) {
    return dir * 0.4886025119029199;  // sqrt(3/(4π))
}

// Band 2 (l=2) 在步骤 30 被移除：二阶 4 系数即可（§7 Radiance Cache 的表示）。
// 保留说明是为了让"为什么少了一档"这件事在代码里可见，而不是看起来像漏写。

// 将方向 dir 投影为 **4** 个 SH 系数（bands 0/1）
void SHBasis(float3 dir, out float sh[4]) {
    sh[0] = kSH_Y00;                                    // l=0,m=0
    float3 b1 = SH_Band1(dir);
    sh[1] = b1.y; sh[2] = b1.z; sh[3] = b1.x;          // l=1: m=-1,0,1
}

```

```hlsl
// Engine/Shader/Shaders/Lumen/ScreenProbeSampling.slang:66-78
    if (i == 0u) return kSH_Y00;
    if (i == 1u) return kSH_Y1 * d.y;
    if (i == 2u) return kSH_Y1 * d.z;
    return kSH_Y1 * d.x;
}

/// 由**辐射度**的 4 系数重建**辐照度**：E(n) = π·l0·Y00 + (2π/3)·(l1·Y1(n) + l2·Y2(n) + l3·Y3(n))
/// （Â0 = π、Â1 = 2π/3 是"clamped cosine 卷积"的解析系数；白炉下它给出 E ≡ π）
float3 SHRadianceToIrradiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 n) {
    const float A0 = 3.14159265359;
    const float A1 = 2.09439510239;   // 2π/3
    return l0 * (A0 * kSH_Y00)
         + (l1 * (kSH_Y1 * n.y) + l2 * (kSH_Y1 * n.z) + l3 * (kSH_Y1 * n.x)) * A1;
```

**注意两处细节**：① 基函数里**没有任何三角函数**——$l\le1$ 的 $\cos\phi$/$\sin\phi$ 已经在
"实数化 + 用 $(x,y,z)$ 表示"时被吸收掉了，这就是它能便宜到四次乘加的原因；
② 两侧都把方向当**单位向量**用（`d.y`/`d.z`/`d.x` 直接当基函数值），所以调用方必须保证 $|\omega|=1$。
③ **轴序**：`SHBasis` 里 `i==3` 返回的是 `d.x` ⇒ $l=1$ 三项的顺序是 $(y,z,x)$，与 §3.6 的约定一致（此处最容易抄错，见 §7.1）。

：

- $Y_{00}$ 是**常数**（均匀的"底噪"）——它捕获球面函数的平均值；
- $l=1$ 的三个代表**沿三个轴的一阶变化**（像球面的"梯度"），能表达"某个方向更亮"；
- $l=2$ 能表达"赤道亮、两极暗"这类**二次**结构（例如天空亮、地面暗）；
- 带越高，方向细节越细；**截断到 $l\le1$ 意味着只剩"平均值 + 一个方向梯度"**。

### 3.4 正交归一性的验证方式

工程里不必逐条验证，但可以记两个常用积分：

$$\int_\Omega Y_{00}\,\mathrm d\omega = Y_{00}\cdot 4\pi = \sqrt{\pi}\approx1.77245,\qquad
\int_\Omega Y_{00}^2\,\mathrm d\omega = \frac{1}{4\pi}\cdot4\pi = 1$$

第一个式子就是"球面上常数 1 的 0 阶系数"，也是**白炉判据**的解析值来源之一（§5.5）。

### 3.5 为什么工程只留 4 个系数

- **物理上够用**：clamped cosine 核（漫反射的角响应）非常低通。Ramamoorthi & Hanrahan 的分析指出
  **前三个带（$l\le2$，9 个系数）即可复原约 99% 的辐照度**；$l\ge3$ 的贡献更小。
- **工程上更省**：探针数量巨大（DDGI 网格可达上千颗），每颗多存 5 个 float4 会直接吃掉显存与带宽。
- **实测结论**：工程里探针的"格子感"主要来自**探针网格分辨率**而不是 SH 阶数
  （`RT_DDGI.slang:57-65` 记录了两轮实测：换插值权重无效，把最长轴格数 16→24/32 才把幅度调制
  从 35.67% 降到 22.42%/20.11%）。

于是工程的选择是：**$l\le1$（4 个系数）+ 提高网格分辨率**，而不是"$l\le2$ + 稀疏网格"。

### 3.6 轴序与符号约定（最容易错的地方）

工程约定（`LumenSH.h:7-10`、`DDGI.comp.slang:73-76` 两侧一致）：

$$Y_{1,-1}\propto y,\qquad Y_{1,0}\propto z,\qquad Y_{1,1}\propto x$$

即 $l=1$ 三项的排列顺序是 $(y,z,x)$ 而**不是** $(x,y,z)$，而"极轴"是 $z$（与工程"z 向上"一致）。
**混用轴序是静默错误**：画面不会崩，只会让光照方向整体转 90°，很难归因。

---

## 四、投影与重建

### 4.1 投影（分析）：系数 = 内积

$$f_{lm} = \int_\Omega f(\omega)\,Y_{lm}(\omega)\,\mathrm d\omega$$

**工程对照**：两套实现都把"投影"写成**累加 + 乘权重**，区别只在采样域与权重常数：

```hlsl
// Engine/Shader/Shaders/GI/DDGI.comp.slang:253-267
    // SH 投影
    float basis[4];
    SHBasis(dir, basis);
    for (int j = 0; j < 4; j++) {
        sh[j].rgb += radiance * basis[j];
    }
    validSamples += 1.0;
}

// 归一化：蒙特卡洛积分缩放因子 4π/N × 强度（u_Params.x）
if (validSamples > 0.0) {
    float scale = (4.0 * 3.14159265359) / validSamples * u_Params.x;
    for (int j = 0; j < 4; j++) {
        sh[j].rgb *= scale;
    }
```

```hlsl
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:74-84
    l0 += L * SHBasis(0u, d);
    l1 += L * SHBasis(1u, d);
    l2 += L * SHBasis(2u, d);
    l3 += L * SHBasis(3u, d);
    eRef += L * max(0.0, dot(d, n));         // 参考：逐光线 cos 加权求和
}

// 均匀半球 pdf = 1/(2π) ⇒ 系数 = (2π/N)·Σ；参考辐照度 = (2π/N)·Σ L·cos
const float k = 6.28318530718 / (float)N;
l0 *= k; l1 *= k; l2 *= k; l3 *= k;
eRef *= k;
```

**逐行对应**：`Σ L·Y_j` 就是 $\sum_j f(\omega_j)Y_{lm}(\omega_j)$；`4π/N` 与 `2π/N` 就是 $1/p(\omega)$
在两种采样域下的取值。**注意 DDGI 的分母是 `validSamples` 而不是总采样数**（被挡的样本 `continue` 掉了），
所以它实际是个比值估计量——白炉下仍然得到 1，但对非恒定环境有偏（§6.1 有记）。

### 4.2 重建（合成）：级数求和

$$f(\omega) \approx \sum_{l=0}^{L}\sum_{m=-l}^{l} f_{lm}\,Y_{lm}(\omega)$$

截断到 $L$ 就是"低通近似"。$L=1$ 时展开成工程里的 4 项：

$$f(\omega)\approx f_{00}Y_{00} + f_{1,-1}Y_{1,-1}(\omega) + f_{1,0}Y_{1,0}(\omega) + f_{1,1}Y_{1,1}(\omega)$$

**工程对照**：重建就是"逐带求和"，$Y_{lm}(\omega)$ 的角度部分直接写成 `dir.x/y/z`：

```hlsl
// Engine/Shader/Shaders/RT_DDGI.slang:31-35
float3 EvalDDGI_SH(float4 sh[4], float3 dir) {
    float3 result = sh[0].rgb * kDDGI_SH_A0;                 // l=0, m=0
    result += sh[1].rgb * dir.y * kDDGI_SH_A1;               // l=1, m=-1
    result += sh[2].rgb * dir.z * kDDGI_SH_A1;               // l=1, m=0
    result += sh[3].rgb * dir.x * kDDGI_SH_A1;               // l=1, m=1
```

> **注意它不是"原样重建"**：常数 `kDDGI_SH_A0/A1` 已经把 §5 的 Lambert 卷积 $\hat A_l$ 乘进去了
> （$\hat A_0Y_{00}=0.8862269$、$\hat A_1Y_1=1.0233267$），所以这个函数返回的是**辐照度 $E$**，
> 而不是 $L(\omega)$。这也是"评估函数名里没有 E 却返回 E"容易看错的地方。

### 4.3 截断的两个后果

| 后果 | 说明 |
|---|---|
| **细节丢失** | 高频方向结构（小光源、锐利明暗）被抹成低频 |
| **振铃 / 负值** | 截断的吉布斯现象会让重建出现负值（"负光照"），工程里通常直接钳到 0：`return max(result, 0.0)`（`RT_DDGI.slang:38`） |

工程注释里提到：若振铃明显，可改用 **SH 窗口化**（如 Hanning 窗）在截断处平滑衰减系数。

**工程对照**：负值截断就一行，但它的理由写在代码注释里（"二阶更明显"这条尤其值得注意——
带的阶数越低，截断越狠）：

```hlsl
// Engine/Shader/Shaders/RT_DDGI.slang:36-38
// 负值截断：辐照度物理上非负；SH 表示余弦波瓣时仍可能有轻微振铃（二阶更明显），
// 截断避免负辐照度进入画面。若后续振铃明显，可改为 SH 窗口化（如 Hanning）平滑抑制。
return max(result, 0.0);
```

### 4.4 卷积定理（zonal 核）—— 这条是 Lambert 卷积的理论基础

若核函数只依赖夹角（$g(\omega\cdot\omega')$，称 zonal / 旋转对称），那么**卷积在系数域变成逐带缩放**：

$$(f * g)(n) = \int_\Omega f(\omega)\,g(n\cdot\omega)\,\mathrm d\omega = \sum_{l,m}\hat g_l\,f_{lm}\,Y_{lm}(n),
\qquad \hat g_l = 2\pi\int_{-1}^{1} g(t)\,P_l(t)\,\mathrm dt$$

**这条定理正是"探针存辐射度 SH、评估时乘 $A_l$"的全部理由**：投影端与评估端各做一半，中间的系数原封不动。

**工程对照**：这条定理的"为什么"在引擎里有一段注释直接写着（`RT_DDGI.slang:17-26`），
它把"能做什么/不能做什么"讲得比多数论文摘要还清楚：

```hlsl
// Engine/Shader/Shaders/RT_DDGI.slang:17-26
// ── Lambert 余弦波瓣的 SH 卷积系数（M5.3）──
// 探针投影得到的是**辐射度** SH（L_lm = 4π/N·Σ L·Y，不含 cos）；
// 要得到「辐照度」E(n) = ∫L(ω)·max(0, n·ω)dω，必须在**评估端**乘 SH 卷积系数：
//     E(n) = Σ_l A_l · L_lm · Y_lm(n)
//   A_0 = π,  A_1 = 2π/3,  A_2 = π/4      (Ramamoorthi & Hanrahan 2001)
// 注意：不能在「投影时乘 cos」——cos 依赖评估方向（法线），而探针存储时方向未知；
//       卷积只能在评估端以与 n 无关的 A_l 形式应用。
// 下方常量即 A_l 与各 Y_lm 归一化常数的乘积：
```

对应的评估实现（Lumen 侧的同一件事）：

```hlsl
// Engine/Shader/Shaders/Lumen/ScreenProbeSampling.slang:74-81
float3 SHRadianceToIrradiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 n) {
    const float A0 = 3.14159265359;
    const float A1 = 2.09439510239;   // 2π/3
    return l0 * (A0 * kSH_Y00)
         + (l1 * (kSH_Y1 * n.y) + l2 * (kSH_Y1 * n.z) + l3 * (kSH_Y1 * n.x)) * A1;
}
```

**对照读法**：`l0*(A0*kSH_Y00)` 就是 $\hat A_0 L_{00}Y_{00}(n)$（$Y_{00}$ 与 $n$ 无关）；
括号里三项是 $\hat A_1\sum_m L_{1m}Y_{1m}(n)$，其中 $Y_{1m}(n)$ 被展开成 $k_{Y1}\cdot n_{\{y,z,x\}}$。

### 4.5 加法定理（理解 zonal 核为什么可分离）

$$\sum_{m=-l}^{l} Y_{lm}(\omega)\,Y_{lm}(\omega') = \frac{2l+1}{4\pi}P_l(\omega\cdot\omega')$$

它把"两个方向的基函数乘积求和"化成"只依赖夹角的勒让德多项式"，这正是 §4.4 成立的原因。

---

## 五、Lambert 卷积：SH 在漫反射 GI 里的核心

### 5.1 clamped cosine 核

漫反射要的辐照度是"入射光乘余弦"的半球积分：

$$E(n) = \int_\Omega L(\omega)\,\max(0,\,n\cdot\omega)\,\mathrm d\omega$$

把 $g(t)=\max(0,t)$ 代进 §4.4 的 $\hat g_l$：

$$\hat A_l = 2\pi\int_{0}^{1} t\,P_l(t)\,\mathrm dt$$

### 5.2 逐带求值（工程常数的来历）

| $l$ | 积分 | $\hat A_l$ | 数值 |
|---|---|---|---|
| 0 | $2\pi\int_0^1 t\,\mathrm dt = 2\pi\cdot\frac12$ | $\pi$ | 3.1415927 |
| 1 | $2\pi\int_0^1 t^2\,\mathrm dt = 2\pi\cdot\frac13$ | $2\pi/3$ | 2.0943951 |
| 2 | $2\pi\int_0^1 t\cdot\frac{3t^2-1}{2}\mathrm dt = \pi(\frac34-\frac12)$ | $\pi/4$ | 0.7853982 |
| 3 | 奇函数在 $[0,1]$ 上的加权积分为 0 | **0** | 0 |
| 4 | $-\pi/24$ | $-0.1308997$ | 负值（高阶会反号） |

**前三个常数就是工程里的 `kDDGI_SH_A0/A1`**（`RT_DDGI.slang:21` 引 Ramamoorthi & Hanrahan 2001）：
$A_0=\pi$、$A_1=2\pi/3$（工程的 band-2 已移除，但注释保留了 $A_2=\pi/4$）。

于是辐照度的 SH 重建式（$L$ 为**辐射度** SH 系数）：

$$\boxed{\;E(n) = \sum_{l,m} \hat A_l\,L_{lm}\,Y_{lm}(n)\;}$$

**工程对照**：这三个常数在代码里就是一行注释加一行定义：

```hlsl
// Engine/Shader/Shaders/RT_DDGI.slang:19-21
//   A_0 = π,  A_1 = 2π/3,  A_2 = π/4      (Ramamoorthi & Hanrahan 2001)
```

Lumen 侧把 $A_0$/$A_1$ 写成显式常量并在重建式里用：

```hlsl
// Engine/Shader/Shaders/Lumen/ScreenProbeSampling.slang:75-77
const float A0 = 3.14159265359;
const float A1 = 2.09439510239;   // 2π/3
```

（`2.09439510239` 就是 $2\pi/3$；`RT_DDGI.slang:27` 另有一条注释说明 band-2 的 $A_2$
与 5 个 $l=2$ 项是**一起移除**的——"只改一边会让少的那一档变成静默偏移"。）

### 5.3 为什么"不能在投影端乘 cos"

`RT_DDGI.slang:22-23` 明确写了这条：

> 不能在「投影时乘 cos」——cos 依赖评估方向（法线），而探针存储时方向未知；
> 卷积只能在评估端以与 n 无关的 $A_l$ 形式应用。

展开说：余弦项是 $\max(0,\,n\cdot\omega)$，其中 **$n$ 是"要算哪一点的法线"，投影时并不知道**
（同一颗探针会被几万个不同法线的像素查询）。所以：

- **投影端**：只做与 $n$ 无关的事——把入射辐射度投影成 $L_{lm}$；
- **评估端**：拿到查询法线 $n$ 后，乘上 $A_l$ 并用 $Y_{lm}(n)$ 重建。

### 5.4 工程实现：预乘常数

评估端每次都要算 $A_l\cdot Y_{lm}(n)$。由于 $Y_{lm}$ 的"角度部分"就是 $x/y/z$，工程把常数预先乘好：

$$\hat A_0\cdot Y_{00} = \pi\cdot\sqrt{\tfrac{1}{4\pi}} = \frac{\sqrt\pi}{2} = 0.8862269254527580$$

$$\hat A_1\cdot Y_{1} = \frac{2\pi}{3}\cdot\sqrt{\tfrac{3}{4\pi}} = 1.0233267079464886$$

对应 `RT_DDGI.slang:25-26` 的 `kDDGI_SH_A0` / `kDDGI_SH_A1`。评估实现见 §4.2 的代码块（`RT_DDGI.slang:31-35` 的 `EvalDDGI_SH`）——它就是把这两个常数直接乘在基函数上，于是四次乘加即得辐照度。

> **精度说明**：解析值与源码里的十进制常量在末位差 1~3 ulp——
> $\hat A_0Y_{00}$ 解析为 `0.8862269254527579`、代码写 `...580`；
> $\hat A_1Y_1$ 解析为 `1.0233267079464883`、代码写 `...886`。
> 这是十进制常量的正常舍入，**不是约定差异**；核对常数时不要因末位不同就以为用错了公式。

（`RT_DDGI.slang:31-39`；代码已在 §4.2 引用。）

### 5.5 白炉解析判据（工程用它做验收）

设半球内入射辐射度恒为 1（**白炉**），则解析上 $E(n)\equiv\pi$（因为 $\int_\Omega\cos\theta\,\mathrm d\omega=\pi$）。
用 SH 走一遍：

- **半球采样投影**（Lumen 的做法）：$l_0 = \int_{2\pi}1\cdot Y_{00}\,\mathrm d\omega = 2\pi Y_{00} = \sqrt\pi \approx 1.77245$
- **整球采样投影**（DDGI 的做法，若球面处处为 1）：$l_0 = 4\pi Y_{00} = 2\sqrt\pi \approx 3.54491$

两者都能得到 $E=\pi$：以半球为例
$E = \hat A_0 l_0 Y_{00} = \pi\cdot(2\pi Y_{00})\cdot Y_{00} = 4\pi^2\cdot\frac{1}{4\pi} = \pi$ ✓

**这就是工程里那条"解析验收"的来历**（`LumenSH.h:11,23-24`）：

> 白炉下 $E(n)\equiv\pi$，且 $l_0\equiv 2\pi Y_{00}=\sqrt\pi$——**与采样方向、采样数无关**。

它的价值在于：**不需要参考图、不需要跑 GPU**，就能用一条断言钉死"基函数常数、卷积系数、重建式"
三者是否自洽。`LumenSH.h` 之所以专门做一份 C++ 镜像，就是为了让这条断言能在单测里跑。

**工程对照**：白炉不是"概念"，它在代码里是一个**显式分支 + 一条定点统计**：

```hlsl
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:64-72,82-84
// 白炉：辐射度恒为 1（不看 atlas）⇒ l0 必须等于 √π，与方向/采样数无关
// 非白炉：只剩"这条光线有没有打到几何"这一道门。**不**再按"页是否命中"过滤 ——
// 步骤 22 对缺页已经返回中性值 0.18（而不是黑），那正是它要继续往下的语义；
// 若在这里再滤掉，实测每条探针只剩 0.9 条有效光线（8 条里 0.9 条），SH 等于没积。
const float4 s = u_Radiance[flat];
const bool   rayHit = u_RayResult[flat].y > 0.5f;
const float3 L = furnace ? float3(1.0, 1.0, 1.0)
                         : (rayHit ? s.xyz : float3(0.0, 0.0, 0.0));
if (rayHit || furnace) ++validRays;
// ...
const float k = 6.28318530718 / (float)N;
l0 *= k; l1 *= k; l2 *= k; l3 *= k;
eRef *= k;
```

```hlsl
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:97-100
InterlockedAdd(u_Stats[2], (uint)(abs(l0.r) * 4096.0 + 0.5));
if (furnace) InterlockedAdd(u_Stats[3], (uint)(abs(l0.r - 1.7724539) * 65536.0 + 0.5));
```

```cpp
// Engine/Render/Lumen/LumenSH.h:23-24（CPU 侧镜像里的同一个解析值）
inline constexpr double kSHWhiteFurnaceL0 = 1.7724538509055159;   // √π
```

**对照读法**：shader 里的 `1.7724539` 与 CPU 常量 `1.7724538509055159` 是同一个 $\sqrt\pi$；
源码里的 `const float k = 6.28318530718 / (float)N;` 就是 $2\pi/N$（半球均匀采样的 $1/p$）；
`u_Stats[2]` 累计的是 $l_0$ 本身（白炉下应等于 $\sqrt\pi$），`u_Stats[3]` 累计的是**与 $\sqrt\pi$ 的偏差**——
于是"SH 投影是否正确"变成了一条可读数的、与场景无关的判据。

### 5.6 量纲：从 SH 到画面还要再除 π

SH 给的是**辐照度 $E$**；而引擎的统一约定是几何源返回**出射辐射度** $L_o = \mathrm{albedo}\times E/\pi$。
所以链路是：

$$\underbrace{L_{lm}}_{\text{radiance SH in probe}} \;\xrightarrow{\ \times \hat A_l Y_{lm}(n)\ }\; E(n)
\;\xrightarrow{\ \times \mathrm{albedo}/\pi\ }\; L_o$$

工程里前半段在评估函数内完成、后半段在合成端完成（DDGI 的合成端就是"乘 albedo/π"，
`DeferredLighting.frag.slang` 的 `GISOURCE_DDGI` 分支）。**漏掉那个 $1/\pi$ 就是量级差 $\pi$ 倍的经典错误**。

**工程对照**：那个 $1/\pi$ 在合成端只是一次乘法，但代码注释专门解释了为什么必须有它：

```hlsl
// Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang —— 合成端（DDGI 分支）
// EvalDDGI_SH 已施加 Lambert 卷积 A_l → 返回**辐照度 E** → 需补 albedo/π
return SampleDDGI(worldPos, N) * albedo * (1.0 / HE_PI);
```

同一个换算在另一条路径上也只出现一次（RTGI 的 miss 回退把 $E$ 当成"沿该方向的入射辐射度"来用）：

```hlsl
// Engine/Shader/Shaders/RayTracing/RT_GI.rgen.slang —— miss 回退
// 量纲对齐：SampleDDGI 返回的是**辐照度 E**（EvalDDGI_SH 已施加 Lambert 卷积
// A_l），而命中的 ClosestHit 给的是**辐射度 L** —— 二者不能直接相加。
radiance += SampleDDGI(worldPos, dir) * (1.0 / 3.14159265);
```

**计数方法**：整条链路里 $1/\pi$ **只应出现一次**——要么在评估端（返回 $E$，合成端补 $\mathrm{albedo}/\pi$），
要么在投影端（直接存 $E/\pi$）。两处都补就是差 $\pi$ 倍，都不补同样是差 $\pi$ 倍。

---

## 六、工程实现逐行对照

### 6.1 DDGI 探针：4 系数 + 整球均匀采样（$4\pi/N$）

| 环节 | 工程实现 | 对应理论 |
|---|---|---|
| 采样方向 | Fibonacci 球面 32 条（`FibonacciSphere`） | 近似**整球均匀**采样，pdf $=1/4\pi$ |
| 投影基 | `SHBasis(dir, basis)`：$[Y_{00},\ Y_1 y,\ Y_1 z,\ Y_1 x]$ | §3.3 的 $i=0..3$ |
| 累加 | `sh[j].rgb += radiance * basis[j]` | 蒙特卡洛估计分子 |
| 归一化 | `scale = 4π / validSamples * intensity`（`DDGI.comp.slang:264`） | $L_{lm}\approx\frac{4\pi}{N}\sum_j L_jY_{lm}(\omega_j)$ |
| 时域 | 与历史 SH 逐系数 `lerp`（`:273-279`） | 降低方差、加速收敛 |
| 评估 | `EvalDDGI_SH`：预乘常数 × 基函数，再钳负 | §5.4 |
| 空间重建 | 8 邻域**三线性插值**（`SampleDDGI`） | 探针之间的空间插值（SH 之外的另一层近似） |

> 两点工程细节值得记住：
> ① **分母是 `validSamples` 而不是总采样数**（被挡/无数据的样本被 `continue` 掉）——这实质是
> 一个比值估计量，形式与 SSGI 的 $\sum L\cos/\sum\cos$ 同类；白炉下仍然得到 1，但对非恒定环境是有偏的。
> ② **用户强度 $\mathrm{intensity}$ 被乘进了存储的 SH**（`:264` 的 `u_Params.x`）⇒ 缓冲里存的量带了
> 用户缩放，改强度需要重新收敛；这是"表示与调参混在一起"的一处已知瑕疵。

### 6.2 Lumen 屏幕探针：4 系数 + 半球均匀采样（$2\pi/N$）

Lumen 的探针是沿**法线半球**追踪的（不是整球），所以蒙特卡洛 pdf 不同，权重也不同：

$$L_{lm}\approx\frac{1}{N}\sum_j\frac{L_jY_{lm}(\omega_j)}{p(\omega_j)}
=\frac{2\pi}{N}\sum_j L_jY_{lm}(\omega_j)\qquad\Bigl(p=\frac{1}{2\pi}\Bigr)$$

代码注释原文如此（`Lumen/Lumen_ScreenProbe_SHProject.comp.slang:6-7,81`）。
**同一个 SH 表示、不同的采样域 ⇒ 归一化常数不同（$4\pi$ vs $2\pi$）**，这是最容易互相抄错的地方。

### 6.3 三处约定必须完全一致

| 约定 | 工程值 | 出处 |
|---|---|---|
| $Y_{00}$ | 0.28209479177387814 | `DDGI.comp.slang:61`、`LumenSH.h:20` |
| $Y_{1}$（$l=1$ 归一化） | 0.48860251190291992 | `DDGI.comp.slang:65`、`LumenSH.h:22` |
| $l=1$ 轴序 | $(y,z,x)$ 对应 $m=(-1,0,1)$ | `DDGI.comp.slang:75`、`LumenSH.h:9` |
| $\hat A_0,\hat A_1$ | $\pi$、$2\pi/3$ | `RT_DDGI.slang:21`、`LumenSH.h:39-40` |
| 探针存储 | 每探针 4 × `float4`，RGB 存系数、A 闲置 | `GI_DDGI.h` 的 `kFloats4PerProbe = 4`、`RT_DDGI.slang:11` |

### 6.4 数据流小结

```
入射辐射度（追踪/GBuffer/IBL）
   ↓ 沿 N 个方向采样（DDGI：整球 Fibonacci 32；Lumen：法线半球）
   ↓ 乘基函数累加            sh[j] += L · Y_j(ω)
   ↓ 乘 MC 权重              × 4π/N  或  × 2π/N
   ↓ 时域混合                与历史 lerp
[探针缓冲：每探针 4 × float4 的辐射度 SH]
   ↓ 空间插值                8 邻域三线性（DDGI）
   ↓ 评估（Lambert 卷积）    E(n) = Σ Â_l · L_lm · Y_lm(n)，再 max(·,0)
   ↓ 乘 albedo/π → L_o       合成端（三通道归一化加权）
```

---

## 七、要点与踩坑清单

### 7.1 归一化约定的三种写法（混用必错）

| 约定 | $Y_{00}$ | $l=1$ 常数 | 说明 |
|---|---|---|---|
| 本文/工程 | $0.5\sqrt{1/\pi}=0.28209$ | $0.5\sqrt{3/\pi}=0.48860$ | Ramamoorthi & Hanrahan，**正交归一** |
| 未归一化（只留角度部分） | $1$ | 直接用 $x,y,z$ | 系数含 $1/4\pi$ 之类的因子，易与上面混算 |
| 含 $\sqrt{2l+1}$ 的 zonal 写法 | $\sqrt{1/4\pi}$ | $\sqrt{3/4\pi}$ | 与前两者只差常数，但 $A_l$ 要跟着改 |

**判断方法**：用 §3.4 的 $\int_\Omega Y_{00}^2\,\mathrm d\omega = 1$ 与 §5.5 的白炉解析值 $\sqrt\pi$ 去校验——
只要这两个数对得上，整套约定就是自洽的。

### 7.2 投影的离散化：域、pdf、有效样本

一般的蒙特卡洛写法：

$$f_{lm}\approx\frac{1}{N}\sum_{j=1}^{N}\frac{f(\omega_j)Y_{lm}(\omega_j)}{p(\omega_j)}$$

| 采样域 | pdf | 权重 |
|---|---|---|
| 整球均匀 | $1/4\pi$ | $4\pi/N$（DDGI） |
| 半球均匀 | $1/2\pi$ | $2\pi/N$（Lumen） |
| 余弦加权（半球） | $\cos\theta/\pi$ | $\pi/N \cdot 1/\cos\theta$（注意：$l=0$ 的估计量质量最好，高阶方差大） |

**常见错误**：换了采样分布而没换权重（结果整体缩放错）、少了方向归一化（非单位向量会让基函数值错）。

### 7.3 截断到 $l\le1$ 能表达什么

| 能 | 不能 |
|---|---|
| 平均亮度（$l=0$） | 小光源在球面上的尖锐亮斑 |
| 某一方向的整体偏亮/偏暗（$l=1$） | "赤道亮两极暗"这类二次结构（需 $l=2$） |
| 平滑的环境梯度 | 镜面反射那种极窄的波瓣（SH 完全不适合，改用 GGX 预滤波） |

**判断标准**：目标函数越接近"低阶多项式×方向"，SH 越划算；越尖锐，越不该用 SH。

### 7.4 负值与振铃

- 截断会让重建出现负值 ⇒ 物理上非法（负辐照度），工程一律 `max(·,0)`；
- 钳负会**丢能量**（画面轻微变暗），但对低频 GI 通常可接受；
- 若振铃明显：窗口化（Hanning）、提高阶数、或改回逐方向采样。

### 7.5 SH 的旋转：为什么不能"直接转系数"

- **带内混合**：旋转会把同一带的系数互相混合，$l$ 带需要 $(2l+1)\times(2l+1)$ 的**旋转矩阵**
  （Wigner D 矩阵的实形式）；$l=0$ 不变、$l=1$ 是 3×3、$l=2$ 是 5×5……
- **只有绕 z 轴的旋转**是简单的（每个 $m$ 只乘 $e^{\mathrm im\alpha}$，实形式下是同带内的 2×2 旋转）；
- 通用旋转的标准做法：把目标轴转到 z 轴 → 绕 z 转 → 转回来（"zonal 轴旋转 + 三明治"）。
- **工程含义**：世界空间的探针 SH **不需要旋转**（探针本身就是世界空间的）；但若要用 SH 表示
  "可旋转的天空盒环境光"，就必须处理这件事——这也正是 `SkyboxComponent::rotation`
  目前没有渲染侧消费者的原因之一（07 §2.2 的缺口表已记档）。

### 7.6 存储与精度

| 项 | 工程现状 | 可优化方向 |
|---|---|---|
| 每探针 | 4 × `float4` = 64 B（RGB 用、A 闲置） | 打包成 4×11 bit 定点、或 fp16 ⇒ 显存/带宽减半以上 |
| 精度 | 当前 fp32 | 定点压缩会引入量化误差，需重新标定白炉判据 |
| 布局 | `StructuredBuffer<float4>`，探针索引 × 4 | 保持**同一次改动里同时改投影端与评估端**（`RT_DDGI.slang:27-28` 的警告） |

### 7.7 验收方法（可复现）

1. **解析判据**：白炉下 $l_0 = 2\pi Y_{00} = \sqrt\pi$（半球投影）或 $4\pi Y_{00} = 2\sqrt\pi$（整球投影），
   且 $E(n)\equiv\pi$，与方向/采样数无关 —— 可用 CPU 镜像断言（`LumenSH.h` 的做法）；
2. **常量核对**：$\hat A_0Y_{00} = \sqrt\pi/2 = 0.8862269254527580$、
   $\hat A_1Y_1 = 1.0233267079464886$，与代码常量逐位比对；
3. **端到端**：探针 SH 在整个球面/半球上的平均应当等于输入辐射度的平均（`Σ L` 与 `l_0` 的关系可解析验算）；
3. **端到端**：探针 SH 在整个球面/半球上的平均应当等于输入辐射度的平均（`Σ L` 与 `l_0` 的关系可解析验算）。
   工程里更硬的做法是**同帧算两份**：SH 重建的 $E$ 与"逐光线 cos 加权求和"的参考 $E$ 各写一个缓冲，
   差值就是"SH 带限 + 有限采样"的误差（不是猜的）：

```hlsl
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:78,91-93
eRef += L * max(0.0, dot(d, n));         // 参考：逐光线 cos 加权求和
// ...
const float3 eSh = SHRadianceToIrradiance(l0, l1, l2, l3, n);
u_IrradSh[tid.x]  = float4(eSh, 1.0);
u_IrradRef[tid.x] = float4(eRef, 1.0);
```
4. **回归**：改动投影端（阶数、权重、轴序）必须同时改动评估端与合成端，并重跑上述判据。

---

## 八、公式速查

| 名称 | 公式 |
|---|---|
| 内积 | $\langle f,g\rangle=\int_\Omega fg\,\mathrm d\omega$ |
| 投影 | $f_{lm}=\int_\Omega f(\omega)Y_{lm}(\omega)\,\mathrm d\omega$ |
| 重建 | $f(\omega)\approx\sum_{l\le L}\sum_m f_{lm}Y_{lm}(\omega)$ |
| $l$ 带的基函数个数 | $2l+1$；到 $L$ 共 $(L+1)^2$ |
| 加法定理 | $\sum_m Y_{lm}(\omega)Y_{lm}(\omega')=\frac{2l+1}{4\pi}P_l(\omega\cdot\omega')$ |
| zonal 卷积 | $(f*g)(n)=\sum_{l,m}\hat g_lf_{lm}Y_{lm}(n)$，$\hat g_l=2\pi\int_{-1}^1g(t)P_l(t)\mathrm dt$ |
| Lambert 卷积系数 | $\hat A_0=\pi$、$\hat A_1=\frac{2\pi}{3}$、$\hat A_2=\frac{\pi}{4}$、$\hat A_3=0$ |
| 辐照度重建 | $E(n)=\sum_{l,m}\hat A_lL_{lm}Y_{lm}(n)$ |
| 整球均匀投影权重 | $L_{lm}\approx\frac{4\pi}{N}\sum_jL_jY_{lm}(\omega_j)$ |
| 半球均匀投影权重 | $L_{lm}\approx\frac{2\pi}{N}\sum_jL_jY_{lm}(\omega_j)$ |
| 白炉（半球投影） | $l_0=2\pi Y_{00}=\sqrt\pi$，$E(n)\equiv\pi$ |
| 工程预乘常数 | $\hat A_0Y_{00}=0.8862269254527580$、$\hat A_1Y_1=1.0233267079464886$ |

---

## 九、延伸阅读

- Ramamoorthi, R., & Hanrahan, P. (2001). *An Efficient Representation for Irradiance Environment Maps.*
  SIGGRAPH —— 本文 §5 的全部结论（含 $\hat A_l$ 表与"$l\le2$ 复原约 99% 辐照度"）的原始出处。
- Sloan, P.-P. (2008). *Stupid Spherical Harmonics (SH) Tricks.* —— 工程向的 SH 技巧合集
  （旋转、窗口化、压缩、乘积）。
- Majercik, Z., et al. (2019). *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields.*
  —— DDGI 的原始论文（探针网格 + SH + 三线性插值 + 可见性）。
- 项目内：`docs/HugEngine引擎介绍/07.全局光照GI本质、实现与架构优化.md`（§2.5 DDGI、§2.7 Lumen 的落地细节）
  与 `docs/HugEngine引擎介绍/08.Lumen实现分析.md`（屏幕探针与滤波）。

---

> 本文涵盖：球面数学基础、球谐基函数（$l\le2$ 显式形式）、投影与重建、zonal 卷积定理、
> Lambert 卷积系数推导、白炉解析判据、以及工程里 DDGI / Lumen 两套 SH 实现的逐行对照与踩坑清单。
> 生成日期：2026-10-09　HugEngine 渲染引擎
