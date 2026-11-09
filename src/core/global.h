#ifndef ASGLOBAL_H
#define ASGLOBAL_H

// DLL export/import visibility control for axc-core.
#ifdef _WIN32
#    ifdef AXCCORELIB_EXPORTS
#        define AXCCORE_API __declspec(dllexport)
#    else
#        define AXCCORE_API __declspec(dllimport)
#    endif
#else
#    define AXCCORE_API
#endif

#include "ajson.h"
#include "ascript.h"

#endif // !ASGLOBAL_H
