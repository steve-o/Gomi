/* http://msdn.microsoft.com/en-us/magazine/hh580731.aspx
 */

#ifndef __MS_TIMER_HH__
#define __MS_TIMER_HH__

#include "unique_handle.hh"

namespace ms
{

	struct timer_traits
	{
		static PTP_TIMER invalid() throw()
		{
			return nullptr;
		}

		static void close (PTP_TIMER value) throw()
		{
/* In some cases, callback functions might run after CloseThreadpoolTimer has been called. To prevent this behavior:
 * 1. Call the SetThreadpoolTimer function with the pftDueTime parameter set to NULL and the msPeriod and msWindowLength parameters set to 0.
 */
			SetThreadpoolTimer (value, NULL, 0, 0);
/* 2. Call the WaitForThreadpoolTimerCallbacks function.
 */
			WaitForThreadpoolTimerCallbacks (value, true);
/* 3. Call CloseThreadpoolTimer.
 */
			CloseThreadpoolTimer (value);
		}
	};

/* Example usage:
 *
 * timer t (CreateThreadpoolTimer (on_timer, context, nullptr));
 */
	typedef unique_handle<PTP_TIMER, timer_traits> timer;

} /* namespace ms */

#endif /* __MS_TIMER_HH__ */

/* eof */
