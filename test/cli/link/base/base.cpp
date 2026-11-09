// Alexis Script — base.so (mt + ex + st + sys)
#include "ascript.h"

using namespace alx::script;

void register_mt(fwrap&);
void register_ex(fwrap&);
void register_st(fwrap&);
void register_sys(fwrap&);
void register_fs(fwrap&);

// fly-level: once after dlopen, builds template (stateless registrations are copied)
extern "C" void alexis_script_load(fwrap& args) {
    register_mt(args);
    register_ex(args);
    register_st(args);
    register_sys(args);
    register_fs(args);
}
// No alexis_script_create: all modules stateless — impl falls back to template copy
