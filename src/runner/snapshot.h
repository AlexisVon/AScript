#ifndef ARUNNER_SNAPSHOT_H
#define ARUNNER_SNAPSHOT_H

#include "../proto/message.h"

#include <ascript.h>

#include <string>
#include <vector>

// snapshot — lazy variable views served inside the paused hook callback
namespace runner {
    class snapshot {
    public:
        // serve — one page of _ref's children; false when the ref does not resolve
        static bool serve(alx::script::hook_info& _info, const std::string& _ref, alx::uint_64 _ofst,
                          alx::uint_64 _limit, const std::string& _owner,
                          std::vector<proto::snapshot_node>& _nodes, bool& _more);
        // resolve — the value at a frame/root ref; false when it does not resolve
        static bool resolve(alx::script::hook_info& _info, const std::string& _ref,
                            const alx::variant*& _out, std::string& _error);
    };
} // namespace runner

#endif // ARUNNER_SNAPSHOT_H
