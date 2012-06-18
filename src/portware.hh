/* Portware specific business logic.
 */

#ifndef __PORTWARE_HH__
#define __PORTWARE_HH__
#pragma once

#include <cmath>
#include <cstdint>

namespace portware
{

static inline
double
round_half_up (double x)
{
	return std::floor (x + 0.5);
}

/* mantissa of 10E6
 */
static inline
int64_t
mantissa (double x)
{
	return (int64_t) round_half_up (x * 1000000.0);
}

/* round a double value to 6 decimal places using round half up
 */
static inline
double
round (double x)
{
	return (double) mantissa (x) / 1000000.0;
}

} // namespace portware

#endif /* __PORTWARE_HH__ */

/* eof */
