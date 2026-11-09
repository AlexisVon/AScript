// Ollama link — handle-based API with area system
#include "ahttp.h"
#include "ajson.h"
#include "aplatform.h"
#include "ascript.h"

using namespace alx;
using namespace alx::script;
using namespace alx::http;

// --- context struct ---
struct ollama_ctx {
    client* cli;
    std::string model;
    int_64 timeout;
    varvec history;
    std::string addr;

    ollama_ctx() : cli(nullptr), timeout(60) {}
    ~ollama_ctx() { delete cli; }
};

// --- forward declarations ---
static void fn_chat(fwrap& args);
static void fn_generate(fwrap& args);
static void fn_show(fwrap& args);
static void fn_clear(fwrap& args);
static void fn_alive(fwrap& args);

// --- helpers ---
static std::string parse_host(const std::string& url) {
    size_t pos = url.find("://");
    std::string raw = (pos != std::string::npos) ? url.substr(pos + 3) : url;
    if (!raw.empty() && raw.back() == '/') raw.pop_back();
    return raw;
}

static client* connect_host(const std::string& addr, int_64 timeout) {
    client* cli = new client(addr);
    if (!cli->open()) {
        delete cli;
        return nullptr;
    }
    datetime wait;
    while (wait.elapsed_ms() < (uint_64) timeout * 1000 && !cli->is_connect()) high_precision_sleep(50);
    if (!cli->is_connect()) {
        delete cli;
        return nullptr;
    }
    return cli;
}

static std::string post_json(client* cli, const std::string& host,
                             const std::list<std::string>& path,
                             const json_object& body, int_64 timeout) {
    if (!cli || !cli->is_connect()) return "";
    request req(action::POST, version::V1_1, true);
    req.set_urlwords(path);
    req.set_content(body);
    req.set_field("Host", host);
    std::future<reply> fut;
    if (!cli->exec(req, fut)) return "";
    auto status = fut.wait_for(std::chrono::seconds((long) timeout));
    if (status == std::future_status::timeout) return "";
    reply rep = fut.get();
    return std::string(rep.get_content().begin(), rep.get_content().end());
}

static std::string get_json(client* cli, const std::string& host,
                            const std::list<std::string>& path, int_64 timeout) {
    if (!cli || !cli->is_connect()) return "";
    request req(action::GET, version::V1_1, true);
    req.set_urlwords(path);
    req.set_field("Host", host);
    std::future<reply> fut;
    if (!cli->exec(req, fut)) return "";
    if (std::future_status::timeout == fut.wait_for(std::chrono::seconds((long) timeout))) return "";
    reply rep = fut.get();
    return std::string(rep.get_content().begin(), rep.get_content().end());
}

// --- global functions ---

// olm.list(host, [timeout]) → vec | string(error)
static void fn_list(fwrap& args) {
    if (args.size() < 1 || !args[0].is<std::string>())
        args.raise(variant(std::string("ollama.list: expected a host string")), error_type::ArgError);

    std::string host = parse_host(args[0].to<std::string>());
    int_64 timeout = 5;
    if (args.size() > 1 && args[1].is<int_64>())
        timeout = args[1].to<int_64>();

    client* cli = connect_host(host, timeout);
    if (!cli) {
        args.freturn(variant(std::string("connection to '") + host + "' failed"));
        return;
    }

    std::string body = get_json(cli, host, {"api", "tags"}, timeout);
    delete cli;

    bool ok = false;
    json_object obj = json_doc::from_json(body, &ok);
    if (ok && obj.contain("models") && obj["models"].is_array()) {
        json_array models = obj["models"].to_array();
        varvec result;
        for (size_t i = 0; i < models.size(); i++) {
            if (models[i].is_object()) {
                json_object m = models[i].to_object();
                varmap mv;
                if (m.contain("name")) mv["name"] = variant(m["name"].to_string());
                if (m.contain("size")) mv["size"] = variant(m["size"].to_intg());
                result.push_back(variant(std::move(mv)));
            }
        }
        args.freturn(variant(std::move(result)));
    } else {
        args.freturn(variant(std::string("invalid response from '") + host + "'"));
    }
}

// olm.connect(key, host, model, [timeout]) → bool
static void fn_connect(fwrap& args) {
    if (args.size() < 3 || !args[0].is<std::string>() ||
        !args[1].is<std::string>() || !args[2].is<std::string>())
        args.raise(variant(std::string("ollama.connect: expected (key, host, model, [timeout])")), error_type::ArgError);

    std::string key = args[0].to<std::string>();
    std::string host = parse_host(args[1].to<std::string>());
    std::string model = args[2].to<std::string>();
    int_64 timeout = 10;
    if (args.size() > 3 && args[3].is<int_64>())
        timeout = args[3].to<int_64>();

    ollama_ctx* ctx = new ollama_ctx();
    ctx->model = model;
    ctx->timeout = timeout;
    ctx->addr = host;
    ctx->cli = connect_host(host, timeout);
    if (!ctx->cli) {
        delete ctx;
        args.freturn(variant(false));
        return;
    }

    args.remove(key);
    args.wrap<ollama_ctx>(ctx, key, {
                                        {"chat", fn_chat},
                                        {"generate", fn_generate},
                                        {"show", fn_show},
                                        {"clear", fn_clear},
                                        {"alive", fn_alive},
                                    });
    args.freturn(variant(true));
}

// --- area functions ---

static void fn_chat(fwrap& args) {
    if (args.size() < 1)
        args.raise(variant(std::string("ollama.chat: expected (input, [options])")), error_type::ArgError);

    ollama_ctx* ctx = args.unwrap<ollama_ctx>();
    if (!ctx || !ctx->cli)
        throw script_exception(error_type::RuntimeError, "ollama.chat: not connected");

    varvec new_msgs;
    if (args[0].is<std::string>()) {
        varmap m;
        m["role"] = variant(std::string("user"));
        m["content"] = args[0].to<std::string>();
        new_msgs.push_back(variant(std::move(m)));
    } else if (args[0].is<varmap>()) {
        new_msgs.push_back(args[0]);
    } else if (args[0].is<varvec>()) {
        new_msgs = args[0].to<varvec>();
    } else {
        args.raise(variant(std::string("ollama.chat: input must be string, map, or vec")), error_type::TypeError);
    }

    for (auto& m : new_msgs) ctx->history.push_back(m);

    json_array messages;
    for (auto& m : ctx->history) {
        if (m.is<varmap>()) {
            varmap mv = m.to<varmap>();
            json_object msg;
            if (mv.contain("role")) msg["role"] = json_value(mv["role"].to<std::string>());
            if (mv.contain("content")) msg["content"] = json_value(mv["content"].to<std::string>());
            messages.append(json_value(std::move(msg)));
        }
    }

    json_object body;
    body["model"] = json_value(ctx->model);
    body["messages"] = json_value(std::move(messages));
    body["stream"] = json_value(false);
    if (args.size() > 1 && args[1].is<varmap>())
        body["options"] = json_value(json_object::from_varmap(args[1].to<varmap>()));

    std::string resp = post_json(ctx->cli, ctx->addr, {"api", "chat"}, body, ctx->timeout);
    if (resp.empty()) return;

    bool ok = false;
    json_object obj = json_doc::from_json(resp, &ok);
    if (ok && obj.contain("message") && obj["message"].is_object()) {
        json_object msg = obj["message"].to_object();
        if (msg.contain("content")) {
            std::string content = msg["content"].to_string();
            varmap reply;
            reply["role"] = variant(std::string("assistant"));
            reply["content"] = variant(content);
            ctx->history.push_back(variant(std::move(reply)));
            args.freturn(variant(content));
            return;
        }
    }
    args.freturn(variant(resp));
}

static void fn_generate(fwrap& args) {
    if (args.size() < 1 || !args[0].is<std::string>())
        args.raise(variant(std::string("ollama.generate: expected (prompt, [options])")), error_type::ArgError);

    ollama_ctx* ctx = args.unwrap<ollama_ctx>();
    if (!ctx || !ctx->cli)
        throw script_exception(error_type::RuntimeError, "ollama.generate: not connected");

    json_object body;
    body["model"] = json_value(ctx->model);
    body["prompt"] = json_value(args[0].to<std::string>());
    body["stream"] = json_value(false);
    if (args.size() > 1 && args[1].is<varmap>())
        body["options"] = json_value(json_object::from_varmap(args[1].to<varmap>()));

    std::string resp = post_json(ctx->cli, ctx->addr, {"api", "generate"}, body, ctx->timeout);
    if (resp.empty()) return;

    bool ok = false;
    json_object obj = json_doc::from_json(resp, &ok);
    if (ok && obj.contain("response"))
        args.freturn(variant(obj["response"].to_string()));
    else
        args.freturn(variant(resp));
}

// olm.show() | olm.show(addr, model)
static void fn_show(fwrap& args) {
    // global: olm.show(addr, model, [timeout])
    if (args.size() >= 2 && args[0].is<std::string>() && args[1].is<std::string>()) {
        std::string host = parse_host(args[0].to<std::string>());
        std::string model = args[1].to<std::string>();
        int_64 timeout = 5;
        if (args.size() > 2 && args[2].is<int_64>())
            timeout = args[2].to<int_64>();
        client* cli = connect_host(host, timeout);
        if (!cli) return;
        json_object body;
        body["name"] = json_value(model);
        std::string resp = post_json(cli, host, {"api", "show"}, body, timeout);
        delete cli;
        args.freturn(variant(resp));
        return;
    }

    // area: olm.<key>.show()
    if (args.size() != 0)
        args.raise(variant(std::string("ollama.show: expected () or (addr, model)")), error_type::ArgError);

    ollama_ctx* ctx = args.unwrap<ollama_ctx>();
    if (!ctx || !ctx->cli)
        throw script_exception(error_type::RuntimeError, "ollama.show: not connected");

    json_object body;
    body["name"] = json_value(ctx->model);
    std::string resp = post_json(ctx->cli, ctx->addr, {"api", "show"}, body, ctx->timeout);
    args.freturn(variant(resp));
}

static void fn_clear(fwrap& args) {
    ollama_ctx* ctx = args.unwrap<ollama_ctx>();
    if (ctx) ctx->history.clear();
}

static void fn_alive(fwrap& args) {
    ollama_ctx* ctx = args.unwrap<ollama_ctx>();
    args.freturn(variant(ctx && ctx->cli && ctx->cli->is_connect()));
}

// --- link entry points ---

extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
void alexis_script_load(fwrap& args) {
    args.bind("list", fn_list);
    args.bind("show", fn_show);
    args.bind("connect", fn_connect);
}
