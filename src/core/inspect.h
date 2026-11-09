#ifndef ACORE_INSPECT_H
#define ACORE_INSPECT_H

#include "global.h"

#include "aanyptr.h"
#include "avariant.h"

#include <string>
#include <thread>

namespace alx {
    namespace script {
        class engine;
    }
} // namespace alx

// Host handle metadata — the host-side twin of the engine's link type table.
// Registrations run during extension install; lookups run on the engine thread.
namespace core {
    namespace inspect {
        using describe_fn = std::string (*)(const alx::anyptr&); // empty = no summary
        using view_fn = alx::variant (*)(const alx::anyptr&); // null = no view

        // add — name a host handle type and give it optional hooks; _name is stored, not copied
        AXCCORE_API void add(alx::uint_64 _rid, const char* _name, describe_fn _describe = nullptr,
                             view_fn _view = nullptr);
        // name — registered type name, or null
        AXCCORE_API const char* name(const alx::anyptr& _p);
        // describe — one-line summary from the host table
        AXCCORE_API bool describe(const alx::anyptr& _p, std::string& _out);
        // view — view payload from the host table
        AXCCORE_API bool view(const alx::anyptr& _p, alx::variant& _out);

        // child_fn — attach/detach notice for engines created by $thd_create
        using child_fn = void (*)(void* _ud, bool _attach, alx::script::engine* _child,
                                  std::thread::id _parent, std::thread::id _child_tid);
        // set_child_observer — install the thd observer; null uninstalls
        AXCCORE_API void set_child_observer(child_fn _fn, void* _ud);
        // notify_child — thd's attach/detach call; no-op without an observer
        AXCCORE_API void notify_child(bool _attach, alx::script::engine* _child,
                                      std::thread::id _parent, std::thread::id _child_tid);
    } // namespace inspect
} // namespace core

#endif // ACORE_INSPECT_H
