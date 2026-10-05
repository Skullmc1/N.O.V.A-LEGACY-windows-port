#include "jni.h"

#include <map>
#include <mutex>
#include <unordered_map>

#include "libc.h"

namespace {
struct JMember { std::string cls, name, sig; };

std::mutex g_mutex;
std::unordered_map<std::string, JObj*> g_classes;
std::unordered_map<std::string, JavaMethod> g_methods;
std::unordered_map<std::string, JRet> g_static_fields;
u64 g_env = 0, g_vm = 0;

const char* const TYPES[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
const char* const PRIMS[] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
const int PRIM_SIZE[] = {1, 1, 2, 2, 4, 8, 4, 8};

JObj* obj(u64 h) { return reinterpret_cast<JObj*>(h); }
u64 handle(JObj* o) { return reinterpret_cast<u64>(o); }
const char* cstr(u64 p) { return reinterpret_cast<const char*>(p); }

std::vector<std::string> param_types(const std::string& sig) {
    std::vector<std::string> out;
    size_t i = 1;
    while (i < sig.size() && sig[i] != ')') {
        size_t start = i;
        while (sig[i] == '[') i++;
        if (sig[i] == 'L') i = sig.find(';', i);
        i++;
        out.push_back(sig.substr(start, i - start));
    }
    return out;
}

u64 make_result(Thread& t, const std::string& rtype, const JRet& r) {
    char k = rtype[0];
    if (k == 'V') return 0;
    if (k == 'F') { t.sets(0, static_cast<float>(r.kind == JRet::Dbl ? r.d : static_cast<double>(r.i))); return t.x(0); }
    if (k == 'D') { t.setd(0, r.kind == JRet::Dbl ? r.d : static_cast<double>(r.i)); return t.x(0); }
    if (k == 'L' || k == '[') {
        if (r.kind == JRet::Str) return jni::new_string(r.s);
        if (r.kind == JRet::Obj) return handle(r.o);
        if (r.kind == JRet::Int) return r.i;       // explicit null or a handle
        if (rtype == "Ljava/lang/String;") return jni::new_string("");
        if (k == '[') {
            auto* a = new JObj;
            a->cls = rtype;
            if (rtype.size() == 2) { a->elem_size = rtype[1] == 'J' || rtype[1] == 'D' ? 8 : rtype[1] == 'I' || rtype[1] == 'F' ? 4 : rtype[1] == 'S' || rtype[1] == 'C' ? 2 : 1; }
            else a->is_object_array = true;
            return handle(a);
        }
        return jni::new_object(rtype.substr(1, rtype.size() - 2));
    }
    return r.i;
}

void call_method(Thread& t, int kind /*0 virtual,1 nonvirtual,2 static*/, char variant) {
    int id_reg = kind == 1 ? 3 : 2;
    auto* m = reinterpret_cast<JMember*>(t.x(id_reg));
    if (!m) { t.setx(0, 0); return; }
    auto ptypes = param_types(m->sig);
    std::string rtype = m->sig.substr(m->sig.find(')') + 1);
    std::vector<JArg> args;
    if (variant == 'A') {
        const u64* base = reinterpret_cast<const u64*>(t.x(id_reg + 1));
        for (size_t i = 0; i < ptypes.size(); i++) {
            JArg a; a.i = base[i];
            if (ptypes[i] == "D") memcpy(&a.d, &base[i], 8);
            else if (ptypes[i] == "F") { float f; memcpy(&f, &base[i], 4); a.d = f; }
            args.push_back(a);
        }
    } else {
        std::unique_ptr<VArgs> src;
        if (variant == 'V') src = std::make_unique<VaList>(t.x(id_reg + 1));
        else src = std::make_unique<RegArgs>(t, id_reg + 1);
        for (auto& p : ptypes) {
            JArg a;
            if (p == "F" || p == "D") a.d = src->fp(); else a.i = src->gp();
            args.push_back(a);
        }
    }
    std::string key = m->cls + "." + m->name;
    JavaMethod fn;
    {
        std::lock_guard lk(g_mutex);
        auto it = g_methods.find(key);
        if (it != g_methods.end()) fn = it->second;
    }
    JRet r;
    if (fn) {
        r = fn(args);
    } else {
        std::string shown;
        for (size_t i = 0; i < args.size(); i++) {
            if (i) shown += ", ";
            if (ptypes[i] == "Ljava/lang/String;" && args[i].i) shown += "'" + jni::to_string(args[i].i).substr(0, 60) + "'";
            else if (ptypes[i] == "F" || ptypes[i] == "D") shown += std::to_string(args[i].d);
            else shown += std::to_string(static_cast<i64>(args[i].i));
        }
        log_once("java:" + key + m->sig, "[java] %s%s (%s)   (default)", key.c_str(), m->sig.c_str(), shown.c_str());
    }
    t.setx(0, make_result(t, rtype, r));
}

void field_access(Thread& t, bool set, bool is_static) {
    auto* m = reinterpret_cast<JMember*>(t.x(2));
    if (!m || set) { if (!set) t.setx(0, 0); return; }
    std::string key = m->cls + "." + m->name;
    JRet r;
    {
        std::lock_guard lk(g_mutex);
        auto it = g_static_fields.find(key);
        if (it != g_static_fields.end()) r = it->second;
        else log_once("field:" + key, "[java] get field %s (%s)   (default)", key.c_str(), m->sig.c_str());
    }
    t.setx(0, make_result(t, m->sig, r));
}

void array_op(Thread& t, const std::string& op, int prim, const std::string& what) {
    int size = PRIM_SIZE[prim];
    if (op == "New") {
        auto* a = new JObj;
        a->cls = std::string("[") + PRIMS[prim];
        a->elem_size = size;
        a->bytes.resize(static_cast<size_t>(static_cast<i32>(t.x(1))) * size);
        t.setx(0, handle(a));
        return;
    }
    JObj* a = obj(t.x(1));
    if (what == "Elements") {
        if (op == "Get") {
            if (t.x(2)) *reinterpret_cast<u8*>(t.x(2)) = 0;        // isCopy = false: we hand out the real storage
            if (a->bytes.empty()) a->bytes.reserve(16);
            t.setx(0, reinterpret_cast<u64>(a->bytes.data()));
        }
        return;                                                   // Release: nothing to copy back
    }
    size_t start = static_cast<size_t>(static_cast<i32>(t.x(2))) * size, n = static_cast<size_t>(static_cast<i32>(t.x(3))) * size;
    if (start + n > a->bytes.size()) return;
    if (op == "Get") memcpy(reinterpret_cast<void*>(t.x(4)), a->bytes.data() + start, n);
    else memcpy(a->bytes.data() + start, reinterpret_cast<void*>(t.x(4)), n);
}

Handler handler_for(const std::string& name) {
    // Call<Kind><Type>Method<Variant>
    if (name.rfind("Call", 0) == 0 && name.find("Method") != std::string::npos) {
        int kind = name.rfind("CallStatic", 0) == 0 ? 2 : name.rfind("CallNonvirtual", 0) == 0 ? 1 : 0;
        char variant = name.back() == 'V' ? 'V' : name.back() == 'A' ? 'A' : ' ';
        return [kind, variant](Thread& t) { call_method(t, kind, variant); };
    }
    for (const char* type : TYPES) {
        std::string ty = type;
        if (name == "Get" + ty + "Field") return [](Thread& t) { field_access(t, false, false); };
        if (name == "GetStatic" + ty + "Field") return [](Thread& t) { field_access(t, false, true); };
        if (name == "Set" + ty + "Field" || name == "SetStatic" + ty + "Field") return [](Thread& t) {};
    }
    for (int p = 0; p < 8; p++) {
        std::string pr = PRIMS[p];
        for (const char* op : {"New", "Get", "Release", "Set"})
            for (const char* what : {"", "Elements", "Region"})
                if (name == std::string(op) + pr + "Array" + what) {
                    std::string o = op, w = what;
                    return [o, p, w](Thread& t) { array_op(t, o, p, w); };
                }
    }
    static const std::map<std::string, Handler> simple = {
        {"GetVersion", [](Thread& t) { t.setx(0, 0x10006); }},
        {"FindClass", [](Thread& t) { t.setx(0, jni::find_class(cstr(t.x(1)))); }},
        {"GetObjectClass", [](Thread& t) { JObj* o = obj(t.x(1)); t.setx(0, jni::find_class(o ? o->cls : "java/lang/Object")); }},
        {"GetSuperclass", [](Thread& t) { t.setx(0, jni::find_class("java/lang/Object")); }},
        {"IsInstanceOf", [](Thread& t) { t.setx(0, 1); }},
        {"IsSameObject", [](Thread& t) { t.setx(0, t.x(1) == t.x(2)); }},
        {"NewGlobalRef", [](Thread& t) { t.setx(0, t.x(1)); }},
        {"NewLocalRef", [](Thread& t) { t.setx(0, t.x(1)); }},
        {"NewWeakGlobalRef", [](Thread& t) { t.setx(0, t.x(1)); }},
        {"PopLocalFrame", [](Thread& t) { t.setx(0, t.x(1)); }},
        {"NewStringUTF", [](Thread& t) { t.setx(0, jni::new_string(t.x(1) ? cstr(t.x(1)) : "")); }},
        {"GetStringUTFChars", [](Thread& t) {
             if (t.x(2)) *reinterpret_cast<u8*>(t.x(2)) = 1;
             t.setx(0, reinterpret_cast<u64>(g_strdup(jni::to_string(t.x(1)).c_str())));
         }},
        {"ReleaseStringUTFChars", [](Thread& t) { g_free(reinterpret_cast<void*>(t.x(2))); }},
        {"GetStringUTFLength", [](Thread& t) { t.setx(0, jni::to_string(t.x(1)).size()); }},
        {"GetStringLength", [](Thread& t) { t.setx(0, jni::to_string(t.x(1)).size()); }},
        {"GetStringUTFRegion", [](Thread& t) {
             std::string s = jni::to_string(t.x(1));
             size_t start = static_cast<size_t>(t.x(2)), n = static_cast<size_t>(t.x(3));
             if (start <= s.size()) { std::string part = s.substr(start, n); memcpy(reinterpret_cast<void*>(t.x(4)), part.c_str(), part.size() + 1); }
         }},
        {"GetArrayLength", [](Thread& t) {
             JObj* a = obj(t.x(1));
             t.setx(0, !a ? 0 : a->is_object_array ? a->elems.size() : a->elem_size ? a->bytes.size() / a->elem_size : 0);
         }},
        {"NewObjectArray", [](Thread& t) {
             auto* a = new JObj;
             a->cls = "[Ljava/lang/Object;";
             a->is_object_array = true;
             a->elems.resize(static_cast<size_t>(static_cast<i32>(t.x(1))), t.x(3));
             t.setx(0, handle(a));
         }},
        {"GetObjectArrayElement", [](Thread& t) {
             JObj* a = obj(t.x(1));
             size_t i = static_cast<size_t>(t.x(2));
             t.setx(0, a && i < a->elems.size() ? a->elems[i] : 0);
         }},
        {"SetObjectArrayElement", [](Thread& t) {
             JObj* a = obj(t.x(1));
             size_t i = static_cast<size_t>(t.x(2));
             if (a && i < a->elems.size()) a->elems[i] = t.x(3);
         }},
        {"GetJavaVM", [](Thread& t) { *reinterpret_cast<u64*>(t.x(1)) = g_vm; t.setx(0, 0); }},
        {"RegisterNatives", [](Thread& t) {
             JObj* c = obj(t.x(1));
             const u64* m = reinterpret_cast<const u64*>(t.x(2));
             for (int i = 0; i < static_cast<i32>(t.x(3)); i++)
                 logf("[java] RegisterNatives %s.%s%s", c->str.c_str(), cstr(m[3 * i]), cstr(m[3 * i + 1]));
             t.setx(0, 0);
         }},
        {"GetPrimitiveArrayCritical", [](Thread& t) {
             JObj* a = obj(t.x(1));
             if (t.x(2)) *reinterpret_cast<u8*>(t.x(2)) = 0;
             t.setx(0, reinterpret_cast<u64>(a->bytes.data()));
         }},
    };
    auto it = simple.find(name);
    if (it != simple.end()) return it->second;

    auto member = [](Thread& t) {
        JObj* c = obj(t.x(1));
        t.setx(0, reinterpret_cast<u64>(new JMember{c ? c->str : "?", cstr(t.x(2)), cstr(t.x(3))}));
    };
    if (name == "GetMethodID" || name == "GetStaticMethodID" || name == "GetFieldID" || name == "GetStaticFieldID") return member;
    if (name == "NewObject" || name == "NewObjectV" || name == "NewObjectA" || name == "AllocObject")
        return [](Thread& t) {
            JObj* c = obj(t.x(1));
            log_once("new:" + c->str, "[java] new %s", c->str.c_str());
            t.setx(0, jni::new_object(c->str));
        };
    for (const char* zero : {"DeleteGlobalRef", "DeleteLocalRef", "DeleteWeakGlobalRef", "ExceptionCheck",
                             "ExceptionOccurred", "ExceptionClear", "ExceptionDescribe", "PushLocalFrame",
                             "EnsureLocalCapacity", "MonitorEnter", "MonitorExit", "ReleaseStringChars",
                             "ReleasePrimitiveArrayCritical", "Throw", "ThrowNew", "UnregisterNatives"})
        if (name == zero) return [](Thread& t) { t.setx(0, 0); };
    return [name](Thread& t) {
        log_once("jni:" + name, "!! unimplemented JNI function %s (from %s)", name.c_str(), guest::symbolize(t.lr()).c_str());
        t.setx(0, 0);
    };
}

std::string jni_name(int i) {
    static std::map<int, std::string> names;
    if (names.empty()) {
        const std::pair<int, const char*> fixed[] = {
            {4, "GetVersion"}, {5, "DefineClass"}, {6, "FindClass"}, {10, "GetSuperclass"}, {13, "Throw"}, {14, "ThrowNew"},
            {15, "ExceptionOccurred"}, {16, "ExceptionDescribe"}, {17, "ExceptionClear"}, {18, "FatalError"},
            {19, "PushLocalFrame"}, {20, "PopLocalFrame"}, {21, "NewGlobalRef"}, {22, "DeleteGlobalRef"},
            {23, "DeleteLocalRef"}, {24, "IsSameObject"}, {25, "NewLocalRef"}, {26, "EnsureLocalCapacity"},
            {27, "AllocObject"}, {28, "NewObject"}, {29, "NewObjectV"}, {30, "NewObjectA"}, {31, "GetObjectClass"},
            {32, "IsInstanceOf"}, {33, "GetMethodID"}, {94, "GetFieldID"}, {113, "GetStaticMethodID"},
            {144, "GetStaticFieldID"}, {163, "NewString"}, {164, "GetStringLength"}, {165, "GetStringChars"},
            {166, "ReleaseStringChars"}, {167, "NewStringUTF"}, {168, "GetStringUTFLength"}, {169, "GetStringUTFChars"},
            {170, "ReleaseStringUTFChars"}, {171, "GetArrayLength"}, {172, "NewObjectArray"},
            {173, "GetObjectArrayElement"}, {174, "SetObjectArrayElement"}, {215, "RegisterNatives"},
            {216, "UnregisterNatives"}, {217, "MonitorEnter"}, {218, "MonitorExit"}, {219, "GetJavaVM"},
            {220, "GetStringRegion"}, {221, "GetStringUTFRegion"}, {222, "GetPrimitiveArrayCritical"},
            {223, "ReleasePrimitiveArrayCritical"}, {226, "NewWeakGlobalRef"}, {227, "DeleteWeakGlobalRef"},
            {228, "ExceptionCheck"}, {229, "NewDirectByteBuffer"}, {230, "GetDirectBufferAddress"},
            {231, "GetDirectBufferCapacity"}, {232, "GetObjectRefType"}};
        for (auto& [n, s] : fixed) names[n] = s;
        const std::pair<int, const char*> calls[] = {{34, "Call"}, {64, "CallNonvirtual"}, {114, "CallStatic"}};
        for (auto& [base, prefix] : calls)
            for (int ty = 0; ty < 10; ty++)
                for (int v = 0; v < 3; v++)
                    names[base + ty * 3 + v] = std::string(prefix) + TYPES[ty] + "Method" + (v == 1 ? "V" : v == 2 ? "A" : "");
        for (int ty = 0; ty < 9; ty++) {
            names[95 + ty] = std::string("Get") + TYPES[ty] + "Field";
            names[104 + ty] = std::string("Set") + TYPES[ty] + "Field";
            names[145 + ty] = std::string("GetStatic") + TYPES[ty] + "Field";
            names[154 + ty] = std::string("SetStatic") + TYPES[ty] + "Field";
        }
        for (int p = 0; p < 8; p++) {
            names[175 + p] = std::string("New") + PRIMS[p] + "Array";
            names[183 + p] = std::string("Get") + PRIMS[p] + "ArrayElements";
            names[191 + p] = std::string("Release") + PRIMS[p] + "ArrayElements";
            names[199 + p] = std::string("Get") + PRIMS[p] + "ArrayRegion";
            names[207 + p] = std::string("Set") + PRIMS[p] + "ArrayRegion";
        }
    }
    auto it = names.find(i);
    return it == names.end() ? "jni#" + std::to_string(i) : it->second;
}
}  // namespace

void jni::init() {
    static u64 env_table[240], vm_table[8];
    for (int i = 0; i < 240; i++) {
        std::string name = jni_name(i);
        env_table[i] = guest::make_stub("JNI:" + name, handler_for(name));
    }
    static u64 env_ptr = reinterpret_cast<u64>(env_table);
    g_env = reinterpret_cast<u64>(&env_ptr);
    auto get_env = [](Thread& t) { *reinterpret_cast<u64*>(t.x(1)) = g_env; t.setx(0, 0); };
    for (int i = 0; i < 8; i++)
        vm_table[i] = guest::make_stub("JavaVM#" + std::to_string(i),
                                       i == 4 || i == 6 || i == 7 ? Handler(get_env) : Handler([](Thread& t) { t.setx(0, 0); }));
    static u64 vm_ptr = reinterpret_cast<u64>(vm_table);
    g_vm = reinterpret_cast<u64>(&vm_ptr);
}

u64 jni::env() { return g_env; }
u64 jni::vm() { return g_vm; }

u64 jni::find_class(const std::string& name) {
    std::lock_guard lk(g_mutex);
    auto& slot = g_classes[name];
    if (!slot) {
        slot = new JObj;
        slot->cls = "java/lang/Class";
        slot->str = name;
    }
    return handle(slot);
}

u64 jni::new_object(const std::string& cls) {
    auto* o = new JObj;
    o->cls = cls;
    return handle(o);
}

u64 jni::new_string(const std::string& s) {
    auto* o = new JObj;
    o->cls = "java/lang/String";
    o->is_string = true;
    o->str = s;
    return handle(o);
}

std::string jni::to_string(u64 jstr) {
    JObj* o = obj(jstr);
    return o && o->is_string ? o->str : "";
}

void jni::define(const std::string& cls, const std::string& name, JavaMethod fn) {
    std::lock_guard lk(g_mutex);
    g_methods[cls + "." + name] = std::move(fn);
}

void jni::define_static_field(const std::string& cls, const std::string& name, JRet value) {
    std::lock_guard lk(g_mutex);
    g_static_fields[cls + "." + name] = std::move(value);
}
