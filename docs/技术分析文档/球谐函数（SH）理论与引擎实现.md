# 球谐函数（SH）理论与引擎实现

> **本文定位**：把 GI 计算里的球谐（Spherical Harmonics, SH）从**理论推导**到**引擎逐处实现**讲完整。
>
> 本文由两份文档合并整理而成：
> - 《球谐函数（SH）理论与全局光照应用》—— 数学推导、公式、白炉判据、踩坑清单；
> - 《球谐函数（SH）在引擎中的实现分析》—— 每一处代码的原理、数据流与接线。
>
> 合并后去掉了重复段落（白炉、量纲、DDGI/Lumen 对照、验收方法各只保留一处），
> 并按"理论 → 实现 → 约定 → 验收 → 速查"重新组织。公式与常数都对当前源码核对过：
> `Y00 = 0.28209479177387814`、`Y1 = 0.4886025119029199`、`A0 = π`、`A1 = 2π/3`、白炉解析值 `√π`。

## 目录

**第一部分：理论**

- [1. 为什么 GI 里会出现 SH](#1-为什么-gi-里会出现-sh)
- [2. 球面数学准备](#2-球面数学准备)
- [3. 球谐基函数](#3-球谐基函数)
- [4. 投影与重建](#4-投影与重建)
- [5. Lambert 卷积](#5-lambert-卷积)

**第二部分：引擎实现**

- [6. 实现总览](#6-实现总览)
- [7. 共享实现](#7-共享实现)
- [8. 站点 A：Lumen Screen Probe](#8-站点-alumen-screen-probe)
- [9. 站点 B：DDGI](#9-站点-bddgi)
- [10. 跨站点约定与踩坑](#10-跨站点约定与踩坑)
- [11. 不变量与验收](#11-不变量与验收)
- [12. 文件索引](#12-文件索引)
- [13. 已知限制与后续](#13-已知限制与后续)
- [14. 公式速查](#14-公式速查)
- [15. 延伸阅读](#15-延伸阅读)

---

# 第一部分：理论

## 1. 为什么 GI 里会出现 SH

### 1.1 问题的来源：渲染方程里的方向积分

漫反射的渲染方程是**球面上的积分**：

$$
L_o(x, \omega_o) = \frac{\rho(x)}{\pi}\int_{\Omega} L_i(x, \omega)\,(n\cdot\omega)\,\mathrm d\omega
$$

关键困难在于被积函数不只是"数"，而是 **$L_i(x,\omega)$：一个定义在球面上的函数**——它对每个入射方向 $\omega$ 有一个值。要算这个积分，理论上需要知道入射光在整个半球上的分布。

三类做法：

| 做法 | 代表 | 代价 |
|---|---|---|
| 逐方向采样（把积分当蒙特卡洛求和） | 路径追踪、RTGI | 每个着色点几十~几千条射线 |
| **把方向函数压成少数几个系数** | **SH（本文）** | 一次点积（几个乘加），与采样数无关 |
| 预烘成与方向无关的标量 | 纯环境光常数项 | 丢失方向性 |

SH 属于中间那一类：**用几个实数近似一整个球面函数**，让"方向上的分布"变成"每组几个系数"。

### 1.2 降维的直觉：用"球面的傅里叶"近似

一维周期函数可以用正弦/余弦（傅里叶级数）展开；**球面**上对应的正交基就是 **球谐函数**。只保留低频几项，就得到"模糊但便宜"的球面函数近似——而 GI 恰好天然低频（漫反射是强的低通滤波），这就是 SH 在 GI 里成立的根本原因。

### 1.3 工程里的用法

| 用法 | 工程实例 |
|---|---|
| **探针存入射光分布** | DDGI：每探针 4 系数，沿 Fibonacci 方向取入射辐射度后投影（`GI/DDGI.comp.slang`） |
| **屏幕探针的 SH 投影** | Lumen：屏幕探针沿 SDF 追踪后投影为 4 系数（`Lumen/Lumen_ScreenProbe_SHProject.comp.slang`） |
| **环境光辐照度的解析近似** | Ramamoorthi & Hanrahan 2001 的经典结论（工程目前是"改进方向"） |

本引擎只用前两种；镜面反射走预过滤 cubemap / SSR / RT 反射，不用 SH。

---

## 2. 球面数学准备

### 2.1 球坐标与立体角

方向用单位向量 $\omega = (x,y,z)$，或球坐标 $(\theta,\phi)$：

$$
x = \sin\theta\cos\phi,\qquad y = \sin\theta\sin\phi,\qquad z = \cos\theta
$$

（本文采用 **z 为极轴**的右手系；工程里 z 是"上"，见 §3.6 的轴序说明。）

立体角元：

$$
\mathrm d\omega = \sin\theta\,\mathrm d\theta\,\mathrm d\phi
$$

整个球面的立体角是 $\int_0^{2\pi}\!\!\int_0^{\pi}\sin\theta\,\mathrm d\theta\,\mathrm d\phi = 4\pi$；半球是 $2\pi$。**这个 $4\pi$ / $2\pi$ 后面会反复出现**（蒙特卡洛的 pdf、白炉的解析值都来自它）。

### 2.2 球面上的内积与正交基

在球面上定义内积（把函数当向量）：

$$
\langle f, g\rangle = \int_{\Omega} f(\omega)\,g(\omega)\,\mathrm d\omega
$$

一组基 $\{Y_i\}$ 若满足 $\langle Y_i, Y_j\rangle = \delta_{ij}$ 就是**正交归一基**。于是一个球面函数可以像向量一样展开：

$$
f(\omega) = \sum_i c_i Y_i(\omega),\qquad c_i = \langle f, Y_i\rangle = \int_\Omega f(\omega)Y_i(\omega)\,\mathrm d\omega
$$

**这两条式子是后面一切的骨架**：第一式是"重建（合成）"，第二式是"投影（分析）"。

### 2.3 为什么基函数是勒让德多项式

我们希望基函数能分离变量：$Y(\theta,\phi) = \Theta(\theta)\Phi(\phi)$，并让 $\Phi$ 是 $\phi$ 的周期函数。把拉普拉斯方程在球坐标下分离变量，$\Theta$ 满足的方程解就是**连带勒让德函数** $P_l^m(\cos\theta)$。

- $l$ 称**带（band）/ 阶**，$m$ 称**次数**，取值 $l \ge 0$、$-l \le m \le l$；
- 前几个勒让德多项式：$P_0(t)=1$、$P_1(t)=t$、$P_2(t)=\frac{3t^2-1}{2}$、$P_3(t)=\frac{5t^3-3t}{2}$；
- 连带勒让德函数 $P_l^m(t) = (-1)^m(1-t^2)^{m/2}\dfrac{\mathrm d^m}{\mathrm dt^m}P_l(t)$（含 Condon–Shortley 相位）。

> 工程实践里**几乎不需要手算连带勒让德函数**：用到的 $l\le2$ 的显式形式在 §3.3 直接给出。

---

## 3. 球谐基函数

### 3.1 复数形式

$$
Y_l^m(\theta,\phi) = K_l^m\,P_l^m(\cos\theta)\,e^{\mathrm i m\phi},\qquad
K_l^m = \sqrt{\frac{2l+1}{4\pi}\frac{(l-m)!}{(l+m)!}}
$$

它天然正交归一：$\int Y_l^m \overline{Y_{l'}^{m'}}\,\mathrm d\omega = \delta_{ll'}\delta_{mm'}$。但渲染里我们处理的是实值函数，复基用起来别扭（系数成对共轭），所以工程一律用**实数形式**。

### 3.2 实数形式（工程用这个）

把 $e^{\mathrm im\phi}$ 的实部/虚部拆开并吸收归一化因子，得到实球谐：

$$
Y_l^{m} =
\begin{cases}
\sqrt{2}\,K_l^{|m|}\,P_l^{|m|}(\cos\theta)\cos(|m|\phi) & m > 0\\[2pt]
K_l^{0}\,P_l^{0}(\cos\theta) & m = 0\\[2pt]
\sqrt{2}\,K_l^{|m|}\,P_l^{|m|}(\cos\theta)\sin(|m|\phi) & m < 0
\end{cases}
$$

它们同样正交归一，且都是**实值**。$l$ 固定时共有 $2l+1$ 个基函数；到 $l=L$ 为止共 $(L+1)^2$ 个。

| 最大带 $L$ | 系数个数 $(L+1)^2$ | 常见叫法 |
|---|---|---|
| 0 | 1 | 常数环境光 |
| **1** | **4** | **二阶 4 系数**（工程用；DDGI / Lumen 探针） |
| 2 | 9 | "SH-9"（经典环境光辐照度近似） |
| 3 | 16 | 少量反射类用途 |

> 注意命名混乱：带 $l$ 从 0 开始数，所以"$l\le1$（4 个系数）"常被叫**二阶**、"$l\le2$（9 个系数）"叫**三阶**。本文一律写清楚"$l\le$ 几 / 几个系数"。

### 3.3 $l\le2$ 的显式基函数与工程常数

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

**工程对照**：工程只保留 $i=0..3$，常数写在 `SHCommon.slang`，投影端（DDGI）与评估端（RT / Lumen）include **同一个文件**：

```slang
// Engine/Shader/Shaders/SHCommon.slang:39-40,57-70
static const float kSH_Y00 = 0.28209479177387814;   // 0.5·√(1/π)
static const float kSH_Y1  = 0.4886025119029199;    // 0.5·√(3/π)

// 第 i 个基函数在方向 d 上的值（i ∈ [0,4)）。d 必须是**单位向量**。
float SHBasis(uint i, float3 d) {
    if (i == 0u) return kSH_Y00;
    if (i == 1u) return kSH_Y1 * d.y;   // m = -1
    if (i == 2u) return kSH_Y1 * d.z;   // m =  0
    return kSH_Y1 * d.x;                // m = +1（i == 3）
}

// 一次取齐 4 个基函数值（投影端累加用：sh[j] += L·basis[j]），避免在投影循环里逐次分支。
void SHBasis(float3 d, out float basis[4]) {
    basis[0] = kSH_Y00;
    const float3 b1 = d * kSH_Y1;
    basis[1] = b1.y; basis[2] = b1.z; basis[3] = b1.x;   // l=1: m=-1,0,1
}
```

**三处细节**：① 基函数里**没有任何三角函数**——$l\le1$ 的 $\cos\phi$/$\sin\phi$ 已经在"实数化 + 用 $(x,y,z)$ 表示"时被吸收掉，这是它能便宜到几次乘加的原因；② 两侧都把方向当**单位向量**用（`d.y`/`d.z`/`d.x` 直接当基函数值），调用方必须保证 $|\omega|=1$；③ $l=1$ 三项的轴序是 $(y,z,x)$，见 §3.6。

**直观理解**：

- $Y_{00}$ 是**常数**（均匀的"底噪"）——捕获球面函数的平均值；
- $l=1$ 的三个代表**沿三个轴的一阶变化**（像球面的"梯度"），能表达"某个方向更亮"；
- $l=2$ 能表达"赤道亮、两极暗"这类**二次**结构；
- 带越高方向细节越细；**截断到 $l\le1$ 意味着只剩"平均值 + 一个方向梯度"**。

### 3.4 正交归一性的验证方式

工程里不必逐条验证，但可以记两个常用积分：

$$
\int_\Omega Y_{00}\,\mathrm d\omega = Y_{00}\cdot 4\pi = \sqrt{\pi}\approx1.77245,\qquad
\int_\Omega Y_{00}^2\,\mathrm d\omega = \frac{1}{4\pi}\cdot4\pi = 1
$$

第一个式子就是"球面上常数 1 的 0 阶系数"，也是**白炉判据**的解析值来源之一（§5.5）。

### 3.5 为什么工程只留 4 个系数

- **物理上够用**：clamped cosine 核（漫反射的角响应）非常低通。Ramamoorthi & Hanrahan 的分析指出**前三个带（$l\le2$，9 个系数）即可复原约 99% 的辐照度**；$l\ge3$ 的贡献更小；
- **工程上更省**：探针数量巨大（DDGI 网格可达上千颗），每颗多存 5 个 float4 会直接吃掉显存与带宽；
- **实测结论**：工程里探针的"格子感"主要来自**探针网格分辨率**而不是 SH 阶数（`RT_DDGI.slang` 记录了两轮实测：换插值权重无效，把最长轴格数 16→24/32 才把幅度调制从 35.67% 降到 22.42%/20.11%）。

于是工程的选择是：**$l\le1$（4 个系数）+ 提高网格分辨率**，而不是"$l\le2$ + 稀疏网格"。

### 3.6 轴序与符号约定（最容易错的地方）

工程约定（Slang 侧 `SHCommon.slang` 与 C++ 镜像 `LumenSH.h` 一致）：

$$
Y_{1,-1}\propto y,\qquad Y_{1,0}\propto z,\qquad Y_{1,1}\propto x
$$

即 $l=1$ 三项的排列顺序是 $(y,z,x)$ 而**不是** $(x,y,z)$，而"极轴"是 $z$（与工程"z 向上"一致）。

**混用轴序是静默错误**：画面不会崩，只会让光照方向整体转 90°，很难归因。

---

## 4. 投影与重建

### 4.1 投影（分析）：系数 = 内积

$$
f_{lm} = \int_\Omega f(\omega)\,Y_{lm}(\omega)\,\mathrm d\omega
$$

**工程对照**：两套实现都把"投影"写成**累加 + 乘权重**，区别只在采样域与权重常数：

```slang
// Engine/Shader/Shaders/GI/DDGI.comp.slang:235-250
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
}
```

```slang
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

**逐行对应**：`Σ L·Y_j` 就是 $\sum_j f(\omega_j)Y_{lm}(\omega_j)$；`4π/N` 与 `2π/N` 就是 $1/p(\omega)$ 在两种采样域下的取值。**注意 DDGI 的分母是 `validSamples` 而不是总采样数**（被挡的样本 `continue` 掉了），所以它实际是个比值估计量——白炉下仍然得到 1，但对非恒定环境有偏（§9.2 有记）。

### 4.2 重建（合成）：级数求和

$$
f(\omega) \approx \sum_{l=0}^{L}\sum_{m=-l}^{l} f_{lm}\,Y_{lm}(\omega)
$$

截断到 $L$ 就是"低通近似"。$L=1$ 时展开成工程里的 4 项：

$$
f(\omega)\approx f_{00}Y_{00} + f_{1,-1}Y_{1,-1}(\omega) + f_{1,0}Y_{1,0}(\omega) + f_{1,1}Y_{1,1}(\omega)
$$

**工程对照**：重建就是"逐带求和"，$Y_{lm}(\omega)$ 的角度部分直接写成 `dir.x/y/z`：

```slang
// Engine/Shader/Shaders/SHCommon.slang:74-77
// 由 4 系数**辐射度** SH 在方向 d 上求值（**不做**余弦卷积）⇒ 结果仍是辐射度 L(d)。
float3 SHEvalRadiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 d) {
    return l0 * kSH_Y00 + (l1 * d.y + l2 * d.z + l3 * d.x) * kSH_Y1;
}
```

> **注意它不是"原样重建"**：这个函数返回的是**辐射度 $L(d)$**；要辐照度得用 `SHRadianceToIrradiance`（§5.4）。

### 4.3 截断的两个后果

| 后果 | 说明 |
|---|---|
| **细节丢失** | 高频方向结构（小光源、锐利明暗）被抹成低频 |
| **振铃 / 负值** | 截断的吉布斯现象会让重建出现负值（"负光照"），工程里通常直接钳到 0 |

工程注释里提到：若振铃明显，可改用 **SH 窗口化**（如 Hanning 窗）在截断处平滑衰减系数。

```slang
// Engine/Shader/Shaders/RT_DDGI.slang:23-25
// 负值截断：辐照度物理上非负；SH 表示余弦波瓣时仍可能有轻微振铃（二阶更明显），
// 截断避免负辐照度进入画面。若后续振铃明显，可改为 SH 窗口化（如 Hanning）平滑抑制。
return max(e, 0.0);
```

### 4.4 卷积定理（zonal 核）—— Lambert 卷积的理论基础

若核函数只依赖夹角（$g(\omega\cdot\omega')$，称 zonal / 旋转对称），那么**卷积在系数域变成逐带缩放**：

$$
(f * g)(n) = \int_\Omega f(\omega)\,g(n\cdot\omega)\,\mathrm d\omega = \sum_{l,m}\hat g_l\,f_{lm}\,Y_{lm}(n),
\qquad \hat g_l = 2\pi\int_{-1}^{1} g(t)\,P_l(t)\,\mathrm dt
$$

**这条定理正是"探针存辐射度 SH、评估时乘 $A_l$"的全部理由**：投影端与评估端各做一半，中间的系数原封不动。

**工程对照**：这条定理的"为什么"在引擎里有一段注释直接写着（`RT_DDGI.slang:17-26`）：

```slang
// Engine/Shader/Shaders/RT_DDGI.slang:17-20
// 在方向 dir 处评估 **4** 系数 SH 的辐照度（bands 0/1，含 Lambert 卷积 Â_l）。
// 【为什么卷积在评估端】探针里存的是**辐射度** SH（投影时不乘 cos）：cos 依赖评估方向
// （法线），而探针存储时该方向未知 —— 卷积只能在这里以与 n 无关的 Â_l 形式施加。
// 常数与重建式都在 `SHCommon.slang`（本文件此前自带一份 kDDGI_SH_A0/A1，已收敛到那里）。
```

对应的评估实现（共享版本，不钳负）：

```slang
// Engine/Shader/Shaders/SHCommon.slang:79-85
// 由 4 系数**辐射度** SH 重建**辐照度**（施加 clamped-cosine 卷积 Â_l）。白炉下返回 π。
// 【为什么不做负值截断】截断与否是调用方的语义选择，不属共享实现：DDGI 探针求值需要截断
//（负辐照度无物理意义），而 Lumen 的诊断路径要保留原值才能统计偏差。
float3 SHRadianceToIrradiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 n) {
    return l0 * kSH_A0Y00
         + (l1 * n.y + l2 * n.z + l3 * n.x) * kSH_A1Y1;
}
```

**对照读法**：`l0*(A0*kSH_Y00)` 就是 $\hat A_0 L_{00}Y_{00}(n)$（$Y_{00}$ 与 $n$ 无关）；括号里三项是 $\hat A_1\sum_m L_{1m}Y_{1m}(n)$，其中 $Y_{1m}(n)$ 被展开成 $k_{Y1}\cdot n_{\{y,z,x\}}$。

### 4.5 加法定理（理解 zonal 核为什么可分离）

$$
\sum_{m=-l}^{l} Y_{lm}(\omega)\,Y_{lm}(\omega') = \frac{2l+1}{4\pi}P_l(\omega\cdot\omega')
$$

它把"两个方向的基函数乘积求和"化成"只依赖夹角的勒让德多项式"，这正是 §4.4 成立的原因。

---

## 5. Lambert 卷积

### 5.1 clamped cosine 核

漫反射要的辐照度是"入射光乘余弦"的半球积分：

$$
E(n) = \int_\Omega L(\omega)\,\max(0,\,n\cdot\omega)\,\mathrm d\omega
$$

把 $g(t)=\max(0,t)$ 代进 §4.4 的 $\hat g_l$：

$$
\hat A_l = 2\pi\int_{0}^{1} t\,P_l(t)\,\mathrm dt
$$

### 5.2 逐带求值（工程常数的来历）

| $l$ | 积分 | $\hat A_l$ | 数值 |
|---|---|---|---|
| 0 | $2\pi\int_0^1 t\,\mathrm dt = 2\pi\cdot\frac12$ | $\pi$ | 3.1415927 |
| 1 | $2\pi\int_0^1 t^2\,\mathrm dt = 2\pi\cdot\frac13$ | $2\pi/3$ | 2.0943951 |
| 2 | $2\pi\int_0^1 t\cdot\frac{3t^2-1}{2}\mathrm dt = \pi(\frac34-\frac12)$ | $\pi/4$ | 0.7853982 |
| 3 | 奇函数在 $[0,1]$ 上的加权积分为 0 | **0** | 0 |
| 4 | $-\pi/24$ | $-0.1308997$ | 负值（高阶会反号） |

工程只保留 $l\le1$，但注释保留了 $A_2$ 备查：

```slang
// Engine/Shader/Shaders/SHCommon.slang:42-45
// ---- Lambert 余弦卷积系数 Â_l = 2π·∫₀¹ t·P_l(t)dt ----
static const float kSH_A0 = 3.14159265358979323846;   // π
static const float kSH_A1 = 2.09439510239319549231;   // 2π/3
static const float kSH_A2 = 0.78539816339744830961;   // π/4（band 2 已移除，保留备查）
```

于是辐照度的 SH 重建式（$L$ 为**辐射度** SH 系数）：

$$
\boxed{\;E(n) = \sum_{l,m} \hat A_l\,L_{lm}\,Y_{lm}(n)\;}
$$

（`kSH_A1 = 2.09439510239319549231` 就是 $2\pi/3$；`SHCommon.slang` 头注释说明 band-2 的 $A_2$ 与 5 个 $l=2$ 项是**一起移除**的——"只改一边会让少的那一档变成静默偏移"。）

### 5.3 为什么"不能在投影端乘 cos"

`RT_DDGI.slang` 明确写了这条：

> 不能在「投影时乘 cos」——cos 依赖评估方向（法线），而探针存储时方向未知；卷积只能在评估端以与 n 无关的 $A_l$ 形式应用。

展开说：余弦项是 $\max(0,\,n\cdot\omega)$，其中 **$n$ 是"要算哪一点的法线"，投影时并不知道**（同一颗探针会被几万个不同法线的像素查询）。所以：

- **投影端**：只做与 $n$ 无关的事——把入射辐射度投影成 $L_{lm}$；
- **评估端**：拿到查询法线 $n$ 后，乘上 $A_l$ 并用 $Y_{lm}(n)$ 重建。

### 5.4 工程实现：预乘常数

评估端每次都要算 $A_l\cdot Y_{lm}(n)$。由于 $Y_{lm}$ 的"角度部分"就是 $x/y/z$，工程把常数预先乘好：

$$
\hat A_0\cdot Y_{00} = \pi\cdot\sqrt{\tfrac{1}{4\pi}} = \frac{\sqrt\pi}{2} = 0.8862269254527580
$$

$$
\hat A_1\cdot Y_{1} = \frac{2\pi}{3}\cdot\sqrt{\tfrac{3}{4\pi}} = 1.0233267079464886
$$

```slang
// Engine/Shader/Shaders/SHCommon.slang:47-53
// ---- 预乘形式：Â_l 与 Y_lm 归一化常数的乘积（评估端最省算力）----
//   kSH_A0Y00 = Â0·Y00 = π·√(1/4π) = √π/2 ≈ 0.8862269255
//   kSH_A1Y1  = Â1·Y1  = (2π/3)·√(3/4π)      ≈ 1.0233267079
static const float kSH_A0Y00 = 0.8862269254527580;
static const float kSH_A1Y1  = 1.0233267079464886;

static const uint kSHCoeffCount = 4;   // l ≤ 1 ⇒ (1+1)² = 4 个系数
```

> **精度说明**：解析值与源码里的十进制常量在末位差 1~3 ulp——$\hat A_0Y_{00}$ 解析为 `0.8862269254527579`、代码写 `...580`；$\hat A_1Y_1$ 解析为 `1.0233267079464883`、代码写 `...886`。这是十进制常量的正常舍入，**不是约定差异**；核对常数时不要因末位不同就以为用错了公式。

### 5.5 白炉解析判据（工程用它做验收）

设半球内入射辐射度恒为 1（**白炉**），则解析上 $E(n)\equiv\pi$（因为 $\int_\Omega\cos\theta\,\mathrm d\omega=\pi$）。用 SH 走一遍：

- **半球采样投影**（Lumen 的做法）：$l_0 = \int_{2\pi}1\cdot Y_{00}\,\mathrm d\omega = 2\pi Y_{00} = \sqrt\pi \approx 1.77245$；
- **整球采样投影**（DDGI 的做法，若球面处处为 1）：$l_0 = 4\pi Y_{00} = 2\sqrt\pi \approx 3.54491$。

两者都能得到 $E=\pi$：以半球为例
$E = \hat A_0 l_0 Y_{00} = \pi\cdot(2\pi Y_{00})\cdot Y_{00} = 4\pi^2\cdot\frac{1}{4\pi} = \pi$ ✓

**这就是工程里那条"解析验收"的来历**：

> 白炉下 $E(n)\equiv\pi$，且 $l_0\equiv 2\pi Y_{00}=\sqrt\pi$——**与采样方向、采样数无关**。

它的价值在于：**不需要参考图、不需要跑 GPU**，就能用一条断言钉死"基函数常数、卷积系数、重建式"三者是否自洽。`LumenSH.h` 专门做一份 C++ 镜像，就是为了让这条断言能在单测里跑。

**工程对照**：白炉不是"概念"，它在代码里是一个**显式分支 + 一条定点统计**：

```slang
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

```slang
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:97-100
InterlockedAdd(u_Stats[2], (uint)(abs(l0.r) * 4096.0 + 0.5));
if (furnace) InterlockedAdd(u_Stats[3], (uint)(abs(l0.r - 1.7724539) * 65536.0 + 0.5));
```

```cpp
// Engine/Render/Lumen/LumenSH.h:23-24（CPU 侧镜像里的同一个解析值）
inline constexpr double kSHWhiteFurnaceL0 = 1.7724538509055159;   // √π
```

**对照读法**：shader 里的 `1.7724539` 与 CPU 常量 `1.7724538509055159` 是同一个 $\sqrt\pi$；源码里的 `const float k = 6.28318530718 / (float)N;` 就是 $2\pi/N$（半球均匀采样的 $1/p$）；`u_Stats[2]` 累计的是 $l_0$ 本身（白炉下应等于 $\sqrt\pi$），`u_Stats[3]` 累计的是**与 $\sqrt\pi$ 的偏差**——于是"SH 投影是否正确"变成了一条可读数的、与场景无关的判据。

### 5.6 量纲：从 SH 到画面还要再除 π

SH 给的是**辐照度 $E$**；而引擎的统一约定是几何源返回**出射辐射度** $L_o = \mathrm{albedo}\times E/\pi$。所以链路是：

$$
\underbrace{L_{lm}}_{\text{radiance SH in probe}} \;\xrightarrow{\ \times \hat A_l Y_{lm}(n)\ }\; E(n)
\;\xrightarrow{\ \times \mathrm{albedo}/\pi\ }\; L_o
$$

工程里前半段在评估函数内完成、后半段在合成端完成：

```slang
// Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang:210-213
case GISOURCE_DDGI:
    // EvalDDGI_SH 已施加 Lambert 卷积 A_l → 返回**辐照度 E** → 需补 albedo/π
    return SampleDDGI(worldPos, N) * albedo * (1.0 / HE_PI);
```

同一个换算在另一条路径上也只出现一次（RTGI 的 miss 回退把 $E$ 当成"沿该方向的入射辐射度"来用）：

```slang
// Engine/Shader/Shaders/RayTracing/RT_GI.rgen.slang:90-99
// 量纲对齐：SampleDDGI 返回的是**辐照度 E**（EvalDDGI_SH 已施加 Lambert 卷积
// A_l），而命中的 ClosestHit 给的是**辐射度 L** —— 二者不能直接相加。
radiance += SampleDDGI(worldPos, dir) * (1.0 / 3.14159265);
```

**计数方法**：整条链路里 $1/\pi$ **只应出现一次**——要么在评估端（返回 $E$，合成端补 $\mathrm{albedo}/\pi$），要么在投影端（直接存 $E/\pi$）。两处都补就是差 $\pi$ 倍，都不补同样是差 $\pi$ 倍。

---

# 第二部分：引擎实现

## 6. 实现总览

### 6.1 两套 SH 系统

| 系统 | 存在形式 | 投影方式 | 求值方式 | 消费者 |
|---|---|---|---|---|
| **Lumen Screen Probe** | `ScreenProbe` 结构体里的 `shR/shG/shB`（通道主序，每通道 4 系数） | 每探针 N 条**均匀半球**光线，`2π/N` 归一化 | 逐像素按 GBuffer 法线重建辐照度 | `Lumen_ProbeIrradiance` → DeferredLighting |
| **DDGI 探针网格** | 每探针 `float4 sh[4]`（系数主序，RGB 在 `.rgb`） | 每探针 N 个**整球** Fibonacci 方向，`4π/N` 归一化 | 8 探针三线性插值 + 按法线求辐照度 | DeferredLighting（`GISOURCE_DDGI`）、RT GI miss 回退 |

两套系统**共用同一份基函数/重建/采样方向实现**（`SHCommon.slang`），并且可以互相喂数据：DDGI 的投影可以把 Screen Probe 的 SH 当作逐方向辐射度来源（§9.5）。

### 6.2 数据流

```
                    ┌──────────────────────────── Lumen Screen Probe ────────────────────────────┐
ScreenProbe_Gather ─┤ 每(探针,光线) 辐射度  ──▶ SHProject ──▶ shR/shG/shB ──▶ Filter ──▶ shR/shG/shB ──▶ ProbeIrradiance ──▶ 屏幕辐照度纹理
                    └──────────────────────────────────────────────────────────────────────────────┘
                                                                         │
                                                                         ▼ （可选：作为 DDGI 的逐方向输入）
                    ┌──────────────────────── DDGI ────────────────────────────────────────────────┐
DDGI_Trace ────────▶│ 每探针 Fibonacci 方向辐射度 ──▶ DDGI.comp 投影 ──▶ 4×float4 SH ──▶ RT_DDGI 求值 ──▶ DeferredLighting / RT_GI
                    └──────────────────────────────────────────────────────────────────────────────┘
```

逐步骤口径：

```
入射辐射度（追踪/GBuffer/IBL）
   ↓ 沿 N 个方向采样（DDGI：整球 Fibonacci；Lumen：法线半球）
   ↓ 乘基函数累加            sh[j] += L · Y_j(ω)
   ↓ 乘 MC 权重              × 4π/N  或  × 2π/N
   ↓ 时域混合                与历史 lerp
[探针缓冲：每探针 4 × float4 的辐射度 SH]
   ↓ 空间插值                8 邻域三线性（DDGI）
   ↓ 评估（Lambert 卷积）    E(n) = Σ Â_l · L_lm · Y_lm(n)，再 max(·,0)
   ↓ 乘 albedo/π → L_o       合成端
```

### 6.3 代码地图

| 位置 | 角色 |
|---|---|
| `Engine/Shader/Shaders/SHCommon.slang` | 唯一实现：基函数、辐射度→辐照度、Fibonacci 方向 |
| `Engine/Render/Lumen/LumenSH.h` | C++ 镜像：白炉常量、重建式、探针采样参考实现（供单测） |
| `Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang` | Screen Probe 投影 |
| `Engine/Shader/Shaders/Lumen/ScreenProbe_Filter.comp.slang` | Screen Probe 空间 + 时域滤波 |
| `Engine/Shader/Shaders/Lumen/Lumen_ProbeIrradiance.comp.slang` | 逐像素辐照度重建 |
| `Engine/Shader/Shaders/Lumen/ScreenProbe.slang` | 探针结构体（含 `shR/shG/shB`） |
| `Engine/Shader/Shaders/Lumen/ScreenProbeSampling.slang` | 探针光线方向（PCG 随机与半球/GGX 采样） |
| `Engine/Shader/Shaders/GI/DDGI.comp.slang` | DDGI 投影 + 时域混合 |
| `Engine/Shader/Shaders/RT_DDGI.slang` | DDGI 求值 + 三线性插值 |
| `Engine/Shader/Shaders/RayTracing/DDGI_Trace.rgen.slang` | 与投影共享 Fibonacci 方向 |
| `Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang` | 合成端（`GISOURCE_DDGI`） |
| `Engine/Shader/Shaders/RayTracing/RT_GI.rgen.slang` | RT GI miss 回退 |
| `Engine/Render/GI/GI_DDGI.h` | DDGI 探针布局与约束说明 |
| `Engine/Render/Lumen/LumenScene_ProbeFilter.cpp` | 滤波 pass 的 push constant / 历史结构体 |
| `Engine/Render/Lumen/LumenScene_Irradiance.cpp` | 重建 pass 的 PSO 与调度 |
| `Engine/Render/Lumen/LumenScene_SurfaceCache.cpp` | 投影 pass 的 PSO |

---

## 7. 共享实现

### 7.1 常数、基函数与重建（SHCommon.slang）

```slang
// Engine/Shader/Shaders/SHCommon.slang:38-70
static const float kSH_Y00 = 0.28209479177387814;   // 0.5·√(1/π)
static const float kSH_Y1  = 0.4886025119029199;    // 0.5·√(3/π)
static const uint  kSHCoeffCount = 4;               // l ≤ 1 ⇒ (1+1)² = 4 个系数

float SHBasis(uint i, float3 d) {
    if (i == 0u) return kSH_Y00;
    if (i == 1u) return kSH_Y1 * d.y;   // m = -1
    if (i == 2u) return kSH_Y1 * d.z;   // m =  0
    return kSH_Y1 * d.x;                // m = +1（i == 3）
}

// 由 4 系数**辐射度** SH 在方向 d 上求值（**不做**余弦卷积）⇒ 结果仍是辐射度 L(d)。
float3 SHEvalRadiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 d) {
    return l0 * kSH_Y00 + (l1 * d.y + l2 * d.z + l3 * d.x) * kSH_Y1;
}

// 由 4 系数**辐射度** SH 重建**辐照度**（施加 clamped-cosine 卷积 Â_l）。白炉下返回 π。
float3 SHRadianceToIrradiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 n) {
    return l0 * kSH_A0Y00
         + (l1 * n.y + l2 * n.z + l3 * n.x) * kSH_A1Y1;
}
```

### 7.2 Fibonacci 采样方向

```slang
// Engine/Shader/Shaders/SHCommon.slang:94-98
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}
```

- 极角按 `(i+0.5)/n` 在 $\cos\theta$ 上均匀（避开两极点退化），方位角按黄金角递增，方向在球面上近似**面积均匀**；
- 它被**投影端**（`DDGI.comp`）和**追踪端**（`DDGI_Trace.rgen`）共用——两侧必须取到同一组方向，否则同一条采样在不同路径下不是同一个点；
- 集中到 `SHCommon.slang` 之后，这条一致性由"同一个函数"保证，不再依赖两份拷贝逐位一致。

### 7.3 两种存储布局

`SHCommon.slang` 头注释明确支持两种布局，具体打包/解包由调用点完成：

| 布局 | 形式 | 使用者 | 特点 |
|---|---|---|---|
| **系数主序** | `float4 sh[4]`，`sh[i].rgb` = 第 i 个系数的 RGB | DDGI 探针缓冲 | 按系数连续，求值时 `sh[0].rgb … sh[3].rgb` 顺序读；每探针 4×16B = 64B |
| **通道主序** | `float4 shR, shG, shB`，每个通道 4 个系数 | Lumen ScreenProbe | 按通道连续，便于逐通道滤波/统计；结构体里三个 float4 并排 |

两种布局在数学上等价，只是内存访问模式不同：DDGI 的求值循环按系数展开，Lumen 的滤波按通道展开。

### 7.4 C++ 镜像（LumenSH.h）

```cpp
// Engine/Render/Lumen/LumenSH.h:22-45
inline constexpr double kSHY00 = 0.28209479177387814;
inline constexpr double kSHY1  = 0.48860251190291992;
/// 白炉下 0 阶系数的解析值：2π·Y00 = √π
inline constexpr double kSHWhiteFurnaceL0 = 1.7724538509055159;   // √π

inline double SHBasis(uint32_t i, double dx, double dy, double dz) {
    switch (i) {
        case 0: return kSHY00;
        case 1: return kSHY1 * dy;
        case 2: return kSHY1 * dz;
        default: return kSHY1 * dx;
    }
}

inline double SHRadianceToIrradiance(double l0, double l1, double l2, double l3,
                                     double nx, double ny, double nz) {
    constexpr double kA0 = 3.14159265358979323846;
    constexpr double kA1 = 2.09439510239319549231;   // 2π/3
    return l0 * (kA0 * kSHY00)
         + (l1 * (kSHY1 * ny) + l2 * (kSHY1 * nz) + l3 * (kSHY1 * nx)) * kA1;
}
```

着色器侧不方便做单元测试，而"白炉下 0 阶项等于 √π"是**解析可验证**的，于是用 C++ 版本把这条不变量钉死：任何一侧改动都会被单测或运行期读数抓住。文件里还有与 shader 逐位一致的 `LumenProbePcg` / `LumenProbeU01` / `LumenSampleUniformHemisphere`，保证 CPU 参考与 GPU 用同一组方向。

### 7.5 唯一出处的演进与构建登记

**收敛前后**：同一组 SH 常数与重建公式此前散在 4 个着色器里，各有各的写法与精度：

| 原位置 | 原来定义了什么 |
|---|---|
| `GI/DDGI.comp.slang` | `kSH_Y00`、`SH_Band1`、`SHBasis(float3,out float[4])`、`FibonacciSphere`，另有一处把 $Y_{00}/Y_1$ **命名成 `A0/A1`** 的局部常量 |
| `RT_DDGI.slang` | `kDDGI_SH_Y00`、`kDDGI_SH_B1`、预乘 `kDDGI_SH_A0/A1`、`EvalDDGI_SH` 的求值式 |
| `Lumen/ScreenProbeSampling.slang` | `kSH_Y00`、`kSH_Y1`、`SHBasis(uint,float3)`、`SHRadianceToIrradiance` |
| `RayTracing/DDGI_Trace.rgen.slang` | `FibonacciSphere`（注释要求"必须与 DDGI.comp 逐位一致"） |

现在**只剩一处**（`SHCommon.slang`）：基函数常数、卷积系数、预乘常数、两个 `SHBasis` 重载、`SHEvalRadiance`/`SHRadianceToIrradiance`、`FibonacciSphere`、以及文件头里的约定/轴序/白炉说明。

**为什么要合**：这组常数构成"**投影端与评估端必须同约定**"的不变量（阶数、轴序、卷积系数、采样方向四项）。分散时只能靠注释要求"两份拷贝逐位一致"，任何一次改动都可能只改一半。

**构建侧**：`SHCommon.slang` 已登记进 `Engine/Shader/CMakeLists.txt` 的 `SLANG_INCLUDES`（同时补上此前漏登记的 `Lumen/ScreenProbeSampling.slang` 与 `Lumen/LumenFarField.slang`——漏登记的后果是改这两个文件不会触发重编）。登记后改 SHCommon 会触发全部着色器重编：实测 146 个 `.spv.h` 重新生成、构建 0 错误。这正是"共享契约变更必须所有使用者一起重编"的正确代价。

**数值影响（实测）**：字面量统一为完整精度后，与原 10 位截断形式在 fp32 下舍入到同一个 float（差值 ~2.6e-11，远小于该量级的 fp32 间隔 3e-8）⇒ 常数本身零变化。评估式的结合顺序由"逐项乘 $\hat A_1Y_1$ 再相加"改为"$(\sum_i l_i y_i)\cdot\hat A_1Y_1$"，100 万组随机输入的对比结果是：相对偏差均值 < 1 ulp、最大 12 ulp；白炉判据 $E\equiv\pi$ 仍在 1 ulp 内。也就是说：这次重构在画面上不可见，而 §11 的验收判据全部照旧可用。

---

## 8. 站点 A：Lumen Screen Probe

### 8.1 链路

```
ScreenProbe_Gather（每 (探针, 光线) 追踪辐射度）
   → Lumen_ScreenProbe_SHProject（投影成 4 系数 × RGB）
   → ScreenProbe_Filter（3×3 空间 YCoCg AABB + 时域 EMA）
   → Lumen_ProbeIrradiance（逐像素按法线重建辐照度）
   → DeferredLighting（合成间接漫反射）
```

### 8.2 投影

**输入**：步骤 21 追踪、步骤 22 着色的每 `(探针, 光线)` 辐射度（`u_Radiance`）与命中标记（`u_RayResult`）。

**估计量**：光线是半球均匀采样，$p = \frac{1}{2\pi}$，所以第 i 个系数

$$
l_i \approx \frac{1}{N}\sum_{j=1}^{N}\frac{L_j\,Y_i(d_j)}{1/(2\pi)}
= \frac{2\pi}{N}\sum_{j=1}^{N} L_j\,Y_i(d_j)
$$

```slang
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:59-84
float3 l0 = 0, l1 = 0, l2 = 0, l3 = 0, eRef = 0;
for (uint r = 0u; r < N; ++r) {
    const float2 u = ProbeRayRandom(probeKey * N + r, dims.z);
    const float3 d = (mode.x == 0u) ? SampleGgxReflection(n, roughness, u, 0.002)
                                    : SampleUniformHemisphere(n, u);

    const float4 s = u_Radiance[flat];
    const bool   rayHit = u_RayResult[flat].y > 0.5f;
    const float3 L = furnace ? float3(1.0, 1.0, 1.0)
                             : (rayHit ? s.xyz : float3(0.0, 0.0, 0.0));

    l0 += L * SHBasis(0u, d);
    l1 += L * SHBasis(1u, d);
    l2 += L * SHBasis(2u, d);
    l3 += L * SHBasis(3u, d);
    eRef += L * max(0.0, dot(d, n));         // 参考：逐光线 cos 加权求和
}

// 均匀半球 pdf = 1/(2π) ⇒ 系数 = (2π/N)·Σ
const float k = 6.28318530718 / (float)N;
l0 *= k; l1 *= k; l2 *= k; l3 *= k;
eRef *= k;
```

**逐通道写回**：

```slang
// 同一文件 86-93
probe.shR = float4(l0.r, l1.r, l2.r, l3.r);
probe.shG = float4(l0.g, l1.g, l2.g, l3.g);
probe.shB = float4(l0.b, l1.b, l2.b, l3.b);
u_Probes[tid.x] = probe;

const float3 eSh = SHRadianceToIrradiance(l0, l1, l2, l3, n);   // SH 重建的 E
u_IrradSh[tid.x]  = float4(eSh, 1.0);
u_IrradRef[tid.x] = float4(eRef, 1.0);                          // 逐光线求和的 E_ref
```

**为什么同时算 `E_sh` 和 `E_ref`**：一个是"SH 带限 + 卷积"的结果，一个是"用同样有限的光线逐条累加 cos"的参考值。两者之差就是 SH 表示本身引入的误差（白炉下应为 0），交给 CPU 统计——不是猜的，而是可读数的。

**有效光线语义**：非白炉时，只有命中几何的光线贡献辐射度（未命中贡献 0，相当于把"天空"当作 0）；白炉时全部有效。缺页的中性值 0.18 要保留，否则 8 条光线里只剩约 0.9 条有效，SH 几乎积不出东西。

**统计缓冲**（`u_Stats`）：

| 槽位 | 含义 |
|---|---|
| 0 | 有效探针数 |
| 1 | 有效光线数 |
| 2 | `Σ |l0|·4096`（l0 本身，白炉下应 ≈ √π） |
| 3 | `Σ |l0 − √π|·65536`（白炉偏差） |

### 8.3 滤波

**为什么需要**：每探针只有 8~16 条光线，单探针 SH 的蒙特卡洛方差很大；直接重建会在画面上形成探针级的闪烁与色块。

**空间滤波**：对每个探针，取 3×3 单元邻域（最多 9 个）的探针，**逐 SH 系数**把该系数的 RGB 当作颜色做 YCoCg 变换，计算均值 μ 与标准差 σ，构造 AABB：

$$
[\mu - \gamma\sigma,\ \mu + \gamma\sigma]
$$

把邻居值 clamp 进 AABB 后求平均。默认 $\gamma = 1.0$。

**为什么用 YCoCg**：亮度与色度解耦。截断时优先压掉色度离群值，亮度均值更稳；直接对 RGB 做 AABB 容易出现"某个通道的离群值把整体亮度拉偏"。

```slang
// Engine/Shader/Shaders/Lumen/ScreenProbe_Filter.comp.slang:138-190
for (uint k = 0u; k < 4u; ++k) {
    float3 acc = float3(0.0), acc2 = float3(0.0);
    for (uint s = 0u; s < n; ++s) {
        const ScreenProbe q = u_Probes[nbr[s]];
        const float3 ycc = RgbToYCoCg(float3(q.shR[k], q.shG[k], q.shB[k]));
        nbrY[s][k] = ycc;
        acc  += ycc;
        acc2 += ycc * ycc;
    }
    const float3 mu = acc / (float)n;
    meanY[k] = mu;
    stdY[k]  = sqrt(max(acc2 / (float)n - mu * mu, float3(0.0)));
}

for (uint k = 0u; k < 4u; ++k) {
    const float3 lo = meanY[k] - gamma * stdY[k];
    const float3 hi = meanY[k] + gamma * stdY[k];
    float3 acc = float3(0.0);
    for (uint s = 0u; s < n; ++s) {
        acc += clamp(nbrY[s][k], lo, hi);
    }
    spatialY[k] = acc / (float)n;
}
```

**时域滤波**：把上一帧的探针重投影到当前帧（比较世界位置距离与法线一致性），通过门限后把历史 SH 也 clamp 进同一个 AABB，再与空间结果做 EMA：

$$
\text{out} = \operatorname{lerp}\big(\text{spatial},\ \operatorname{clamp}(\text{hist}),\ \alpha\big)
$$

默认 $\alpha = 0.9$（历史权重）。

```slang
// 同一文件 196-216
const float dist = length(hist.position.xyz - p.position.xyz);
const float tol  = params.w * max(clip.w, 1.0);   // 容差随视图深度缩放
const float ndot = dot(h.normal.xyz, normalize(hist.normal.xyz));
if (hist.position.w > 0.5 && dist <= tol && ndot >= 0.9) {
    const float a = clamp(params.y, 0.0, 0.98);
    for (uint k = 0u; k < 4u; ++k) {
        const float3 hv = RgbToYCoCg(float3(hist.shR[k], hist.shG[k], hist.shB[k]));
        const float3 lo = meanY[k] - gamma * stdY[k];
        const float3 hi = meanY[k] + gamma * stdY[k];
        resultY[k] = lerp(spatialY[k], clamp(hv, lo, hi), a);
    }
}
```

**模式**（环境变量 `HE_LUMEN_PROBE_FILTER`，只在建资源时读一次）：

| 模式 | 行为 |
|---|---|
| 0 `off` | 直通（A/B 基线），仍写历史 |
| 1 `spatial` | 只做空间 AABB |
| 2 `spatial+time`（默认） | 空间 + 时域 EMA |
| 3 `keepmean` | 在模式 2 基础上，用"截断前/后的亮度之和比"把亮度均值补回来（reSTIR 式 bias correction，限幅 0.5~2.0） |

### 8.4 逐像素重建

**为什么不直接用探针法线**：SH 是**方向函数**，同一探针的 4 个系数可以重建出任意方向的辐照度。屏幕上的像素与探针法线不同（尤其探针是按 16×16 单元合并的），所以必须用**像素自己的 GBuffer 法线** $n$ 去求值，否则物体会显得"平"、光照方向感消失。

```slang
// Engine/Shader/Shaders/Lumen/Lumen_ProbeIrradiance.comp.slang:44-72
const uint probeIndex = u_CellProbe[cell];
// 逐通道各取自己的 4 个系数（shR 的 xyzw = R 通道的 l0..l3）
const float3 l0 = float3(probe.shR.x, probe.shG.x, probe.shB.x);
const float3 l1 = float3(probe.shR.y, probe.shG.y, probe.shB.y);
const float3 l2 = float3(probe.shR.z, probe.shG.z, probe.shB.z);
const float3 l3 = float3(probe.shR.w, probe.shG.w, probe.shB.w);

const float3 n = DecodeNormal(u_GBufferNormal.Load(int3((int2)px, 0)));
const float3 eIrradiance = SHRadianceToIrradiance(l0, l1, l2, l3, n);

const float3 albedo = u_GBufferAlbedo.Load(int3((int2)px, 0)).rgb;
const float3 outRadiance = (flags.x > 0.5) ? eIrradiance
                                           : (albedo * eIrradiance / 3.14159265359);
u_Irradiance[px] = float4(outRadiance, 1.0);
```

**量纲**：`SHRadianceToIrradiance` 返回的是辐照度 $E$；漫反射出射辐亮度是 $\rho E/\pi$（Lambert BRDF 自带 $1/\pi$），所以非诊断模式下再乘 `albedo/π`。`flags.x` 是诊断开关：打开时直接输出 $E$，便于和白炉/参考值对比。

**逐通道取系数**是最容易写错的地方：`shR = (l0,l1,l2,l3)` 属于 R 通道，重建时 `l0` 必须由 `shR.x, shG.x, shB.x` 组合而成。

### 8.5 数据结构与 C++ 接线

**探针结构体**（96 B，6×16 B）：

```slang
// Engine/Shader/Shaders/Lumen/ScreenProbe.slang:30-38
SP_STRUCT ScreenProbe {
    SP_FLOAT4 position;   // xyz = 世界坐标, w = 有效性
    SP_FLOAT4 normal;     // xyz = 世界法线, w = 线性深度
    SP_FLOAT4 uv;         // xy = 屏幕 uv, zw = tile 坐标
    SP_FLOAT4 shR;        // 二阶 SH 的 R 通道（L1：4 系数）
    SP_FLOAT4 shG;
    SP_FLOAT4 shB;
    SP_UINT4  meta;
};
```

**滤波的 push constant 与历史**（80 B）：

```cpp
// Engine/Render/Lumen/LumenScene_ProbeFilter.cpp:44-58
struct ProbeFilterPC {
    float4x4 prevViewProj;
    uint4    dims;      // x = 探针数, y = cellsX, z = cellsY, w = 模式
    float4   params;    // x = γ, y = α, z = 历史可用, w = 重投影相对容差
};

struct ProbeFilterHistory {
    float4 position;
    float4 normal;
    float4 shR, shG, shB;
};
```

**pass 入口**：

| 函数 | 文件 | 作用 |
|---|---|---|
| `LumenScene::RunScreenProbeSHProject` | `LumenScene.h` | 投影 pass |
| `LumenScene::RunScreenProbeFilter` | `LumenScene_ProbeFilter.cpp` | 滤波 pass |
| `LumenScene::RunProbeIrradiance` | `LumenScene_Irradiance.cpp` | 逐像素重建 pass |

---

## 9. 站点 B：DDGI

### 9.1 探针布局

```cpp
// Engine/Render/GI/GI_DDGI.h:20-21
// 探针数据布局（StructuredBuffer，每探针 4 float4 —— 步骤 30 的 L5 升级）：
//   [0..3]  SH 系数 band 0/1（4×float4，RGB 存在 .rgb）
// 三阶 9 系数 → 二阶 4 系数：与 Screen Probe 的 shR/shG/shB 同构，每探针 256 B → 64 B。
```

每探针 64 B = 4 个 `float4`；`DDGI.comp` 的 `kStride = 4` 与 `RT_DDGI.slang` 的 `kDDGI_Stride` **必须同时改**（头文件注释已标注这条耦合）。

### 9.2 投影（整球 Fibonacci 方向）

DDGI 探针覆盖的是**整球**入射辐射度（世界空间，与视角无关），所以采样方向用整球 Fibonacci，归一化因子是 $4\pi/N$：

$$
l_i \approx \frac{4\pi}{N}\sum_{j=1}^{N} L_j\,Y_i(d_j)
$$

```slang
// Engine/Shader/Shaders/GI/DDGI.comp.slang:59-208
static const uint kStride = 4;   // 步骤 30：每探针 4 个 SH 系数（16 → 4）

void WriteSH(uint probeIndex, float4 sh[4]) {
    const uint base = probeIndex * kStride;
    for (uint i = 0u; i < kStride; i++) {
        u_Probes[base + i] = (i < 4u) ? sh[i] : float4(0, 0, 0, 0);
    }
}

// ……每个探针的采样循环……
for (int i = 0; i < numSamples; i++) {
    float3 dir = FibonacciSphere(i, numSamples);     // 与 DDGI_Trace 共用方向
    float3 samplePos = probePos + dir * stepDist;

    float3 radiance = float3(0.0);
    bool   valid    = false;
    if (spIndex != 0xFFFFFFFFu) {
        // 主路径：Screen Probe 的 4 系数辐射度 SH 沿本探针采样方向求值
        radiance = EvalScreenProbeRadiance(spIndex, dir);
        valid    = dot(radiance, radiance) > 0.0001;
    } else if (u_Flags.w > 0.5) {
        // A 路径：DDGI_Trace 硬件光追给出的逐方向辐射度
        float4 traced = u_TracedRadiance[probeIndex * uint(numSamples) + uint(i)];
        radiance = traced.rgb;
        valid    = dot(radiance, radiance) > 0.0001;
    } else if (u_Flags.x > 0.5) {
        // B 路径：RSM 世界空间辐射度（VPL 出射）
        // ……采样 u_RSMRadianceMap……
    } else {
        // 回退：IBL 环境辐射度（视角无关）
        radiance = u_IBLIrradiance.SampleLevel(u_LinearSampler, dir, 0).rgb;
    }

    if (!valid) continue;

    float basis[4];
    SHBasis(dir, basis);
    for (int j = 0; j < 4; j++) sh[j].rgb += radiance * basis[j];
    validSamples += 1.0;
}

// 归一化：蒙特卡洛积分缩放因子 4π/N × 强度
if (validSamples > 0.0) {
    float scale = (4.0 * 3.14159265359) / validSamples * u_Params.x;
    for (int j = 0; j < 4; j++) sh[j].rgb *= scale;
} else {
    sh[0] = float4(0.02, 0.03, 0.08, 0.0);   // 完全无可见采样 → 弱环境光兜底
}

// 时间混合：与上一帧历史 SH 系数 lerp（减少闪烁，加速收敛）
if (historyValid && blendAlpha > 0.0) {
    uint base = probeIndex * kStride;
    for (int j = 0; j < 4; j++) {
        float4 hist = u_ProbeHistory[base + j];
        sh[j].rgb = lerp(sh[j].rgb, hist.rgb, blendAlpha);
    }
}
```

几个关键点：

- **四种辐射度来源**按优先级选择：Screen Probe SH 求值 → 硬件光追逐方向 → RSM → IBL；这保证探针在任何配置下都有数据；
- **`u_Params`**：`x = intensity`、`y = numSamples`、`z = blendAlpha`、`w = historyValid`；
- **`validSamples` 而不是 `numSamples`** 做归一化：无效采样（未命中/无数据）不参与积分，避免把 0 当作真实辐射度拉低结果。这实质是一个**比值估计量**，白炉下仍然得到 1，但对非恒定环境有偏；
- **完全无采样时的兜底** `(0.02, 0.03, 0.08)`：宁可给一点冷色环境光，也不要让整片探针变黑（"看起来正常但静默失去 GI"是更难查的 bug）；
- **历史混合**直接对 4 个系数的 RGB 做 lerp，因为 SH 是线性的，逐系数混合等价于对辐射度函数混合；
- **已知瑕疵**：用户强度 `intensity` 被乘进了存储的 SH ⇒ 缓冲里存的量带了用户缩放，改强度需要重新收敛（"表示与调参混在一起"）。

### 9.3 求值：辐照度 + 三线性插值

在方向 `dir` 上求辐照度（**含 Lambert 卷积**）：

```slang
// Engine/Shader/Shaders/RT_DDGI.slang:17-27
float3 EvalDDGI_SH(float4 sh[4], float3 dir) {
    const float3 e = SHRadianceToIrradiance(sh[0].rgb, sh[1].rgb, sh[2].rgb, sh[3].rgb, dir);
    // 负值截断：辐照度物理上非负；SH 表示余弦波瓣时仍可能有轻微振铃（二阶更明显），
    // 截断避免负辐照度进入画面。若后续振铃明显，可改为 SH 窗口化（如 Hanning）平滑抑制。
    return max(e, 0.0);
}
```

**为什么截断在调用方而不是共享实现**：`SHCommon.slang` 的注释明确说"截断与否是调用方的语义选择"。DDGI 的求值端需要截断（负辐照度无物理意义），而 Lumen 的诊断路径要保留原值才能统计偏差。

**三线性插值**：世界坐标落在探针网格的某个 cell 里，取包围它的 8 个探针，按 `frac` 做三线性权重，每个探针用**同一个法线**求辐照度后加权平均：

```slang
// Engine/Shader/Shaders/RT_DDGI.slang:29-96
float3 gridCoord = (worldPos - u_DDGIGridOrigin.xyz) / u_DDGIGridSize.w;
int3   baseCoord = int3(floor(gridCoord));
float3 frac      = frac(gridCoord);
baseCoord = clamp(baseCoord, int3(0, 0, 0), int3(...));   // 钳到网格边界

float3 result = 0.0, totalWeight = 0.0;
for (int dz = 0; dz <= 1; dz++)
for (int dy = 0; dy <= 1; dy++)
for (int dx = 0; dx <= 1; dx++) {
    int3 coord = clamp(baseCoord + int3(dx, dy, dz), ...);
    float weight = (1.0 - abs(float(dx) - frac.x))
                 * (1.0 - abs(float(dy) - frac.y))
                 * (1.0 - abs(float(dz) - frac.z));
    if (weight <= 0.0) continue;

    uint   bufBase = baseIdx * kDDGI_Stride;
    float4 sh[4];
    for (int i = 0; i < 4; i++) sh[i] = u_DDGIProbes[bufBase + i];

    result      += EvalDDGI_SH(sh, normal) * weight;
    totalWeight += weight;
}
return result / max(totalWeight, 0.001);
```

注释记录了两种"更聪明"的权重方案（距离权重、smoothstep）都被实测证伪——真正的问题是**网格分辨率**（最长轴 16 → 24/32 才把格子感从 35% 降到 22%），所以代码保留纯三线性，不做无谓的改动。

### 9.4 消费端

**延迟光照**（`GISOURCE_DDGI` 分支）：

```slang
// Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang:210-213
case GISOURCE_DDGI:
    // EvalDDGI_SH 已施加 Lambert 卷积 A_l → 返回辐照度 E → 需乘 albedo/π
    return SampleDDGI(worldPos, N) * albedo * (1.0 / HE_PI);
```

**RT GI 的 miss 回退**：

```slang
// Engine/Shader/Shaders/RayTracing/RT_GI.rgen.slang:90-99
// 量纲对齐：SampleDDGI 返回的是辐照度 E（已施加 Lambert 卷积），
// 而命中的 ClosestHit 给的是辐射度 L —— 二者不能直接相加。
// 这里把 DDGI 当作「沿该方向的入射辐照度」，按局部环境近似均匀的假设做 E/π = L 换算。
radiance += SampleDDGI(worldPos, dir) * (1.0 / 3.14159265);
```

两个消费端都必须做 `E → L` 的 `/π` 换算，这是 SH 求值端"返回辐照度"的直接后果；写成注释是因为这一步漏掉不会有编译错误，只会让间接光偏亮 π 倍。

### 9.5 与 Screen Probe 的接口

DDGI 可以把 Screen Probe 的 SH 当作逐方向的辐射度来源（`DDGI.comp` 的主路径）：

```slang
// Engine/Shader/Shaders/GI/DDGI.comp.slang:117-124
float3 EvalScreenProbeRadiance(uint idx, float3 dir) {
    const ScreenProbe sp = u_ScreenProbes[idx];
    // 通道主序布局：shR/shG/shB 每个通道 4 个系数
    const float3 l0 = float3(sp.shR.x, sp.shG.x, sp.shB.x);
    const float3 l1 = float3(sp.shR.y, sp.shG.y, sp.shB.y);
    const float3 l2 = float3(sp.shR.z, sp.shG.z, sp.shB.z);
    const float3 l3 = float3(sp.shR.w, sp.shG.w, sp.shB.w);
    const float3 r  = SHEvalRadiance(l0, l1, l2, l3, dir);   // 辐射度，不卷积
    return max(r, 0.0);
}
```

注意这里用的是 **`SHEvalRadiance`（不卷积）**：Screen Probe 存的是辐射度 SH，求出的也是辐射度；DDGI 再把它当作逐方向入射辐射度，重新投到自己的整球 SH 上。这条链路把"屏幕空间探针"的细节注入"世界空间探针"，同时也说明为什么两套系统的 SH 必须是同一套基函数与轴序。

---

## 10. 跨站点约定与踩坑

### 10.1 三处约定必须完全一致

| 约定 | 工程值 | 出处 |
|---|---|---|
| $Y_{00}$ | 0.28209479177387814 | `SHCommon.slang`、`LumenSH.h` |
| $Y_{1}$（$l=1$ 归一化） | 0.48860251190291992 | `SHCommon.slang`、`LumenSH.h` |
| $l=1$ 轴序 | $(y,z,x)$ 对应 $m=(-1,0,1)$ | `SHCommon.slang`、`LumenSH.h` |
| $\hat A_0,\hat A_1$ | $\pi$、$2\pi/3$ | `SHCommon.slang`、`LumenSH.h` |
| 探针存储 | 每探针 4 × `float4`，RGB 存系数、A 闲置 | `GI_DDGI.h`、`RT_DDGI.slang` |
| 布局耦合 | `DDGI.comp` 的 `kStride` 与 `RT_DDGI` 的 `kDDGI_Stride` 必须同步 | `GI_DDGI.h` 注释 |
| 采样方向一致 | DDGI 投影与追踪共用 `FibonacciSphere`；Screen Probe 投影与追踪共用 `ProbeRayRandom` | `SHCommon.slang`、`ScreenProbeSampling.slang` |
| 量纲 | SH 求值返回 E，合成端要乘 `albedo/π` 或 `1/π` | `DeferredLighting`、`RT_GI` |
| 精度 | 常数用完整精度字面量 | `SHCommon.slang` 头注释 |

### 10.2 归一化约定的三种写法（混用必错）

| 约定 | $Y_{00}$ | $l=1$ 常数 | 说明 |
|---|---|---|---|
| 本文/工程 | $0.5\sqrt{1/\pi}=0.28209$ | $0.5\sqrt{3/\pi}=0.48860$ | Ramamoorthi & Hanrahan，**正交归一** |
| 未归一化（只留角度部分） | $1$ | 直接用 $x,y,z$ | 系数含 $1/4\pi$ 之类的因子，易与上面混算 |
| 含 $\sqrt{2l+1}$ 的 zonal 写法 | $\sqrt{1/4\pi}$ | $\sqrt{3/4\pi}$ | 与前两者只差常数，但 $A_l$ 要跟着改 |

**判断方法**：用 §3.4 的 $\int_\Omega Y_{00}^2\,\mathrm d\omega = 1$ 与 §5.5 的白炉解析值 $\sqrt\pi$ 去校验——只要这两个数对得上，整套约定就是自洽的。

### 10.3 投影的离散化：域、pdf、有效样本

一般的蒙特卡洛写法：

$$
f_{lm}\approx\frac{1}{N}\sum_{j=1}^{N}\frac{f(\omega_j)Y_{lm}(\omega_j)}{p(\omega_j)}
$$

| 采样域 | pdf | 权重 |
|---|---|---|
| 整球均匀 | $1/4\pi$ | $4\pi/N$（DDGI） |
| 半球均匀 | $1/2\pi$ | $2\pi/N$（Lumen） |
| 余弦加权（半球） | $\cos\theta/\pi$ | $\pi/N \cdot 1/\cos\theta$（$l=0$ 的估计量质量最好，高阶方差大） |

**常见错误**：换了采样分布而没换权重（结果整体缩放错）、少了方向归一化（非单位向量会让基函数值错）。

### 10.4 截断到 $l\le1$ 能表达什么

| 能 | 不能 |
|---|---|
| 平均亮度（$l=0$） | 小光源在球面上的尖锐亮斑 |
| 某一方向的整体偏亮/偏暗（$l=1$） | "赤道亮两极暗"这类二次结构（需 $l=2$） |
| 平滑的环境梯度 | 镜面反射那种极窄的波瓣（SH 完全不适合，改用 GGX 预滤波） |

**判断标准**：目标函数越接近"低阶多项式×方向"，SH 越划算；越尖锐，越不该用 SH。

### 10.5 负值与振铃

- 截断会让重建出现负值 ⇒ 物理上非法（负辐照度），工程一律 `max(·,0)`；
- 钳负会**丢能量**（画面轻微变暗），但对低频 GI 通常可接受；
- 若振铃明显：窗口化（Hanning）、提高阶数、或改回逐方向采样。

### 10.6 SH 的旋转：为什么不能"直接转系数"

- **带内混合**：旋转会把同一带的系数互相混合，$l$ 带需要 $(2l+1)\times(2l+1)$ 的**旋转矩阵**（Wigner D 矩阵的实形式）；$l=0$ 不变、$l=1$ 是 3×3、$l=2$ 是 5×5……
- **只有绕 z 轴的旋转**是简单的（每个 $m$ 只乘 $e^{\mathrm im\alpha}$，实形式下是同带内的 2×2 旋转）；
- 通用旋转的标准做法：把目标轴转到 z 轴 → 绕 z 转 → 转回来（"zonal 轴旋转 + 三明治"）；
- **工程含义**：世界空间的探针 SH **不需要旋转**（探针本身就是世界空间的）；但若要用 SH 表示"可旋转的天空盒环境光"，就必须处理这件事——这也是 `SkyboxComponent::rotation` 目前没有渲染侧消费者的原因之一。

### 10.7 存储与精度

| 项 | 工程现状 | 可优化方向 |
|---|---|---|
| 每探针 | 4 × `float4` = 64 B（RGB 用、A 闲置） | 打包成 4×11 bit 定点、或 fp16 ⇒ 显存/带宽减半以上 |
| 精度 | 当前 fp32 | 定点压缩会引入量化误差，需重新标定白炉判据 |
| 布局 | `StructuredBuffer<float4>`，探针索引 × 4 | 保持**同一次改动里同时改投影端与评估端** |

---

## 11. 不变量与验收

1. **白炉解析判据**：半球均匀采样下 $l_0 = 2\pi Y_{00} = \sqrt\pi$（整球投影是 $4\pi Y_{00}=2\sqrt\pi$），重建 $E(n)\equiv\pi$，与方向/采样数无关。`SHProject` 把它写进统计（`u_Stats[2]`/`[3]`），CPU 侧除以探针数就能读出偏差；CPU 侧用 `LumenSH.h` 的 `kSHWhiteFurnaceL0` 做断言；
2. **常量核对**：$\hat A_0Y_{00} = \sqrt\pi/2 = 0.8862269254527580$、$\hat A_1Y_1 = 1.0233267079464886$，与代码常量逐位比对；
3. **参考辐照度对照**：`u_IrradSh`（SH 重建）与 `u_IrradRef`（逐光线 cos 求和）同帧对比，差值即"带限 + 有限采样"误差（白炉下应为 0）；
4. **端到端**：探针 SH 在整个球面/半球上的平均应当等于输入辐射度的平均（$\Sigma L$ 与 $l_0$ 的关系可解析验算）；
5. **统计读数**：投影（有效探针/光线）、滤波（输入/输出一阶二阶矩、历史使用数）、重建（辐照度均值/最大值）都能在日志里读到，便于定位"SH 没积出来"还是"滤波压过头"；
6. **逐位一致**：DDGI 投影与 `DDGI_Trace` 共用 Fibonacci 方向；Screen Probe 投影与追踪共用 `ProbeRayRandom`（PCG 与 `LumenSH.h` 的 C++ 版本逐位一致）；
7. **回归**：改动投影端（阶数、权重、轴序）必须同时改动评估端与合成端，并重跑上述判据。

```slang
// Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang:78,91-93
eRef += L * max(0.0, dot(d, n));         // 参考：逐光线 cos 加权求和
// ...
const float3 eSh = SHRadianceToIrradiance(l0, l1, l2, l3, n);
u_IrradSh[tid.x]  = float4(eSh, 1.0);
u_IrradRef[tid.x] = float4(eRef, 1.0);
```

---

## 12. 文件索引

| 文件 | 关键内容 |
|---|---|
| `Engine/Shader/Shaders/SHCommon.slang` | `kSH_Y00/Y1`、`kSH_A0/A1`、`kSH_A0Y00/A1Y1`、`SHBasis`、`SHEvalRadiance`、`SHRadianceToIrradiance`、`FibonacciSphere` |
| `Engine/Render/Lumen/LumenSH.h` | `kSHY00/kSHY1`、`kSHWhiteFurnaceL0`、`SHBasis`、`SHRadianceToIrradiance`、PCG/半球采样镜像 |
| `Engine/Shader/Shaders/Lumen/Lumen_ScreenProbe_SHProject.comp.slang` | 投影估计量、白炉分支、`E_sh/E_ref`、统计 |
| `Engine/Shader/Shaders/Lumen/ScreenProbe_Filter.comp.slang` | 3×3 YCoCg AABB、时域 EMA、模式 |
| `Engine/Shader/Shaders/Lumen/Lumen_ProbeIrradiance.comp.slang` | 逐像素法线重建、`albedo/π` |
| `Engine/Shader/Shaders/Lumen/ScreenProbe.slang` | `ScreenProbe` 结构体（96 B） |
| `Engine/Shader/Shaders/Lumen/ScreenProbeSampling.slang` | 探针光线方向（PCG、半球/GGX 采样） |
| `Engine/Shader/Shaders/GI/DDGI.comp.slang` | `kStride`、`WriteSH`、`4π/N` 归一化、历史混合、`EvalScreenProbeRadiance` |
| `Engine/Shader/Shaders/RT_DDGI.slang` | `EvalDDGI_SH`、负值截断、`SampleDDGI` 三线性 |
| `Engine/Shader/Shaders/RayTracing/DDGI_Trace.rgen.slang` | `FibonacciSphere` 采样方向 |
| `Engine/Shader/Shaders/Lighting/DeferredLighting.frag.slang` | `GISOURCE_DDGI`：`SampleDDGI * albedo/π` |
| `Engine/Shader/Shaders/RayTracing/RT_GI.rgen.slang` | miss 回退：`SampleDDGI * 1/π` |
| `Engine/Render/GI/GI_DDGI.h` | 探针布局（4×float4 = 64 B）、`kStride` 同步约束 |
| `Engine/Render/Lumen/LumenScene_ProbeFilter.cpp` | `ProbeFilterPC`、`ProbeFilterHistory`、`RunScreenProbeFilter` |
| `Engine/Render/Lumen/LumenScene_Irradiance.cpp` | `RunProbeIrradiance` |
| `Engine/Render/Lumen/LumenScene_SurfaceCache.cpp` | `RunScreenProbeSHProject` 的 PSO |
| `Engine/Render/Lumen/LumenScene.h` | 三个 pass 的声明、滤波参数默认值 |
| `Engine/Shader/CMakeLists.txt` | `SHCommon.slang` 的 `SLANG_INCLUDES` 登记 |

---

## 13. 已知限制与后续

1. **只到 $l\le1$**：4 系数只能表达低频；如果未来需要更锐的间接光方向性（例如大面积光源的近场 GI），需要恢复 band 2（$\hat A_2 = \pi/4$）并同时改投影端与求值端；
2. **振铃**：单侧 SH 表示余弦波瓣时可能出现轻微负值，目前用 `max(e, 0)` 截断；更优雅的做法是 SH 窗口化（Hanning）或更高阶；
3. **探针密度**：DDGI 的格子感主要来自分辨率（注释实测：最长轴 16 → 24/32 才能明显下降），不是插值权重；
4. **Screen Probe 方差**：8~16 条光线的单探针估计仍偏噪，靠 AABB + 时域 EMA 压制；提高采样数或更好的重要性采样是后续方向；
5. **两套 SH 的一致性**：它们共享基函数，但投影域不同（单侧半球 vs 整球）；DDGI 读 Screen Probe 时做了"单侧 → 整球"的近似外推，未来可以统一成同一套探针表示；
6. **存储压缩**：当前 fp32、每探针 64 B；定点/fp16 压缩是显存与带宽的下一步优化，但需要重新标定白炉判据。

---

## 14. 公式速查

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
| 出射辐亮度 | $L_o=\mathrm{albedo}\cdot E/\pi$ |

---

## 15. 延伸阅读

- Ramamoorthi, R., & Hanrahan, P. (2001). *An Efficient Representation for Irradiance Environment Maps.* SIGGRAPH —— §5 的全部结论（含 $\hat A_l$ 表与"$l\le2$ 复原约 99% 辐照度"）的原始出处。
- Sloan, P.-P. (2008). *Stupid Spherical Harmonics (SH) Tricks.* —— 工程向的 SH 技巧合集（旋转、窗口化、压缩、乘积）。
- Majercik, Z., et al. (2019). *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields.* —— DDGI 的原始论文（探针网格 + SH + 三线性插值 + 可见性）。
- 项目内：`docs/HugEngine引擎介绍/07.全局光照GI本质、实现与架构优化.md`（§2.5 DDGI、§2.7 Lumen 的落地细节）、`docs/HugEngine引擎介绍/08.Lumen实现分析.md`（屏幕探针与滤波）。
