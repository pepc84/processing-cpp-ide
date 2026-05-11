#include "Processing.h"
namespace Processing {
using ::std::vector; using ::std::string;
using ::std::sort; using ::std::shuffle;
using ::std::pair; using ::std::make_pair;
using ::std::deque; using ::std::unordered_map;
using ::std::to_string;
/**
 * Rounded Rect Stress Test
 * Tests rect() with rounded corners in many configurations:
 * different sizes, radii, fill/stroke combos, overlaps,
 * rotations, and transparency -- anything that could expose
 * triangle fan seam bugs.
 */

void setup() {
    size(1200, 700);
    frameRate(60);
}

void draw() {
    background(30);

    float t = frameCount * 0.02f;

    // --- Row 1: varying corner radii, opaque fill ---
    for (int i = 0; i < 10; i++) {
        float r = i * 8;
        fill(80 + i*18, 120, 200);
        noStroke();
        rect(20 + i*115, 20, 100, 80, r);
    }

    // --- Row 2: tiny rects (r close to half size = near-circle) ---
    for (int i = 0; i < 20; i++) {
        float sz = 10 + i * 4;
        fill(200, 80 + i*8, 80);
        noStroke();
        rect(20 + i*58, 130, sz, sz, sz/2);
    }

    // --- Row 3: wide thin bars (stress asymmetric aspect ratio) ---
    for (int i = 0; i < 6; i++) {
        fill(80, 200, 120, 180);
        noStroke();
        rect(20, 230 + i*30, 400 - i*40, 20, 10);
    }

    // --- Row 4: tall thin bars ---
    for (int i = 0; i < 8; i++) {
        fill(200, 200, 80, 150);
        noStroke();
        rect(650 + i*60, 230, 20, 120 + i*10, 10);
    }

    // --- Row 5: animated rotation with filled rounded rects ---
    pushMatrix();
    translate(200, 480);
    for (int i = 0; i < 12; i++) {
        pushMatrix();
        rotate(t + i * TWO_PI / 12);
        translate(80, 0);
        fill(255, 150 - i*10, 50, 180);
        noStroke();
        rect(-25, -15, 50, 30, 10);
        popMatrix();
    }
    popMatrix();

    // --- Row 6: stroke only (no fill), various weights ---
    for (int i = 0; i < 8; i++) {
        noFill();
        stroke(100 + i*20, 200, 255);
        strokeWeight(1 + i * 0.5f);
        rect(500 + i*80, 420, 60, 60, 5 + i*4);
    }
    strokeWeight(1);

    // --- Row 7: both fill AND stroke ---
    for (int i = 0; i < 6; i++) {
        fill(80 + i*30, 60, 180 - i*20, 200);
        stroke(255);
        strokeWeight(2);
        rect(20 + i*130, 570, 110, 80, 20);
    }
    strokeWeight(1);

    // --- Centre: large overlapping rects with alpha (blending stress) ---
    noStroke();
    for (int i = 0; i < 8; i++) {
        float angle = t * 0.7f + i * TWO_PI / 8;
        float cx = 880 + cos(angle) * 60;
        float cy = 380 + sin(angle) * 40;
        fill(255, 200, 50, 60);
        rect(cx - 60, cy - 40, 120, 80, 18);
    }

    // --- Corner cases: r = 0 (sharp), r = exactly half ---
    noStroke();
    fill(255, 80, 80);
    rect(1000, 20, 80, 80, 0);      // r=0: plain rect
    fill(80, 255, 80);
    rect(1100, 20, 80, 80, 40);     // r=half: full circle
    fill(80, 80, 255);
    rect(1000, 120, 160, 40, 20);   // r=half of height
    fill(255, 255, 80);
    rect(1000, 180, 40, 160, 20);   // r=half of width

    // --- Label ---
    fill(255); noStroke(); textAlign(LEFT);
    text(std::string("Rounded rect stress test - frame ") + str(frameCount), 10, height - 10);
}

static void _sketchWire() {
}
static int _autoWire = []{ _wireCallbacksFn = _sketchWire; return 0; }();
} // namespace Processing
