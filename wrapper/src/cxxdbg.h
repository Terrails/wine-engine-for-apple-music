/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Debug aid: log every C++ exception thrown in the process ("CXX throw: ...") and a
 * module+offset backtrace for breakpoints and access violations ("FATAL exception ..."). */
#pragma once

void cxxdbg_install(void);
