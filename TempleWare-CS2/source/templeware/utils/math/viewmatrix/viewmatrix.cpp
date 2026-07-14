#include "viewmatrix.h"

#include "../../../../../external/imgui/imgui.h"
#include <iostream>
#include <cmath>

bool ViewMatrix::WorldToScreen(const Vector_t& position, Vector_t& out) const {
    if (!viewMatrix)
        return false;

    const float w = viewMatrix->matrix[3][0] * position.x + viewMatrix->matrix[3][1] * position.y + viewMatrix->matrix[3][2] * position.z + viewMatrix->matrix[3][3];
    if (w <= 0.001f)
        return false;

    const float invW = 1.0f / w;
    const ImVec2 wS = ImGui::GetIO().DisplaySize;
    if (wS.x <= 1.f || wS.y <= 1.f)
        return false;

    const float centerX = wS.x * 0.5f;
    const float centerY = wS.y * 0.5f;

    out.x = centerX + ((viewMatrix->matrix[0][0] * position.x + viewMatrix->matrix[0][1] * position.y + viewMatrix->matrix[0][2] * position.z + viewMatrix->matrix[0][3]) * invW * centerX);
    out.y = centerY - ((viewMatrix->matrix[1][0] * position.x + viewMatrix->matrix[1][1] * position.y + viewMatrix->matrix[1][2] * position.z + viewMatrix->matrix[1][3]) * invW * centerY);
    return true;
}

bool ViewMatrix::WorldToClip(const Vector_t& position, float& numX, float& numY, float& w) const {
    if (!viewMatrix)
        return false;

    w = viewMatrix->matrix[3][0] * position.x + viewMatrix->matrix[3][1] * position.y + viewMatrix->matrix[3][2] * position.z + viewMatrix->matrix[3][3];
    numX = viewMatrix->matrix[0][0] * position.x + viewMatrix->matrix[0][1] * position.y + viewMatrix->matrix[0][2] * position.z + viewMatrix->matrix[0][3];
    numY = viewMatrix->matrix[1][0] * position.x + viewMatrix->matrix[1][1] * position.y + viewMatrix->matrix[1][2] * position.z + viewMatrix->matrix[1][3];

    return std::isfinite(w) && std::isfinite(numX) && std::isfinite(numY);
}