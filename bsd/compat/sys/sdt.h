/* X OS compat: stub out all SDT/DTrace probes */
#ifndef _SYS_SDT_H_
#define _SYS_SDT_H_

#define SDT_PROBE_DEFINE1(provider, mod, func, name, arg1)
#define SDT_PROBE_DEFINE2(provider, mod, func, name, arg1, arg2)
#define SDT_PROBE_DEFINE3(provider, mod, func, name, arg1, arg2, arg3)
#define SDT_PROBE_DEFINE4(provider, mod, func, name, arg1, arg2, arg3, arg4)
#define SDT_PROBE_DEFINE5(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5)
#define SDT_PROBE_DEFINE6(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, arg6)
#define SDT_PROBE_DEFINE7(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, arg6, arg7)

#define SDT_PROBE_DEFINE1_XLATE(provider, mod, func, name, arg1, xarg1)
#define SDT_PROBE_DEFINE2_XLATE(provider, mod, func, name, arg1, arg2, xarg1, xarg2)
#define SDT_PROBE_DEFINE3_XLATE(provider, mod, func, name, arg1, arg2, arg3, xarg1, xarg2, xarg3)
#define SDT_PROBE_DEFINE4_XLATE(provider, mod, func, name, arg1, arg2, arg3, arg4, xarg1, xarg2, xarg3, xarg4)
#define SDT_PROBE_DEFINE5_XLATE(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, xarg1, xarg2, xarg3, xarg4, xarg5)
#define SDT_PROBE_DEFINE6_XLATE(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, arg6, xarg1, xarg2, xarg3, xarg4, xarg5, xarg6)
#define SDT_PROBE_DEFINE7_XLATE(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, arg6, arg7, xarg1, xarg2, xarg3, xarg4, xarg5, xarg6, xarg7)

#define SDT_PROBE1(provider, mod, func, name, arg1)
#define SDT_PROBE2(provider, mod, func, name, arg1, arg2)
#define SDT_PROBE3(provider, mod, func, name, arg1, arg2, arg3)
#define SDT_PROBE4(provider, mod, func, name, arg1, arg2, arg3, arg4)
#define SDT_PROBE5(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5)
#define SDT_PROBE6(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, arg6)
#define SDT_PROBE7(provider, mod, func, name, arg1, arg2, arg3, arg4, arg5, arg6, arg7)

#define SDT_PROBE0(provider, mod, func, name)

#define DTRACE_PROBE(name)
#define DTRACE_PROBE1(name, arg1)
#define DTRACE_PROBE2(name, arg1, arg2)
#define DTRACE_PROBE3(name, arg1, arg2, arg3)
#define DTRACE_PROBE4(name, arg1, arg2, arg3, arg4)
#define DTRACE_PROBE5(name, arg1, arg2, arg3, arg4, arg5)

#define SDT_PROVIDER_DEFINE(provider)
#define SDT_PROVIDER_DECLARE(prov)
#define SDT_PROBE_REGISTERY_ENTRY()

#define SDT_PROBE_DECLARE(provider, mod, func, name)

#define MIB_SDT_PROBE1(...)
#define MIB_SDT_PROBE2(...)
#define MIB_SDT_PROBE3(...)
#define MIB_SDT_PROBE4(...)

#endif /* _SYS_SDT_H_ */

/* Unnumbered variant used for provider/probe registration */
#define SDT_PROBE_DEFINE(provider, mod, func, name)
#define SDT_PROBE_DEFINE0(provider, mod, func, name)
