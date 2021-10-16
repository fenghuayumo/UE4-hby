#ifdef HAVE_PTHREAD
# undef HAVE_PTHREAD
#endif
#ifdef HAVE_POSIX_SEMAPHORES
# undef HAVE_POSIX_SEMAPHORES
#endif

#define ILMBASE_INTERNAL_NAMESPACE_CUSTOM 1
#define IMATH_INTERNAL_NAMESPACE Imath_2_3
#define IEX_INTERNAL_NAMESPACE Iex_2_3
#define ILMTHREAD_INTERNAL_NAMESPACE IlmThread_2_3
#define IMATH_NAMESPACE Imath
#define IEX_NAMESPACE Iex
#define ILMTHREAD_NAMESPACE IlmThread
#define ILMBASE_VERSION_STRING "2.3.0"
#define ILMBASE_PACKAGE_STRING "IlmBase 2.3.0"
#define ILMBASE_VERSION_MAJOR 2
#define ILMBASE_VERSION_MINOR 3
#define ILMBASE_VERSION_PATCH 0

// Version as a single hex number, e.g. 0x01000300 == 1.0.3
#define ILMBASE_VERSION_HEX ((ILMBASE_VERSION_MAJOR << 24) | \
                             (ILMBASE_VERSION_MINOR << 16) | \
                             (ILMBASE_VERSION_PATCH <<  8))
