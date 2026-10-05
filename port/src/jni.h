// A stand-in Java VM: the JNI function table the native code calls, plus C++
// implementations of the handful of Java methods it actually depends on.
#pragma once
#include "guest.h"

#include <functional>
#include <string>
#include <vector>

struct JObj {
    std::string cls;            // class name, e.g. "java/lang/String"
    std::string str;            // string value / class name for Class objects
    std::vector<u8> bytes;      // primitive array storage
    std::vector<u64> elems;     // object array storage
    int elem_size = 0;          // >0 for primitive arrays
    bool is_string = false, is_object_array = false;
};

struct JArg { u64 i = 0; double d = 0; };

struct JRet {
    enum Kind { None, Int, Dbl, Str, Obj } kind = None;
    u64 i = 0;
    double d = 0;
    std::string s;
    JObj* o = nullptr;
    JRet() = default;
    JRet(int v) : kind(Int), i(static_cast<u64>(static_cast<i64>(v))) {}
    JRet(long long v) : kind(Int), i(static_cast<u64>(v)) {}
    JRet(bool v) : kind(Int), i(v) {}
    JRet(double v) : kind(Dbl), d(v) {}
    JRet(const char* v) : kind(Str), s(v) {}
    JRet(const std::string& v) : kind(Str), s(v) {}
    JRet(JObj* v) : kind(Obj), o(v) {}
};

using JavaMethod = std::function<JRet(const std::vector<JArg>&)>;

namespace jni {
void init();                                   // builds the JNIEnv / JavaVM tables
u64 env();
u64 vm();
u64 find_class(const std::string& name);       // jclass handle
u64 new_object(const std::string& cls);
u64 new_string(const std::string& s);
std::string to_string(u64 jstr);
void define(const std::string& cls, const std::string& name, JavaMethod fn);
void define_static_field(const std::string& cls, const std::string& name, JRet value);
}

void register_java_methods();
