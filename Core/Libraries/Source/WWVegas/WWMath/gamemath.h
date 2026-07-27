/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GameMath - bit-identical trigonometry for lockstep simulation.
//
// GeneralsX @feature Claude 28/07/2026
//
// WHY THIS EXISTS - measured, not theorised.
//
// Mac (Apple libm, arm64) and Windows (MSVC UCRT, x64) return results ONE ULP apart for cosf/sinf
// at angles near 45 degrees. Captured from two peers in a live LAN match at the same clean commit,
// dumping the raw transform bits of every object at frame 0:
//
//   mac BF34D116  win BF34D117    -0.706315398 vs -0.706315458   45.06 deg
//   mac BF34919D  win BF34919E    -0.705346882 vs -0.705346942   45.14 deg
//   mac BF349F7C  win BF349F7D    -0.705558538 vs -0.705558598   45.13 deg
//   mac 3F34D111  win 3F34D112     0.706315100 vs  0.706315160   45.06 deg
//
// Six objects out of 323 on one map, delta 5.96e-08 = 2^-24 every time. Thing::setOrientation
// (Thing.cpp:232-241) writes those raw results into the object transform, and Object::crc
// (Object.cpp:4007) hashes the 48 bytes with NO epsilon - so one bit kills the match before frame
// 1. Rocks survived only because they sit at angles where the two libraries happen to agree.
//
// WHY THIS IS BIT-IDENTICAL EVERYWHERE.
//
// IEEE-754 specifies +, -, * and the float<->double conversions as CORRECTLY ROUNDED: every
// conforming platform must produce the same bits. It says nothing about sin/cos/exp/log, which is
// precisely why libm implementations are free to differ. An approximation built only from basic
// arithmetic is therefore deterministic by construction, where a libm call never can be.
//
// Two build settings are load-bearing for that guarantee, and both are already in place:
//   * -ffp-contract=off (cmake/compilers.cmake:68) and /fp:precise (:61) stop the compiler fusing
//     a multiply and an add into an FMA, which rounds once instead of twice and would change the
//     result.
//   * x64 and arm64 have no x87 excess-precision problem; the _M_IX86 inline-asm branch in
//     wwmath.h is unreachable on both targets.
//
// ACCURACY. Cody-Waite range reduction with a three-part pi/2, then a Taylor series evaluated in
// double over |r| <= pi/4. Truncation error is under 1e-11, far below float epsilon (~6e-8 at
// these magnitudes), so the returned float is the correctly rounded result in practice - and,
// unlike libm, the SAME correctly rounded result on every platform.
//
// LIMITATION. Reduction precision degrades for very large |x|, as it does in any Cody-Waite
// scheme. Game angles are radians in roughly -2pi..2pi, so this is not reachable here, but do not
// lift this into a general-purpose math library without a full Payne-Hanek reduction.

#pragma once

#ifndef GAMEMATH_H
#define GAMEMATH_H

namespace GameMath
{

// Cody-Waite split of pi/2. Each part is exactly representable in double, so subtracting them one
// at a time preserves low bits that a single-constant reduction would discard.
const double GM_PIO2_HI     = 1.57079632673412561417e+00;
const double GM_PIO2_MID    = 6.07710050650619224932e-11;
const double GM_PIO2_LO     = 2.02226624879595063154e-21;
const double GM_TWO_OVER_PI = 6.36619772367581382433e-01;

// Horner form keeps the operation ORDER fixed. That matters: the determinism argument rests on the
// sequence of roundings being identical, not merely on the arithmetic being algebraically equal.

// sin(r) = r - r^3/3! + r^5/5! - r^7/7! + r^9/9! - r^11/11!
inline double gmPolySin(double r)
{
	const double r2 = r * r;
	double p =        -2.50521083854417187751e-08;   // -1/11!
	p = p * r2 +       2.75573192239858906526e-06;   // +1/9!
	p = p * r2 -       1.98412698412698412526e-04;   // -1/7!
	p = p * r2 +       8.33333333333333321769e-03;   // +1/5!
	p = p * r2 -       1.66666666666666657415e-01;   // -1/3!
	p = p * r2 +       1.0;
	return r * p;
}

// cos(r) = 1 - r^2/2! + r^4/4! - r^6/6! + r^8/8! - r^10/10!
inline double gmPolyCos(double r)
{
	const double r2 = r * r;
	double p =        -2.75573192239858906526e-07;   // -1/10!
	p = p * r2 +       2.48015873015873015658e-05;   // +1/8!
	p = p * r2 -       1.38888888888888894189e-03;   // -1/6!
	p = p * r2 +       4.16666666666666643537e-02;   // +1/4!
	p = p * r2 -       5.00000000000000000000e-01;   // -1/2!
	p = p * r2 +       1.0;
	return p;
}

// Reduce x to r in [-pi/4, pi/4] and report the quadrant. Rounding n uses truncation after a
// half-offset rather than any libm rounding helper: a cast from double to integer is exact, so it
// cannot itself introduce a platform difference.
inline double gmReduce(double x, int &quadrant)
{
	const double q = x * GM_TWO_OVER_PI;
	const long long n = (long long)(q >= 0.0 ? q + 0.5 : q - 0.5);
	const double dn = (double)n;

	double r = x - dn * GM_PIO2_HI;
	r = r - dn * GM_PIO2_MID;
	r = r - dn * GM_PIO2_LO;

	quadrant = (int)(n & 3);
	if (quadrant < 0)
	{
		quadrant += 4;   // n is negative for negative angles; & 3 on a signed value can be negative
	}
	return r;
}

inline float Sin(float x)
{
	int quadrant;
	const double r = gmReduce((double)x, quadrant);
	switch (quadrant)
	{
		case 0:  return (float) gmPolySin(r);
		case 1:  return (float) gmPolyCos(r);
		case 2:  return (float)-gmPolySin(r);
		default: return (float)-gmPolyCos(r);
	}
}

inline float Cos(float x)
{
	int quadrant;
	const double r = gmReduce((double)x, quadrant);
	switch (quadrant)
	{
		case 0:  return (float) gmPolyCos(r);
		case 1:  return (float)-gmPolySin(r);
		case 2:  return (float)-gmPolyCos(r);
		default: return (float) gmPolySin(r);
	}
}

} // namespace GameMath

#endif // GAMEMATH_H
