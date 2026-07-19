#ifndef slic3r_SpiralVase_hpp_
#define slic3r_SpiralVase_hpp_

#include "../libslic3r.h"
#include "../GCodeReader.hpp"

namespace Slic3r {

class SpiralVase
{
public:
    class SpiralPoint
    {
    public:
        SpiralPoint(float paramx, float paramy) : x(paramx), y(paramy) {}

    public:
        float x, y;
    };
    SpiralVase(const PrintConfig &config) : m_config(config)
    {
        m_reader.z() = (float)m_config.z_offset;
        m_reader.apply_config(m_config);
        m_previous_layer = NULL;
        m_smooth_spiral = config.spiral_mode_smooth;
        // Track C1: warped spiral is gated behind clay mode + the explicit
        // non-planar option, so stock spiral output is unchanged when off.
        m_nonplanar_enable = config.ldm_modded_printer.value && config.ldm_nonplanar_enable.value
                             && config.ldm_slump_budget_frac.value > 0.f;
    };

    void 		enable(bool en) {
   		m_transition_layer = en && ! m_enabled;
    	m_enabled 		   = en;
    }

    std::string process_layer(const std::string &gcode, bool last_layer);
    void set_max_xy_smoothing(float max) {
        m_max_xy_smoothing = max;
    }

    // Track C1: per-vertex Z warp added to the spiral Z ramp. factor is the
    // normalized arc position s in [0,1) around the loop. Returns 0 when
    // non-planar is disabled, so stock output is byte-identical.
    // Increment 1: plumbing stub (always 0). Increment 2 feeds the B2
    // support-margin field to compute step-relief dips.
    float warp_dz(float factor) const {
        if (!m_nonplanar_enable)
            return 0.f;
        (void)factor;
        return 0.f;
    }
private:
    const PrintConfig  &m_config;
    GCodeReader 		m_reader;
    float               m_max_xy_smoothing = 0.f;
    bool                m_nonplanar_enable = false;

    bool 				m_enabled = false;
    // First spiral vase layer. Layer height has to be ramped up from zero to the target layer height.
    bool 				m_transition_layer = false;
    // Whether to interpolate XY coordinates with the previous layer. Results in no seam at layer changes
    bool                m_smooth_spiral = false;
    std::vector<SpiralPoint> * m_previous_layer;
};
}

#endif // slic3r_SpiralVase_hpp_
