#include "RecomputeGate.hpp"

bool CRecomputeGate::changed(std::size_t geometryFingerprint, const SGlobalSeamDefaults& defaults) noexcept {
    if (m_lastGeometry == geometryFingerprint && m_lastDefaults == defaults)
        return false;

    m_lastGeometry = geometryFingerprint;
    m_lastDefaults = defaults;
    return true;
}

void CRecomputeGate::reset() noexcept {
    m_lastGeometry.reset();
    m_lastDefaults.reset();
}
