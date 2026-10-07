// tests/test_adjacency.cpp
#include "../src/Adjacency.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_two_window_clean_edge() {
    // A | B, sharing the vertical line x=100, both spanning y=0..200
    SSeamBox a{0, 0, 100, 200, false, 1};
    SSeamBox b{100, 0, 100, 200, false, 2};
    auto flags = computeTouchingCorners(a, {a, b}, 2.0);
    CHECK(flags.topRight && flags.bottomRight);     // A's right edge touches B
    CHECK(!flags.topLeft && !flags.bottomLeft);      // A's left edge faces the monitor edge
}

static void test_t_junction() {
    // Top-left and bottom-left quadrants (A, C) stacked on the left; B spans the full right column.
    SSeamBox a{0, 0, 100, 100, false, 1};
    SSeamBox c{0, 100, 100, 100, false, 3};
    SSeamBox b{100, 0, 100, 200, false, 2};
    std::vector<SSeamBox> all{a, b, c};
    auto flagsA = computeTouchingCorners(a, all, 2.0);
    auto flagsC = computeTouchingCorners(c, all, 2.0);
    auto flagsB = computeTouchingCorners(b, all, 2.0);
    CHECK(flagsA.bottomRight); // A's bottom-right is the T-junction point
    CHECK(flagsC.topRight);    // C's top-right is the same point
    CHECK(flagsB.topLeft && flagsB.bottomLeft); // B touches both A and C along its left edge
}

static void test_x_junction_four_way_split() {
    SSeamBox tl{0, 0, 100, 100, false, 1};
    SSeamBox tr{100, 0, 100, 100, false, 2};
    SSeamBox bl{0, 100, 100, 100, false, 3};
    SSeamBox br{100, 100, 100, 100, false, 4};
    std::vector<SSeamBox> all{tl, tr, bl, br};
    CHECK(computeTouchingCorners(tl, all, 2.0).bottomRight);
    CHECK(computeTouchingCorners(tr, all, 2.0).bottomLeft);
    CHECK(computeTouchingCorners(bl, all, 2.0).topRight);
    CHECK(computeTouchingCorners(br, all, 2.0).topLeft);
}

static void test_single_window_never_flags() {
    SSeamBox a{0, 0, 200, 200, false, 1};
    auto flags = computeTouchingCorners(a, {a}, 2.0);
    CHECK(!flags.topLeft && !flags.topRight && !flags.bottomLeft && !flags.bottomRight);
}

static void test_floating_window_excluded_both_ways() {
    // A tiled window sits right next to a FLOATING window at the same boundary.
    SSeamBox tiled{0, 0, 100, 200, false, 1};
    SSeamBox floating{100, 0, 100, 200, true, 2};
    std::vector<SSeamBox> all{tiled, floating};

    // The tiled window must NOT flatten against the floating one.
    auto tiledFlags = computeTouchingCorners(tiled, all, 2.0);
    CHECK(!tiledFlags.topRight && !tiledFlags.bottomRight);

    // The floating window itself must never flag any corner, even though it's
    // touching a tiled neighbor.
    auto floatFlags = computeTouchingCorners(floating, all, 2.0);
    CHECK(!floatFlags.topLeft && !floatFlags.bottomLeft);
}

static void test_tolerance_and_hysteresis_boundary() {
    SSeamBox a{0, 0, 100, 200, false, 1};
    SSeamBox barelyTouching{104, 0, 100, 200, false, 2}; // 4px gap, tolerance 6
    CHECK(computeTouchingCorners(a, {a, barelyTouching}, 6.0).topRight);

    SSeamBox justOutside{107, 0, 100, 200, false, 2}; // 7px gap, tolerance 6
    CHECK(!computeTouchingCorners(a, {a, justOutside}, 6.0).topRight);
}

static void test_clamp_corner_radius() {
    CHECK(clampCornerRadius(22, 200, 200) == 22);   // no clamp needed
    CHECK(clampCornerRadius(22, 30, 200) == 15);    // width/2 = 15, smaller than radius
    CHECK(clampCornerRadius(22, 200, 10) == 5);     // height/2 = 5, smaller than radius and width/2
}

int main() {
    test_two_window_clean_edge();
    test_t_junction();
    test_x_junction_four_way_split();
    test_single_window_never_flags();
    test_floating_window_excluded_both_ways();
    test_tolerance_and_hysteresis_boundary();
    test_clamp_corner_radius();

    if (failures == 0) {
        std::printf("All adjacency tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d adjacency test(s) failed.\n", failures);
    return 1;
}
