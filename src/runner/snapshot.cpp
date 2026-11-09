#include "snapshot.h"

#include "core/extend.h"
#include "core/inspect.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>

namespace runner {
    namespace {
        constexpr alx::uint_64 kDefaultLimit = 200;
        constexpr size_t kValueLimit = 4096;

        // window — one page of a snapshot request
        struct window {
            alx::uint_64 ofst = 0;
            alx::uint_64 limit = kDefaultLimit;
            std::string owner; // owning .axp path for embedded module refs
        };

        // split — '/'-separated path segments
        std::vector<std::string> split(const std::string& _path) {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= _path.size()) {
                const size_t slash = _path.find('/', start);
                const size_t end = slash == std::string::npos ? _path.size() : slash;
                if (end > start) out.push_back(_path.substr(start, end - start));
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            return out;
        }

        // page — the window's slice of _keys; reports whether more remain
        std::vector<std::string> page(std::vector<std::string> _keys, const window& _w, bool& _more) {
            const alx::uint_64 size = _keys.size();
            const alx::uint_64 begin = std::min(_w.ofst, size);
            const alx::uint_64 end = std::min(size, begin + _w.limit);
            _more = end < size;
            return std::vector<std::string>(_keys.begin() + static_cast<size_t>(begin),
                                            _keys.begin() + static_cast<size_t>(end));
        }

        // open_path — the path a GUI can open: embedded "@key" carries its owner .axp
        std::string open_path(const std::string& _path, const window& _w) {
            return !_path.empty() && _path[0] == '@' ? _w.owner + _path : _path;
        }

        // truncated — a rendered value capped at 4 KiB
        std::string truncated(const std::string& _s) {
            return _s.size() <= kValueLimit ? _s : _s.substr(0, kValueLimit) + "…";
        }

        // type_of — the name the script's type() gives _v
        std::string type_of(alx::script::hook_info& _info, const alx::variant& _v) {
            try {
                return _info.wktype(_v);
            } catch (...) {
                return "unknown";
            }
        }

        // container_size — element count of a map/vec/lst, 0 otherwise
        alx::uint_64 container_size(const alx::variant& _v) {
            if (_v.is<alx::varmap>()) return _v.to<alx::varmap>().size();
            if (_v.is<alx::varvec>()) return _v.to<alx::varvec>().size();
            if (_v.is<alx::varlst>()) return _v.to<alx::varlst>().size();
            return 0;
        }

        // summary — "map(3)" / "vec[10]" / "lst[2]" for an expandable value
        std::string summary(const alx::variant& _v) {
            const std::string n = std::to_string(container_size(_v));
            if (_v.is<alx::varmap>()) return "map(" + n + ")";
            if (_v.is<alx::varvec>()) return "vec[" + n + "]";
            if (_v.is<alx::varlst>()) return "lst[" + n + "]";
            return std::string();
        }

        // make_node — one snapshot node; ref points at the value's children
        proto::snapshot_node make_node(alx::script::hook_info& _info, const std::string& _name,
                                       const alx::variant& _v, const std::string& _ref) {
            proto::snapshot_node n;
            n.name = _name;
            n.ref = _ref;
            if (_v.null()) {
                n.type = "null";
                n.value = "null";
                return n;
            }
            if (_v.is<alx::anyptr>()) {
                const alx::anyptr& p = _v.to<alx::anyptr>();
                n.kind = "handle";
                n.handle = core::utils::handle_hex(p);
                n.type = type_of(_info, _v);
                std::string d;
                try {
                    _info.wkdesc(p, d);
                } catch (...) {
                    d.clear();
                }
                if (d.empty()) core::inspect::describe(p, d);
                n.value = d.empty() ? n.handle : d;
                n.shared = p.shared();
                return n;
            }
            if (_info.wkis_ent(&_v)) {
                n.type = "module";
                n.kind = "module";
                n.value = "<module>";
                n.has_children = true;
                return n;
            }
            if (_info.wkis_link(&_v)) {
                n.type = "link";
                n.kind = "link";
                n.value = "<link>";
                n.has_children = true;
                return n;
            }
            if (_info.wkis_func(&_v)) {
                n.type = "func";
                n.value = "<func>";
                return n;
            }
            n.type = type_of(_info, _v);
            if (_v.is<alx::varmap>() || _v.is<alx::varvec>() || _v.is<alx::varlst>()) {
                n.value = summary(_v);
                n.has_children = container_size(_v) > 0;
                return n;
            }
            n.value = truncated(core::utils::repr(_v));
            return n;
        }

        // step — resolve one "key", "key[i]" or "[i]" segment against _v
        const alx::variant* step(alx::script::hook_info& _info, const alx::variant& _v,
                                 const std::string& _seg) {
            std::string key = _seg;
            bool indexed = false;
            alx::uint_64 index = 0;
            if (_seg.size() > 1 && _seg.back() == ']') {
                const size_t br = _seg.rfind('[');
                if (br != std::string::npos) {
                    key = _seg.substr(0, br);
                    index = static_cast<alx::uint_64>(std::strtoull(_seg.c_str() + br + 1, nullptr, 10));
                    indexed = true;
                }
            }

            const alx::variant* cur = &_v;
            if (!key.empty()) {
                if (_info.wkis_ent(cur)) {
                    const void* e = _info.wken_of(cur);
                    cur = e ? _info.wken_data_cptr(e, key.c_str()) : nullptr;
                } else if (_info.wkis_link(cur)) {
                    const void* l = _info.wklk_of(cur);
                    cur = l ? _info.wklk_data_cptr(l, key.c_str()) : nullptr;
                } else if (cur->is<alx::varmap>()) {
                    const alx::varmap& m = cur->to<alx::varmap>();
                    cur = m.contain(key) ? &m.value(key) : nullptr;
                } else {
                    return nullptr;
                }
                if (!cur) return nullptr;
            }
            if (!indexed) return cur;
            if (cur->is<alx::varvec>()) {
                const alx::varvec& a = cur->to<alx::varvec>();
                return index < a.size() ? &a[index] : nullptr;
            }
            if (cur->is<alx::varlst>()) {
                const alx::varlst& l = cur->to<alx::varlst>();
                if (index >= l.size()) return nullptr;
                auto it = l.begin();
                std::advance(it, static_cast<long>(index));
                return &*it;
            }
            return nullptr;
        }

        // _v_leaf — rendered leaf value, truncated
        std::string _v_leaf(const alx::variant& _v) {
            return truncated(core::utils::repr(_v));
        }

        // module_path — an imported module's path, empty for the root
        std::string module_path(alx::script::hook_info& _info, const void* _e) {
            if (!_e) return std::string();
            std::string path = _info.wken_path(_e);
            if (path.empty()) path = _info.wken_file(_e);
            if (path == "::") path.clear();
            return path;
        }

        // find_module — depth-first search of the module tree by path
        const void* find_module(alx::script::hook_info& _info, const void* _root, const std::string& _path) {
            if (!_root) return nullptr;
            std::vector<const void*> stack{_root};
            while (!stack.empty()) {
                const void* e = stack.back();
                stack.pop_back();
                for (const auto& key : _info.wken_data_keys(e)) {
                    const alx::variant* v = _info.wken_data_cptr(e, key.c_str());
                    if (!v || !_info.wkis_ent(v)) continue;
                    const void* child = _info.wken_of(v);
                    if (!child) continue;
                    if (module_path(_info, child) == _path) return child;
                    stack.push_back(child);
                }
            }
            return nullptr;
        }

        // module_nodes — the root's imported modules and links, one page
        bool module_nodes(alx::script::hook_info& _info, const window& _w,
                          std::vector<proto::snapshot_node>& _nodes, bool& _more) {
            const void* root = _info.wken_root();
            if (!root) return false;
            for (const auto& key : page(_info.wken_data_keys(root), _w, _more)) {
                const alx::variant* v = _info.wken_data_cptr(root, key.c_str());
                if (!v) continue;
                proto::snapshot_node n;
                n.name = key;
                if (_info.wkis_ent(v)) {
                    const void* child = _info.wken_of(v);
                    const std::string path = module_path(_info, child);
                    n.type = "module";
                    n.kind = "module";
                    n.value = open_path(path, _w);
                    if (!path.empty()) n.ref = "mod:" + path;
                    n.has_children = true;
                    _nodes.push_back(std::move(n));
                } else if (_info.wkis_link(v)) {
                    n.type = "link";
                    n.kind = "link";
                    n.value = "<link>";
                    n.has_children = true;
                    _nodes.push_back(std::move(n));
                }
            }
            return true;
        }

        // module_members — names stored in one imported module; nested modules get their own ref
        bool module_members(alx::script::hook_info& _info, const void* _e, const window& _w,
                            std::vector<proto::snapshot_node>& _nodes, bool& _more) {
            if (!_e) return false;
            for (const auto& key : page(_info.wken_data_keys(_e), _w, _more)) {
                const alx::variant* v = _info.wken_data_cptr(_e, key.c_str());
                if (!v) continue;
                proto::snapshot_node n;
                n.name = key;
                if (_info.wkis_ent(v)) {
                    const void* child = _info.wken_of(v);
                    const std::string path = module_path(_info, child);
                    n.type = "module";
                    n.kind = "module";
                    n.value = open_path(path, _w);
                    if (!path.empty()) n.ref = "mod:" + path;
                    n.has_children = true;
                } else if (_info.wkis_link(v)) {
                    n.type = "link";
                    n.kind = "link";
                    n.value = "<link>";
                    n.has_children = true;
                } else {
                    n.type = type_of(_info, *v);
                    n.value = _v_leaf(*v);
                }
                _nodes.push_back(std::move(n));
            }
            return true;
        }

        // entity_children — names stored directly in _entity, one page
        bool entity_children(alx::script::hook_info& _info, const void* _entity, const std::string& _ref,
                             const window& _w, std::vector<proto::snapshot_node>& _nodes, bool& _more) {
            if (!_entity) return false;
            for (const auto& key : page(_info.wken_data_keys(_entity), _w, _more)) {
                const alx::variant* v = _info.wken_data_cptr(_entity, key.c_str());
                if (!v) continue;
                _nodes.push_back(make_node(_info, key, *v, _ref + "/" + key));
            }
            return true;
        }

        // children_of — children of a value reached by _ref, one page
        bool children_of(alx::script::hook_info& _info, const alx::variant& _v, const std::string& _ref,
                         const window& _w, std::vector<proto::snapshot_node>& _nodes, bool& _more) {
            if (_info.wkis_ent(&_v))
                return entity_children(_info, _info.wken_of(&_v), _ref, _w, _nodes, _more);
            if (_info.wkis_link(&_v)) {
                const void* l = _info.wklk_of(&_v);
                if (!l) return true;
                for (const auto& key : page(_info.wklk_data_keys(l), _w, _more)) {
                    const alx::variant* v = _info.wklk_data_cptr(l, key.c_str());
                    if (!v) continue;
                    _nodes.push_back(make_node(_info, key, *v, _ref + "/" + key));
                }
                return true;
            }
            if (_v.is<alx::varmap>()) {
                const alx::varmap& m = _v.to<alx::varmap>();
                alx::uint_64 index = 0;
                alx::uint_64 emitted = 0;
                for (auto it = m.begin(); it != m.end(); ++it, ++index) {
                    if (index < _w.ofst) continue;
                    if (emitted == _w.limit) break;
                    _nodes.push_back(make_node(_info, it.key(), it.value(), _ref + "/" + it.key()));
                    ++emitted;
                }
                _more = m.size() > _w.ofst + emitted;
                return true;
            }
            if (_v.is<alx::varvec>()) {
                const alx::varvec& a = _v.to<alx::varvec>();
                const size_t begin = static_cast<size_t>(std::min<alx::uint_64>(_w.ofst, a.size()));
                alx::uint_64 emitted = 0;
                for (size_t i = begin; i < a.size() && emitted < _w.limit; ++i, ++emitted) {
                    const std::string name = "[" + std::to_string(_w.ofst + emitted) + "]";
                    _nodes.push_back(make_node(_info, name, a[i], _ref + "/" + name));
                }
                _more = a.size() > _w.ofst + emitted;
                return true;
            }
            if (_v.is<alx::varlst>()) {
                const alx::varlst& l = _v.to<alx::varlst>();
                auto it = l.begin();
                for (alx::uint_64 n = 0; n < _w.ofst && it != l.end(); ++n) ++it;
                alx::uint_64 emitted = 0;
                for (; it != l.end() && emitted < _w.limit; ++it, ++emitted) {
                    const std::string name = "[" + std::to_string(_w.ofst + emitted) + "]";
                    _nodes.push_back(make_node(_info, name, *it, _ref + "/" + name));
                }
                _more = l.size() > _w.ofst + emitted;
                return true;
            }
            return true; // a leaf has no children
        }

        // value_of — the value at a '/'-separated path below _entity
        bool value_of(alx::script::hook_info& _info, const void* _entity, const std::string& _path,
                      const alx::variant*& _out) {
            _out = nullptr;
            if (!_entity) return false;
            const std::vector<std::string> segs = split(_path);
            if (segs.empty()) return false;
            const alx::variant* v = _info.wken_data_cptr(_entity, segs[0].c_str());
            for (size_t i = 1; v && i < segs.size(); ++i) v = step(_info, *v, segs[i]);
            _out = v;
            return v != nullptr;
        }

        // frame_value — the value at a '/'-separated local path in _frame
        bool frame_value(alx::script::hook_info& _info, alx::uint_64 _frame, const std::string& _path,
                         const alx::variant*& _out) {
            _out = nullptr;
            if (_frame >= _info.wkfm_size()) return false;
            const std::vector<std::string> segs = split(_path);
            if (segs.empty()) return false;
            const alx::variant* v = _info.wkfm_data_cptr(_frame, segs[0].c_str());
            for (size_t i = 1; v && i < segs.size(); ++i) v = step(_info, *v, segs[i]);
            _out = v;
            return v != nullptr;
        }

        // frame_locals — one page of frame _frame's locals
        bool frame_locals(alx::script::hook_info& _info, alx::uint_64 _frame, const std::string& _ref,
                          const window& _w, std::vector<proto::snapshot_node>& _nodes, bool& _more) {
            if (_frame >= _info.wkfm_size()) return false;
            for (const auto& key : page(_info.wkfm_data_keys(_frame), _w, _more)) {
                const alx::variant* v = _info.wkfm_data_cptr(_frame, key.c_str());
                if (!v) continue;
                _nodes.push_back(make_node(_info, key, *v, _ref + "/" + key));
            }
            return true;
        }
    } // namespace

    bool snapshot::serve(alx::script::hook_info& _info, const std::string& _ref, alx::uint_64 _ofst,
                         alx::uint_64 _limit, const std::string& _owner,
                         std::vector<proto::snapshot_node>& _nodes, bool& _more) {
        _more = false;
        window w;
        w.ofst = _ofst;
        w.limit = _limit ? _limit : kDefaultLimit;
        w.owner = _owner;
        if (_ref.empty()) return false;

        if (_ref == "modules") return module_nodes(_info, w, _nodes, _more);
        if (_ref.rfind("mod:", 0) == 0) {
            const std::string path = _ref.substr(4);
            const void* e = find_module(_info, _info.wken_root(), path);
            if (!e) return false;
            return module_members(_info, e, w, _nodes, _more);
        }
        if (_ref == "root")
            return entity_children(_info, _info.wken_root(), _ref, w, _nodes, _more);
        if (_ref.rfind("root/", 0) == 0) {
            const alx::variant* v = nullptr;
            std::string err;
            if (!resolve(_info, _ref, v, err)) return false;
            return children_of(_info, *v, _ref, w, _nodes, _more);
        }

        if (_ref.size() > 1 && _ref[0] == 'f' && std::isdigit(static_cast<unsigned char>(_ref[1]))) {
            const size_t slash = _ref.find('/');
            const std::string head = _ref.substr(0, slash);
            const alx::uint_64 frame =
                static_cast<alx::uint_64>(std::strtoull(head.c_str() + 1, nullptr, 10));
            if (slash == std::string::npos)
                return frame_locals(_info, frame, _ref, w, _nodes, _more);
            const alx::variant* v = nullptr;
            std::string err;
            if (!resolve(_info, _ref, v, err)) return false;
            return children_of(_info, *v, _ref, w, _nodes, _more);
        }
        return false;
    }

    bool snapshot::resolve(alx::script::hook_info& _info, const std::string& _ref,
                           const alx::variant*& _out, std::string& _error) {
        _out = nullptr;
        if (_ref.rfind("root/", 0) == 0) {
            if (value_of(_info, _info.wken_root(), _ref.substr(5), _out)) return true;
            _error = "view ref does not resolve: " + _ref;
            return false;
        }
        if (_ref.size() > 1 && _ref[0] == 'f' && std::isdigit(static_cast<unsigned char>(_ref[1]))) {
            const size_t slash = _ref.find('/');
            if (slash == std::string::npos) {
                _error = "view ref names a frame, not a value: " + _ref;
                return false;
            }
            const alx::uint_64 frame =
                static_cast<alx::uint_64>(std::strtoull(_ref.c_str() + 1, nullptr, 10));
            if (frame_value(_info, frame, _ref.substr(slash + 1), _out)) return true;
            _error = "view ref does not resolve: " + _ref;
            return false;
        }
        _error = "view ref is not a value: " + _ref;
        return false;
    }
} // namespace runner
