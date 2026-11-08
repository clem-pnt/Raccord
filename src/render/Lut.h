#pragma once
#include <QString>
#include <vector>

namespace rc {

// Loads an Adobe/Resolve .cube LUT (1D or 3D). 1D LUTs are expanded to a 33^3
// cube. Output: size^3 RGB triplets, red varying fastest.
bool loadCubeLut(const QString &path, int &size, std::vector<float> &rgb, QString *error = nullptr);

// Monotone cubic curve through control points (x, y in 0..1). Evaluates n samples.
std::vector<float> evalCurve(const std::vector<std::pair<double, double>> &pts, int n, bool periodic = false, double neutral = -1);

} // namespace rc
