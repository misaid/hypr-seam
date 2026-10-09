// tests/test_recomputegate.cpp
#include "../src/RecomputeGate.hpp"
#include <cstdio>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

constexpr std::size_t kGeometry = 0x5eed;

static SGlobalSeamDefaults defaultDefaults() {
    return SGlobalSeamDefaults{
        .baseRadii     = {22, 22, 22, 22},
        .roundingPower = 2.0,
        .seamEnabled   = false,
        .seamRadius    = 2,
        .tolerance     = 6,
    };
}

// A gate primed with defaultDefaults() and kGeometry, so the next identical call is a no-op.
static CRecomputeGate primedGate() {
    CRecomputeGate gate;
    [[maybe_unused]] const bool first = gate.changed(kGeometry, defaultDefaults());
    return gate;
}

static void test_first_call_recomputes() {
    CRecomputeGate gate;
    CHECK(gate.changed(kGeometry, defaultDefaults()));
}

static void test_unchanged_inputs_skip() {
    auto gate = primedGate();
    CHECK(!gate.changed(kGeometry, defaultDefaults()));
    CHECK(!gate.changed(kGeometry, defaultDefaults()));
}

static void test_geometry_change_recomputes() {
    auto gate = primedGate();
    CHECK(gate.changed(kGeometry + 1, defaultDefaults()));
    CHECK(!gate.changed(kGeometry + 1, defaultDefaults()));
}

// Regression: `hyprctl keyword plugin:seam:rounding 10` changed the config but not the
// geometry, and the tick check only looked at geometry, so nothing recomputed.
static void test_config_change_alone_recomputes() {
    auto gate     = primedGate();
    auto defaults = defaultDefaults();
    defaults.baseRadii = {10, 10, 10, 10};
    CHECK(gate.changed(kGeometry, defaults));
    CHECK(!gate.changed(kGeometry, defaults));
}

// Changing any one field of the defaults must trigger a recompute.
static void test_each_defaults_field_recomputes() {
    const auto check = [](auto mutate) {
        auto gate     = primedGate();
        auto defaults = defaultDefaults();
        mutate(defaults);
        return gate.changed(kGeometry, defaults);
    };
    CHECK(check([](SGlobalSeamDefaults& d) { d.baseRadii.topLeft = 0; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.baseRadii.topRight = 0; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.baseRadii.bottomLeft = 0; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.baseRadii.bottomRight = 0; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.roundingPower = 3.0; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.seamEnabled = true; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.seamRadius = 0; }));
    CHECK(check([](SGlobalSeamDefaults& d) { d.tolerance = 30; })); // e.g. general:gaps_in changed
}

static void test_change_back_recomputes() {
    auto gate     = primedGate();
    auto defaults = defaultDefaults();
    defaults.seamEnabled = true;
    CHECK(gate.changed(kGeometry, defaults));
    CHECK(gate.changed(kGeometry, defaultDefaults()));
}

static void test_reset_recomputes() {
    auto gate = primedGate();
    gate.reset();
    CHECK(gate.changed(kGeometry, defaultDefaults()));
}

int main() {
    test_first_call_recomputes();
    test_unchanged_inputs_skip();
    test_geometry_change_recomputes();
    test_config_change_alone_recomputes();
    test_each_defaults_field_recomputes();
    test_change_back_recomputes();
    test_reset_recomputes();

    if (failures == 0) {
        std::printf("All recompute-gate tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d recompute-gate test(s) failed.\n", failures);
    return 1;
}
