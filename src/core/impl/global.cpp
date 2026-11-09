#include "global.h"

// $etype / $vtype — engine-level defines
namespace core {

    // type_name — host type naming for type(): AScript's handles get their names, the rest "handle"
    static const char* type_name(const alx::variant& _v, void*) {
        if (!_v.is<alx::anyptr>()) return nullptr;
        const alx::anyptr& p = _v.to<alx::anyptr>();
        if (p.null()) return nullptr;
        const char* n = utils::handle_name(p);
        return n ? n : "handle";
    }

    void global::install(alx::script::engine* _e) const {
        _e->set_etype(ENGINE_ETYPE);
        _e->set_vtype(ENGINE_VTYPE);
        _e->set_define("etype", std::string(ENGINE_ETYPE));
        _e->set_define("vtype", alx::int_64(ENGINE_VTYPE));
        _e->set_type_extend(type_name);
    }

    const char* global::doc_path() const {
        return "global.json";
    }

} // namespace core
