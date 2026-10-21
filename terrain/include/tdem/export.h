/* export.h — symbol visibility for static / shared builds of tdem. */
#ifndef TDEM_EXPORT_H
#define TDEM_EXPORT_H

#if defined(_WIN32) && defined(TDEM_SHARED)
#  ifdef TDEM_BUILDING
#    define TDEM_API __declspec(dllexport)
#  else
#    define TDEM_API __declspec(dllimport)
#  endif
#elif defined(TDEM_SHARED)
#  define TDEM_API __attribute__((visibility("default")))
#else
#  define TDEM_API
#endif

#endif /* TDEM_EXPORT_H */
