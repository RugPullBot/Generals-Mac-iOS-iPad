// Cross-platform conformance probe for the AI base-placement desync.
//
// AISkirmishPlayer::adjustBuildList did `angle += 3*PI/4; s = sin(angle); c = cos(angle);` with raw
// libm. This prints the exact bits both ways, so running it on macOS/clang and Windows/MSVC and
// diffing the output proves two things at once:
//   1. raw libm really does differ across the two platforms at these angles  (the bug is real)
//   2. GameMath produces identical bits on both                              (the fix works)
#include <cstdio>
#include <cmath>
#include <cstring>
#include "gamemath.h"

typedef float Real;
#define PI 3.14159265359f

static unsigned bits(float f) { unsigned u; std::memcpy(&u, &f, 4); return u; }

int main()
{
    // Exactly the angles AISkirmishPlayer can produce: the gridIndex table plus the
    // unconditional `angle += 3*PI/4`.
    const Real table[9] = { 0, PI/4, PI/2, -PI/4, 0, 3*PI/4, -PI/2, -3*PI/4, PI };

    std::printf("idx  angle_bits  libm_sin  libm_cos  gm_sin    gm_cos    deg\n");
    for (int i = 0; i < 9; ++i) {
        Real a = table[i] + 3*PI/4;
        std::printf("%d    %08X    %08X  %08X  %08X  %08X  %.2f\n",
            i, bits(a),
            bits(sin(a)), bits(cos(a)),
            bits(GameMath::Sin(a)), bits(GameMath::Cos(a)),
            (double)a * 180.0 / 3.14159265358979);
    }

    // The unconditional case: base rotation disabled still lands on 135 degrees.
    Real a = 3*PI/4;
    std::printf("\nUNCONDITIONAL 135deg  angle=%08X  libm_sin=%08X libm_cos=%08X  gm_sin=%08X gm_cos=%08X\n",
        bits(a), bits(sin(a)), bits(cos(a)), bits(GameMath::Sin(a)), bits(GameMath::Cos(a)));
    return 0;
}
